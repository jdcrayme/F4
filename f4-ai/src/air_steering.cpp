// f4-ai/src/air_steering.cpp
//
// AirSteering implementation — see header for the cascade rationale.

#include "f4/ai/air_steering.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 2.0 * PI;
} // namespace

double AirSteering::bearing_to(const geo::WorldPosition& from,
                               const geo::WorldPosition& to) noexcept {
    return std::atan2(to.x - from.x, to.y - from.y);
}

double AirSteering::heading_error(double desired_rad,
                                  double current_rad) noexcept {
    double err = desired_rad - current_rad;
    while (err > PI) err -= TWO_PI;
    while (err < -PI) err += TWO_PI;
    return err;
}

AIControlOutput AirSteering::steer(double desired_heading_rad,
                                   double target_alt_ft,
                                   double target_speed_kts,
                                   const Input& in,
                                   double throttle_floor) const {
    AIControlOutput out;

    // --- Heading: rudder for small corrections, bank-to-turn for large ---
    // Tranche 31 (user guidance): rudder for small lateral adjustments,
    // easier on the ailerons. The bank-to-turn cascade tilts the lift
    // vector — for small heading errors (beam tracking) this degrades the
    // altitude hold (the lateral-altitude coupling). Rudder yawes the nose
    // WITHOUT banking: the lift vector stays vertical, the altitude hold
    // stays decoupled. Ailerons only kick in for large errors (intercept
    // cuts), where the bank cascade's authority is needed.
    //
    // FreeFalcon autopilot.cpp:34: yPedal = headingErr * 0.05 * RTD * vt / cornerSpeed.
    //
    // EXPERIMENT C: zero the cruise-mode rudder. The FCS yaw channel is a
    // beta-command PI loop: any non-zero pedal drives beta AWAY from zero,
    // which creates a side force that opposes the bank turn. The FCS only
    // produces coordinated flight (beta=0) with pedals centered. Banking
    // with non-zero pedal causes the aircraft to yaw sideways, killing the
    // turn rate (~0.6 deg/s observed vs 2.13 deg/s theoretical at 30° bank).
    // The approach-mode rudder-for-small-corrections is preserved below.
    const double hdg_err = heading_error(desired_heading_rad, in.heading_rad);
    const double v_corner = 150.0 * 1.68781;  // ~150 kts corner speed
    const double v_fps = std::max(100.0, in.vcas_kts * 1.68781);
    out.yaw_cmd = 0.0;

    if (std::fabs(hdg_err) > approach_aileron_threshold_rad) {
        // Large heading error (intercept cut): bank-to-turn cascade.
        // EXPERIMENT G (Idea 2A): bank-rate taper. Scale the roll command
        // by proximity to the target bank so the roll arrests itself
        // before crossing zero — Falcon's maxRollDelta taper. Without this
        // the bank overshoots, the AP reverses, and you get a sinusoid.
        const double bank_target = std::clamp(bank_gain * hdg_err,
                                              -max_bank_rad, max_bank_rad);
        const double bank_err = bank_target - in.roll_rad;
        // Taper: 1.0 far from target, 0.0 at target.
        const double phi_to_target = std::fabs(bank_err);
        const double taper_window = 0.20;  // rad (~11 deg), starts decelerating within this of target
        const double taper = std::clamp(phi_to_target / taper_window, 0.0, 1.0);
        out.roll_cmd = std::clamp(taper * (roll_gain * bank_err)
                                  - roll_damp * in.roll_rate_radps,
                                  -1.0, 1.0);
    } else {
        // Small heading error (beam ride): wings-level damping only. No
        // bank command — the rudder handles the lateral, the wings stay
        // level, the lift vector stays vertical. This is the decoupling
        // that lets the altitude hold track the beam through localizer
        // corrections.
        out.roll_cmd = std::clamp(-approach_wings_level_gain * in.roll_rad
                                  - roll_damp * in.roll_rate_radps,
                                  -0.3, 0.3);
    }

    // --- Rudder: (see the lateral channel above — Tranche 31) ---
    //
    // The AI commands ZERO steady-state pedal. The Phase A2
    // "coordinated-turn feedforward" that used to live here
    // (pedal_ff = tan(bank_target) * v / g, scaled by coord_turn_scale)
    // was wrong twice over:
    //
    //   1. Dimensionally inverted. The turn-rate law is w = g*tan(phi)/v;
    //      the code computed tan(phi)*v/g — the reciprocal, ~250x too big
    //      at cruise. At 300 kts / 25 deg bank it produced ~0.73 of FULL
    //      rudder held in every banked turn.
    //   2. Conceptually wrong for this FCS. The yaw channel (fcs.cpp
    //      runYaw) is a beta-command PI loop: any nonzero pedal commands
    //      steady SIDE FORCE (nycmd = yshape*2), and aero.beta is driven
    //      directly to whatever produces it. Coordinated flight means
    //      ZERO lateral acceleration — so any nonzero "coordination"
    //      pedal is anti-coordination. The A2 law pinned |beta| at the
    //      15-deg aero clamp through every turn of the course_intercept /
    //      standard_rate_turn baselines (NAV-D1 traces): the aircraft
    //      flew 100+ ft/s sideways with its nose off the velocity vector.
    //      That was the reported "slipping a lot", the "nose points at
    //      the waypoint", and much of the in-turn altitude loss (a
    //      tilted lift vector reads as missing pitch authority).
    //
    // (The rudder is set by the lateral channel above — Tranche 31.
    // The old NAV-A law centered the pedals; the new law uses them for
    // small heading corrections, decoupling the lateral from the altitude.)

    // --- Altitude: gamma-hold ---
    // Target VS from the altitude error, convert to a commanded
    // flight-path angle, and add the CURRENT estimated alpha so the
    // pitch-attitude command is in equilibrium by construction (zero
    // steady-state VS error). The VS-error term only trims gamma; the
    // speed term damps the phugoid.
    //
    // STAB-E6: vs_ff_fpm (the target path's own vertical rate) is added
    // BEFORE the clamp, so zero altitude error commands the path's own
    // rate — the aircraft RIDES a descending beam instead of leveling
    // at each crossing and diving to re-catch it (the ±250 ft / ±3,000
    // fpm corner-chase observed in the on_glideslope trace t=40-75).
    const double alt_err = target_alt_ft - in.alt_msl_ft;
    // STAB-E7: leaky integral on altitude error (10 s time constant, same
    // leak model as the speed integral). Eliminates the P-only steady-state
    // beam offset. Clamped to alt_integral_max so transients can't wind it
    // to the VS cap.
    alt_integral_ = alt_integral_ * (1.0 - 1.0 / 600.0)
                  + alt_integral_gain * alt_err * (1.0 / 60.0);
    alt_integral_ = std::clamp(alt_integral_, -alt_integral_max, alt_integral_max);
    double vs_corr = vs_gain * alt_err + alt_integral_;
    // STAB-E10: when enabled, clamp the CORRECTION around the path
    // feedforward with an error-scaled window: tight near the path (smooth
    // beam ride), full authority far from it (from-below capture). The
    // window is always clamped by max_vs_fpm regardless.
    if (vs_corr_max_fpm >= 0.0) {
        const double window = std::clamp(vs_corr_max_fpm + 1.5 * std::fabs(alt_err),
                                          0.0, max_vs_fpm);
        vs_corr = std::clamp(vs_corr, -window, window);
    }
    const double vs_target_raw = std::clamp(in.vs_ff_fpm + vs_corr,
                                            -max_vs_fpm, max_vs_fpm);
    // STAB-E29: slew-rate limit the VS command. steer() has no dt
    // parameter; the leaky integrals above use the same fixed 1/60 s
    // major-frame assumption, so the limiter does too (design point 60
    // Hz; the test harness matches). At vs_slew_fpm_per_s = 400 a
    // full-authority change ramps over ~4 s — comparable to the FCS
    // G-lag, so the airframe can actually follow the command instead
    // of ringing through it. See the header comment for the trace
    // evidence.
    double vs_target = vs_target_raw;
    if (vs_slew_fpm_per_s > 0.0) {
        const double step = vs_slew_fpm_per_s / 60.0;
        vs_target = vs_target_ + std::clamp(vs_target_raw - vs_target_,
                                             -step, step);
    }
    vs_target_ = vs_target;
    // v_fps is already in scope (declared by the lateral channel above).
    const double gamma_now = std::asin(std::clamp((in.vs_fpm / 60.0) / v_fps,
                                                  -0.7, 0.7));
    // STAB-E17: clamp the alpha estimate to a physically-sane band. The
    // raw difference (pitch - gamma) explodes during transients: at pitch
    // +20 deg while still sinking at -4,000 fpm (gamma -10.6 deg) it
    // reports alpha = 30.6 deg — a value at which the aircraft would be
    // stalled at these speeds. Feeding that into theta_target pins the
    // pitch target at its +clamp through the ENTIRE descent (the estimate
    // says "you need 30 deg of pitch to hold this"), so the aircraft
    // zooms, the sink reverses, the estimate collapses, and the loop
    // repeats — the ±7,000 fpm / ±25 deg porpoise observed through every
    // pattern phase of the digi_full_mission trace (t=730-1040) and the
    // enroute dip to 105 ft AGL against a 3,000 ft floor (t=660-720).
    // The true trim alpha in these regimes is 2-12 deg; the clamp turns
    // the positive feedback back into a bounded feedforward.
    const double alpha_est = std::clamp(in.pitch_rad - gamma_now,
                                        alpha_min_rad, alpha_max_rad);
    const double gamma_ff = std::clamp((vs_target / 60.0) / v_fps, -0.35, 0.35);
    // EXPERIMENT Q3: Adaptive gamma_corr limit. Full authority (0.15) in
    // level flight (vs_target~0) for phugoid damping; reduced authority
    // during climbs/descents (vs_target high) to prevent the VS-error
    // correction from fighting the commanded VS and causing overshoot.
    // The reduction factor 0.4 was tuned to pass SpeedHold (5.0 kts stdev
    // threshold) while keeping the sustained-turn phugoid damping.
    const double gamma_corr_lim_eff = gamma_corr_limit
        * (1.0 - 0.4 * std::min(1.0, std::fabs(vs_target) / max_vs_fpm));
    const double gamma_corr = std::clamp(path_gain * (vs_target - in.vs_fpm),
                                         -gamma_corr_lim_eff, gamma_corr_lim_eff);
    // NAV-D: bank-compensated alpha feedforward. A level turn needs
    // nz = 1/cos(phi): at 30 deg bank the wing must carry 15.5% more
    // lift, i.e. roughly 15.5% more alpha at the same speed. The alpha
    // estimate alone (pitch - gamma) only discovers that AFTER the
    // aircraft has already sagged — measured in the course_intercept
    // trace: 853 ft lost in the first intercept turn while the VS loop
    // played catch-up through the FCS G-lag (and the FCS's own 1-G bias
    // scales by cos(phi), i.e. the WRONG way for turn compensation).
    // Multiplying the alpha term by 1/cos(phi) supplies the extra pitch
    // up front; wings level it is exactly 1.0 (no behavior change).
    // Clamped at 40 deg bank (x1.30) to stay in the linear regime.
    const double phi_for_lift = std::clamp(std::fabs(in.roll_rad), 0.0, 0.7);
    const double lift_comp = 1.0 / std::max(0.3, std::cos(phi_for_lift));
    const double theta_target = std::clamp(
        alpha_est * lift_comp + gamma_ff + gamma_corr
            - speed_damp_rad_per_kt * (in.vcas_kts - target_speed_kts),
        min_path_rad, max_path_rad);
    // Pitch-rate damping: subtract Kd*q from the stick command. Same
    // rationale as the roll-rate damping — kills the phugoid by adding
    // explicit derivative feedback that the FCS pitch-rate lag alone
    // cannot provide at high attitude_gain.
    // NAV-D2: bank G-feedforward on the stick. The theta loop above is
    // deliberately soft (low attitude_gain, the anti-phugoid tune), so a
    // roll-in transient sags ~800 ft before the loop discovers the extra
    // alpha needed (course_intercept t=6-21: pitch decayed to -1.5 deg,
    // VS -3,242, nz only recovering through the ~3 s FCS G-lag). The
    // stick feedforward supplies the 1/cos(phi) - 1 load increment
    // immediately: +0.155 stick at 30 deg bank, zero wings level. Bounded
    // by the pitch stick clamp; does not affect steady-state trim (the
    // lift_comp alpha term owns that).
    const double bank_g_ff = bank_g_ff_gain * (lift_comp - 1.0);
    // EXPERIMENT U2: Alpha-rate damping. The phugoid is an alpha-gamma
    // oscillation where alpha leads gamma by ~90°. The pitch-rate damper
    // (q) doesn't directly target this mode — q includes the gravity turn-rate
    // term that's CORRECT in banked flight. Damping the rate of the ACTUAL
    // alpha (derived from pitch - gamma, not the FCS command) targets the
    // phugoid mode directly. Uses the 1/60s major-frame assumption (same
    // as the leaky integrals). Skipped on the first frame (prev_alpha_est_
    // is NaN until the first call seeds it).
    double alpha_rate_damp_term = 0.0;
    if (!std::isnan(prev_alpha_est_)) {
        const double alpha_rate = (alpha_est - prev_alpha_est_) * 60.0;  // rad/s
        alpha_rate_damp_term = -alpha_rate_damp * alpha_rate;
    }
    prev_alpha_est_ = alpha_est;
    out.pitch_cmd = std::clamp(attitude_gain * (theta_target - in.pitch_rad)
                                - pitch_rate_damp * in.pitch_rate_radps
                                + alpha_rate_damp_term
                                + bank_g_ff,
                               pitch_min, pitch_max);

    // --- Speed: PI around a mid throttle setting + speed brake ---
    // The integral eliminates steady-state speed error, which previously
    // forced the LandingModule to reduce speed_damp (the "nose-down bias
    // when fast" workaround at landing_module.cpp:32-36). With the integral,
    // the throttle finds the right setting for any target speed and the
    // speed_damp term can stay at full strength, killing the phugoid.
    const double speed_err = target_speed_kts - in.vcas_kts;
    // Leaky integral: approximates dt=1/60 integration with a 10-second
    // time constant. The leak prevents windup when the target changes.
    // (Uses a fixed dt assumption since steer() has no dt parameter; the
    //  60 Hz major frame is the design point and the test harness matches.)
    speed_integral_ = speed_integral_ * (1.0 - 1.0 / 600.0)
                   + throttle_integral_gain * speed_err;
    speed_integral_ = std::clamp(speed_integral_, -throttle_integral_max,
                                                  throttle_integral_max);
    // EXPERIMENT V2: TECS-inspired energy term on the throttle. The throttle
    // responds to TOTAL energy error (altitude + kinetic), not just speed.
    // This makes it PROACTIVE during altitude changes: when climbing, the
    // throttle increases BEFORE the speed bleeds (instead of reacting after).
    // This prevents the "arrive at target with residual VS" problem that
    // causes the ALT_CAPTURE overshoot.
    //
    // Energy altitude: h_e = h + V²/(2g). In feet:
    //   kinetic_alt_ft = (V_target² - V_actual²) / (2g), V in fps
    // At 300 kts: 1 kt of speed ≈ 13.2 ft of energy altitude.
    const double target_v_fps = target_speed_kts * 1.68781;
    const double kinetic_alt_ft = (target_v_fps * target_v_fps - v_fps * v_fps)
                                   / (2.0 * 32.177);
    const double energy_err_ft = alt_err + kinetic_alt_ft;
    // EXPERIMENT: when the caller specifies a throttle_floor (>=0), use it
    // directly (it's the landing module's per-call minimum). When not
    // specified (<0, the default), use throttle_min (the cruise baseline).
    // This lets the landing module set a LOWER floor (0.20) than the cruise
    // baseline (0.25) — the old max(min, floor) formula kept the higher
    // cruise minimum, preventing idle in the landing descent.
    const double effective_floor = (throttle_floor >= 0.0)
                                   ? std::min(throttle_floor, throttle_min)
                                   : throttle_min;
    out.throttle_cmd = std::clamp(
        throttle_mid + throttle_gain * speed_err + speed_integral_
                      + energy_throttle_gain * energy_err_ft,
        effective_floor, throttle_max);
    // NAV-E: proportional speed brake. The old law was a relay
    // (full board above target+15, slam retracted below) — with the
    // airframe's multi-second energy lag that is a textbook limit-cycle
    // oscillator. Proportional over a 15-kt band starting at target+5.
    // (Exp L's FCS fix eliminated the phugoid that drove the cycling;
    //  the +5kt threshold is fine now that the speed swings are small.)
    //
    // EXPERIMENT W: Predictive descent speed brake. When the aircraft is
    // descending, gravity adds thrust along the flight path (g*sin(gamma)),
    // causing acceleration even at idle throttle. The speed brake should
    // deploy BEFORE the speed exceeds target — based on the predicted speed
    // gain over the next few seconds. The predicted gain is:
    //   dV/dt = g * sin(gamma) = g * (VS/60) / V_fps
    // Over 5s: dV = 5 * 32.177 * (VS/60) / V_fps  (fps)
    // Convert to kts: dV_kts = dV_fps / 1.68781
    // At -2000fpm VS, 250kt: dV_5s = 5*32.177*(-2000/60)/422 / 1.68781 = -8.5 kts
    // (negative VS = descent = speed gain, so the predicted gain is positive)
    double over_speed = in.vcas_kts - (target_speed_kts + 5.0);
    if (predictive_speedbrake_gain > 0.0 && in.vs_fpm < 0.0) {
        // Predicted speed gain from the descent over the look-ahead time
        const double dV_fps = predictive_speedbrake_lookahead_s
                             * 32.177 * (-in.vs_fpm / 60.0) / v_fps;
        const double dV_kts = dV_fps / 1.68781;
        over_speed += predictive_speedbrake_gain * dV_kts;
    }
    out.speed_brake_cmd = -1.0 + 1.85 * std::clamp(over_speed / 15.0, 0.0, 1.0);

    // --- STAB-E46: anti-balloon energy damper ---
    // The phugoid's climb half-cycle cannot be arrested through the pitch
    // channel: full nose-down stick is consumed by the FCS pitch
    // integrator's unwind time (observed: +4,600 fpm zoom at 20 deg pitch
    // AGAINST ptcmd -0.53 for 10+ s, fix18 t=1174-1183). Throttle and
    // speed brake respond in under a second and act on the CURRENT state
    // (no lag): when the actual vertical speed overshoots the commanded
    // by 1,200+ fpm, starve the zoom — chop the throttle and dump the
    // board. Phase-correct damping of the energy oscillation; the
    // counterpart of the landing module's sink guardians for the dive
    // half-cycle. (Fires ONLY on genuine balloons: vs > +800 AND fast
    // enough to give the power away. Lower guards straddle the level beam
    // ride's ±200 fpm band and chop the engine at 155-165 kts, where the
    // trim alpha (~13 deg) equals the pitch attitude and the aircraft
    // CANNOT descend — it floated 880 ft over the threshold at alpha 13,
    // fix22 t=1215-1245.)
    // EXPERIMENT L3: Only fire the balloon guard when the AP is NOT commanding
    // a climb. The guard was designed for level flight (catch the phugoid's
    // uncommanded zoom), but with vs_target=2500 (climb command) an actual
    // VS of 3700 is a 48% overshoot — normal airframe response, not a balloon.
    // Firing during a commanded climb creates a VS spike that causes overshoot.
    if (in.vs_fpm > balloon_guard_fpm &&
        in.vcas_kts > target_speed_kts - 5.0 &&
        in.vs_fpm - vs_target > 1200.0 &&
        vs_target < 500.0) {  // only when AP wants level/descent
        out.throttle_cmd = std::min(out.throttle_cmd, 0.08);
        out.speed_brake_cmd = 1.0;   // full board
    }

    return out;
}

AIControlOutput AirSteering::steer_approach(double desired_heading_rad,
                                                       double target_alt_ft,
                                                       double target_speed_kts,
                                                       const Input& in,
                                                       bool intercept,
                                                       double throttle_floor) const {
    // Tranche 31: the ILS approach technique — pitch for speed (alpha),
    // throttle for altitude (glide slope), rudder for small lateral,
    // ailerons only for large corrections. Decouples the lateral from the
    // altitude (the bank → lift-vector-tilt coupling that breaks the beam
    // ride when the localizer oscillates).
    //
    // FreeFalcon uses the cruise technique (pitch-for-altitude, throttle-
    // for-speed) even on approach (TrackPointLanding, mnvers.cpp:41). This
    // is an IMPROVEMENT over FreeFalcon, permitted by the project's design
    // principle: "Preserve functionality, not code; the implementation is
    // free to use modern architectures."
    AIControlOutput out;

    const double hdg_err = heading_error(desired_heading_rad, in.heading_rad);
    const double abs_hdg_err = std::fabs(hdg_err);

    // --- Lateral: rudder for small corrections, ailerons for large ---
    // FreeFalcon autopilot.cpp:34: yPedal = headingErr * 0.05 * RTD * vt / cornerSpeed.
    // The rudder yawes the nose toward the heading without banking — no
    // lift-vector tilt, no altitude coupling. Below the aileron threshold
    // (~10 deg) this is the only lateral input; above it the bank cascade
    // assists (intercept cuts still need coordinated turns).
    const double v_corner = 150.0 * 1.68781;  // ~150 kts corner speed
    const double v_fps = std::max(100.0, in.vcas_kts * 1.68781);
    const double rudder_scale = v_fps / v_corner;
    out.yaw_cmd = std::clamp(approach_rudder_gain * hdg_err * rudder_scale,
                              -approach_rudder_max, approach_rudder_max);

    if (intercept && abs_hdg_err > approach_aileron_threshold_rad) {
        // Large heading error (intercept cut): use the bank cascade.
        const double bank_target = std::clamp(bank_gain * hdg_err,
                                              -max_bank_rad, max_bank_rad);
        const double bank_err = bank_target - in.roll_rad;
        out.roll_cmd = std::clamp(roll_gain * bank_err - roll_damp * in.roll_rate_radps,
                                  -1.0, 1.0);
    } else {
        // Small heading error (beam ride): wings-level damping only. No
        // bank command — the rudder yawes the nose, the wings stay level,
        // the lift vector stays vertical. This is the decoupling.
        out.roll_cmd = std::clamp(-approach_wings_level_gain * in.roll_rad
                                  - roll_damp * in.roll_rate_radps,
                                  -0.3, 0.3);
    }

    // --- Pitch: for SPEED (alpha), not altitude ---
    // The primary pitch driver is the SPEED ERROR: nose up when slow (more
    // alpha → more lift → slows the descent AND bleeds speed), nose down
    // when fast. The altitude is NOT in the pitch loop — it moves to the
    // throttle. This breaks the lateral-altitude coupling: a bank for a
    // lateral correction no longer fights the altitude hold, because the
    // pitch loop isn't holding altitude.
    //
    // The speed error drives pitch directly (stick per kt). The gamma_ff
    // (the beam's descent rate) is added as a feedforward so the aircraft
    // RIDES the beam instead of commanding level flight.
    const double speed_err = target_speed_kts - in.vcas_kts;  // + = slow
    const double gamma_ff = std::clamp((in.vs_ff_fpm / 60.0) / v_fps, -0.35, 0.35);
    const double alpha_est = std::clamp(in.pitch_rad - gamma_ff,
                                        alpha_min_rad, alpha_max_rad);
    // Pitch target: alpha_est (hold current alpha) + speed correction (nose
    // up if slow) + gamma_ff (ride the beam). The speed gain is the PRIMARY
    // driver — approach_speed_pitch_gain * speed_err.
    const double theta_target = std::clamp(
        alpha_est + gamma_ff + approach_speed_pitch_gain * speed_err * 57.29578,
        min_path_rad, max_path_rad);
    out.pitch_cmd = std::clamp(attitude_gain * (theta_target - in.pitch_rad)
                                - pitch_rate_damp * in.pitch_rate_radps,
                               pitch_min, pitch_max);

    // --- Throttle: for ALTITUDE (glide slope), not speed ---
    // The altitude loop moves to the throttle: more power when below the
    // beam, less when above. This is the ILS technique — the throttle
    // controls the energy (climb/descent rate), the pitch controls the
    // speed (alpha). The leaky integral kills the steady-state beam offset.
    const double alt_err = target_alt_ft - in.alt_msl_ft;  // + = below
    approach_alt_integral_ = approach_alt_integral_ * (1.0 - 1.0 / 600.0)
                           + approach_alt_integral_gain * alt_err * (1.0 / 60.0);
    approach_alt_integral_ = std::clamp(approach_alt_integral_,
                                         -approach_alt_integral_max,
                                         approach_alt_integral_max);
    const double floor = std::max(throttle_min, throttle_floor);
    out.throttle_cmd = std::clamp(
        throttle_mid + approach_alt_throttle_gain * alt_err + approach_alt_integral_,
        floor, throttle_max);

    // Speed brake: proportional over a 15-kt band (same as steer()).
    const double over_speed = in.vcas_kts - (target_speed_kts + 5.0);
    out.speed_brake_cmd = -1.0 + 1.85 * std::clamp(over_speed / 15.0, 0.0, 1.0);

    return out;
}

} // namespace f4::ai

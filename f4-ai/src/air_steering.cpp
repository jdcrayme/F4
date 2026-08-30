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

    // --- Heading: bank-to-turn cascade ---
    // Target bank from heading error (clamped), then roll rate command
    // from bank error. Positive heading error (turn right) commands right
    // roll; the FCS roll channel is not subject to the ground pedal
    // inversion (that lives in GroundSteering for nose-wheel steering).
    //
    // The roll_damp term (-Kd * p) is critical: without it, the bank
    // cascade + FCS roll-rate lag can phase-shift into a sustained
    // limit cycle (the documented "roll flutter" symptom). The damping
    // term adds explicit derivative feedback that kills the cycle.
    const double hdg_err = heading_error(desired_heading_rad, in.heading_rad);
    const double bank_target = std::clamp(bank_gain * hdg_err,
                                          -max_bank_rad, max_bank_rad);
    const double bank_err = bank_target - in.roll_rad;
    out.roll_cmd = std::clamp(roll_gain * bank_err - roll_damp * in.roll_rate_radps,
                              -1.0, 1.0);

    // --- Rudder: centered in sustained flight (NAV-A) ---
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
    // Coordination is the yaw damper's job (Phase A1 holds beta ~ 0 with
    // the pedals centered). A steady turn needs no steady rudder; turn
    // entry/exit adverse yaw is a roll-rate effect, and if this EOM ever
    // grows adverse-yaw coefficients the right home for the compensation
    // is a roll-rate-proportional term there — not a bank-proportional
    // steady pedal here.
    out.yaw_cmd = 0.0;

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
    const double v_fps = std::max(100.0, in.vcas_kts * 1.68781);
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
    const double gamma_corr = std::clamp(path_gain * (vs_target - in.vs_fpm),
                                         -gamma_corr_limit, gamma_corr_limit);
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
    out.pitch_cmd = std::clamp(attitude_gain * (theta_target - in.pitch_rad)
                                - pitch_rate_damp * in.pitch_rate_radps
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
    out.throttle_cmd = std::clamp(
        throttle_mid + throttle_gain * speed_err + speed_integral_,
        std::max(throttle_min, throttle_floor), throttle_max);
    // NAV-E: proportional speed brake. The old law was a relay
    // (full board above target+15, slam retracted below) — with the
    // airframe's multi-second energy lag that is a textbook limit-cycle
    // oscillator: standard_rate_turn t=112-200 showed the board bang-bang
    // +0.80/-1.00, the throttle square-waving 0.08-0.93 in anti-phase,
    // speed square-waving 245-265 kts, and the altitude sawtoothing
    // +-450 ft at 9,500-10,400 (the reported "losing altitude enroute").
    // Proportional over a 15-kt band starting at target+5: brake grows
    // with the actual overspeed, releases gradually as it bleeds, no
    // relay chatter for the phugoid to feed on. Command space is
    // [-1 = retracted, +1 = full]; scale to 0.85 max extension (nav use).
    const double over_speed = in.vcas_kts - (target_speed_kts + 5.0);
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
    if (in.vs_fpm > balloon_guard_fpm &&
        in.vcas_kts > target_speed_kts - 5.0 &&
        in.vs_fpm - vs_target > 1200.0) {
        out.throttle_cmd = std::min(out.throttle_cmd, 0.08);
        out.speed_brake_cmd = 1.0;   // full board
    }

    return out;
}

} // namespace f4::ai

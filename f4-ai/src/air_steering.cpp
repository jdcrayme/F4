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

    // --- Coordinated-turn feedforward (Phase A2) ---
    //
    // Rudder-for-bank: pedal_ff = tan(bank_target) * v / g, mapped to the
    // normalized [-1, +1] command space via coord_turn_scale. Eliminates
    // steady-state sideslip in turns and reduces the load on the FCS yaw
    // damper (Phase A1). Without this, a banked turn produces beta drift
    // through the kinematics of banked flight; the damper then has to
    // react and correct, which excites the roll cascade (beta → side force
    // → rolling moment → phi drift → bank cascade corrects → repeat).
    // The feedforward kills the cycle at the source by anticipating the
    // rudder needed for a coordinated turn.
    //
    // Clamp the bank used in the formula to coord_turn_max_bank_rad so
    // tan() doesn't blow up at high bank angles; the FCS yaw damper
    // handles the residual at high bank.
    constexpr double GRAVITY_FPS2 = 32.174;
    const double bank_for_ff = std::clamp(bank_target,
                                          -coord_turn_max_bank_rad,
                                          coord_turn_max_bank_rad);
    const double v_fps_ff = std::max(100.0, in.vcas_kts * 1.68781);
    const double pedal_ff = std::tan(bank_for_ff) * v_fps_ff / GRAVITY_FPS2;
    out.yaw_cmd = std::clamp(pedal_ff * coord_turn_scale, -1.0, 1.0);

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
    const double theta_target = std::clamp(
        alpha_est + gamma_ff + gamma_corr
            - speed_damp_rad_per_kt * (in.vcas_kts - target_speed_kts),
        min_path_rad, max_path_rad);
    // Pitch-rate damping: subtract Kd*q from the stick command. Same
    // rationale as the roll-rate damping — kills the phugoid by adding
    // explicit derivative feedback that the FCS pitch-rate lag alone
    // cannot provide at high attitude_gain.
    out.pitch_cmd = std::clamp(attitude_gain * (theta_target - in.pitch_rad)
                                - pitch_rate_damp * in.pitch_rate_radps,
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
    out.speed_brake_cmd = (in.vcas_kts > target_speed_kts + 15.0) ? 0.8 : -1.0;

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

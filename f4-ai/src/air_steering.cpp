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
                                   const Input& in) const {
    AIControlOutput out;

    // --- Heading: bank-to-turn cascade ---
    // Target bank from heading error (clamped), then roll rate command
    // from bank error. Positive heading error (turn right) commands right
    // roll; the FCS roll channel is not subject to the ground pedal
    // inversion (that lives in GroundSteering for nose-wheel steering).
    const double hdg_err = heading_error(desired_heading_rad, in.heading_rad);
    const double bank_target = std::clamp(bank_gain * hdg_err,
                                          -max_bank_rad, max_bank_rad);
    out.roll_cmd = std::clamp(roll_gain * (bank_target - in.roll_rad),
                              -1.0, 1.0);

    // --- Altitude: gamma-hold ---
    // Target VS from the altitude error, convert to a commanded
    // flight-path angle, and add the CURRENT estimated alpha so the
    // pitch-attitude command is in equilibrium by construction (zero
    // steady-state VS error). The VS-error term only trims gamma; the
    // speed term damps the phugoid.
    const double alt_err = target_alt_ft - in.alt_msl_ft;
    const double vs_target = std::clamp(vs_gain * alt_err,
                                        -max_vs_fpm, max_vs_fpm);
    const double v_fps = std::max(100.0, in.vcas_kts * 1.68781);
    const double gamma_now = std::asin(std::clamp((in.vs_fpm / 60.0) / v_fps,
                                                  -0.7, 0.7));
    const double alpha_est = in.pitch_rad - gamma_now;   // lift trim
    const double gamma_ff = std::clamp((vs_target / 60.0) / v_fps, -0.35, 0.35);
    const double gamma_corr = std::clamp(path_gain * (vs_target - in.vs_fpm),
                                         -gamma_corr_limit, gamma_corr_limit);
    const double theta_target = std::clamp(
        alpha_est + gamma_ff + gamma_corr
            - speed_damp_rad_per_kt * (in.vcas_kts - target_speed_kts),
        min_path_rad, max_path_rad);
    out.pitch_cmd = std::clamp(attitude_gain * (theta_target - in.pitch_rad),
                               pitch_min, pitch_max);

    // --- Speed: P around a mid throttle setting + speed brake ---
    out.throttle_cmd = std::clamp(
        throttle_mid + throttle_gain * (target_speed_kts - in.vcas_kts),
        throttle_min, throttle_max);
    out.speed_brake_cmd = (in.vcas_kts > target_speed_kts + 15.0) ? 0.8 : -1.0;

    return out;
}

} // namespace f4::ai

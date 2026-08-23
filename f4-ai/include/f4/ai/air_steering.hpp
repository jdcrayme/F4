// f4-ai/include/f4/ai/air_steering.hpp
//
// AirSteering — shared airborne control laws for the air phase (waypoint
// navigation, approach tracking). Produces the air-movement subset of
// AIControlOutput:
//
//   - roll_cmd     bank-to-turn heading tracking (heading cascade → target
//                  bank → roll rate command), damped by current roll
//   - pitch_cmd    altitude tracking via a vertical-speed cascade, damped
//                  by current vertical speed and pitch attitude
//   - throttle_cmd speed hold (P on speed error around a mid setting)
//
// The caller (NavigationModule, LandingModule) decides WHERE to go —
// desired heading, target altitude, target speed — by computing bearings
// to waypoints, glide-slope target altitudes, etc. AirSteering turns
// those targets into stick/throttle. This mirrors GroundSteering, which
// owns the ground-phase laws (nose-wheel steering sign, taxi speed).
//
// Attitude-command rationale: the FCS interprets pstick as a G command.
// Commanding G directly against a position/altitude error winds the FCS
// integrator (observed during takeoff-rotation bring-up: a fixed 0.5
// stick limit-cycled 15deg -> -2deg pitch without lifting off). The
// cascades here keep commands small and proportional near the target.
//
// Dependencies: f4-geo (WorldPosition), f4-ai (AIControlOutput). C++20.

#pragma once

#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"

namespace f4::ai {

// ============================================================================
// AirSteering
// ============================================================================
class AirSteering {
public:
    // Per-tick aircraft state subset the controller consumes.
    struct Input {
        geo::WorldPosition position;   ///< ENU feet
        double heading_rad{0.0};       ///< compass heading (0 = north, CW +)
        double pitch_rad{0.0};         ///< pitch attitude
        double roll_rad{0.0};          ///< roll attitude (+ = right wing down)
        double roll_rate_radps{0.0};   ///< body-axis roll rate p (rad/s, + = rolling right)
        double pitch_rate_radps{0.0};  ///< body-axis pitch rate q (rad/s, + = pitching up)
        double vs_fpm{0.0};            ///< vertical speed (ft/min, + = climbing)
        double vcas_kts{0.0};          ///< calibrated airspeed
        double alt_msl_ft{0.0};        ///< altitude MSL
    };

    // --- Configuration (public doubles, f4-ai module convention) ---

    // Heading channel (bank-to-turn cascade)
    double bank_gain{2.0};             ///< target bank rad per rad of heading error
    double max_bank_rad{0.52};         ///< ~30 deg bank limit for nav comfort
    double roll_gain{3.0};             ///< roll rate command per rad of bank error.
                                       ///< Reduced from 6.0 to 3.0 — with the FCS
                                       ///< kr01=360 deg/s, a 1-deg bank error at
                                       ///< roll_gain=6.0 produced a 4-deg/s roll
                                       ///< rate (way too much for a 1-deg correction),
                                       ///< causing the bank to decay from 30 deg
                                       ///< back to 0 deg instead of holding. At
                                       ///< roll_gain=3.0 the same error produces a
                                       ///< 1-deg/s rate, which the roll_damp term
                                       ///< can stabilize.
    double roll_damp{3.0};             ///< roll-rate damping (rad/s of stick per rad/s of roll rate).
                                       ///< Raised from 1.5 to 3.0 — the FCS's kr01=360 deg/s
                                       ///< produces a large roll rate for small stick inputs
                                       ///< (rstick=-0.087 → -2.7 deg/s roll rate), so the
                                       ///< damping term needs to be strong enough to counteract
                                       ///< the bank_err proportional term near the target bank.
                                       ///< Kills the bank-cascade limit cycle by adding -Kd*p
                                       ///< to the roll command.

    // Altitude channel (gamma-hold: vertical-speed -> flight-path-angle
    // -> pitch attitude). The target pitch is alpha_est + commanded gamma,
    // so a sustained descent needs NO steady-state error — earlier
    // pure-feedback laws either tracked with a huge offset (low gain) or
    // excited the phugoid (high gain: VS lags pitch ~90 deg at the phugoid
    // frequency, so strong VS feedback is anti-damping).
    //
    // TUNING NOTE (altitude phugoid fix): the previous attitude_gain=4.0
    // produced a limit cycle through the FCS's pshape/kp01 shaping — a
    // 3-deg pitch error became a 0.24 stick, which the FCS turned into a
    // 0.46 G command, which over-drove alpha and produced a ±2000 ft
    // phugoid. Reducing attitude_gain to 2.5 and raising pitch_rate_damp
    // to 0.3 kills the cycle: smaller stick commands + more derivative
    // damping. See FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-3.
    double vs_gain{12.0};              ///< target VS (fpm) per ft of altitude error
    double max_vs_fpm{4000.0};         ///< VS cap
    double path_gain{0.0001};          ///< rad of extra gamma per fpm of VS error
    double gamma_corr_limit{0.09};     ///< clamp on the VS-error gamma correction
    double speed_damp_rad_per_kt{0.002}; ///< phugoid damping: nose-down trim when fast
    double min_path_rad{-0.21};        ///< target-pitch clamp (~-12 deg dive)
    double max_path_rad{0.31};         ///< target-pitch clamp (~+18 deg climb)
    double attitude_gain{2.5};         ///< stick per rad of pitch-attitude error
    double pitch_min{-0.35};           ///< stick clamps
    double pitch_max{0.5};
    double pitch_rate_damp{0.3};       ///< pitch-rate damping (stick per rad/s of
                                       ///< body-axis pitch rate q). Raised from 0.15
                                       ///< to kill the altitude phugoid — the FCS's
                                       ///< pitch-rate lag alone wasn't enough damping
                                       ///< at attitude_gain=2.5.

    // Speed channel
    double throttle_mid{0.6};          ///< throttle at on-target speed
    double throttle_gain{0.008};       ///< throttle per kt of underspeed
    double throttle_integral_gain{0.0005}; ///< integral on speed error —
                                            ///< eliminates steady-state speed
                                            ///< error (the cause of the
                                            ///< persistent "nose-down bias
                                            ///< when fast" that forced the
                                            ///< LandingModule to reduce
                                            ///< speed_damp — see
                                            ///< FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-3).
    double throttle_integral_max{0.3}; ///< anti-windup clamp on the integral
    double throttle_min{0.25};
    double throttle_max{1.0};          ///< MIL (nav never selects AB)

    // --- Geometry helpers ---
    [[nodiscard]] static double bearing_to(const geo::WorldPosition& from,
                                           const geo::WorldPosition& to) noexcept;
    [[nodiscard]] static double heading_error(double desired_rad,
                                              double current_rad) noexcept;

    // --- Control law ---

    /// Fly the specified heading/altitude/speed. This is the whole air
    /// control law; callers compose it with their own target selection.
    [[nodiscard]] AIControlOutput steer(double desired_heading_rad,
                                        double target_alt_ft,
                                        double target_speed_kts,
                                        const Input& in) const;

    /// Reset the integral accumulators (call on mode transition, e.g.
    /// when NavigationModule hands off to LandingModule with a different
    /// target speed — otherwise the integral carries a stale offset).
    void reset_integrators() noexcept { speed_integral_ = 0.0; }

private:
    /// Speed-channel integral accumulator (mutable so the public steer()
    /// can remain const — the integral is closed-loop state, not config).
    /// Persists across steer() calls within a single module instance.
    mutable double speed_integral_{0.0};
};

} // namespace f4::ai

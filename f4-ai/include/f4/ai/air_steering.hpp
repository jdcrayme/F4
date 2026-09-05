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
        double vs_ff_fpm{0.0};         ///< STAB-E6: feedforward VS (fpm) — the
                                       ///< TARGET PATH's own climb/descent rate
                                       ///< (e.g. the glide beam's -1,000 fpm at
                                       ///< 3 deg / 190 kts). Added to the
                                       ///< altitude-error term so that at zero
                                       ///< error the aircraft RIDES the path
                                       ///< instead of commanding level flight
                                       ///< and then diving to re-catch it.
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
    double vs_gain{6.0};              ///< target VS (fpm) per ft of altitude error.
                                       ///< Lowered from 12.0 (STAB-E1): at 12 fpm/ft
                                       ///< a 300-ft altitude error commanded a
                                       ///< 3,600 fpm climb, and through the FCS
                                       ///< G-lag (~2 s) + the aircraft's own phugoid
                                       ///< dynamics at 200-250 kts (~30-50 s period)
                                       ///< the loop over-shot each beam crossing and
                                       ///< sustained a ±8-10,000 fpm limit cycle
                                       ///< through the whole enroute phase (observed
                                       ///< in digi_full_mission trace t=200-670).
                                       ///< 6.0 halves the demand and lets the
                                       ///< path_gain damping dominate.
    double alt_integral_gain{1.2};     ///< STAB-E7: leaky integral on altitude
                                       ///< error (fpm of extra VS command per
                                       ///< ft of sustained error, per second).
                                       ///< Kills the P-only steady-state beam
                                       ///< offset (observed: riding 280 ft below
                                       ///< the glide beam for the entire final
                                       ///< because the proportional cascade
                                       ///< undershoots and nothing integrates
                                       ///< the residual out).
    double alt_integral_max{500.0};    ///< STAB-E7: clamp on the altitude integral
                                       ///< (fpm). Bounds the correction so a
                                       ///< transient can't wind it to the VS cap.
    double max_vs_fpm{2500.0};         ///< VS cap. Lowered from 4000 (STAB-E1) —
                                       ///< a 4,000 fpm command through a 2 s FCS
                                       ///< lag is 130+ ft of overshoot before the
                                       ///< aircraft can respond; 2,500 still covers
                                       ///< jet departures/descents.
    double vs_slew_fpm_per_s{400.0};   ///< STAB-E29: slew-rate limit on the
                                       ///< VS command (fpm per second). The
                                       ///< altitude loop's vs_target used to
                                       ///< STEP to the cap on any target
                                       ///< change; the airframe (through the
                                       ///< ~2-3 s FCS G-lag + its own phugoid
                                       ///< dynamics) cannot follow a step —
                                       ///< every capture overshot by
                                       ///< 2,000-3,500 fpm (base leg dive
                                       ///< -5,000 fpm against a -324 fpm
                                       ///< command, fix3 trace t=1055-1080),
                                       ///< and the saturated gamma damper
                                       ///< could not arrest it. Ramping the
                                       ///< command at 400 fpm/s excites
                                       ///< nothing: a full-authority change
                                       ///< takes ~4 s, about the FCS lag.
                                       ///< Negative disables the limiter.
    double balloon_guard_fpm{800.0};   ///< STAB-E46/E48: the anti-balloon
                                       ///< energy damper's VS guard (fpm):
                                       ///< the damper (throttle chop + full
                                       ///< speed brake) fires when the
                                       ///< actual VS exceeds the commanded
                                       ///< by 1,200+ fpm AND vs is above
                                       ///< this guard AND the aircraft is
                                       ///< not slow. PER-TUNE: the pattern
                                       ///< legs want it LOW (~200 — chopping
                                       ///< their zooms is what keeps the
                                       ///< ±5,000-11,000 fpm swings down,
                                       ///< fix23) while the final's ride
                                       ///< band sits at ±200 fpm and a low
                                       ///< guard straddles it, chopping the
                                       ///< approach engine at 155 kts into
                                       ///< a stall-float (fix21/22) — the
                                       ///< final wants ~800.
    double vs_corr_max_fpm{-1.0};      ///< STAB-E10: BASE cap on the VS
                                       ///< CORRECTION around the path
                                       ///< feedforward (fpm). Negative = disabled
                                       ///< (corrections clamp to max_vs_fpm
                                       ///< absolute). The effective window
                                       ///< SCALES with the altitude error
                                       ///< (window = base + 1.5*|err|, clamped
                                       ///< to max_vs_fpm) so close-in beam rides
                                       ///< get a tight ±300-450 fpm while a
                                       ///< from-below capture keeps full
                                       ///< authority — a fixed window either
                                       ///< corner-chased (wide) or could never
                                       ///< catch a 500+ ft from-below entry
                                       ///< (narrow, observed at on_glideslope).
    double path_gain{0.0004};          ///< rad of extra gamma per fpm of VS error.
                                       ///< Raised from 0.0001 (STAB-E1): the VS-error
                                       ///< damping term is what kills the phugoid;
                                       ///< at 0.0001 a 2,000 fpm VS error corrected
                                       ///< only 0.03 rad (~1.7 deg) — too weak to
                                       ///< arrest the zoom/dive cycle. 0.0004 gives
                                       ///< 0.08 rad (~4.6 deg) at the same error.
    double gamma_corr_limit{0.09};     ///< clamp on the VS-error gamma correction
    double alpha_min_rad{-0.06};       ///< STAB-E17: clamp on the alpha estimate
                                       ///< (pitch - gamma) used as the pitch
                                       ///< feedforward trim. ~-3.4 deg: a real
                                       ///< jet never trims below slightly
                                       ///< negative-alpha; the raw difference
                                       ///< during transients (nose-up while
                                       ///< still sinking hard) reports 25-35
                                       ///< deg and pins theta_target at its
                                       ///< clamp — the porpoise driver.
    double alpha_max_rad{0.28};        ///< ~+16 deg: covers the low-speed
                                       ///< high-alpha regimes (approach
                                       ///< config ~12 deg) without
                                       ///< letting a transient estimate drive
                                       ///< the target into the stall region.
    double speed_damp_rad_per_kt{0.002}; ///< phugoid damping: nose-down trim when fast
    double min_path_rad{-0.21};        ///< target-pitch clamp (~-12 deg dive)
    double max_path_rad{0.31};         ///< target-pitch clamp (~+18 deg climb)
    double attitude_gain{1.8};         ///< stick per rad of pitch-attitude error.
                                       ///< Lowered from 2.5 (STAB-E1) — see the
                                       ///< phugoid note above; slower stick = less
                                       ///< energy injected per cycle.
    double pitch_min{-0.35};           ///< stick clamps
    double pitch_max{0.5};
    double pitch_rate_damp{0.5};       ///< pitch-rate damping (stick per rad/s of
                                       ///< body-axis pitch rate q). Raised from 0.3
                                       ///< (STAB-E1) — more derivative feedback to
                                       ///< damp the altitude phugoid through the
                                       ///< FCS pitch-rate lag.
    double bank_g_ff_gain{1.0};        ///< NAV-D2: stick feedforward for the
                                       ///< 1/cos(phi)-1 load increment in banked
                                       ///< flight. 1.0 maps the +15.5% lift need
                                       ///< at 30 deg bank to +0.155 stick,
                                       ///< immediately (the soft theta loop takes
                                       ///< 10+ s to find the same G through the
                                       ///< FCS lag — the in-turn altitude sag).
                                       ///< Zero wings-level; bounded by the stick
                                       ///< clamps.

    // Speed channel
    double throttle_mid{0.6};          ///< throttle at on-target speed
    double throttle_gain{0.005};       ///< throttle per kt of underspeed. Lowered
                                       ///< from 0.008 (STAB-E1): with a ±40 kt phugoid
                                       ///< speed swing, 0.008 slammed the throttle
                                       ///< rail-to-rail (0.04 to 1.00 observed on
                                       ///< final) in anti-phase with the phugoid,
                                       ///< PUMPING the energy oscillation instead of
                                       ///< damping it. 0.005 keeps the speed loop
                                       ///  slower than the phugoid.
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

    // --- Approach mode (Tranche 31: pitch-for-speed + throttle-for-altitude
    // + rudder-for-lateral). The ILS technique: decouple the lateral from
    // the altitude by using rudder for small heading corrections (no bank,
    // no lift-vector tilt) and moving the altitude loop to the throttle.
    // Pitch holds alpha (speed) — independent of bank. See
    // LANDING_PRECISION_FORMATION_AAR_PLAN.md §1.3 (the lateral-altitude
    // coupling root cause) and the user's control-law guidance.
    double approach_rudder_gain{0.05};   ///< yaw_cmd per rad of heading error,
                                          ///<  scaled by v/v_corner (FreeFalcon
                                          ///<  autopilot.cpp:34 form). 0.05 at
                                          ///<  200 kts / 150 corner = 0.067 rad
                                          ///<  pedal per 10-deg heading error.
    double approach_rudder_max{0.3};      ///< clamp on the rudder command
    double approach_aileron_threshold_rad{0.175}; ///< ~10 deg: below this, use
                                          ///<  rudder only; above, the bank
                                          ///<  cascade assists (intercept cuts)
    double approach_speed_pitch_gain{0.003}; ///< stick per kt of speed error
                                          ///<  (nose up when slow). The PRIMARY
                                          ///<  pitch driver in approach mode.
    double approach_alt_throttle_gain{0.0008}; ///< throttle per ft of altitude
                                          ///<  error (more power when below).
    double approach_alt_integral_gain{0.00005}; ///< integral on altitude error
                                          ///<  (kills steady-state beam offset).
    double approach_alt_integral_max{0.3}; ///< clamp on the altitude integral
    double approach_wings_level_gain{2.0}; ///< roll damping when in rudder-only
                                          ///<  mode (keep wings level without
                                          ///<  banking for small corrections)

    // --- Rudder (NAV-A) ---
    // There are no rudder gains here on purpose: the AI commands
    // yaw_cmd = 0 in sustained flight and the FCS yaw damper (fcs.cpp
    // runYaw, Phase A1) holds beta ~ 0. The old Phase A2
    // "rudder-for-bank" feedforward (tan(bank)*v/g, dimensionally
    // inverted ~250x) pinned |beta| at the aero clamp in every turn —
    // see air_steering.cpp for the full post-mortem. If adverse-yaw
    // compensation is ever needed it belongs in the FCS as a
    // roll-rate-proportional term, not a steady bank-proportional pedal.

    // --- Geometry helpers ---
    [[nodiscard]] static double bearing_to(const geo::WorldPosition& from,
                                           const geo::WorldPosition& to) noexcept;
    [[nodiscard]] static double heading_error(double desired_rad,
                                              double current_rad) noexcept;

    // --- Control law ---

    /// Fly the specified heading/altitude/speed. This is the whole air
    /// control law; callers compose it with their own target selection.
    /// throttle_floor (STAB-E36, optional): per-call minimum for the
    /// speed PI — the landing-configuration states (gear + full flaps)
    /// must never allow idle (the drag bucket converts it into an
    /// unstoppable dive) while the clean/flap-1/2 states need idle
    /// authority to BLEED speed. The effective floor is
    /// max(throttle_min, throttle_floor); negative disables the override.
    [[nodiscard]] AIControlOutput steer(double desired_heading_rad,
                                        double target_alt_ft,
                                        double target_speed_kts,
                                        const Input& in,
                                        double throttle_floor = -1.0) const;

    /// Approach-mode steering (Tranche 31): the ILS technique — pitch for
    /// speed (alpha), throttle for altitude (glide slope), rudder for small
    /// lateral, ailerons only for large corrections. Decouples the lateral
    /// from the altitude (the coupling that breaks the beam ride when the
    /// localizer oscillates). `intercept` true = large corrections allowed
    /// (InterceptFinal); false = rudder-only (OnFinal beam ride).
    [[nodiscard]] AIControlOutput steer_approach(double desired_heading_rad,
                                                 double target_alt_ft,
                                                 double target_speed_kts,
                                                 const Input& in,
                                                 bool intercept,
                                                 double throttle_floor = -1.0) const;

    /// Reset the integral accumulators (call on mode transition, e.g.
    /// when NavigationModule hands off to LandingModule with a different
    /// target speed — otherwise the integral carries a stale offset).
    /// Also resets the STAB-E29 VS-command slew state.
    void reset_integrators() noexcept {
        speed_integral_ = 0.0;
        alt_integral_ = 0.0;
        vs_target_ = 0.0;
        approach_alt_integral_ = 0.0;
    }

private:
    /// Speed-channel integral accumulator (mutable so the public steer()
    /// can remain const — the integral is closed-loop state, not config).
    /// Persists across steer() calls within a single module instance.
    mutable double speed_integral_{0.0};
    /// STAB-E7: altitude-channel integral accumulator (fpm of VS command).
    mutable double alt_integral_{0.0};
    /// STAB-E29: last VS command (fpm) — the slew-rate limiter's state.
    mutable double vs_target_{0.0};
    /// Tranche 31: approach-mode altitude integral (throttle-channel).
    mutable double approach_alt_integral_{0.0};
};

} // namespace f4::ai

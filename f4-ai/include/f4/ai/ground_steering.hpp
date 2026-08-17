// f4-ai/include/f4/ai/ground_steering.hpp
//
// GroundSteering — shared low-speed ground steering + speed control for AI
// ground movement (taxi, runway lineup, takeoff roll, landing rollout,
// taxi-in).
//
// Produces the ground-motion subset of AIControlOutput:
//   - yaw_cmd      from compass heading error (drives nose-wheel steering
//                  via PilotInput.ypedal; BrainComponent forces noseSteerOn)
//   - throttle_cmd P-control on speed error, clamped to a low taxi band
//   - wheel_brakes when overspeeding the target or holding position
//
// HEADING / PEDAL SIGN CONVENTION (verified against the EOM):
// The AI layer works in compass headings: 0 = north, clockwise positive
// (this is how IAircraftState::heading_rad(), scenario runway headings and
// the recorder all present it). The EOM's ground steering integrates
// psi_delta = -ypedal * steerRate * dt (eom.cpp) while its velocity trig
// maps psi as north = cos(psi), east = sin(psi) — i.e. psi IS a compass
// angle. Positive ypedal therefore DECREASES the compass heading: it turns
// the aircraft LEFT. To turn right (increase heading) the pedal must be
// negative. That inversion is centralized in pedal_for_heading_error()
// below; the EOM/tests' comments claiming "positive pedal = right turn"
// describe the NED z-down rotation sense, not the compass heading.
//
// Dependencies: f4-geo (WorldPosition), f4-ai (AIControlOutput). C++20.

#pragma once

#include <f4/geo/position.hpp>

#include "f4/ai/ai_output.hpp"

namespace f4::ai {

// ============================================================================
// GroundSteering
// ============================================================================
class GroundSteering {
public:
    // Per-tick aircraft state subset the controller consumes.
    struct Input {
        geo::WorldPosition position;   ///< ENU feet
        double heading_rad{0.0};       ///< compass heading (0 = north, CW +)
        double speed_kts{0.0};         ///< ground speed proxy (vcas)
    };

    // --- Configuration (public doubles, f4-ai module convention) ---
    double heading_gain{3.0};             ///< pedal units per rad of heading error
    double speed_gain{0.06};              ///< throttle per kt of underspeed
    double throttle_creep{0.04};          ///< idle creep to overcome static friction
    double max_throttle{0.25};            ///< upper edge of the taxi throttle band
    double brake_margin_kts{4.0};         ///< brake when above target by this
    double stop_decel_fps2{3.0};          ///< comfortable decel toward a stop point
    double stop_radius_ft{15.0};          ///< "arrived" distance for stop points
    double sharp_turn_heading_rad{0.5};   ///< ~30 deg: slow down in sharp turns
    double sharp_turn_speed_kts{8.0};     ///< reduced speed for sharp turns

    // --- Geometry helpers (compass conventions, ENU frame) ---

    /// Compass bearing from `from` to `to`: atan2(east, north).
    [[nodiscard]] static double bearing_to(const geo::WorldPosition& from,
                                           const geo::WorldPosition& to) noexcept;

    /// Smallest signed difference desired - current, wrapped to [-pi, pi].
    /// Positive means the desired heading is clockwise (right) of current.
    [[nodiscard]] static double heading_error(double desired_rad,
                                              double current_rad) noexcept;

    // --- Control laws ---

    /// Steer toward `target`, regulating speed toward `target_speed_kts`.
    /// When `stop_at_target`, decelerate so the aircraft stops at the target
    /// (used for hold-short points and parking spots).
    [[nodiscard]] AIControlOutput steer_toward(const geo::WorldPosition& target,
                                               const Input& in,
                                               double target_speed_kts,
                                               bool stop_at_target) const;

    /// Roll along `desired_heading_rad` at `target_speed_kts` (runway lineup,
    /// takeoff roll, landing rollout). With `stop`, hold brakes at idle.
    [[nodiscard]] AIControlOutput align_heading(double desired_heading_rad,
                                                const Input& in,
                                                double target_speed_kts,
                                                bool stop) const;

    /// Hold position: brakes on, throttle idle.
    [[nodiscard]] AIControlOutput hold() const;

private:
    /// Pedal command for a heading error — the ONE place the EOM's inverted
    /// nose-wheel sign lives. Positive error (need to turn right) produces
    /// a negative pedal command.
    [[nodiscard]] double pedal_for_heading_error(double heading_error_rad) const noexcept;

    /// Throttle + brakes for a speed target (shared by both control laws).
    void apply_speed_control(AIControlOutput& out,
                             double target_speed_kts,
                             double speed_kts) const noexcept;
};

} // namespace f4::ai

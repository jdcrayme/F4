// f4-ai/src/ground_steering.cpp
//
// GroundSteering implementation — see header for the sign-convention notes.

#include "f4/ai/ground_steering.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 2.0 * PI;
constexpr double FPS_PER_KT = 1.6878098571011957;  // 1 knot in ft/s
} // namespace

// ============================================================================
// Geometry helpers
// ============================================================================

double GroundSteering::bearing_to(const geo::WorldPosition& from,
                                  const geo::WorldPosition& to) noexcept {
    // Compass bearing: 0 = +Y (north), 90 deg = +X (east) -> atan2(east, north).
    return std::atan2(to.x - from.x, to.y - from.y);
}

double GroundSteering::heading_error(double desired_rad,
                                     double current_rad) noexcept {
    double err = desired_rad - current_rad;
    while (err > PI) err -= TWO_PI;
    while (err < -PI) err += TWO_PI;
    return err;
}

// ============================================================================
// Control laws
// ============================================================================

AIControlOutput GroundSteering::steer_toward(const geo::WorldPosition& target,
                                             const Input& in,
                                             double target_speed_kts,
                                             bool stop_at_target) const {
    AIControlOutput out;
    out.gear_handle_down = true;

    const double desired_hdg = bearing_to(in.position, target);
    const double hdg_err = heading_error(desired_hdg, in.heading_rad);
    out.yaw_cmd = pedal_for_heading_error(hdg_err);

    // Speed target: slow down for sharp turns, and decelerate toward a stop
    // when the target is a stop point (v = sqrt(2*a*d) from the stop radius).
    double v_target = target_speed_kts;
    if (std::abs(hdg_err) > sharp_turn_heading_rad) {
        v_target = std::min(v_target, sharp_turn_speed_kts);
    }
    if (stop_at_target) {
        const double dx = target.x - in.position.x;
        const double dy = target.y - in.position.y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        const double brake_dist = std::max(0.0, dist - stop_radius_ft);
        const double v_limit_fps = std::sqrt(2.0 * stop_decel_fps2 * brake_dist);
        v_target = std::min(v_target, v_limit_fps / FPS_PER_KT);
    }

    apply_speed_control(out, v_target, in.speed_kts);
    return out;
}

AIControlOutput GroundSteering::align_heading(double desired_heading_rad,
                                              const Input& in,
                                              double target_speed_kts,
                                              bool stop) const {
    AIControlOutput out;
    out.gear_handle_down = true;

    const double hdg_err = heading_error(desired_heading_rad, in.heading_rad);
    out.yaw_cmd = pedal_for_heading_error(hdg_err);

    if (stop) {
        out.throttle_cmd = 0.0;
        out.wheel_brakes = true;
    } else {
        apply_speed_control(out, target_speed_kts, in.speed_kts);
    }
    return out;
}

AIControlOutput GroundSteering::hold() const {
    AIControlOutput out;
    out.gear_handle_down = true;
    out.throttle_cmd = 0.0;
    out.wheel_brakes = true;
    return out;
}

// ============================================================================
// Private helpers
// ============================================================================

double GroundSteering::pedal_for_heading_error(double heading_error_rad) const noexcept {
    // Positive compass error = turn right = NEGATIVE pedal (see header).
    return std::clamp(-heading_gain * heading_error_rad, -1.0, 1.0);
}

void GroundSteering::apply_speed_control(AIControlOutput& out,
                                         double target_speed_kts,
                                         double speed_kts) const noexcept {
    if (target_speed_kts <= 0.5) {
        // Stopped target: idle and hold brakes once (nearly) stopped.
        out.throttle_cmd = 0.0;
        out.wheel_brakes = (speed_kts < 1.0);
        return;
    }
    out.throttle_cmd = std::clamp(throttle_creep + speed_gain * (target_speed_kts - speed_kts),
                                  0.0, max_throttle);
    out.wheel_brakes = (speed_kts > target_speed_kts + brake_margin_kts);
}

} // namespace f4::ai

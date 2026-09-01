// f4-ai/src/ground_avoid_module.cpp
//
// GroundAvoidModule implementation — see ground_avoid_module.hpp for the
// design notes (FreeFalcon reference §18.3 constants; the pullupTimer).

#include "f4/ai/modules/ground_avoid_module.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai::modules {

AirSteering::Input GroundAvoidModule::steering_input(
    const flight::IAircraftState& s) const noexcept {
    AirSteering::Input in;
    in.position = geo::WorldPosition(s.position_east_ft(),
                                     s.position_north_ft(),
                                     s.altitude_msl_ft());
    in.heading_rad = s.heading_rad();
    in.pitch_rad = s.pitch_angle_rad();
    in.roll_rad = s.roll_angle_rad();
    in.roll_rate_radps = s.roll_rate_radps();
    in.pitch_rate_radps = s.pitch_rate_radps();
    in.vs_fpm = s.vertical_speed_fpm();
    in.vcas_kts = s.vcas_kts();
    in.alt_msl_ft = s.altitude_msl_ft();
    return in;
}

void GroundAvoidModule::reset() {
    pulling_ = false;
    hold_timer_ = 0.0;
    clearance_ft_ = 1.0e6;
    air_.reset_integrators();
}

AIControlOutput GroundAvoidModule::update(
    double dt, const flight::IAircraftState* state,
    const GroundAvoidModule::TerrainPicture& picture) {
    AIControlOutput out{};

    // No ownship state, on the ground, module disarmed, or no picture:
    // idle. An aircraft without a terrain picture (a host that never
    // pushed one) flies exactly as it did before this module existed.
    if (state == nullptr || state->on_ground() || !cfg_.enabled ||
        !picture.valid) {
        if (pulling_) reset();
        return out;
    }

    const double alt = state->altitude_msl_ft();

    // --- The two clearance terms -----------------------------------------
    // Now: under the jet. Predicted: where the jet will be at the look-
    // ahead horizon if it keeps its current sink (climbs never hurt) over
    // the worst terrain in the forward cone.
    const double clearance_now = alt - picture.terrain_here_ft;

    const double sink_fpm = std::min(0.0, state->vertical_speed_fpm());
    const double pred_alt =
        alt + (sink_fpm / 60.0) * cfg_.lookahead_sec;
    const double clearance_pred = pred_alt - picture.terrain_ahead_ft;

    clearance_ft_ = std::min(clearance_now, clearance_pred);

    const bool needed = clearance_ft_ < cfg_.min_clearance_ft;
    const bool released =
        clearance_ft_ > cfg_.min_clearance_ft + cfg_.clear_margin_ft;

    if (needed) {
        pulling_ = true;
        hold_timer_ = cfg_.pullup_hold_sec;  // re-arm the continuation
    } else if (pulling_) {
        if (released) {
            // Clear by the margin: let the hold clock run down anyway (a
            // ridge line can dip in and out of the margin band; the hold
            // guarantees one clean recovery, not a flap fest).
        }
        hold_timer_ -= dt;
        if (hold_timer_ <= 0.0 && released) {
            reset();
            return out;
        }
    } else {
        return out;  // clean picture, never armed — nothing to do
    }

    // --- Fly the recovery -------------------------------------------------
    // Hold the current heading (roll toward level, low bank cap), climb
    // hard to terrain_ahead + 2x the clearance floor, full throttle. The
    // AirSteering VS/altitude cascade does the flying; the caps below are
    // the escape tune, not the nav-comfort defaults.
    //
    // The comfort limiters come OFF for the recovery (the nav-comfort
    // tune's STAB-E29 400 fpm/s VS-command slew ramps a 10,000-fpm
    // escape command over ~25 s — a pull-up is a NOW maneuver; and
    // STAB-E46's energy damper would chop the throttle and dump the
    // board against the recovery climb). A recovery lives seconds; the
    // phugoid machinery those limiters fight does not apply.
    const double target_alt =
        std::max(alt, picture.terrain_ahead_ft) +
        cfg_.climb_clearance_mult * cfg_.min_clearance_ft;

    air_.max_vs_fpm = cfg_.max_vs_fpm;
    air_.max_bank_rad = cfg_.max_bank_rad;
    air_.vs_slew_fpm_per_s = -1.0;    // STAB-E29: OFF during the pull-up
    air_.balloon_guard_fpm = 1.0e9;   // STAB-E46: OFF during the pull-up

    out = air_.steer(state->heading_rad(), target_alt,
                     cfg_.escape_speed_kts, steering_input(*state));

    // Full power and boards in — the escape is a max-performance event
    // (GPWS doctrine: thrust levers full forward). Set explicitly — the
    // steering speed loop's PI would park at a cruise setting when the
    // target speed happens to match, and the proportional brake would
    // creep out against the AB acceleration.
    out.throttle_cmd = 1.5;
    out.speed_brake_cmd = -1.0;

    // The override: preempt every other rung in the brain's ladder.
    out.has_override = true;
    return out;
}

} // namespace f4::ai::modules

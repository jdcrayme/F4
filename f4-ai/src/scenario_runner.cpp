// f4-ai/src/scenario_runner.cpp
//
// ScenarioRunner implementation — the headless simulation loop.

#include "f4/ai/scenario_runner.hpp"

#include <cmath>

namespace f4::ai {

// ============================================================================
// Map AIControlOutput to PilotInput
// ============================================================================

flight::PilotInput ScenarioRunner::map_to_pilot_input(
    const ai::AIControlOutput& ai_out)
{
    flight::PilotInput pi;
    pi.pstick    = ai_out.pitch_cmd;
    pi.rstick    = ai_out.roll_cmd;
    pi.ypedal    = ai_out.yaw_cmd;
    pi.throttle  = ai_out.throttle_cmd;
    pi.speedBrake = ai_out.speed_brake_cmd;
    pi.gearHandle = ai_out.gear_handle_down ? 1.0 : -1.0;
    pi.wheelBrakes = ai_out.wheel_brakes;
    pi.parkingBrake = ai_out.parking_brake;
    pi.noseSteerOn = true;  // always on for AI

    pi.validate();
    return pi;
}

// ============================================================================
// Build snapshot
// ============================================================================

recorder::FlightSnapshot ScenarioRunner::build_snapshot(
    std::uint64_t tick, double sim_time_s) const
{
    recorder::FlightSnapshot snap;
    snap.tick = tick;
    snap.sim_time_s = sim_time_s;
    snap.entity_id = entity_id_;
    snap.callsign = callsign_;

    // From flight model state
    if (fm_) {
        const auto& state = fm_->state();
        const auto& kin = state.kin;

        snap.position = geo::WorldPosition(
            kin.x, kin.y, -kin.z);  // NED z-down -> ENU z-up

        snap.heading_rad = flight::to_radians(kin.psi);
        snap.pitch_rad = flight::to_radians(kin.theta);
        snap.roll_rad = flight::to_radians(kin.phi);
        snap.altitude_msl_ft = -kin.z;  // NED: z-down, altitude = -z
        snap.vcas_kts = state.vcas;
        snap.vt_fps = kin.vt;
        snap.mach = state.mach;

        // Ground speed (horizontal component of velocity)
        snap.gs_kts = std::sqrt(kin.xdot * kin.xdot + kin.ydot * kin.ydot)
                      * 3600.0 / 6076.12;  // ft/s -> knots

        snap.on_ground = !state.gear.inAir;
        snap.ground_speed_kts = snap.on_ground ? snap.gs_kts : 0.0;

        snap.engine_rpm = state.engine.rpm;
        snap.afterburner_lit = state.engine.aburnLit;
        snap.fuel_lbs = state.fuel.fuel_lbs;

        snap.nz = state.loads.nzcgs;
        snap.nx = state.loads.nxcgs;
    }

    // AI brain state (if available)
    if (brain_) {
        snap.ai_mode = brain_->current_mode();
    }

    return snap;
}

// ============================================================================
// Run the simulation loop
// ============================================================================

void ScenarioRunner::run(double duration_s, double dt)
{
    const std::uint64_t total_ticks = static_cast<std::uint64_t>(duration_s / dt);
    total_ticks_ = 0;
    total_sim_time_ = 0.0;

    for (std::uint64_t tick = 0; tick < total_ticks; ++tick) {
        const double sim_time = tick * dt;

        // 1. AI produces control inputs
        ai::AIControlOutput ai_out;
        if (brain_) {
            ai_out = brain_->update(dt);
        }

        // 2. Map AI output to pilot input
        auto pilot_input = map_to_pilot_input(ai_out);

        // 3. Flight model integrates one step
        //    FlightModel::update requires (dt, input, groundZ_ft, groundNormal).
        //    Use the flight model's cached ground state for groundZ and normal.
        if (fm_) {
            const double groundZ = fm_->state().gear.groundZ_ft;
            const auto& groundNormal = fm_->state().gear.groundNormal;
            fm_->update(dt, pilot_input, groundZ, groundNormal);
        }

        // 4. Update entity transform from flight model state
        if (fm_ && world_) {
            const auto& kin = fm_->state().kin;
            auto pos = geo::WorldPosition(kin.x, kin.y, -kin.z);
            // EntityWorld transform update would go here
            (void)pos;  // suppress unused warning until entity update is wired
        }

        // 5. Process messages (ATC responses, etc.)
        if (bus_) {
            bus_->flush_pending();
        }

        // 6. Record snapshot (at the configured rate)
        if (recorder_ && (tick % snapshot_rate_ == 0)) {
            auto snap = build_snapshot(tick, sim_time);

            // Fill in control inputs from the AI output
            snap.pitch_cmd = ai_out.pitch_cmd;
            snap.roll_cmd = ai_out.roll_cmd;
            snap.yaw_cmd = ai_out.yaw_cmd;
            snap.throttle_cmd = ai_out.throttle_cmd;
            snap.speed_brake_cmd = ai_out.speed_brake_cmd;
            snap.gear_handle_down = ai_out.gear_handle_down;
            snap.wheel_brakes = ai_out.wheel_brakes;
            snap.parking_brake = ai_out.parking_brake;

            recorder_->record(snap);
        }

        total_ticks_ = tick + 1;
        total_sim_time_ = sim_time + dt;
    }

    // Set scenario name on the recorder
    if (recorder_ && !scenario_name_.empty()) {
        recorder_->set_scenario_name(scenario_name_);
    }
}

} // namespace f4::ai

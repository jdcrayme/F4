// f4-simulation/src/simulation.cpp
//
// Simulation — the orchestration layer that ties EntityWorld + MessageBus +
// ModelDatabase + AircraftConfig together and runs the tick loop.
//
// The aircraft entity is composed of four sibling components (see
// Docs/AIRCRAFT_BINDING_DESIGN.md for the full rationale):
//   - TransformComponent       (where it is — position + quaternion)
//   - FlightModelComponent     (how it moves — 6-DOF EOM, FCS, gear; runs in pass 2)
//   - VisualModelComponent     (what the renderer draws — ModelRecord* + LOD + DOF state)
//   - BrainComponent           (who's flying — wraps TakeoffModule, runs in pass 1)
//
// The brain finds the flight model via interface-based lookup
// (get_interface<IAircraftState>()), not a raw pointer. The entity ID is the
// binding — there is no AircraftClass equivalent.

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/visual_model_component.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/atc/stub_atc.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/snapshot.hpp>

#include <cmath>
#include <stdexcept>

namespace f4::simulation {

Simulation::Simulation(Scenario scenario, std::filesystem::path asset_dir)
    : scenario_(std::move(scenario))
    , asset_dir_(std::move(asset_dir))
    , model_db_(std::make_unique<f4::models::ModelDatabase>())
{
}

Simulation::~Simulation() = default;

void Simulation::initialize() {
    // Order matters: ATC must be subscribed BEFORE the brain's first update()
    // tick (which publishes a TaxiRequest). The brain's initialize() runs
    // lazily on first update(), so we just need StubATC alive before tick().
    wire_atc();
    load_models();
    load_aircraft_config();
    spawn_aircraft();

    // Set up the recorder if the scenario enables it.
    if (scenario_.record) {
        recorder_ = std::make_unique<f4::recorder::FlightRecorder>();
        recorder_->set_scenario_name(scenario_.name);
    }
}

void Simulation::load_models() {
    const auto& hdr = scenario_.models_hdr_path;
    const auto& lod = scenario_.models_lod_path;
    if (hdr.empty() || lod.empty()) {
        // No models loaded — VisualModelComponent::model_record will be null.
        // The renderer should skip drawing in that case. This is acceptable
        // for headless runs that don't need rendering.
        return;
    }
    auto err = model_db_->load(hdr, lod);
    if (!err.empty()) {
        throw std::runtime_error("Simulation::load_models: " + err);
    }
    if (!scenario_.models_tex_path.empty()) {
        auto tex_err = model_db_->load_tex(scenario_.models_tex_path);
        if (!tex_err.empty()) {
            // Non-fatal: textures are optional for the geometry to load.
            // The renderer will use vertex colors as a fallback.
        }
    }
}

void Simulation::load_aircraft_config() {
    if (scenario_.aircraft.empty()) return;  // validate() in scenario loader catches this
    const auto& path = scenario_.aircraft.front().aircraft_config_path;
    if (path.empty()) return;
    auto result = f4::data::loadConfig(path);
    if (!result.ok) {
        std::string msg = "Simulation::load_aircraft_config: failed to load '" + path + "':";
        for (const auto& e : result.errors) msg += "\n  " + e;
        throw std::runtime_error(msg);
    }
    aircraft_cfg_ = std::move(result.config);
}

void Simulation::spawn_aircraft() {
    using namespace f4::entities;
    using namespace f4::flight;
    using namespace f4::ai;
    using namespace f4::simulation;  // VisualModelComponent lives here

    if (scenario_.aircraft.empty()) {
        throw std::runtime_error("Simulation::spawn_aircraft: scenario has no aircraft");
    }
    const auto& sc = scenario_.aircraft.front();

    auto h = world_.create();

    // 1. TransformComponent — initial pose at the parking spot.
    //    TransformComponent uses a quaternion (qw,qx,qy,qz) for orientation,
    //    not Euler angles. Convert heading -> quaternion about Z-up axis.
    auto& tf = h.add<TransformComponent>();
    tf.position = sc.parking_spot;            // WorldPosition (ENU feet)
    const double hdg = sc.heading_rad;
    const double h2 = hdg * 0.5;
    tf.qw = std::cos(h2);  tf.qx = 0.0;  tf.qy = 0.0;  tf.qz = std::sin(h2);

    // 2. FlightModelComponent — initialized from AircraftConfig.
    //    Implements IAircraftState + IPilotInputSink. The brain will find
    //    it via interface-based lookup (get_interface<IAircraftState>()).
    auto& fm = h.add<FlightModelComponent>();
    fm.init(aircraft_cfg_,
            /*alt_ft=*/sc.parking_spot.z,
            /*vt_ftps=*/0.0,
            /*hdg_rad=*/hdg,
            /*inAir=*/false);
    // Set the ground plane at the parking spot's altitude so the FM sits on
    // the ground instead of falling. NED frame: up = (0, 0, -1).
    fm.set_ground(sc.parking_spot.z, f4::math::Vec3d{0.0, 0.0, -1.0});

    // 3. VisualModelComponent — the renderable handle (DrawableBSP* equivalent).
    //    This is the ONLY new component type. The renderer reads it to draw
    //    the F-16 mesh; the host syncs its gear switch from the FM each tick.
    auto& vis = h.add<VisualModelComponent>();
    if (model_db_->valid()) {
        vis.model_record = model_db_->model(sc.vis_type_index);
        // If the lookup failed, model_record stays null and the renderer
        // will skip drawing. We don't throw — the sim should still run
        // headless even if the model isn't available.
    }
    vis.active_lod = 0;  // highest detail
    // Default ModelState: gear down (switch #10, child 0 = down per f4-models-viewer)
    f4::models::SwitchState gear_switch;
    gear_switch.switch_number = 10;
    gear_switch.active_child  = 0;  // 0 = gear down
    vis.model_state.switches.push_back(gear_switch);

    // 4. BrainComponent — wraps TakeoffModule, runs in pass 1 (priority 100).
    //    Already implements taxi waypoint following via check_taxi_progress().
    //    We just configure its thresholds; the taxi route comes from the
    //    StubATC's TaxiClearance message (wire_atc() populates it from scenario_).
    auto& brain = h.add<BrainComponent>();
    brain.module().rotate_speed_kts = 140.0;
    brain.module().gear_up_alt_ft = 200.0;
    brain.module().departure_alt_ft = scenario_.airfield.departure_altitude_ft;
    brain.module().taxi_speed_kts = 15.0;

    aircraft_entity_ = h.id();
}

void Simulation::wire_atc() {
    atc_ = std::make_unique<f4::ai::atc::StubATC>(bus_);  // subscribes immediately
    f4::ai::atc::AirfieldConfig af;
    af.active_runway_id = scenario_.airfield.active_runway_id;
    af.active_runway_name = scenario_.airfield.active_runway_name;
    af.runway_heading_rad = scenario_.airfield.runway_heading_rad;
    af.threshold_position = scenario_.airfield.threshold_position;
    af.threshold_altitude_ft = scenario_.airfield.threshold_altitude_ft;
    af.departure_altitude_ft = scenario_.airfield.departure_altitude_ft;
    af.taxi_route = scenario_.airfield.taxi_route;
    af.runway_end_position = scenario_.airfield.runway_end_position;
    atc_->set_airfield(af);
}

void Simulation::tick(double dt) {
    if (paused_) return;
    const double scaled_dt = dt * time_scale_;

    // Two-pass: brains (pass 1, priority >= 75) then physics (pass 2, < 75).
    // BrainComponent runs first, reads IAircraftState, writes IPilotInputSink.
    // FlightModelComponent runs second, consumes the pending input, integrates.
    // All via interface-based lookup — the brain doesn't know it's talking to
    // a FlightModelComponent, just that the entity provides those interfaces.
    world_.update_all(scaled_dt, bus_);
    bus_.flush_pending();  // drain deferred ATC messages (TaxiClearance, etc.)

    // Sync TransformComponent from the FM's integrated state.
    // FM stores position in NED (x=north, y=east, z=down); TransformComponent
    // uses ENU (x=east, y=north, z=up) + a quaternion for orientation.
    auto h = entities::EntityHandle(aircraft_entity_, &world_);
    auto* tf = h.get<entities::TransformComponent>();
    auto* fm = h.get<f4::flight::FlightModelComponent>();
    if (tf && fm) {
        const auto& s = fm->state();
        // NED -> ENU: enu.x = ned.y (east), enu.y = ned.x (north), enu.z = -ned.z (up)
        tf->position = f4::geo::WorldPosition(s.kin.y, s.kin.x, -s.kin.z);

        // Euler (psi=yaw, theta=pitch, phi=roll) -> quaternion (ZYX, Hamilton, body-to-world)
        const double psi   = f4::flight::to_radians(s.kin.psi);
        const double theta = f4::flight::to_radians(s.kin.theta);
        const double phi   = f4::flight::to_radians(s.kin.phi);
        const double hp = psi * 0.5, ht = theta * 0.5, hr = phi * 0.5;
        const double cpsi = std::cos(hp), spsi = std::sin(hp);
        const double cth  = std::cos(ht), sth  = std::sin(ht);
        const double cph  = std::cos(hr), sph  = std::sin(hr);
        // q = q_Z(psi) * q_Y(theta) * q_X(phi)
        tf->qw = cph * cth * cpsi + sph * sth * spsi;
        tf->qx = sph * cth * cpsi - cph * sth * spsi;
        tf->qy = cph * sth * cpsi + sph * cth * spsi;
        tf->qz = cph * cth * spsi - sph * sth * cpsi;
    }

    // Sync VisualModelComponent gear switch from FM gear position.
    // F-16 gear is switch #10 (per f4-models-viewer comment); 0=down, 1=up.
    // AeroState::gearPos is 1.0 when fully down, 0.0 when fully up (the FM
    // auto-commands gearHandle based on gear.inAir — see flight_model.cpp:263).
    auto* vis = h.get<VisualModelComponent>();
    if (vis && fm && !vis->model_state.switches.empty()) {
        vis->model_state.switches[0].active_child = (fm->state().aero.gearPos > 0.5) ? 0 : 1;
    }

    sim_time_s_ += scaled_dt;
    ++tick_;

    if (recorder_) record_snapshot();
}

void Simulation::record_snapshot() {
    // Minimal snapshot for now: timing + position + basic kinematics.
    // Full implementation will populate control inputs, AI mode/state,
    // intended path, cross-track error, fuel, engine state, G-loads.
    f4::recorder::FlightSnapshot snap;
    snap.sim_time_s = sim_time_s_;
    snap.tick = tick_;
    snap.entity_id = aircraft_entity_.value;

    auto h = entities::EntityHandle(aircraft_entity_, &world_);
    auto* tf = h.get<entities::TransformComponent>();
    if (tf) {
        snap.position = tf->position;
        snap.altitude_msl_ft = tf->position.z;
    }

    auto* fm = h.get<f4::flight::FlightModelComponent>();
    if (fm) {
        const auto& s = fm->state();
        snap.vcas_kts = s.vcas;
        snap.vt_fps = s.kin.vt;
        snap.altitude_agl_ft = -s.kin.z - s.gear.groundZ_ft;
        snap.on_ground = !s.gear.inAir;
        snap.heading_rad = f4::flight::to_radians(s.kin.psi);
        snap.pitch_rad   = f4::flight::to_radians(s.kin.theta);
        snap.roll_rad    = f4::flight::to_radians(s.kin.phi);
    }

    recorder_->record(snap);
}

void Simulation::write_recording() {
    if (!recorder_ || scenario_.record_path.empty()) return;
    recorder_->write_json(scenario_.record_path, scenario_.name);
}

} // namespace f4::simulation

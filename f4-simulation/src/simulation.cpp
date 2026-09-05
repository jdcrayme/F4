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
//
// Phase 2: the sim tracks a VECTOR of aircraft entities (one per Flight in
// the campaign, or one per ScenarioAircraft entry, depending on spawn_mode).
// tick() and record_snapshot() iterate the vector. The Phase 1 singleton
// accessor aircraft_entity() is retained as a convenience for hosts that
// only care about one aircraft.

#include "f4/simulation/simulation.hpp"
#include "f4/simulation/visual_model_component.hpp"
#include "f4/simulation/campaign_bridge.hpp"
#include "f4/simulation/combat_bridge.hpp"
#include "f4/simulation/bubble_manager.hpp"
#include "f4/simulation/frames.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/atc/stub_atc.hpp>
#include <f4/ai/modules/takeoff_module.hpp>
#include <f4/ai/modules/navigation_module.hpp>
#include <f4/ai/modules/landing_module.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/recorder/fcs_trace.hpp>
#include <f4/recorder/snapshot.hpp>

#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/bomb_battery.hpp>
#include <f4/weapons/f4_weapons.hpp>
#include <f4/sensors/f4_sensors.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <f4/world_convert/class_table.hpp>
#include <f4/world/world_loader.hpp>
#include <f4/world/detail/world_state.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace f4::simulation {

namespace {

// The default SimData AI data file (brain archetypes + FORMDAT
// formations): the build tree's generated fixture directory — the same
// conversion the f4-convert pipeline runs for any Falcon 4.0 SimData.zip.
// Compile-time injected (f4-simulation/CMakeLists.txt); empty path when
// the library was built without one, in which case a scenario that wants
// the data must set brain_data_path /
// formation_library_path explicitly (apply_simdata_ai_profiles throws
// with that instruction).
[[nodiscard]] std::filesystem::path simdata_default_file(
    const char* leaf) noexcept {
#ifdef F4_SIMDATA_DEFAULT_DIR
    return std::filesystem::path(F4_SIMDATA_DEFAULT_DIR) / leaf;
#else
    (void)leaf;
    return {};
#endif
}

/// Comma-joined name list for error messages ("Generic, SEAD, Strike, ...")
/// — keeps an unknown-name failure actionable instead of a bare miss.
[[nodiscard]] std::string join_names(
    const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += ", ";
        out += names[i];
    }
    return out;
}

} // namespace

Simulation::Simulation(Scenario scenario, std::filesystem::path asset_dir)
    : scenario_(std::move(scenario))
    , asset_dir_(std::move(asset_dir))
    , model_db_(std::make_unique<f4::models::ModelDatabase>())
{
}

Simulation::~Simulation() = default;

void Simulation::initialize() {
    // Real-airbase derivation runs FIRST: it rewrites scenario_.airfield
    // (runway, taxi routes, parking) and resolves aircraft parking:auto
    // spawns before any entity is created.
    if (scenario_.has_airbase_source) {
        derive_real_airbase();
    }

    // B.3 fix: the campaign-flights path derives its airfield from the
    // world JSON's first airbase objective — but it used to do that INSIDE
    // spawn_aircraft(), i.e. AFTER wire_atc() had already wired StubATC
    // against the (empty, hand-authored) scenario airfield. Result: no
    // taxi route ever reached the ATC, taxi clearances never came back,
    // and every campaign aircraft sat parked at the ramp forever.
    // Deriving BEFORE wire_atc() fixes the ordering.
    if (scenario_.spawn_mode == SpawnMode::CampaignFlights &&
        scenario_.airfield.taxi_route.empty()) {
        derive_campaign_airfield();
    }

    // Order matters: ATC must be subscribed BEFORE the brain's first update()
    // tick (which publishes a TaxiRequest). The brain's initialize() runs
    // lazily on first update(), so we just need StubATC alive before tick().
    wire_atc();
    load_models();
    load_aircraft_config();
    // Load the class table ONCE, before every consumer: the spawn
    // paths below and the BubbleManager (init_bubble_manager) borrow
    // this member for the Simulation's lifetime. Used to be loaded
    // per-path into stack locals — one of which outlived its scope
    // inside the BubbleManager (the Start Session access violation).
    load_class_table();
    // C6: the campaign-combat doctrine's data leg, BEFORE any aircraft
    // exists (the arm installs non-owning archetype pointers into
    // brain_data_ — it must be loaded exactly once, up front, and
    // never re-loaded while brains hold pointers into it). Loud on
    // failure: an armed campaign without its doctrine data is a
    // misconfiguration, not a degraded mode.
    if (scenario_.combat.campaign_armed) {
        ensure_campaign_brain_data();
    }
    spawn_aircraft();
    // Step 11: wingman refs resolve AFTER all aircraft exist (a lead may
    // sit anywhere in the list). Marks the wingman brains + records the
    // (wingman, lead) pairs the tick loop feeds pictures through.
    resolve_wingman_refs();
    // SimData AI profiles resolve after the wingman refs — a formation is
    // injected into a wingman module that already knows it IS a wingman,
    // and archetype pointers land on brains that already exist.
    apply_simdata_ai_profiles();
    spawn_airfield_features();  // Phase 2A: features spawn after aircraft

    // Mode B: spawn parked aircraft from Squadrons (after the airfield is
    // derived, so parking spots are available). Squadrons don't move, so
    // these are spawned once at initialize() — no per-tick re-deaggregation.
    spawn_squadron_aircraft();

    // Mode B: initialize the BubbleManager for ground/naval units. The
    // manager is only constructed when the world contains campaign units
    // (i.e. spawn_mode == CampaignFlights). For the scenario-list spawn
    // path, the world has no VehicleCompositionComponent entities, so the
    // BubbleManager would be a no-op — we skip it to avoid the overhead.
    init_bubble_manager();

    // Set up the recorder if the scenario enables it.
    if (scenario_.record) {
        recorder_ = std::make_unique<f4::recorder::FlightRecorder>();
        recorder_->set_scenario_name(scenario_.name);

        // M4: subscribe the recorder to the combat bus transitions so a
        // recorded fight carries its event stream alongside the kinematic
        // tracks. Harmless when combat is disabled (no combat messages
        // publish); no-op when the recorder exists but nothing fires.
        attach_combat_event_recorder(*this);
    }

    // Set up the FCS/AI/EOM CSV trace writer if the scenario enables it.
    // Independent of `record`: a scenario may want the lightweight CSV trace
    // without the full replay JSON, or vice versa.
    if (!scenario_.fcs_trace_path.empty()) {
        fcs_trace_ = std::make_unique<f4::recorder::FcsTraceWriter>();
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

void Simulation::normalize_waypoint_frame() {
    // NAV-D2: rotate runway-frame waypoints into ENU about the airfield
    // threshold. The rotation used to live ONLY at the end of
    // derive_real_airbase() — so scenarios with a hand-authored (synthetic)
    // airfield, which never call derive_real_airbase, handed the AI
    // RUNWAY-FRAME waypoint coordinates while the aircraft flies in ENU.
    // With the runway threshold 500 ft east of the frame origin the AI then
    // flew a parallel course 500 ft off (course_intercept trace: perfect
    // LNAV tracking of the WRONG line, xte pinned at -492 ft while the
    // correction read 0). The same 500 ft shift leaks into the isolated
    // landing scenarios' localizer geometry (the stubborn ~200-500 ft
    // cross-track residual). Idempotent: the flag clears after rotation.
    if (!scenario_.waypoints_runway_frame) return;
    const double hs = scenario_.airfield.runway_heading_rad;
    const double sh = std::sin(hs), ch = std::cos(hs);
    const auto& thr = scenario_.airfield.threshold_position;
    for (auto& wp : scenario_.waypoints) {
        const double rx = wp.position.x;   // right of heading
        const double ry = wp.position.y;   // downrange
        wp.position.x = thr.x + rx * ch + ry * sh;
        wp.position.y = thr.y - rx * sh + ry * ch;
        // z (MSL) authored absolute — unchanged.
    }
    scenario_.waypoints_runway_frame = false;
}

void Simulation::spawn_aircraft() {
    // Dispatcher: pick the spawn path based on the scenario's spawn_mode.
    //   - ScenarioList (Phase 1): one entity per ScenarioAircraft entry,
    //     hand-authored parking spots + headings. Backward-compatible.
    //   - CampaignFlights (Phase 2): load world JSON + class table, find
    //     every Flight-class unit, spawn a child aircraft entity per flight.
    normalize_waypoint_frame();  // NAV-D2 (idempotent; also done late in
                                 // derive_real_airbase on the real path)
    if (scenario_.spawn_mode == SpawnMode::CampaignFlights) {
        spawn_from_campaign_flights();
    } else {
        spawn_from_scenario_list();
    }
}

bool Simulation::register_aircraft(entities::EntityId id) {
    // Unknown entity (never created, or destroyed since): reject loudly
    // enough to be visible in the return value without throwing — hosts
    // register from bus-fed spawn paths where a lookup miss is data, not
    // a program bug.
    if (!id.valid()) return false;
    {
        auto h = entities::EntityHandle(id, &world_);
        if (h.get<f4::flight::FlightModelComponent>() == nullptr) {
            return false;
        }
    }
    for (const auto existing : aircraft_entities_) {
        if (existing == id) return false;  // idempotent
    }
    aircraft_entities_.push_back(id);
    return true;
}

bool Simulation::retire_aircraft(entities::EntityId id) {
    // C5's wreck reaper — see the header for the lifetime contract.
    // Order of operations: every in-memory reference to the id is
    // dropped BEFORE world().destroy() so nothing walks a destroyed
    // entity, and the destroy is last so a failure above leaves the
    // world intact.
    if (!id.valid()) return false;
    const auto it = std::find(aircraft_entities_.begin(),
                              aircraft_entities_.end(), id);
    if (it == aircraft_entities_.end()) return false;  // not ours to reap
    aircraft_entities_.erase(it);

    // Wingman pairs die with either member — the survivor degrades to
    // single-ship, the same rung-drop a dead lead already causes in
    // push_wingman_lead_pictures() (invalid picture → no formation).
    wingman_pairs_.erase(
        std::remove_if(wingman_pairs_.begin(), wingman_pairs_.end(),
                       [id](const WingmanPair& p) {
                           return p.wingman == id || p.lead == id;
                       }),
        wingman_pairs_.end());

    // Radar policies are ownship-keyed; the scenario-list path and the
    // C6 campaign arm both build them here (retire reaps either kind).
    combat_policies_.erase(
        std::remove_if(combat_policies_.begin(), combat_policies_.end(),
                       [v = id.value](
                           const std::unique_ptr<RadarBackedDetectionPolicy>&
                               p) { return p->ownship_id() == v; }),
        combat_policies_.end());

    world_.destroy(id);
    ++retired_aircraft_;
    return true;
}

void Simulation::ensure_campaign_brain_data() {
    // C6's data leg (see initialize's call site): resolve + load the
    // BRAINDAT archetype table exactly once. Same path discipline as
    // apply_simdata_ai_profiles — the scenario's explicit path wins,
    // the build-tree generated fixture is the default — but the failure
    // is LOUD here (an armed campaign without doctrine data would arm
    // fighters and leave every defensive role unarmed: a silent
    // behavior change, exactly what the house rules forbid).
    if (brain_data_loaded_) return;  // apply_simdata loaded it (scenario)
    auto path = scenario_.brain_data_path;
    if (path.empty()) path = simdata_default_file("braindata.json");
    if (path.empty()) {
        throw std::runtime_error(
            "Simulation::ensure_campaign_brain_data: combat.campaign_armed "
            "but no brain data was configured (no brain_data_path and no "
            "build-time SimData default — set brain_data_path to a "
            "brain2json output)");
    }
    auto result = f4::data::loadBrainData(path.string());
    if (!result.ok) {
        std::string msg =
            "Simulation::ensure_campaign_brain_data: failed to load "
            "brain data '" + path.string() + "':";
        for (const auto& e : result.errors) msg += "\n  " + e;
        throw std::runtime_error(msg);
    }
    brain_data_ = std::move(result.data);
    brain_data_loaded_ = true;
}

bool Simulation::arm_campaign_aircraft(entities::EntityId id) {
    // The C6 opt-in: everything below exists for the armed campaign
    // only; an unarmed sim answers false without touching the entity
    // (the pre-C6 world, byte-for-byte).
    if (!scenario_.combat.campaign_armed) return false;
    if (!id.valid()) return false;

    entities::EntityHandle h(id, &world_);
    std::unique_ptr<RadarBackedDetectionPolicy> policy;
    const auto result = arm_campaign_combat(
        h, weapon_table_,
        scenario_.combat.radar_rng_seed,
        campaign_arm_index_,
        scenario_.combat.fighter_hit_points,
        scenario_.combat.bvr_hold,
        scenario_.combat.missiles_hold,
        scenario_.combat.guns_hold,
        brain_data_loaded_ ? &brain_data_ : nullptr,
        &policy);
    if (!result.armed) {
        // Not a candidate (no origin/brain/store) or already armed —
        // EXCEPT the doctrine-failure shapes, which are misconfigurations
        // the caller must hear about, not silently fly past: a defensive
        // role with no disengaged archetype would FIGHT (the exact
        // behavior C6 exists to prevent), and a missing brain-data leg
        // means the eager load contract broke. Loud.
        if (result.archetype == "<no brain data>" ||
            result.archetype == "<no disengaged archetype>") {
            throw std::runtime_error(
                "Simulation::arm_campaign_aircraft: campaign combat "
                "doctrine failed for entity " +
                std::to_string(id.value) + ": " + result.archetype);
        }
        return false;
    }

    // The detection policy (C6.2 — the M2 flip): stored with the
    // scenario path's policies (retire_aircraft reaps by ownship id,
    // so campaign wrecks clean up exactly like scenario ones) and
    // installed on the brain's SensorFusion — radar truth, not
    // GCI-omniscience.
    if (policy) {
        auto* brain = h.get<f4::ai::BrainComponent>();
        combat_policies_.push_back(std::move(policy));
        if (brain != nullptr) {
            brain->sensors().set_detection_policy(
                combat_policies_.back().get());
        }
    }

    ++campaign_arm_index_;
    ++campaign_armed_total_;
    if (result.role == CampaignCombatRole::Fighter) {
        ++campaign_armed_fighters_;
    } else {
        ++campaign_armed_defensive_;
    }
    return true;
}

void Simulation::spawn_from_scenario_list() {
    using namespace f4::entities;
    using namespace f4::flight;
    using namespace f4::ai;
    using namespace f4::simulation;  // VisualModelComponent lives here

    if (scenario_.aircraft.empty()) {
        throw std::runtime_error("Simulation::spawn_aircraft: scenario has no aircraft");
    }

    // Combat chain (M3): the weapon class table every launch goes through.
    // Built-in placeholder set for now; the FALCON4.WST import replaces the
    // card contents without touching call sites (COMBAT_CHAIN_PLAN.md §5).
    if (scenario_.combat.enabled) {
        weapon_table_ = weapons::WeaponClassTable::with_builtins();
    }

    for (std::size_t ac_index = 0; ac_index < scenario_.aircraft.size(); ++ac_index) {
        const auto& sc = scenario_.aircraft[ac_index];
        auto h = world_.create();

        // 1. TransformComponent — initial pose at the parking spot.
        //    TransformComponent uses a quaternion (qw,qx,qy,qz) for orientation,
        //    not Euler angles. Convert heading -> quaternion about Z-up axis.
        auto& tf = h.add<TransformComponent>();
        tf.position = sc.parking_spot;            // WorldPosition (ENU feet)
        const double hdg = sc.heading_rad;
        const double h2 = hdg * 0.5;
        // Compass heading -> ENU quaternion (negative about +z; see frames.hpp).
        const auto q0 = f4::simulation::enu_quat_from_compass(h2 * 2.0);
        tf.qw = q0.w;  tf.qx = q0.x;  tf.qy = q0.y;  tf.qz = q0.z;

        // 2. FlightModelComponent — initialized from AircraftConfig.
        //    Implements IAircraftState + IPilotInputSink. The brain will find
        //    it via interface-based lookup (get_interface<IAircraftState>()).
        //    Phase 0d: spawn at a small non-zero vt (5 ft/s) when on the
        //    ground to avoid the first-tick transient where qsom=0 forces
        //    the FCS into the ground guard (see FLIGHT_CONTROL_NEXT_STEPS.md
        //    §3.3). When the scenario explicitly requests spawn_in_air, the
        //    FM is initialized with inAir=true and the scenario's initial_vt_fps.
        auto& fm = h.add<FlightModelComponent>();
        const bool spawn_airborne = sc.spawn_in_air;
        const double spawn_vt = spawn_airborne
            ? std::max(sc.initial_vt_fps, 100.0)  // need meaningful qsom to fly
            : (sc.initial_vt_fps > 0.0 ? sc.initial_vt_fps : 5.0);
        fm.init(aircraft_cfg_,
                /*alt_ft=*/sc.parking_spot.z,
                /*vt_ftps=*/spawn_vt,
                /*hdg_rad=*/hdg,
                /*inAir=*/spawn_airborne,
                /*north_ft=*/sc.parking_spot.y,
                /*east_ft=*/sc.parking_spot.x);
        // Set the ground plane at the parking spot's altitude so the FM sits
        // on the ground instead of falling. set_ground takes the terrain's
        // MSL altitude (positive up) — the EOM/gear internally convert to
        // NED. (The FM's groundZ consumers were standardized on MSL-up;
        // they previously mixed NED and MSL conventions, which only agreed
        // for ground at exactly 0 ft.)
        const double spawn_ground_z = spawn_airborne ? 0.0 : sc.parking_spot.z;
        fm.set_ground(spawn_ground_z, f4::math::Vec3d{0.0, 0.0, -1.0});
        // Also set the default flat terrain source to the parking altitude,
        // so when no real TerrainSource is provided, tick() keeps the
        // ground at the parking altitude (preserves pre-terrain behavior).
        default_terrain_ = f4::terrain::FlatTerrainSource(spawn_ground_z);

        // The sortie's fuel load: the config's internalFuel is the tank
        // CAPACITY; the scenario's initial_fuel_lbs is what this jet
        // actually launched with (the DigitalBrain's fuel check — joker/
        // bingo — reads this gauge, and a scenario wanting a near-king
        // jet sets it low). 0/unset keeps the config default (full tanks).
        if (sc.initial_fuel_lbs > 0.0) {
            fm.model().set_internal_fuel_lbs(sc.initial_fuel_lbs);
        }

        // 3. VisualModelComponent — the renderable handle (DrawableBSP* equivalent).
        //    This is the ONLY new component type. The renderer reads it to draw
        //    the F-16 mesh; the host syncs its gear switch from the FM each tick.
        auto& vis = h.add<VisualModelComponent>();
        vis.vis_type = sc.vis_type_index;  // V-3DLIVE (identity w/o db)
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

        // 4. BrainComponent — the mission sequencer (takeoff -> navigation
        //    -> landing), runs in pass 1 (priority 100). The taxi route
        //    comes from the StubATC's TaxiClearance message (wire_atc()
        //    populates it from scenario_); the air route + taxi-in route
        //    are injected here as the mission plan.
        auto& brain = h.add<BrainComponent>();
        brain.takeoff().rotate_speed_kts = 140.0;
        brain.takeoff().gear_up_alt_ft = 200.0;
        brain.takeoff().departure_alt_ft = scenario_.airfield.departure_altitude_ft;
        brain.takeoff().taxi_speed_kts = 15.0;

        MissionPlan plan;
        plan.route.reserve(scenario_.waypoints.size());
        for (const auto& wp : scenario_.waypoints) {
            plan.route.push_back(modules::NavigationModule::Waypoint{
                wp.name, wp.position, wp.speed_kts});
        }
        plan.taxi_in_route = scenario_.airfield.taxi_in_route;
        plan.fly_traffic_pattern = scenario_.approach_is_pattern();
        if (scenario_.start_in_approach) {
            plan.start_phase = MissionPlan::StartPhase::Approach;
        }
        if (scenario_.start_enroute) {
            plan.start_phase = MissionPlan::StartPhase::Enroute;
        }
        brain.set_mission_plan(std::move(plan));

        // The arbiter's fuel policy (FrameExec step 2): joker/bingo in
        // pounds of usable fuel, from the scenario's fuel block. Zero
        // bingo (the default) leaves the brain exactly as fuel-blind as
        // it was before this rung existed.
        brain.set_fuel_policy(scenario_.fuel.joker_lbs,
                              scenario_.fuel.bingo_lbs);

        // 5. Team identity — the TEAM tag ("blue"/"red") rides on EVERY
        //    aircraft, combat or not: SensorFusion's hostility rule, the
        //    radar's IFF classification, RWR emitter roles, and missile
        //    team-copying all read it. Before the combat chain nothing
        //    consumed it for scenario-list aircraft, so this is additive.
        h.set_tag(entities::tags::TEAM, entities::TagValue::from(sc.team));

        // 6. Combat component set (M3 integration): stores, signature,
        //    radar, RWR, damage state, gun + identity. When the scenario
        //    leaves combat disabled (the default) NONE of this exists and
        //    the world is bit-for-bit what it was before the combat chain.
        if (scenario_.combat.enabled) {
            attach_combat_loadout(h, weapon_table_, sc,
                                  scenario_.combat.radar_rng_seed,
                                  ac_index,
                                  scenario_.combat.fighter_hit_points);

            // The gun's ammo ledger: the store's gun station (attached
            // just above; 511 for a standard M61A1 load). The brain's
            // gun fire control budgets itself against the same number.
            int gun_rounds = 0;
            if (const auto* store =
                    h.get<weapons::WeaponStoreComponent>()) {
                const auto station = store->find_with_category(
                    weapon_table_, weapons::WeaponCategory::Gun);
                if (station != weapons::WeaponStoreComponent::npos) {
                    gun_rounds = store->station(station)->rounds;
                }
            }

            // M3 tactics: the brain becomes a fighting brain — the
            // combat ladder (defensive > WVR > BVR > mission) activates
            // and the fire controls get the table-derived envelopes
            // (BVR from the longest-range A/A class, WVR/IR from the
            // heater class, guns from the M61A1 card + the drum count).
            // Per-aircraft hold_fire and the combat block's ROE flags
            // (bvr_hold / missiles_hold / guns_hold) ride along. The
            // detection policy (radar-truth instead of GCI-omniscience)
            // is installed on the brain's SensorFusion; the Simulation
            // owns the policy objects for the world's lifetime.
            configure_brain_combat(brain, weapon_table_, sc.hold_fire,
                                   scenario_.combat.bvr_hold,
                                   scenario_.combat.missiles_hold,
                                   scenario_.combat.guns_hold,
                                   gun_rounds);
            combat_policies_.push_back(
                std::make_unique<RadarBackedDetectionPolicy>(
                    world_, h.id().value));
            brain.sensors().set_detection_policy(
                combat_policies_.back().get());
        }

        aircraft_entities_.push_back(h.id());
    }
}

void Simulation::resolve_wingman_refs() {
    // Step 11 (wingman/2-ship): after ALL aircraft spawned (a lead may sit
    // anywhere in the list), resolve each "lead_callsign" to an entity.
    // scenario_.aircraft and aircraft_entities_ are index-aligned on the
    // scenario-list path; the campaign path has no wingman refs (its
    // roster is flight-derived, not hand-authored) — resolve against the
    // scenario list only, which is where the field can appear at all.
    if (wingman_pairs_.empty() &&
        std::none_of(scenario_.aircraft.begin(), scenario_.aircraft.end(),
                     [](const ScenarioAircraft& a) {
                         return !a.lead_callsign.empty();
                     })) {
        return;  // nothing to do — the pre-Step-11 world, byte-for-byte
    }
    if (scenario_.spawn_mode != SpawnMode::ScenarioList ||
        scenario_.aircraft.size() != aircraft_entities_.size()) {
        throw std::runtime_error(
            "Simulation::resolve_wingman_refs: wingman refs are only "
            "supported on the scenario-list spawn path");
    }

    // callsign -> aircraft index (first wins; duplicate callsigns are a
    // scenario authoring error — the scenario format's existing
    // convention, same as the unknown-callsign check below).
    std::map<std::string, std::size_t> index_by_callsign;
    for (std::size_t i = 0; i < scenario_.aircraft.size(); ++i) {
        index_by_callsign[scenario_.aircraft[i].callsign] = i;
    }

    for (std::size_t i = 0; i < scenario_.aircraft.size(); ++i) {
        const auto& sc = scenario_.aircraft[i];
        if (sc.lead_callsign.empty()) continue;

        if (sc.lead_callsign == sc.callsign) {
            throw std::runtime_error(
                "Simulation::resolve_wingman_refs: aircraft '" +
                sc.callsign + "' lists itself as flight lead");
        }
        const auto it = index_by_callsign.find(sc.lead_callsign);
        if (it == index_by_callsign.end()) {
            throw std::runtime_error(
                "Simulation::resolve_wingman_refs: aircraft '" +
                sc.callsign + "' references unknown flight lead '" +
                sc.lead_callsign + "'");
        }

        // Same team, or the wingman's missiles would IFF off its own lead.
        const auto& lead_sc = scenario_.aircraft[it->second];
        if (lead_sc.team != sc.team) {
            throw std::runtime_error(
                "Simulation::resolve_wingman_refs: wingman '" +
                sc.callsign + "' (team " + sc.team +
                ") cannot follow lead '" + sc.lead_callsign +
                "' (team " + lead_sc.team + ")");
        }

        entities::EntityHandle wing(aircraft_entities_[i], &world_);
        auto* brain = wing.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;  // no brain — nothing to mark
        brain->set_flight_lead(aircraft_entities_[it->second].value);

        wingman_pairs_.push_back(
            WingmanPair{aircraft_entities_[i],
                        aircraft_entities_[it->second]});
    }
}

void Simulation::apply_simdata_ai_profiles() {
    // SimData AI wiring — the scenario JSON's engine-agnostic Data/ side.
    // Gate 1: does anything reference the data at all? No references =
    // no loads = no behavior change (the pre-SimData world). This is the
    // compatibility contract: every scenario authored before SimData
    // support flies byte-for-byte the same after it.
    const bool any_brain_profile = std::any_of(
        scenario_.aircraft.begin(), scenario_.aircraft.end(),
        [](const ScenarioAircraft& a) { return !a.brain_profile.empty(); });
    const bool any_formation = std::any_of(
        scenario_.aircraft.begin(), scenario_.aircraft.end(),
        [](const ScenarioAircraft& a) { return !a.formation.empty(); });
    if (!any_brain_profile && !any_formation) return;

    // Same path restriction as resolve_wingman_refs: the fields live on
    // hand-authored scenario aircraft; the campaign-flights roster has no
    // per-aircraft authoring to read them from.
    if (scenario_.spawn_mode != SpawnMode::ScenarioList ||
        scenario_.aircraft.size() != aircraft_entities_.size()) {
        throw std::runtime_error(
            "Simulation::apply_simdata_ai_profiles: brain_profile / "
            "formation are only supported on the scenario-list spawn "
            "path");
    }

    // --- Load the data (lazily, per side) --------------------------------
    if (any_brain_profile && !brain_data_loaded_) {
        // C6 note: campaign arming may have loaded brain_data_ already
        // (ensure_campaign_brain_data) — the campaign brains hold
        // non-owning archetype pointers into it, so it is NEVER re-loaded
        // while they live; the scenario profiles resolve against the same
        // rows.
        auto path = scenario_.brain_data_path;
        if (path.empty()) path = simdata_default_file("braindata.json");
        if (path.empty()) {
            throw std::runtime_error(
                "Simulation::apply_simdata_ai_profiles: scenario references "
                "brain_profile but no brain data was configured (no "
                "brain_data_path and no build-time SimData default — set "
                "brain_data_path to a brain2json output)");
        }
        auto result = f4::data::loadBrainData(path.string());
        if (!result.ok) {
            std::string msg =
                "Simulation::apply_simdata_ai_profiles: failed to load "
                "brain data '" + path.string() + "':";
            for (const auto& e : result.errors) msg += "\n  " + e;
            throw std::runtime_error(msg);
        }
        brain_data_ = std::move(result.data);
        brain_data_loaded_ = true;
    }

    if (any_formation) {
        auto path = scenario_.formation_library_path;
        if (path.empty()) path = simdata_default_file("formdat.json");
        if (path.empty()) {
            throw std::runtime_error(
                "Simulation::apply_simdata_ai_profiles: scenario references "
                "formation but no formation library was configured (no "
                "formation_library_path and no build-time SimData default "
                "— set formation_library_path to a form2json output)");
        }
        auto result = f4::data::loadFormationLibrary(path.string());
        if (!result.ok) {
            std::string msg =
                "Simulation::apply_simdata_ai_profiles: failed to load "
                "formation library '" + path.string() + "':";
            for (const auto& e : result.errors) msg += "\n  " + e;
            throw std::runtime_error(msg);
        }
        formation_library_ = std::move(result.data);
        formation_library_loaded_ = true;
    }

    // --- Resolve names and inject (scenario order == entity order) -------
    for (std::size_t i = 0; i < scenario_.aircraft.size(); ++i) {
        const auto& sc = scenario_.aircraft[i];
        entities::EntityHandle h(aircraft_entities_[i], &world_);
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;  // no brain — nothing to inject

        if (!sc.brain_profile.empty()) {
            const auto* archetype =
                brain_data_.find_archetype(sc.brain_profile);
            if (archetype == nullptr) {
                std::vector<std::string> known;
                known.reserve(brain_data_.archetypes.size());
                for (const auto& a : brain_data_.archetypes)
                    known.push_back(a.name);
                throw std::runtime_error(
                    "Simulation::apply_simdata_ai_profiles: aircraft '" +
                    sc.callsign + "' references unknown brain_profile '" +
                    sc.brain_profile + "' (known: " +
                    join_names(known) + ")");
            }
            // Non-owning pointer into brain_data_ — owned for the
            // Simulation's lifetime (see the member declaration).
            brain->set_brain_archetype(archetype);
        }

        if (!sc.formation.empty()) {
            const auto* formation =
                formation_library_.find_by_name(sc.formation);
            if (formation == nullptr) {
                std::vector<std::string> known;
                known.reserve(formation_library_.formations.size());
                for (const auto& f : formation_library_.formations)
                    known.push_back(f.name);
                throw std::runtime_error(
                    "Simulation::apply_simdata_ai_profiles: aircraft '" +
                    sc.callsign + "' references unknown formation '" +
                    sc.formation + "' (known: " + join_names(known) + ")");
            }
            // The wingman role was validated (formation requires
            // lead_callsign; resolve_wingman_refs resolved it) — the
            // module is flying before the slot lands. Non-owning: the
            // optional copies the slot VALUES, geometry re-read per
            // update from formation_slot_.
            brain->wingman().command_formation_slot(*formation);
        }
    }
}

void Simulation::push_wingman_lead_pictures() {
    // Step 11: the wingman module's eyes. Per tick, BEFORE update_all:
    // read the lead's transform (position/velocity — synced at the END of
    // the last tick, so one tick old, the same data every other brain
    // sees), its FM state (CAS + airborne), its damage state (alive),
    // and its brain's engagement target (the SORT hint). A lead that is
    // dead, missing, or on the ground pushes an INVALID picture — the
    // wingman's Formation rung empties and it flies as a single-ship.
    for (const auto& pair : wingman_pairs_) {
        entities::EntityHandle wing(pair.wingman, &world_);
        auto* brain = wing.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;

        f4::ai::modules::WingmanModule::LeadPicture p{};
        p.entity_id = pair.lead.value;

        entities::EntityHandle lead(pair.lead, &world_);
        const auto* tf = lead.get<entities::TransformComponent>();
        const auto* fm = lead.get<f4::flight::FlightModelComponent>();
        const auto* dmg = lead.get<entities::DamageStateComponent>();
        const auto* lead_brain = lead.get<f4::ai::BrainComponent>();

        const bool alive = (dmg == nullptr) || !dmg->killed;
        if (tf != nullptr && fm != nullptr && alive &&
            fm->state().gear.inAir) {
            p.valid = true;
            p.position = tf->position;
            p.velocity = tf->velocity();
            // Velocity heading (atan2(east, north)): at formation
            // distances the lead's nose IS its velocity vector to well
            // inside the station tolerance.
            if (p.velocity.x != 0.0 || p.velocity.y != 0.0) {
                p.heading_rad = std::atan2(p.velocity.x, p.velocity.y);
            }
            p.vcas_kts = fm->state().vcas;
            p.alt_msl_ft = tf->position.z;   // ENU z = MSL
        }
        brain->update_lead_picture(p);

        // The sort hint: the lead's CURRENT engagement (0 when the lead
        // is not fighting — the wingman then picks its own best target).
        brain->set_lead_engagement(
            lead_brain != nullptr ? lead_brain->combat_engagement_id()
                                  : 0u);
    }
}

void Simulation::push_air_picture_(double dt) {
    // PERF-1 (PERFORMANCE_PLAN.md §3): ONE walk over the transform
    // bucket — but only on ticks where at least one brain's fusion will
    // actually REBUILD (the demand query mirrors the fusion's own
    // update() decision exactly; see BrainComponent::wants_air_picture).
    // Cruise ticks with no expiring skill timer and no visible hostile
    // missile pay nothing — the pre-flight taxi phase and the quiet
    // cruise phases keep their baseline cost, and the merge phase (every
    // brain under a missile threat refreshes every tick) pays the walk
    // ONCE instead of once per brain.
    //
    // The snapshot must reproduce the fusion's own world query EXACTLY —
    // same entities, same entity-index order, same values, same clutter
    // rule — so the armed war's ledger bytes stay identical to the
    // per-brain-walk build (the baseline MD5 is the proof).
    //
    // Single pass over the roster: query demand per brain, remember it,
    // and push the picture pointer (or the null that restores the
    // fusion's world query) in the same iteration. Brains that rebuild
    // this tick get a fresh snapshot; brains that don't get nullptr —
    // inert either way, and no rebuild happens without the demand flag
    // that built the snapshot.
    const f4::ai::AirPicture* push = nullptr;
    bool any_demand = false;
    for (const auto eid : aircraft_entities_) {
        entities::EntityHandle h(eid, &world_);
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;
        if (!any_demand && brain->wants_air_picture(dt)) {
            any_demand = true;
        }
    }

    if (any_demand) {
        // Build: bucket copy (the with_component snapshot-by-value
        // contract), one EntityHandle::get per entity (~4,400 in a
        // populated save), the shared clutter predicate, and — for the
        // survivors only (~100-200: airborne aircraft + missiles +
        // anything stationary at altitude) — the team/role tag reads and
        // the contact fill. Team strings intern into a first-seen table
        // so contacts carry an index instead of a copy.
        air_picture_.contacts.clear();
        air_picture_.teams.clear();

        for (const auto eid :
             world_.with_component<entities::TransformComponent>()) {
            entities::EntityHandle h(eid, &world_);
            const auto* tf = h.get<entities::TransformComponent>();
            if (tf == nullptr) continue;

            // The same C6 rule the fusion's world walk applies (and the
            // radar's candidate walk): stationary low-altitude entities
            // are ground clutter, not air picture.
            if (tf->is_ground_clutter()) continue;

            f4::ai::AirPictureContact c;
            c.entity_id = eid.value;
            c.position = tf->position;
            c.velocity = tf->velocity();

            if (auto team_tag = h.get_tag(entities::tags::TEAM)) {
                if (const auto* s = team_tag->as_string()) {
                    // Intern (linear scan — a war has a handful of
                    // teams).
                    std::int16_t idx = -1;
                    for (std::size_t i = 0; i < air_picture_.teams.size();
                         ++i) {
                        if (air_picture_.teams[i] == *s) {
                            idx = static_cast<std::int16_t>(i);
                            break;
                        }
                    }
                    if (idx < 0) {
                        air_picture_.teams.push_back(*s);
                        idx = static_cast<std::int16_t>(
                            air_picture_.teams.size() - 1);
                    }
                    c.team = idx;
                }
            }
            if (auto role_tag = h.get_tag(entities::tags::ROLE)) {
                if (const auto* s = role_tag->as_string()) {
                    c.is_missile = (*s == "missile");
                }
            }
            air_picture_.contacts.push_back(c);
        }
        push = &air_picture_;
    }

    // Push (or clear) on every roster brain in one pass. A brain that
    // initializes its fusion AFTER a push (the first combat tick) clears
    // the pointer in initialize() and rebuilds via the world path that
    // one tick — output-identical either way.
    for (const auto eid : aircraft_entities_) {
        entities::EntityHandle h(eid, &world_);
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain == nullptr) continue;
        brain->set_air_picture(push);
    }
}

void Simulation::push_safety_pictures() {
    // The arbiter's safety rungs are engine-agnostic: the host is their
    // entire view of terrain and traffic. Per tick, BEFORE update_all:
    //
    //   Terrain picture — from the SAME TerrainSource the FM's ground
    //   plane uses (the set_ground loop right above this call reads the
    //   same source one statement earlier): elevation under the jet and
    //   the max elevation in the look-ahead cone along the ground track.
    //   The probes ride the transform's velocity (one tick old — the
    //   same staleness every brain sees; a 6-second look-ahead absorbs
    //   16 ms with room to spare).
    //
    //   Traffic picture — every OTHER airborne aircraft within 1 NM of
    //   this one (friendlies included: the formation mate you are about
    //   to fly through is exactly the aircraft cavoid exists for), with
    //   velocity + body roll rate from the same transform snapshot, plus
    //   the OWN velocity so the module's relative-geometry extrapolation
    //   runs on one consistent picture.
    //
    // O(n^2) over airborne aircraft with n = scenario roster size (<= a
    // handful) — negligible, and identical to FreeFalcon's CollisionCheck
    // scan shape.

    struct Airborne {
        entities::EntityId id;
        f4::ai::BrainComponent* brain;
        const entities::TransformComponent* tf;
    };
    std::vector<Airborne> airborne;
    airborne.reserve(aircraft_entities_.size());

    for (const auto eid : aircraft_entities_) {
        entities::EntityHandle h(eid, &world_);
        auto* brain = h.get<f4::ai::BrainComponent>();
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        const auto* tf = h.get<entities::TransformComponent>();
        if (brain == nullptr || tf == nullptr || fm == nullptr) continue;
        if (!fm->state().gear.inAir) continue;  // ground aircraft don't collide
        // Wrecks don't fly: a killed aircraft's transform freezes (its FM
        // stops), and a frozen "intruder" inside the extrapolation window
        // arms every passing jet's break against a corpse (observed: the
        // gun-kill at 418 ft, the cavoid arm 3 ticks later). The damage
        // component rides every combat aircraft; non-combat worlds have
        // none (nullptr = alive by construction).
        const auto* dmg = h.get<entities::DamageStateComponent>();
        if (dmg != nullptr && dmg->killed) continue;
        airborne.push_back({eid, brain, tf});
    }

    constexpr double kTrafficGateFt = 6076.12;  // 1 NM — everything cavoid can use

    for (const auto& self : airborne) {
        // --- Terrain picture ---------------------------------------------
        f4::ai::modules::GroundAvoidModule::TerrainPicture tp;
        const auto& pos = self.tf->position;
        f4::terrain::TerrainSource* ts =
            terrain_source_ ? terrain_source_ : &default_terrain_;
        tp.valid = true;
        tp.terrain_here_ft = ts->elevation_at_ft(pos.x, pos.y);

        // Look-ahead probes along the ground track at 1/3, 2/3, 3/3 of
        // the module's configured horizon (the ground speed comes from
        // the same transform velocity; a slow jet simply probes less far).
        const auto& vel = self.tf->velocity();
        const double ground_speed =
            std::sqrt(vel.x * vel.x + vel.y * vel.y);
        const double horizon_ft = ground_speed *
            self.brain->ground_avoid().config().lookahead_sec;
        double ahead = tp.terrain_here_ft;
        for (double frac = 1.0 / 3.0; frac < 1.01; frac += 1.0 / 3.0) {
            const double d = horizon_ft * frac;
            const double px = pos.x + (ground_speed > 1.0 ? vel.x / ground_speed * d : 0.0);
            const double py = pos.y + (ground_speed > 1.0 ? vel.y / ground_speed * d : 0.0);
            ahead = std::max(ahead, ts->elevation_at_ft(px, py));
        }
        tp.terrain_ahead_ft = ahead;
        self.brain->update_terrain_picture(tp);

        // --- Traffic picture ---------------------------------------------
        std::vector<f4::ai::modules::CollisionAvoidModule::Intruder> traffic;
        for (const auto& other : airborne) {
            if (other.id == self.id) continue;
            const double dx = other.tf->position.x - pos.x;
            const double dy = other.tf->position.y - pos.y;
            const double dz = other.tf->position.z - pos.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > kTrafficGateFt)
                continue;
            f4::ai::modules::CollisionAvoidModule::Intruder intr;
            intr.entity_id = other.id.value;
            intr.position = other.tf->position;
            intr.velocity = other.tf->velocity();
            intr.roll_rate_radps = other.tf->p;  // body roll rate (droll)
            traffic.push_back(intr);
        }
        self.brain->update_traffic(std::move(traffic), vel);
    }
}

void Simulation::derive_campaign_airfield() {
    // B.3: pre-wire_atc airfield derivation for campaign_flights runs.
    // Loads the world JSON's objectives (NOT the whole populate path —
    // spawn_from_campaign_flights does that later), finds the first
    // airbase-class objective, and rewrites scenario_.airfield so that
    // wire_atc() hands StubATC the REAL runway + taxi route. Without this,
    // campaign aircraft wait for a taxi clearance that can never come.
    // (See the ordering comment in initialize().)
    if (scenario_.world_json_path.empty()) return;  // spawn fails loudly later

    f4::world::WorldState ws;
    ws.load(scenario_.world_json_path);
    for (const auto& obj : ws.objectives) {
        if (auto af = derive_airfield_from_objective(obj, 36)) {
            scenario_.airfield = std::move(*af);
            return;
        }
    }
    // No airbase objective: leave the airfield as-is — spawn fails loudly
    // with the specific "no airbase objective" message.
}

void Simulation::spawn_from_campaign_flights() {
    // Phase 2 campaign-derivation path. Loads the world JSON referenced by
    // scenario_.world_json_path, populates the EntityWorld with teams +
    // objectives + units (via f4-world::populate_world), then calls
    // spawn_aircraft_from_flights() to compose one aircraft entity per
    // Flight-class unit found.
    if (scenario_.world_json_path.empty() || scenario_.class_table_path.empty()) {
        throw std::runtime_error(
            "Simulation::spawn_from_campaign_flights: scenario is missing "
            "world_json_path or class_table_path");
    }
    if (scenario_.aircraft.empty()) {
        throw std::runtime_error(
            "Simulation::spawn_from_campaign_flights: scenario.aircraft[0] must "
            "carry the shared aircraft_config_path + vis_type_index template");
    }

    // 1. Load the WorldState from the world JSON. We need the raw
    //    ObjectiveState vector to derive the airfield (GroundLayoutList
    //    is on ObjectiveState, not on an ECS component yet).
    f4::world::WorldState ws;
    ws.load(scenario_.world_json_path);

    // 2. Populate the EntityWorld from the WorldState. This creates team +
    //    objective + unit entities with their domain components
    //    (TeamComponent, TransformComponent, SquadronComponent,
    //    FlightPlanComponent, ...). Cross-references (Flight→Squadron,
    //    Squadron→Airbase) are resolved here too. The populated maps ride
    //    along: the objective map resolves the saved waypoints' strike
    //    targets into entities (the A-G tranche's route arming).
    auto populated = f4::world::populate_world(world_, ws);

    // 3. Airfield: initialize() normally pre-derived it into
    //    scenario_.airfield (B.3 fix — BEFORE wire_atc). The local
    //    derivation stays as the fallback for hosts that called
    //    spawn_aircraft() out of order, and overrides only a still-empty
    //    taxi route (never a hand-authored one).
    ScenarioAirfield derived = scenario_.airfield;
    if (derived.taxi_route.empty()) {
        for (const auto& obj : ws.objectives) {
            auto maybe_af = derive_airfield_from_objective(obj, 36);
            if (maybe_af) {
                derived = std::move(*maybe_af);
                break;
            }
        }
        if (derived.taxi_route.empty()) {
            throw std::runtime_error(
                "Simulation::spawn_from_campaign_flights: no airbase objective "
                "with a runway list found in the world JSON, and the scenario "
                "JSON does not carry a hand-authored airfield block");
        }
    }

    // 4. The class table was loaded by initialize() (load_class_table)
    //    — the member is the ONE table every spawn path + the
    //    BubbleManager share. entity_type → vis_type lookups below go
    //    through class_table_.

    // 5. Use the bridge function to spawn one aircraft per Flight unit
    //    (B.3: with the scenario's campaign_flight_filter applied — team /
    //    mission / cap — and each spawned aircraft carrying the MissionPlan
    //    built from its flight's saved waypoints).
    //
    //    B.3+ (per-airbase fix): every AIRBASE objective in the save gets
    //    its ScenarioAirfield derived up front and passed down, so each
    //    flight's aircraft taxis/departs on ITS OWN squadron's runway —
    //    the first-airbase-only route the pre-fix code handed to every
    //    aircraft sent them taxiing across the theater (QC catch).
    airbase_airfields_.clear();
    for (const auto& obj : ws.objectives) {
        if (auto af = derive_airfield_from_objective(obj, 36)) {
            if (obj.id_num != 0) {
                airbase_airfields_[obj.id_num] = std::move(*af);
            }
        }
    }

    // 4b. The weapon class table (A-G tranche): the campaign spawn arms
    //     every flight's decoded loadout through it (wire stations +
    //     doctrine MK-82 fill + the strike fire control). NOT gated on
    //     combat.enabled — ordnance delivery is a mission behavior; only
    //     the A/A combat sweeps stay combat-gated.
    weapon_table_ = weapons::WeaponClassTable::with_builtins();

    const auto& template_ac = scenario_.aircraft.front();
    FlightSpawnFilter filter;
    filter.team = scenario_.campaign_flight_filter.team;
    filter.mission = scenario_.campaign_flight_filter.mission;
    filter.max_flights = scenario_.campaign_flight_filter.max_flights;
    aircraft_entities_ = spawn_aircraft_from_flights(
        world_, class_table_, *model_db_, aircraft_cfg_,
        derived, template_ac, filter,
        airbase_airfields_.empty() ? nullptr : &airbase_airfields_,
        &populated.objective_id_map, &weapon_table_,
        // G2: the populated world's unit map resolves UNIT-targeted
        // delivery waypoints (saved CAS/BAI flights whose strike point
        // carries a battalion VU).
        &populated.unit_id_map);

    if (aircraft_entities_.empty()) {
        throw std::runtime_error(
            "Simulation::spawn_from_campaign_flights: no Flight-class units "
            "found in the world JSON — cannot spawn any aircraft");
    }

    // 5b. C6: arm every spawned campaign aircraft (the mission-role
    //     doctrine — see arm_campaign_aircraft). The scenario's
    //     campaign_armed flag gates it; the late-spawner path arms
    //     through the SAME method in the session's adopt cadence, so
    //     bulk-fed and bus-fed fleets arm identically.
    if (scenario_.combat.campaign_armed) {
        for (const auto eid : aircraft_entities_) {
            arm_campaign_aircraft(eid);
        }
    }

    // 6. Register every derived airbase with the ATC (B.3+): the StubATC
    //    answers TaxiRequest/TakeoffRequest per airbase_id, falling back
    //    to the default airfield wire_atc() configured. Without this, the
    //    per-flight home-base tag would arrive at an ATC that can't
    //    resolve it.
    if (atc_ && !airbase_airfields_.empty()) {
        for (const auto& [vu, af] : airbase_airfields_) {
            f4::ai::atc::AirfieldConfig cfg;
            cfg.active_runway_id = af.active_runway_id;
            cfg.active_runway_name = af.active_runway_name;
            cfg.runway_heading_rad = af.runway_heading_rad;
            cfg.threshold_position = af.threshold_position;
            cfg.threshold_altitude_ft = af.threshold_altitude_ft;
            cfg.departure_altitude_ft = af.departure_altitude_ft;
            cfg.pattern_altitude_ft = af.threshold_altitude_ft + 1500.0;
            cfg.taxi_route = af.taxi_route;
            cfg.runway_end_position = af.runway_end_position;
            cfg.runway_width_ft = af.runway_width_ft;
            cfg.runway_length_ft = af.runway_length_ft;
            atc_->set_airbase_airfield(vu, cfg);
        }
    }
}

void Simulation::spawn_airfield_features() {
    // Phase 2A: spawn one entity per ScenarioFeature. Each carries
    // TransformComponent + VisualModelComponent (no FM, no brain). The
    // renderer iterates all VisualModelComponent-bearing entities uniformly,
    // so features and aircraft share the same draw path.
    //
    // Feature entities are static — their TransformComponent is set once at
    // spawn and never updated. The sim's tick() loop only syncs
    // aircraft_entities_ (which have a FlightModelComponent); features are
    // excluded from that loop because they have no FM to sync from.
    using namespace f4::entities;
    using namespace f4::simulation;  // VisualModelComponent lives here

    for (const auto& sf : scenario_.airfield_features) {
        auto h = world_.create();

        // TransformComponent — position + heading quaternion (rotation about Z-up).
        auto& tf = h.add<TransformComponent>();
        tf.position = sf.position;
        const double h2 = sf.heading_rad * 0.5;
        // Compass heading -> ENU quaternion (negative about +z; see frames.hpp).
        const auto q0 = f4::simulation::enu_quat_from_compass(h2 * 2.0);
        tf.qw = q0.w;  tf.qx = q0.x;  tf.qy = q0.y;  tf.qz = q0.z;

        // VisualModelComponent — the renderable handle. Same component type
        // as the aircraft's, resolved the same way (model_record pointer from
        // ModelDatabase). No gear switch sync needed (features have no gear).
        // V-3DLIVE: vis_type recorded for hosts that resolve meshes from
        // their own db (the session's features carry no model_record).
        auto& vis = h.add<VisualModelComponent>();
        vis.vis_type = sf.vis_type_index;
        if (model_db_->valid()) {
            vis.model_record = model_db_->model(sf.vis_type_index);
        }
        vis.active_lod = 0;

        feature_entities_.push_back(h.id());
    }
}

void Simulation::derive_real_airbase() {
    // Load the referenced world JSON and find the selected objective.
    f4::world::WorldState ws;
    ws.load(scenario_.airbase_source.world_json_path);

    const f4::world::ObjectiveState* obj = nullptr;
    for (const auto& o : ws.objectives) {
        if (o.x == scenario_.airbase_source.grid_x &&
            o.y == scenario_.airbase_source.grid_y) {
            obj = &o;
            break;
        }
    }
    if (!obj && !scenario_.airbase_source.name.empty()) {
        // Objectives carry nameid, not display names; treat the selector
        // as a numeric nameid first, then give up with a precise error.
        char* end = nullptr;
        const long nid = std::strtol(scenario_.airbase_source.name.c_str(), &end, 10);
        if (end && *end == 0) {
            for (const auto& o : ws.objectives) {
                if (o.nameid == static_cast<int16_t>(nid) && !o.ground_layout.empty()) {
                    obj = &o;
                    break;
                }
            }
        }
    }
    if (!obj) {
        throw std::runtime_error(
            "Simulation::derive_real_airbase: objective not found in '" +
            scenario_.airbase_source.world_json_path.string() + "'");
    }

    const int runway_id = scenario_.airbase_source.active_heading_deg / 10;
    auto derived = derive_airfield_from_objective(*obj, runway_id);
    if (!derived) {
        throw std::runtime_error(
            "Simulation::derive_real_airbase: selected objective has no usable "
            "runway ground layout");
    }

    // Keep hand-authored settings the layout cannot provide (e.g. a custom
    // departure altitude), then take the derived airfield.
    if (scenario_.airfield.departure_overridden) {
        derived->departure_altitude_ft = scenario_.airfield.departure_altitude_ft;
    }
    scenario_.airfield = std::move(*derived);

    // Stash the raw layout for the renderer (objective-local lists + the
    // objective's ENU position).
    scenario_.layout_lists = obj->ground_layout;
    constexpr double FT_PER_GRID = 1024.0;
    scenario_.layout_center = f4::geo::WorldPosition(
        static_cast<double>(obj->x) * FT_PER_GRID,
        static_cast<double>(obj->y) * FT_PER_GRID,
        static_cast<double>(obj->z));

    // Real 3D feature models (buildings, runway/taxiway sections, towers):
    // FALCON4.ct maps FeatureEntryState.index (a descriptionIndex; the
    // entity_type is index + 100) -> KoreaObj vis_type[0]. Features with
    // no model (lights, trucks) or the (0,0,0) placeholder are skipped.
    if (!scenario_.airbase_source.class_table_path.empty()) {
        f4::world_convert::ClassTable ct;
        ct.load_auto(scenario_.airbase_source.class_table_path);
        int skipped_no_vistype = 0;
        for (const auto& f : obj->features) {
            if (f.index == 0 && f.offset_x == 0 && f.offset_y == 0) continue;
            const auto entity_type = static_cast<std::uint16_t>(100 + f.index);
            const auto vis_type = ct.vis_type_for(entity_type, 0);
            if (vis_type <= 0) { ++skipped_no_vistype; continue; }
            ScenarioFeature sf;
            sf.name = f.name;
            sf.vis_type_index = vis_type;
            sf.position = f4::geo::WorldPosition(
                scenario_.layout_center.x + f.offset_x,
                scenario_.layout_center.y + f.offset_y,
                scenario_.layout_center.z + f.offset_z);
            sf.heading_rad = static_cast<double>(f.facing) * 0.017453292519943295;
            scenario_.airfield_features.push_back(std::move(sf));
        }
    }

    // Rotate runway-frame waypoints into ENU about the derived threshold.
    if (scenario_.waypoints_runway_frame) {
        const double hs = scenario_.airfield.runway_heading_rad;
        const double sh = std::sin(hs), ch = std::cos(hs);
        const auto& thr = scenario_.airfield.threshold_position;
        for (auto& wp : scenario_.waypoints) {
            const double rx = wp.position.x;   // right of heading
            const double ry = wp.position.y;   // downrange
            wp.position.x = thr.x + rx * ch + ry * sh;
            wp.position.y = thr.y - rx * sh + ry * ch;
            // z (MSL) authored absolute — unchanged.
        }
        scenario_.waypoints_runway_frame = false;   // normalized
    }

    // Resolve parking:auto aircraft to the synthesized spots.
    for (auto& ac : scenario_.aircraft) {
        if (!ac.parking_auto) continue;
        if (scenario_.airfield.parking_spots.empty()) {
            throw std::runtime_error(
                "Simulation::derive_real_airbase: aircraft '" + ac.callsign +
                "' requests parking:auto but the layout yielded no spots");
        }
        const auto idx = static_cast<std::size_t>(ac.parking_index) %
                         scenario_.airfield.parking_spots.size();
        ac.parking_spot = scenario_.airfield.parking_spots[idx].position;
        ac.heading_rad = scenario_.airfield.parking_spots[idx].heading_rad;
    }
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
    // Pattern altitude: 1500 ft above the threshold (typical radar pattern).
    af.pattern_altitude_ft = scenario_.airfield.threshold_altitude_ft + 1500.0;
    af.taxi_route = scenario_.airfield.taxi_route;
    af.runway_end_position = scenario_.airfield.runway_end_position;
    af.runway_width_ft = scenario_.airfield.runway_width_ft;
    af.runway_length_ft = scenario_.airfield.runway_length_ft;
    atc_->set_airfield(af);
}


namespace {
// F4_TICK_PROF=1 phase timing (perf triage only; off by default).
struct TickProf {
    bool on = std::getenv("F4_TICK_PROF") != nullptr;
    double t_ground = 0, t_update_all = 0, t_intents = 0, t_sweeps = 0,
           t_sync = 0, t_guns = 0, t_bubble = 0, t_record = 0, t_total = 0;
    int n = 0;
};
TickProf g_prof{};
} // namespace

void Simulation::tick(double dt) {
    if (paused_) return;
    const auto prof_t0 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    // dt is AUTHORITATIVE and FIXED: hosts call tick() once per unit of
    // owed sim time, always with the same dt (the scenario's sim_dt;
    // the player drains a wall-clock accumulator in whole sim_dt
    // ticks). The flight model subdivides it into 6 minor steps (1/360 s
    // at the default 1/60 s sim_dt) — the operating point the FCS's
    // discrete PI/lead-lag filters were tuned for. Scaling dt here
    // would silently move every integrator and filter off its tuned
    // discretization (the old internal time_scale_ multiplication did
    // exactly that, forcing the player's 4x slider cap — see
    // FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2; it has been removed,
    // pacing belongs to the host, and set_trace_time_scale() carries
    // the CSV metadata that used to ride on it).

    // Two-pass: brains (pass 1, priority >= 75) then physics (pass 2, < 75).
    // BrainComponent runs first, reads IAircraftState, writes IPilotInputSink.
    // FlightModelComponent runs second, consumes the pending input, integrates.
    // All via interface-based lookup — the brain doesn't know it's talking to
    // a FlightModelComponent, just that the entity provides those interfaces.
    //
    // Phase 2: update_all iterates EVERY behavioral component on EVERY entity,
    // so all aircraft brains + FMs advance in lockstep. The sync loop below
    // then walks the spawned aircraft_entities_ list to pull their per-instance
    // state out of the FM and into the renderer-facing TransformComponent +
    // VisualModelComponent.
    //
    // IMPORTANT: the ground-elevation query happens BEFORE world_.update_all()
    // so the brain and the FM see the SAME groundZ on the same tick. Previously
    // the query ran AFTER update_all(), which meant the brain's altitude-AGL
    // read was based on the previous tick's terrain — at high time_scale this
    // produced a "terrain phugoid" where the brain commanded climb against a
    // stale ground reference while the FM's ground clamp fired on the current
    // reference. See FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-2.
    const auto prof_t1 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    for (const auto eid : aircraft_entities_) {
        auto h = entities::EntityHandle(eid, &world_);
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (!fm) continue;
        const auto& s_cur = fm->state();
        const double east_ft  = s_cur.kin.y;   // NED east = ENU east
        const double north_ft = s_cur.kin.x;   // NED north = ENU north
        f4::terrain::TerrainSource* ts = terrain_source_ ? terrain_source_ : &default_terrain_;
        const double ground_z = ts->elevation_at_ft(east_ft, north_ft);
        fm->set_ground(ground_z, f4::math::Vec3d{0.0, 0.0, -1.0});

        // C6: steer the radar's search bar onto the ground track —
        // FreeFalcon's radar is boresighted to the nose, and a campaign
        // fighter flying EAST under the M2 placeholder's fixed
        // north-centered bar never paints the hostile off its nose (the
        // 9-minute armed war: 96 airborne, 33 fighters, zero detections
        // that mattered). North-flying rigs (every M2/M3/M4 test and
        // scenario) keep the exact north bar they were pinned with;
        // slow/parked aircraft keep their last bar (the clutter rule
        // keeps their scans cheap).
        if (auto* radar = h.get<sensors::RadarSimComponent>();
            radar != nullptr && radar->mode() == sensors::RadarMode::Search) {
            // NED: kin.xdot = north velocity, kin.ydot = east velocity.
            const double track_north = s_cur.kin.xdot;
            const double track_east = s_cur.kin.ydot;
            constexpr double kTrackMinFps = 100.0;
            const double track_speed =
                std::sqrt(track_east * track_east +
                          track_north * track_north);
            if (track_speed > kTrackMinFps) {
                radar->scan.azimuth_center_rad =
                    std::atan2(track_east, track_north);  // CW from north
            }
        }
    }
    const auto prof_t2 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    // Combat chain (M3): stamp the sim clock the sensor/weapon components
    // read (message stamps + scan-interval carry). Both use static clocks
    // by design — the host owns time (documented in radar_component.hpp /
    // missile_battery.hpp). Stamped BEFORE update_all so this tick's
    // messages carry the time at which they occur. Gated: worlds without
    // combat never touch the static clocks at all.
    const bool combat_on = scenario_.combat.enabled;
    const double t_now = sim_time_s_ + dt;
    if (combat_on) {
        sensors::RadarSimComponent::set_sim_time(t_now);
        weapons::MissileSimComponent::set_sim_time(t_now);
        weapons::BombSimComponent::set_sim_time(t_now);
    }

    // Step 11 (wingman/2-ship): push each wingman's lead picture + sort
    // hint BEFORE the brains run — the wingman module is engine-agnostic,
    // the host is its eyes. Runs with combat on or off (formation is not
    // a combat behavior); no-op when no aircraft declared a lead_callsign.
    if (!wingman_pairs_.empty()) {
        push_wingman_lead_pictures();
    }

    // The arbiter's safety rungs (M3): terrain + traffic pictures BEFORE
    // the brains run — the ground-avoid and collision-avoid modules are
    // engine-agnostic, the host is their eyes. Runs with combat on or
    // off (safety is not a tactic) and for every airborne aircraft.
    push_safety_pictures();

    // PERF-1: the shared air picture — ONE world walk per tick feeding
    // every combat brain's SensorFusion rebuild (the beam-fight
    // every-tick refresh made the per-brain walk the merge-phase
    // collapse; see PERFORMANCE_PLAN.md §1). Combat-gated: unarmed
    // worlds keep the fusion's own world query (and its goldens).
    if (combat_on) {
        push_air_picture_(dt);
    }


    world_.update_all(dt, bus_);
    bus_.flush_pending();  // drain deferred ATC messages (TaxiClearance, etc.)
    const auto prof_t3 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

    // Combat chain (M3 tactics): execute the combat brains' intents
    // (radar locks + weapon releases) NOW — after update_all (the brains
    // produced the intents in pass 1) but before update_rwr, so the
    // victim's RWR sees the launch warning THIS tick and the defense
    // starts one tick earlier. launch_missile creates entities outside
    // update_all (the same rule sweep_spent_missiles follows: never
    // mutate the world mid-iteration). Commanding STT here takes effect
    // on the next tick's scan.
    if (combat_on) {
        // Active roster, not the whole brain population: campaign worlds
        // hold thousands of dormant parked-inventory brains (squadron
        // deaggregation) — the intents pass must cost the active aircraft,
        // not the world (FreeFalcon fidelity-tiering: no sim work outside
        // the simulated set).
        execute_brain_combat_intents(world_, bus_, weapon_table_, t_now,
                                     &aircraft_entities_);
    }
    const auto prof_t4 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

    // Combat chain (M3): the two world-level sweeps the ECS tick can't run
    // itself (both mutate/iterate the world between ticks, never inside
    // update_all):
    //   update_rwr          — rebuild every RwrComponent's warning picture
    //                         from the radars + missiles that paint them
    //                         (publishes RwrWarningMessage on new lock/
    //                         launch transitions).
    //   sweep_spent_missiles— destroy terminal missile entities (Detonated/
    //                         Expired). The kill message already flowed; the
    //                         corpse sweep belongs to the host.
    if (combat_on) {
        sensors::update_rwr(world_, bus_, t_now);
        weapons::sweep_spent_missiles(world_);
        weapons::sweep_spent_bombs(world_);
    }
    const auto prof_t5 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

    // Per-aircraft sync: pull FM state → TransformComponent + VisualModelComponent.
    for (const auto eid : aircraft_entities_) {
        auto h = entities::EntityHandle(eid, &world_);
        auto* tf = h.get<entities::TransformComponent>();
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (!tf || !fm) continue;

        const auto& s = fm->state();
        // NED -> ENU: enu.x = ned.y (east), enu.y = ned.x (north), enu.z = -ned.z (up)
        tf->position = f4::geo::WorldPosition(s.kin.y, s.kin.x, -s.kin.z);

        // Velocity, same axis swap (found by the M3 combat integration: the
        // sync used to write position + orientation but NEVER velocity, so
        // every combat consumer reading the transform — missile launch
        // (initial velocity + seeker boresight), radar aspect, RWR closure —
        // saw a parked aircraft. The missile launched with 0 ft/s inherited,
        // fell ballistically, and lost the seeker cone immediately.)
        tf->vx = s.kin.ydot;   // east
        tf->vy = s.kin.xdot;   // north
        tf->vz = -s.kin.zdot;  // up

        // NED -> ENU orientation. The FM's quaternion is NED body-to-world
        // (compass yaw positive about z=DOWN); ENU wants compass as a
        // NEGATIVE rotation about z=UP. The basis change is a 180-deg
        // rotation about the NE bisector, so the quaternion conjugates as
        // (w,x,y,z) -> (w,y,x,-z). Storing the NED quaternion raw mirrors
        // heading and scrambles pitch/roll — the "model flying upside
        // down" artifact. See f4/simulation/frames.hpp + test_frames.cpp.
        const f4::simulation::QuatD q_enu = f4::simulation::ned_quat_to_enu(
            {s.kin.quat.w, s.kin.quat.x, s.kin.quat.y, s.kin.quat.z});
        tf->qw = q_enu.w;
        tf->qx = q_enu.x;
        tf->qy = q_enu.y;
        tf->qz = q_enu.z;

        // Sync VisualModelComponent gear switch from FM gear position.
        // F-16 gear is switch #10 (per f4-models-viewer comment); 0=down, 1=up.
        // AeroState::gearPos is 1.0 when fully down, 0.0 when fully up (the FM
        // auto-commands gearHandle based on gear.inAir — see flight_model.cpp:263).
        auto* vis = h.get<VisualModelComponent>();
        if (vis && !vis->model_state.switches.empty()) {
            vis->model_state.switches[0].active_child = (s.aero.gearPos > 0.5) ? 0 : 1;
        }
    }
    const auto prof_t6 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

    // Combat chain (Steps 11-12, guns): fly every GunComponent's burst
    // NOW — after the per-aircraft sync (the tracers emit from the
    // aircraft's FRESH muzzle pose this tick; the burst itself was
    // started by execute_brain_combat_intents above) and outside
    // update_all (hit detection mutates the world: damage + kills).
    // Tracers in flight keep flying after their shooter dies — the
    // stream's physics, not the brain's. No GunComponents exist when
    // combat is off; the sweep is a no-op then.
    if (combat_on) {
        weapons::update_guns(world_, bus_, dt, t_now);
    }
    const auto prof_t7 = g_prof.on ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};

    sim_time_s_ += dt;
    ++tick_;

    // Mode B: per-tick bubble update. Deaggregates ground/naval units
    // entering the player's bubble, reaggregates those leaving it. No-op
    // when bubble_manager_ is null (scenario-list spawn mode).
    update_bubble();

    if (recorder_) record_snapshot();
    if (fcs_trace_) record_fcs_trace_sample();

    if (g_prof.on) {
        const auto us = [](std::chrono::steady_clock::time_point a,
                           std::chrono::steady_clock::time_point b) {
            return std::chrono::duration<double, std::micro>(b - a).count();
        };
        const auto prof_t8 = std::chrono::steady_clock::now();
        g_prof.t_ground    += us(prof_t1, prof_t2);
        g_prof.t_update_all += us(prof_t2, prof_t3);
        g_prof.t_intents   += us(prof_t3, prof_t4);
        g_prof.t_sweeps    += us(prof_t4, prof_t5);
        g_prof.t_sync      += us(prof_t5, prof_t6);
        g_prof.t_guns      += us(prof_t6, prof_t7);
        g_prof.t_record    += us(prof_t7, prof_t8);
        g_prof.t_total     += us(prof_t0, prof_t8);
        ++g_prof.n;
        if (g_prof.n % 600 == 0) {
            std::fprintf(stderr,
                "[tickprof] n=%d total=%.1fms ground=%.2fms update_all=%.2fms "
                "intents=%.2fms sweeps=%.2fms sync=%.2fms guns=%.2fms "
                "record=%.2fms\n",
                g_prof.n, g_prof.t_total / 1000.0, g_prof.t_ground / 1000.0,
                g_prof.t_update_all / 1000.0, g_prof.t_intents / 1000.0,
                g_prof.t_sweeps / 1000.0, g_prof.t_sync / 1000.0,
                g_prof.t_guns / 1000.0, g_prof.t_record / 1000.0);
        }
    }
}

void Simulation::record_snapshot() {
    // Minimal snapshot for now: timing + position + basic kinematics.
    // Full implementation will populate control inputs, AI mode/state,
    // intended path, cross-track error, fuel, engine state, G-loads.
    //
    // Phase 2: one snapshot per aircraft per tick. The FlightRecorder's
    // FlightSnapshot carries an entity_id discriminator so playback can
    // separate the tracks.
    //
    // B.3/QC: scenario.record_every decimates — tick % record_every != 0
    // records nothing (1 = every tick, the original behavior).
    if (scenario_.record_every > 1 &&
        (tick_ % static_cast<std::uint64_t>(scenario_.record_every)) != 0) {
        return;
    }
    for (const auto eid : aircraft_entities_) {
        f4::recorder::FlightSnapshot snap;
        snap.sim_time_s = sim_time_s_;
        snap.tick = tick_;
        snap.entity_id = eid.value;

        auto h = entities::EntityHandle(eid, &world_);
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

        // AI mode/state from the mission sequencer (which module + state
        // is flying the aircraft this tick).
        if (auto* brain = h.get<f4::ai::BrainComponent>(); brain) {
            snap.ai_mode = brain->mode_name();
            snap.ai_state = brain->state_name();
        }

        recorder_->record(snap);
    }

    // M4: missile tracks — one snapshot per LIVE missile per tick, so a
    // recorded fight replays with the flyouts visible (the world viewer
    // draws them from the same snapshot stream; entity_id discriminates).
    // Missiles are swept AFTER they go terminal (sweep_spent_missiles runs
    // before record_snapshot in tick()), so a missile's last in-flight
    // position is the tick BEFORE its detonation — the burst point itself
    // lives in the MissileDetonated combat event, which keeps the replay
    // endpoint covered.
    //
    // with_component() returns a snapshot (by value), so recording here —
    // after the world settled — is safe. No missiles exist in non-combat
    // scenarios; the loop is a no-op.
    for (const auto mid : world_.with_component<weapons::MissileComponent>()) {
        auto h = entities::EntityHandle(mid, &world_);
        const auto* mc = h.get<weapons::MissileComponent>();
        const auto* tf = h.get<entities::TransformComponent>();
        if (mc == nullptr || tf == nullptr) continue;

        f4::recorder::FlightSnapshot snap;
        snap.missile = true;
        snap.sim_time_s = sim_time_s_;
        snap.tick = tick_;
        snap.entity_id = mid.value;
        snap.position = tf->position;
        snap.altitude_msl_ft = tf->position.z;
        snap.on_ground = false;
        snap.vt_fps = mc->missile.velocity().length();
        snap.ai_mode = "Missile";
        snap.ai_state = weapons::missile_status_name(mc->missile.status());
        if (const auto* rec = weapon_table_.get(mc->weapon_handle)) {
            snap.callsign = rec->name;  // the weapon's name reads as the label
        }
        recorder_->record(snap);
    }

    // M5 (A-G): bomb tracks — same treatment as missiles, one snapshot per
    // LIVE bomb per tick, so a recorded strike replays with the falls
    // visible. The bombs are swept after they go terminal (sweep_spent_
    // bombs runs before record_snapshot), so a bomb's last in-flight
    // position is the tick BEFORE its impact — the impact point itself
    // lives in the BombImpact combat event, which keeps the replay
    // endpoint covered (same contract the missile track follows).
    for (const auto bid : world_.with_component<weapons::BombComponent>()) {
        auto h = entities::EntityHandle(bid, &world_);
        const auto* bc = h.get<weapons::BombComponent>();
        const auto* tf = h.get<entities::TransformComponent>();
        if (bc == nullptr || tf == nullptr) continue;

        f4::recorder::FlightSnapshot snap;
        snap.missile = true;   // the replay's weapon-track discriminator
        snap.sim_time_s = sim_time_s_;
        snap.tick = tick_;
        snap.entity_id = bid.value;
        snap.position = tf->position;
        snap.altitude_msl_ft = tf->position.z;
        snap.on_ground = false;
        snap.vt_fps = bc->bomb.velocity().length();
        snap.ai_mode = "Bomb";
        snap.ai_state = weapons::bomb_status_name(bc->bomb.status());
        if (const auto* rec = weapon_table_.get(bc->weapon_handle)) {
            snap.callsign = rec->name;
        }
        recorder_->record(snap);
    }
}

void Simulation::write_recording() {
    if (!recorder_ || scenario_.record_path.empty()) return;
    recorder_->write_json(scenario_.record_path, scenario_.name);
}

void Simulation::write_fcs_trace() {
    if (!fcs_trace_ || scenario_.fcs_trace_path.empty()) return;
    fcs_trace_->write_csv(scenario_.fcs_trace_path.string());
}

void Simulation::record_fcs_trace_sample() {
    // One row per aircraft per tick. The trace's purpose is control-loop
    // diagnosis (see FLIGHT_CONTROL_NEXT_STEPS.md §3.1), so it captures the
    // AI's commands, the FCS's intermediates, the EOM's body rates, and
    // the navigation intent (target alt/speed/heading + cross-track).
    //
    // Fields not relevant to a given phase are left at their default
    // (0.0 / "") — e.g. course_lateral_ft is only meaningful during the
    // landing phase; outside it, it stays 0.0 and the column is empty
    // in plots.
    for (const auto eid : aircraft_entities_) {
        auto h = entities::EntityHandle(eid, &world_);
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (!fm) continue;

        const auto& s = fm->state();
        f4::recorder::FcsTraceSample sample;
        sample.tick       = tick_;
        sample.sim_time_s = sim_time_s_;
        sample.time_scale = trace_time_scale_;

        // --- AI state ---
        if (auto* brain = h.get<f4::ai::BrainComponent>(); brain) {
            sample.ai_mode  = brain->mode_name();
            sample.ai_state = brain->state_name();
        }

        // --- AI commands (from the last PilotInput the brain wrote) ---
        // The pending input was just consumed by the FM this tick; we read
        // the brain's cached last_pilot_input via the FM's pending slot
        // (which still holds the value — the FM clears it next tick).
        const auto& pi = fm->last_consumed_input();
        sample.pitch_cmd       = pi.pstick;
        sample.roll_cmd        = pi.rstick;
        sample.yaw_cmd         = pi.ypedal;
        sample.throttle_cmd    = pi.throttle;
        sample.speed_brake_cmd = pi.speedBrake;
        sample.tef_cmd         = pi.tefCmd;
        sample.lef_cmd         = pi.lefCmd;
        sample.gear_down       = (pi.gearHandle > 0.0);
        sample.wheel_brakes    = pi.wheelBrakes;
        sample.parking_brake   = pi.parkingBrake;

        // --- FCS intermediates (FcsState is a member of AircraftState) ---
        const auto& fcs = s.fcs;
        sample.aoacmd_deg      = f4::flight::to_degrees(fcs.aoacmd);
        sample.pscmd           = fcs.pscmd;
        sample.pstab           = fcs.pstab;
        sample.ptcmd           = fcs.ptcmd;
        sample.nzcgs           = s.loads.nzcgs;
        sample.pitch_integral  = fcs.pitchIntegral.output();
        sample.betcmd_deg      = f4::flight::to_degrees(fcs.betcmd);
        sample.alpha_deg       = f4::flight::to_degrees(s.aero.alpha);
        sample.beta_deg        = f4::flight::to_degrees(s.aero.beta);
        sample.yshape          = fcs.yshape;
        sample.pshape          = fcs.pshape;
        sample.rshape          = fcs.rshape;

        // --- Body rates (rad/s -> deg/s for readability) ---
        constexpr double R2D = 180.0 / 3.14159265358979323846;
        sample.p_dps = s.kin.p * R2D;
        sample.q_dps = s.kin.q * R2D;
        sample.r_dps = s.kin.r * R2D;

        // --- Kinematics ---
        sample.vcas_kts    = s.vcas;
        sample.vt_fps      = s.kin.vt;
        sample.alt_msl_ft  = -s.kin.z;                       // NED z-down
        sample.alt_agl_ft  = -s.kin.z - s.gear.groundZ_ft;
        sample.vs_fpm      = -s.kin.zdot * 60.0;
        sample.heading_deg = f4::flight::to_degrees(s.kin.psi);
        sample.pitch_deg   = f4::flight::to_degrees(s.kin.theta);
        sample.roll_deg    = f4::flight::to_degrees(s.kin.phi);
        sample.x_ft        = s.kin.y;                          // NED y = ENU east
        sample.y_ft        = s.kin.x;                          // NED x = ENU north
        sample.mach       = s.mach;

        // --- Navigation intent (only populated when the relevant module
        // is active; 0.0 otherwise so the column plots as flat-zero outside
        // its phase rather than carrying stale state) ---
        if (auto* brain = h.get<f4::ai::BrainComponent>(); brain) {
            using Phase = f4::ai::BrainComponent::Phase;
            const auto phase = brain->phase();
            if (phase == Phase::Ground) {
                const auto& t = brain->takeoff();
                sample.target_alt_ft     = t.departure_alt_ft;
                sample.target_speed_kts  = t.flyout_speed_kts;
                sample.target_heading_deg = f4::flight::to_degrees(
                    f4::flight::angle_from_radians(t.runway_heading_rad()));
            } else if (phase == Phase::Enroute) {
                const auto& n = brain->navigation();
                const auto* wp = n.current_waypoint();
                if (wp) {
                    sample.target_alt_ft    = wp->position.z;
                    sample.target_speed_kts = wp->speed_kts;
                }
                sample.target_heading_deg = f4::flight::to_degrees(
                    f4::flight::angle_from_radians(n.current_heading_rad()));
            } else if (phase == Phase::Approach) {
                const auto& l = brain->landing();
                sample.target_alt_ft     = l.glide_slope_alt_ft();
                sample.target_speed_kts  = l.approach_speed_kts;
                sample.target_heading_deg = f4::flight::to_degrees(
                    f4::flight::angle_from_radians(l.localizer_heading_rad()));
                sample.course_lateral_ft    = l.course_lateral_ft();
                sample.course_along_ft       = l.course_along_ft();
                sample.localizer_heading_deg = f4::flight::to_degrees(
                    f4::flight::angle_from_radians(l.localizer_heading_rad()));
            }
        }

        // --- Ground / engine ---
        sample.on_ground   = !s.gear.inAir;
        sample.gear_pos    = s.aero.gearPos;
        sample.engine_rpm  = s.engine.rpm;
        sample.fuel_lbs    = s.fuel.fuel_lbs;
        sample.nz         = s.loads.nzcgs;
        sample.nx         = s.loads.nxcgs;

        fcs_trace_->record(sample);
    }
}

// ============================================================================
// Mode B: Unit Deaggregation
// ============================================================================

void Simulation::load_class_table() {
    // The ONE load for the Simulation's lifetime. Empty path → the
    // member stays empty and every consumer degrades gracefully
    // (vis_type_for returns 0, the spawn paths fall back). Malformed
    // content throws (loud) exactly like the per-path loads used to.
    if (!scenario_.class_table_path.empty()) {
        class_table_.load_auto(scenario_.class_table_path);
    }
}

void Simulation::spawn_squadron_aircraft() {
    // Spawn parked aircraft for Squadron units (one per un-tasked pilot slot).
    // No-op when there are no Squadron entities (e.g. scenario-list spawn mode
    // has no campaign units). The Squadron→airbase resolution was fixed by
    // the positional fallback in world_loader.cpp, so this now works for
    // save1.cam where all 72 squadrons have airbase_id=0 in the binary tail.
    //
    // The airfield + template aircraft are required for parking-spot lookup
    // and aircraft-config init. We use the scenario's first aircraft entry
    // as the template (same convention as spawn_from_campaign_flights).
    if (scenario_.aircraft.empty()) return;

    // The class table: initialize() already loaded it into class_table_
    // (the member every spawn path + the BubbleManager share — the
    // pre-fix code loaded a THIRD copy here into another local).
    const auto& template_ac = scenario_.aircraft.front();
    squadron_aircraft_entities_ = spawn_aircraft_from_squadrons(
        world_, class_table_, *model_db_, aircraft_cfg_,
        scenario_.airfield, template_ac);
}

void Simulation::init_bubble_manager() {
    // Only construct the BubbleManager when the world contains campaign
    // units (Battalion/Brigade/TaskForce with VehicleCompositionComponent).
    // For the scenario-list spawn path (no campaign units), the manager
    // would be a no-op — we skip it to avoid the per-tick overhead of
    // with_component<VehicleCompositionComponent>() returning empty.
    const auto unit_ids = world_.with_component<entities::VehicleCompositionComponent>();
    if (unit_ids.empty()) return;

    // The class table: initialize() already loaded it into class_table_
    // — the member the BubbleManager borrows for the Simulation's
    // LIFETIME. The old code loaded a stack local here and passed THAT:
    // the local died at function return, leaving bubble_manager_'s ct_
    // dangling — the first tick's deagg then crashed in
    // ClassTable::vis_type_for() (access violation, viewer Start
    // Session). The member outlives the manager; the contract on the
    // BubbleManager constructor ("ct must outlive the manager") is
    // finally honored at every construction site. When the path was
    // empty the old code still constructed the manager over an empty
    // local — spawn_vehicles_from_unit handles an empty CT gracefully
    // (returns no vehicles, marks the unit as "tried, no model") — and
    // an empty member preserves exactly that behavior.
    //
    // B.0: the deagg radii come from Falcon4.AII when the scenario
    // carries one (aii_path) — SIM_BUBBLE_SIZE / GROUND_BUBBLE_SIZE in
    // campaign grid units, converted to feet by bubble_radii_from_aii().
    // Empty path / missing file → the documented defaults, which convert
    // to exactly the radii the BubbleManager constructor hardcoded before
    // the AII parser existed (1024 / 2560 ft), so every pre-B.0 scenario
    // is byte-identical. A present-but-malformed AII throws (loud).
    const auto [ground_radius_ft, air_radius_ft] =
        bubble_radii_from_aii(scenario_.aii_path);
    default_ground_radius_ft_ = ground_radius_ft;

    bubble_manager_ = std::make_unique<BubbleManager>(
        world_, class_table_, *model_db_, ground_radius_ft, air_radius_ft);
}

void Simulation::update_bubble() {
    if (!bubble_manager_) return;

    // V-3DLIVE: the view bubble wins when the host set one — the
    // deaggregation follows the EYE (the map camera), not the clock:
    // zoom into a battalion and its vehicles spawn, even while the
    // session is paused (the host calls refresh_bubble() as the camera
    // moves). The radius override scales with the host's zoom.
    if (view_bubble_active_) {
        bubble_manager_->set_ground_radius_ft(view_bubble_radius_ft_);
        bubble_manager_->update(view_bubble_center_);
        return;
    }

    // The ownship path (FreeFalcon's bubble): restore the AII default
    // radius, then follow the first (primary) aircraft's position.
    if (aircraft_entities_.empty()) return;
    bubble_manager_->set_ground_radius_ft(default_ground_radius_ft_);

    // Use the first (primary) aircraft's position as the bubble center.
    // Phase 2 has N aircraft, but the bubble follows the camera focus —
    // which is the first aircraft (per aircraft_entity() accessor).
    // A future multi-player / multi-camera scenario would need a different
    // bubble center (or multiple BubbleManagers).
    entities::EntityHandle ownship(aircraft_entities_.front(), &world_);
    const auto* tf = ownship.get<entities::TransformComponent>();
    if (!tf) return;

    bubble_manager_->update(tf->position);
}

} // namespace f4::simulation

// f4-simulation/src/campaign_bridge.cpp
//
// Implementation of the Phase 2 campaign bridge. See campaign_bridge.hpp
// for the architecture rationale — these two functions close the gap
// between f4-world's campaign-derived WorldState and f4-simulation's
// Scenario struct, replacing the hand-authored scenario JSON with data
// pulled from a real Falcon 4.0 campaign save.
//
// Coordinated frames used here:
//   - Campaign grid:   int16 x, y in grid units (1 grid unit = 1024 ft).
//                      ObjectiveState carries these. f4-world::populate_world
//                      converts them to ENU feet using grid_to_feet().
//   - Ground-layout offsets: float x, y in feet, RELATIVE to the objective
//                      center. GroundLayoutPoint carries these. We add them
//                      to the objective's ENU position to get world ENU.
//   - Scenario ENU:    double east_ft, north_ft, up_ft relative to the
//                      theater datum. ScenarioAirfield.threshold_position
//                      and friends use this frame.

#include "f4/simulation/campaign_bridge.hpp"

#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/world_convert/objective_decoder.hpp>  // ObjectiveType::TYPE_AIRBASE
#include <f4/world_convert/theater_data.hpp>        // PointListType
#include <f4/math/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace f4::simulation {

namespace {

// Same constant as f4-world/src/world_loader.cpp (not exported). One grid
// unit = 1024 ft in the Falcon 4.0 campaign.
constexpr double FT_PER_GRID = 1024.0;

// Convert a campaign grid (x, y, z) to ENU feet. z is already in feet.
f4::geo::WorldPosition grid_to_enu(int16_t gx, int16_t gy, float gz) {
    return f4::geo::WorldPosition{
        static_cast<double>(gx) * FT_PER_GRID,
        static_cast<double>(gy) * FT_PER_GRID,
        static_cast<double>(gz)
    };
}

// Add a ground-layout (x, y) offset (feet, relative to objective center)
// to an ENU world position. The z component of the offset is not present
// in GroundLayoutPoint (only x, y) — we use the objective's altitude.
f4::geo::WorldPosition add_offset(const f4::geo::WorldPosition& base,
                                   float dx, float dy) {
    return f4::geo::WorldPosition{
        base.x + static_cast<double>(dx),
        base.y + static_cast<double>(dy),
        base.z
    };
}

// Find the first GroundLayoutList of a given type in the objective's
// ground_layout vector. Returns nullptr if not found.
const f4::entities::GroundLayoutList*
find_layout(const f4::world::ObjectiveState& obj, uint8_t type) {
    for (const auto& l : obj.ground_layout) {
        if (l.type == type) return &l;
    }
    return nullptr;
}

} // namespace

std::optional<ScenarioAirfield>
derive_airfield_from_objective(const f4::world::ObjectiveState& obj,
                                int active_runway_id) {
    // The objective must have at least one ground_layout list to be an
    // airbase. We don't strictly require ObjectiveType == TYPE_AIRBASE
    // because some airstrips carry TYPE_AIRSTRIP and still have runway
    // layouts — the presence of a runway list is the authoritative check.
    if (obj.ground_layout.empty()) return std::nullopt;

    const auto* runway_list = find_layout(obj, f4::world_convert::PLT_RUNWAY);
    if (!runway_list || runway_list->points.size() < 2) return std::nullopt;

    ScenarioAirfield af;
    af.active_runway_id = active_runway_id;
    af.active_runway_name = "Rwy " + std::to_string(active_runway_id);

    // The runway list's heading_deg comes from the on-disk sin/cos pair
    // (see theater_data.cpp:PtHeaderData). Convert to radians.
    constexpr double DEG_TO_RAD = 0.017453292519943295;
    af.runway_heading_rad = runway_list->heading_deg * DEG_TO_RAD;

    // Objective center → ENU feet. The runway points are offsets from
    // this center.
    const auto obj_center = grid_to_enu(obj.x, obj.y, obj.z);

    // First runway point = threshold; last = far end. The on-disk format
    // stores them in order from threshold to rollout end.
    af.threshold_position = add_offset(obj_center,
                                        runway_list->points.front().x,
                                        runway_list->points.front().y);
    af.runway_end_position = add_offset(obj_center,
                                         runway_list->points.back().x,
                                         runway_list->points.back().y);
    af.threshold_altitude_ft = obj_center.z;
    af.departure_altitude_ft = af.threshold_altitude_ft + 2500.0;

    // Build a taxi route. For Phase 2 we use a simple heuristic:
    //   1. Start at the first parking spot (PLT_PARK) if any.
    //   2. If a PLT_FOLLOW_ME list exists, splice its points in (these are
    //      the taxiway centerline points the follow-me truck drives).
    //   3. End at the runway threshold.
    // If no parking list exists, start directly at the threshold (degenerate
    // but valid — the scenario is still playable, the aircraft just starts
    // on the runway).
    std::vector<f4::geo::WorldPosition> route;

    const auto* park_list = find_layout(obj, f4::world_convert::PLT_PARK);
    if (park_list && !park_list->points.empty()) {
        route.push_back(add_offset(obj_center,
                                    park_list->points.front().x,
                                    park_list->points.front().y));
    }

    const auto* follow_list = find_layout(obj, f4::world_convert::PLT_FOLLOW_ME);
    if (follow_list) {
        for (const auto& p : follow_list->points) {
            route.push_back(add_offset(obj_center, p.x, p.y));
        }
    }

    // Always end at the threshold. If the last point is already very close
    // to the threshold, skip the duplicate.
    if (route.empty() ||
        std::hypot(route.back().x - af.threshold_position.x,
                   route.back().y - af.threshold_position.y) > 50.0) {
        route.push_back(af.threshold_position);
    }

    // The validate() in scenario.cpp requires taxi_route.size() >= 2. If
    // we ended up with a 1-point route (e.g. no parking + no follow-me +
    // already at threshold), pad with the runway_end so the route has a
    // threshold + far-end pair.
    if (route.size() < 2) {
        route.push_back(af.runway_end_position);
    }

    af.taxi_route = std::move(route);
    return af;
}

std::vector<f4::entities::EntityId>
spawn_aircraft_from_flights(f4::entities::EntityWorld& world,
                             const f4::world_convert::ClassTable& ct,
                             const f4::models::ModelDatabase& db,
                             const f4::data::AircraftConfig& cfg,
                             const ScenarioAirfield& airfield,
                             const ScenarioAircraft& scenario_aircraft) {
    using namespace f4::entities;
    using namespace f4::flight;
    using namespace f4::ai;
    using namespace f4::simulation;

    // Find every entity with a FlightPlanComponent. f4-world::populate_units
    // created these from the campaign's Flight units — each one represents
    // a single aircraft mission element.
    const auto flight_ids = world.with_component<FlightPlanComponent>();
    if (flight_ids.empty()) return {};

    std::vector<EntityId> spawned;
    spawned.reserve(flight_ids.size());

    // Per-flight lateral offset so multiple aircraft at the same airbase
    // don't overlap. 80 ft is roughly one wingspan + clearance. We spread
    // them along the airbase's east axis (perpendicular to the runway).
    constexpr double OFFSET_STEP_FT = 80.0;
    int flight_index = 0;

    for (const auto flight_id : flight_ids) {
        EntityHandle flight_h(flight_id, &world);
        const auto* fp = flight_h.get<FlightPlanComponent>();
        if (!fp) continue;

        // Resolve the flight's squadron → airbase objective → transform.
        // If any link in the chain is missing, fall back to the airfield's
        // threshold position (so the aircraft still spawns, just on the
        // runway — better than dropping it silently).
        f4::geo::WorldPosition parking_spot = airfield.threshold_position;
        const auto* sq = fp->squadron.value != 0
            ? EntityHandle(fp->squadron, &world).get<SquadronComponent>()
            : nullptr;
        if (sq && sq->airbase.value != 0) {
            auto* tf = EntityHandle(sq->airbase, &world).get<TransformComponent>();
            if (tf) {
                parking_spot = tf->position;
            }
        }

        // Apply the per-flight lateral offset. Alternate +x / -x so
        // successive flights park on opposite sides of the airbase center.
        const double offset = (flight_index % 2 == 0 ? 1.0 : -1.0)
                            * (static_cast<double>(flight_index / 2) + 1.0)
                            * OFFSET_STEP_FT;
        parking_spot.x += offset;

        // Look up the flight's squadron entity_type via its UnitCoreComponent
        // (the squadron is also a unit — it has its own UnitCoreComponent
        // with class_table_index). Resolve that entity_type → vis_type[0]
        // → ModelRecord.
        int16_t vis_type_index = 0;
        if (sq) {
            auto* sq_uc = EntityHandle(fp->squadron, &world).get<UnitCoreComponent>();
            if (sq_uc) {
                vis_type_index = ct.vis_type_for(
                    static_cast<uint16_t>(sq_uc->class_table_index), 0);
            }
        }
        // Fallback: if the CT lookup fails (or no squadron), use the
        // scenario's template vis_type_index. This is the F-16 default
        // for the kunsan scenarios.
        if (vis_type_index <= 0) {
            vis_type_index = scenario_aircraft.vis_type_index;
        }

        // Compose the aircraft entity: Transform + FM + VisualModel + Brain.
        auto h = world.create();

        // 1. TransformComponent — initial pose at the parking spot.
        auto& tf = h.add<TransformComponent>();
        tf.position = parking_spot;
        const double hdg = airfield.runway_heading_rad;
        const double h2 = hdg * 0.5;
        tf.qw = std::cos(h2);  tf.qx = 0.0;  tf.qy = 0.0;  tf.qz = std::sin(h2);

        // 2. FlightModelComponent — init from AircraftConfig, on ground.
        auto& fm = h.add<FlightModelComponent>();
        fm.init(cfg,
                /*alt_ft=*/parking_spot.z,
                /*vt_ftps=*/0.0,
                /*hdg_rad=*/hdg,
                /*inAir=*/false);
        fm.set_ground(parking_spot.z, f4::math::Vec3d{0.0, 0.0, -1.0});

        // 3. VisualModelComponent — the renderable handle.
        auto& vis = h.add<VisualModelComponent>();
        if (db.valid()) {
            vis.model_record = db.model(vis_type_index);
        }
        vis.active_lod = 0;
        f4::models::SwitchState gear_switch;
        gear_switch.switch_number = 10;
        gear_switch.active_child  = 0;  // 0 = gear down
        vis.model_state.switches.push_back(gear_switch);

        // 4. BrainComponent — wraps TakeoffModule.
        auto& brain = h.add<BrainComponent>();
        brain.module().rotate_speed_kts = 140.0;
        brain.module().gear_up_alt_ft = 200.0;
        brain.module().departure_alt_ft = airfield.departure_altitude_ft;
        brain.module().taxi_speed_kts = 15.0;

        spawned.push_back(h.id());
        ++flight_index;
    }

    return spawned;
}

} // namespace f4::simulation

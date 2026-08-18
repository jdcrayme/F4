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
#include "f4/simulation/frames.hpp"

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

namespace {

// Compass heading (rad) of a 2D ENU direction vector (0 = north, CW +).
double compass_heading(double dx, double dy) {
    return std::atan2(dx, dy);
}

double wrap_2pi(double a) {
    constexpr double TAU = 6.283185307179586476925;
    while (a < 0.0) a += TAU;
    while (a >= TAU) a -= TAU;
    return a;
}

double heading_diff_deg(double a, double b) {
    constexpr double D2R = 0.017453292519943295;
    constexpr double R2D = 57.29577951308232;
    const double d = std::abs(wrap_2pi(a * D2R) - wrap_2pi(b * D2R)) * R2D;
    return std::min(d, 360.0 - d);
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

    constexpr double DEG_TO_RAD = 0.017453292519943295;

    // Objective center -> ENU feet. The runway points are offsets from
    // this center.
    const auto obj_center = grid_to_enu(obj.x, obj.y, obj.z);

    // --- Real-PHD structure (verified against the Korea theater DB) -----
    //
    // A PLT_RUNWAY list mixes point kinds in a fixed order:
    //   [0] PT_RUNWAY      far-end marker for the list's heading
    //   [1] PT_TAKEOFF     takeoff position (~305 ft behind the painted
    //                       threshold; the reciprocal list's PT_RUNWAY
    //                       marker sits essentially on it)
    //   [2] PT_TAKE_RUNWAY hold-short / runway access point
    //   [3..] PT_TAXI      an ORDERED taxi polyline, stored RUNWAY->RAMP
    //
    // PLT_RUNWAY_DIM carries the runway rectangle as 4 corner points
    // (its own heading_deg is garbage — use the quad geometry).
    //
    // The old code assumed points were ordered [threshold .. rollout end]
    // — provably wrong on real data (it read the far-end marker as the
    // threshold and a mid-field taxi node as the runway end).
    const auto* chosen = runway_list;
    {
        // Pick the runway-direction list closest to the requested active
        // runway (id 02 <-> heading 020 deg). Falls back to the first.
        const double want = static_cast<double>(active_runway_id) * 10.0;
        const auto* best = runway_list;
        double best_d = 1e9;
        for (const auto& l : obj.ground_layout) {
            if (l.type != f4::world_convert::PLT_RUNWAY) continue;
            const double d = heading_diff_deg(l.heading_deg, want);
            if (d < best_d) { best_d = d; best = &l; }
        }
        chosen = best;
    }

    const f4::entities::GroundLayoutPoint* far_end = nullptr;
    const f4::entities::GroundLayoutPoint* takeoff = nullptr;
    const f4::entities::GroundLayoutPoint* access = nullptr;
    std::vector<const f4::entities::GroundLayoutPoint*> taxi;
    for (const auto& p : chosen->points) {
        switch (p.type) {
            case f4::world_convert::PT_RUNWAY:      if (!far_end) far_end = &p; break;
            case f4::world_convert::PT_TAKEOFF:     if (!takeoff) takeoff = &p; break;
            case f4::world_convert::PT_TAKE_RUNWAY: if (!access) access = &p; break;
            case f4::world_convert::PT_TAXI:        taxi.push_back(&p); break;
            default: break;
        }
    }

    af.runway_heading_rad = chosen->heading_deg * DEG_TO_RAD;
    {
        // "Rwy 02" from heading 020 deg.
        const int num = static_cast<int>(std::lround(chosen->heading_deg / 10.0)) % 36;
        af.active_runway_name = "Rwy " + std::string(num < 10 ? "0" : "") + std::to_string(num);
    }

    if (takeoff && far_end) {
        // Real structure: derive everything from the decoded points.
        const double hs = af.runway_heading_rad;
        const double hx = std::sin(hs), hy = std::cos(hs);

        // Threshold: nearest OTHER list's PT_RUNWAY marker (the reciprocal
        // end sits ~305 ft ahead of the takeoff position); fallback is a
        // 300-ft projection along the heading.
        f4::geo::WorldPosition threshold =
            add_offset(obj_center, takeoff->x + 300.0f * static_cast<float>(hx),
                                   takeoff->y + 300.0f * static_cast<float>(hy));
        double best_d = 1e9;
        for (const auto& l : obj.ground_layout) {
            if (l.type != f4::world_convert::PLT_RUNWAY || &l == chosen) continue;
            for (const auto& p : l.points) {
                if (p.type != f4::world_convert::PT_RUNWAY) continue;
                const double d = std::hypot(p.x - takeoff->x, p.y - takeoff->y);
                if (d < best_d && d < 1200.0) {
                    best_d = d;
                    threshold = add_offset(obj_center, p.x, p.y);
                }
            }
        }

        af.threshold_position = threshold;
        af.runway_end_position = add_offset(obj_center, far_end->x, far_end->y);
        af.threshold_altitude_ft = obj_center.z;
        af.departure_altitude_ft = af.threshold_altitude_ft + 2500.0;

        // Dimensions from the same runway_num's PLT_RUNWAY_DIM quad.
        for (const auto& l : obj.ground_layout) {
            if (l.type != f4::world_convert::PLT_RUNWAY_DIM) continue;
            if (l.runway_num != chosen->runway_num || l.points.size() != 4) continue;
            const double len0 = std::hypot(l.points[0].x - l.points[3].x,
                                           l.points[0].y - l.points[3].y);
            const double len1 = std::hypot(l.points[1].x - l.points[2].x,
                                           l.points[1].y - l.points[2].y);
            const double wid0 = std::hypot(l.points[0].x - l.points[1].x,
                                           l.points[0].y - l.points[1].y);
            const double wid1 = std::hypot(l.points[3].x - l.points[2].x,
                                           l.points[3].y - l.points[2].y);
            af.runway_length_ft = (len0 + len1) * 0.5;
            af.runway_width_ft  = (wid0 + wid1) * 0.5;
            break;
        }

        if (taxi.size() >= 2) {
            // Taxi polyline is stored runway->ramp; reverse for the
            // outbound (ramp->runway) route and end at the hold-short
            // takeoff position.
            std::vector<f4::geo::WorldPosition> route;
            route.reserve(taxi.size() + 2);
            for (auto it = taxi.rbegin(); it != taxi.rend(); ++it) {
                route.push_back(add_offset(obj_center, (*it)->x, (*it)->y));
            }
            if (access) {
                route.push_back(add_offset(obj_center, access->x, access->y));
            }
            route.push_back(add_offset(obj_center, takeoff->x, takeoff->y));
            af.taxi_route = std::move(route);

            // Taxi-in: runway side back to the ramp (stored order).
            std::vector<f4::geo::WorldPosition> in;
            in.reserve(taxi.size() + 2);
            in.push_back(add_offset(obj_center, takeoff->x, takeoff->y));
            if (access) {
                in.push_back(add_offset(obj_center, access->x, access->y));
            }
            for (const auto* p : taxi) {
                in.push_back(add_offset(obj_center, p->x, p->y));
            }
            af.taxi_in_route = std::move(in);

            // Parking: this Korea PD has no PLT_PARK lists anywhere —
            // synthesize spots at the ramp end of the polyline, offset
            // AWAY from the runway (the +cross side of the runway axis),
            // spaced along the ramp, facing back down the taxi-out
            // direction so the aircraft rolls straight out.
            const auto& t0 = *taxi[taxi.size() - 1];   // terminal (ramp end)
            const auto& t1 = *taxi[taxi.size() - 2];
            const double ddx = t0.x - t1.x, ddy = t0.y - t1.y;
            const double dlen = std::max(1.0, std::hypot(ddx, ddy));
            const double dx = ddx / dlen, dy = ddy / dlen;      // ramp->out dir
            // +cross side of the runway axis (away from the runway):
            const double side = hx * (t0.y - takeoff->y) - hy * (t0.x - takeoff->x);
            const double sgn = side >= 0.0 ? 1.0 : -1.0;
            const double px = -hy * sgn, py = hx * sgn;         // perp unit
            const double face = compass_heading(-dx, -dy);      // taxi-out heading
            constexpr int N_SPOTS = 8;
            constexpr double OFF = 90.0;      // perpendicular distance (ft)
            constexpr double SPACING = 80.0;  // along-ramp spacing (ft)
            for (int i = 0; i < N_SPOTS; ++i) {
                ScenarioParkingSpot spot;
                spot.position = f4::geo::WorldPosition(
                    obj_center.x + t0.x + px * OFF - dx * (i * SPACING),
                    obj_center.y + t0.y + py * OFF - dy * (i * SPACING),
                    obj_center.z);
                spot.heading_rad = face;
                af.parking_spots.push_back(spot);
            }
        } else {
            // Real runway but no taxi polyline: straight from threshold.
            af.taxi_route = {af.threshold_position, af.runway_end_position};
        }
        return af;
    }

    // --- Fallback: legacy synthetic shape (tests, hand-built layouts) ---
    // Lists whose points are plain [threshold, far end] PT_RUNWAY pairs
    // plus optional PLT_PARK / PLT_FOLLOW_ME lists.
    af.active_runway_name = "Rwy " + std::to_string(active_runway_id);
    af.threshold_position = add_offset(obj_center,
                                        chosen->points.front().x,
                                        chosen->points.front().y);
    af.runway_end_position = add_offset(obj_center,
                                         chosen->points.back().x,
                                         chosen->points.back().y);
    af.threshold_altitude_ft = obj_center.z;
    af.departure_altitude_ft = af.threshold_altitude_ft + 2500.0;

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
    if (route.empty() ||
        std::hypot(route.back().x - af.threshold_position.x,
                   route.back().y - af.threshold_position.y) > 50.0) {
        route.push_back(af.threshold_position);
    }
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
        // Compass heading -> ENU quaternion (negative about +z; see frames.hpp).
        const auto q0 = f4::simulation::enu_quat_from_compass(hdg);
        tf.qw = q0.w;  tf.qx = q0.x;  tf.qy = q0.y;  tf.qz = q0.z;

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

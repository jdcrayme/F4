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

#include <f4/simulation/campaign_origin.hpp>

#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/flight/angle.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/weapons/bomb.hpp>
#include <f4/world_convert/objective_decoder.hpp>  // ObjectiveType::TYPE_AIRBASE
#include <f4/world_convert/theater_data.hpp>        // PointListType
#include <f4/math/vec3.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>

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

// ---------------------------------------------------------------------------
// B.3+ synthetic airfield for layout-less airfield objectives.
//
// A .cam save embeds ground layouts (runway/taxi points) only for
// Airstrip-class objectives. The 50 real Airbases (Kunsan, Osan, ...)
// carry their runway geometry in the THEATER's static data, which the
// current world JSON does not include — TestCamp.cam decode leaves all
// 50 with an empty ground_layout vector. Without this fallback,
// derive_airfield_from_objective() rejects them, so:
//   - spawn_from_campaign_flights registers NO per-airbase airfield for
//     them, every TaxiRequest falls back to the DEFAULT (first) airfield,
//     and each aircraft taxis ACROSS THE THEATER toward it (observed:
//     5 minutes of 32 fps straight-line taxi, zero takeoffs — the B.3
//     QC harness caught exactly this).
//
// The synthetic field is centered on the objective with a 7000 ft runway
// along the requested active runway heading. It is NOT the real runway
// geometry — the exact threshold/parking positions are approximate. What
// it guarantees is LOCAL ground ops: park -> short taxi -> depart -> fly
// the saved campaign route. When a later tranche loads theater airbase
// layout data (Falcon4.OCD/PHD chain), the real points replace this path
// automatically (the layout-present branch above runs first).
// ---------------------------------------------------------------------------
ScenarioAirfield synthesize_airfield_for_objective(
    const f4::world::ObjectiveState& obj, int active_runway_id) {
    ScenarioAirfield af;
    af.active_runway_id = active_runway_id;
    af.active_runway_name = "Rwy " + std::to_string(active_runway_id);

    constexpr double DEG_TO_RAD = 0.017453292519943295;
    // Runway heading from the requested active runway (id 36 -> 360 deg).
    double hdg_deg = static_cast<double>(active_runway_id) * 10.0;
    if (hdg_deg >= 360.0) hdg_deg -= 360.0;
    af.runway_heading_rad = hdg_deg * DEG_TO_RAD;

    const auto center = grid_to_enu(obj.x, obj.y, obj.z);
    const double hs = af.runway_heading_rad;
    const double hx = std::sin(hs), hy = std::cos(hs);   // along-runway unit

    // 7000 ft runway straddling the objective center; threshold 3500 ft
    // back (toward the approach end).
    constexpr double HALF_LEN_FT = 3500.0;
    af.threshold_position = f4::geo::WorldPosition(
        center.x - hx * HALF_LEN_FT, center.y - hy * HALF_LEN_FT, center.z);
    af.runway_end_position = f4::geo::WorldPosition(
        center.x + hx * HALF_LEN_FT, center.y + hy * HALF_LEN_FT, center.z);
    af.runway_length_ft = 2.0 * HALF_LEN_FT;
    af.runway_width_ft = 150.0;
    af.threshold_altitude_ft = center.z;
    af.departure_altitude_ft = center.z + 2500.0;

    // Taxi route: a ramp point 600 ft before the threshold, offset 300 ft
    // to the right of the centerline, then the threshold itself (hold
    // short). Two waypoints — enough for the TakeoffModule's steer/capture
    // loop; campaign flights park at the objective center + lateral
    // offset, so the first leg is a few hundred feet.
    const double px = hy, py = -hx;                       // right-of-course unit
    af.taxi_route = {
        f4::geo::WorldPosition(
            af.threshold_position.x + hx * 600.0 + px * 300.0,
            af.threshold_position.y + hy * 600.0 + py * 300.0,
            center.z),
        af.threshold_position,
    };
    af.taxi_in_route = af.taxi_route;

    // Parking: a row of 8 spots offset from the center, perpendicular to
    // the runway — same shape the decoded-layout path synthesizes.
    constexpr int N_SPOTS = 8;
    constexpr double OFF_FT = 300.0;       // perpendicular distance (right side)
    constexpr double SPACING_FT = 80.0;    // along-runway spacing
    for (int i = 0; i < N_SPOTS; ++i) {
        ScenarioParkingSpot spot;
        spot.position = f4::geo::WorldPosition(
            center.x + px * OFF_FT - hx * (i * SPACING_FT),
            center.y + py * OFF_FT - hy * (i * SPACING_FT),
            center.z);
        spot.heading_rad = std::atan2(-hx, -hy);   // face back down the runway
        af.parking_spots.push_back(spot);
    }
    return af;
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
    //
    // B.3+ exception: airfield-CLASS objectives with no decoded layout
    // (all 50 TestCamp Airbases — the save never embeds their geometry)
    // get a SYNTHETIC local field instead of rejection. Rejection fed
    // every aircraft at those bases a taxi route to the first DERIVABLE
    // airbase — cross-theater taxi, zero takeoffs (B.3 QC catch).
    if (obj.ground_layout.empty()) {
        if (obj.objective_type == f4::world_convert::TYPE_AIRBASE ||
            obj.objective_type == f4::world_convert::TYPE_AIRSTRIP) {
            return synthesize_airfield_for_objective(obj, active_runway_id);
        }
        return std::nullopt;
    }

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

// ============================================================================
// B.3+ per-airbase airfields
// ============================================================================
std::uint32_t airbase_vu_id(const f4::entities::EntityWorld& world,
                            f4::entities::EntityId airbase_entity) {
    if (!airbase_entity.valid()) return 0;
    // const_cast: EntityHandle needs a mutable world for lookup, but we
    // only read components — the entity system has no const-handle view.
    auto& mut_world = const_cast<f4::entities::EntityWorld&>(world);
    f4::entities::EntityHandle h(airbase_entity, &mut_world);
    const auto* pb = h.get<f4::entities::PropertyBag>();
    if (!pb) return 0;
    const auto it = pb->ints.find("vu_id_num");
    return it == pb->ints.end() ? 0
        : static_cast<std::uint32_t>(it->second);
}

/// Any unit/objective entity's VU_ID.num from its PropertyBag residue
/// (the C1 origin stamp's generic lookup — airbase_vu_id's rule,
/// un-specialized).
std::uint32_t entity_vu_id(const f4::entities::EntityWorld& world,
                           f4::entities::EntityId entity) {
    return airbase_vu_id(world, entity);
}

std::optional<f4::entities::EntityId>
spawn_aircraft_for_flight(f4::entities::EntityWorld& world,
                          f4::entities::EntityId flight_entity,
                          const f4::world_convert::ClassTable& ct,
                          const f4::models::ModelDatabase& db,
                          const f4::data::AircraftConfig& cfg,
                          const ScenarioAirfield& airfield,
                          const ScenarioAircraft& scenario_aircraft,
                          int parking_slot,
                          const AirbaseAirfieldMap* airbase_airfields,
                          const std::unordered_map<std::uint32_t,
                              f4::entities::EntityId>* objective_id_map,
                          const weapons::WeaponClassTable* weapon_table) {
    using namespace f4::entities;
    using namespace f4::flight;
    using namespace f4::ai;
    using namespace f4::simulation;

    EntityHandle flight_h(flight_entity, &world);
    const auto* fp = flight_h.get<FlightPlanComponent>();
    if (!fp) return std::nullopt;

    // Resolve the flight's squadron → airbase objective → transform.
    // If any link in the chain is missing, fall back to the airfield's
    // threshold position (so the aircraft still spawns, just on the
    // runway — better than dropping it silently).
    //
    // B.3+ non-airfield-base guard: the squadron's base objective may NOT
    // be an airfield at all (TestCamp's army-aviation flights sit at Army
    // Base objectives — objective_type 3). Parking there with no runway
    // hands the takeoff AI a TaxiRequest that resolves to the DEFAULT
    // airfield, and the aircraft taxis cross-theater to it (observed in
    // the B.3 QC run: one of 12 flights). When the base resolves no
    // ScenarioAirfield, park at the caller's fallback airfield instead —
    // synthetic start, but the flight still departs locally and flies its
    // SAVED campaign route, which is what the QC asserts on.
    f4::geo::WorldPosition parking_spot = airfield.threshold_position;
    std::uint64_t home_airbase_vu = 0;
    const ScenarioAirfield* base_af = nullptr;
    const auto* sq = fp->squadron.value != 0
        ? EntityHandle(fp->squadron, &world).get<SquadronComponent>()
        : nullptr;
    if (sq && sq->airbase.value != 0) {
        // B.3+: the flight's HOME base — resolve its airfield data so the
        // spawn pose, departure altitude and (downstream) the ATC
        // clearances all reference THIS field, not the first airbase
        // objective in the save.
        home_airbase_vu = airbase_vu_id(world, sq->airbase);
        if (airbase_airfields && home_airbase_vu != 0) {
            const auto it = airbase_airfields->find(
                static_cast<std::uint32_t>(home_airbase_vu));
            if (it != airbase_airfields->end()) {
                base_af = &it->second;
            }
        }
        // Park at the base objective's position only when it is a real
        // airfield (the map resolved one). Without the map the caller
        // keeps the legacy behavior: park at the base objective anyway
        // (single-airfield worlds/tests where the caller's airfield IS
        // the base's airfield).
        auto* tf = EntityHandle(sq->airbase, &world).get<TransformComponent>();
        if (tf && (base_af || !airbase_airfields)) {
            parking_spot = tf->position;
        }
    }
    // The airfield driving heading/departure: the per-base one when
    // resolved, else the fallback the caller passed (scenario airfield).
    const ScenarioAirfield& field = base_af ? *base_af : airfield;

    // Apply the per-flight lateral offset. Alternate +x / -x so
    // successive flights park on opposite sides of the airbase center.
    // 80 ft is roughly one wingspan + clearance, spread along the
    // airbase's east axis (perpendicular to the runway).
    constexpr double OFFSET_STEP_FT = 80.0;
    const double offset = (parking_slot % 2 == 0 ? 1.0 : -1.0)
                        * (static_cast<double>(parking_slot / 2) + 1.0)
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
    const double hdg = field.runway_heading_rad;
    // Compass heading -> ENU quaternion (negative about +z; see frames.hpp).
    const auto q0 = f4::simulation::enu_quat_from_compass(hdg);
    tf.qw = q0.w;  tf.qx = q0.x;  tf.qy = q0.y;  tf.qz = q0.z;

    // 2. FlightModelComponent — init from AircraftConfig, on ground. B.3
    //    fix: pass the parking position's north/east into the FM — the
    //    scenario-list path always did (simulation.cpp:238-244), but the
    //    campaign path left the FM at (0,0) while the TransformComponent
    //    held the real spot. The first tick's FM→Transform sync then
    //    teleported every campaign aircraft to the theater datum, and
    //    they all taxi'd from there in one stacked column.
    auto& fm = h.add<FlightModelComponent>();
    fm.init(cfg,
            /*alt_ft=*/parking_spot.z,
            /*vt_ftps=*/0.0,
            /*hdg_rad=*/hdg,
            /*inAir=*/false,
            /*north_ft=*/parking_spot.y,
            /*east_ft=*/parking_spot.x);
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

    // 4. BrainComponent — wraps TakeoffModule. B.3: when the flight
    // carries a saved waypoint plan, ALSO attach the derived MissionPlan
    // — the aircraft taxis, departs, and flies its campaign route.
    auto& brain = h.add<BrainComponent>();
    brain.module().rotate_speed_kts = 140.0;
    brain.module().gear_up_alt_ft = 200.0;
    brain.module().departure_alt_ft = field.departure_altitude_ft;
    brain.module().taxi_speed_kts = 15.0;
    // B.3+: tag the brain's TakeoffModule with the home airbase so its
    // TaxiRequest/TakeoffRequest carry it — the multi-airbase ATC answers
    // from the registered per-base airfield.
    brain.module().airbase_id = home_airbase_vu;
    if (auto plan = build_mission_plan_from_flight(world, flight_entity,
                                                    objective_id_map)) {
        brain.set_mission_plan(std::move(*plan));
    }

    // 4b. A-G employment tranche: the flight's decoded loadout becomes a
    //     WeaponStoreComponent (wire-faithful bookkeeping + mapped
    //     droppable bombs), and delivery-mission flights get the brain's
    //     strike fire control armed. Null table = hosts that don't want
    //     ordnance (single-purpose route tests) spawn unarmed, exactly as
    //     before this tranche.
    if (weapon_table != nullptr) {
        (void)arm_flight_strike(*weapon_table, h, fp->loadout_stations,
                                fp->mission);
    }

    // 5. TEAM tag — the campaign owner slot mapped into the sim's
    // blue/red/green vocabulary (B.3). Read the flight's owner from
    // its TEAM tag (set by world_loader as the int owner slot).
    {
        const auto team_tag = flight_h.get_tag(tags::TEAM);
        const uint8_t owner = (team_tag && team_tag->as_int())
            ? static_cast<uint8_t>(*team_tag->as_int()) : 0;
        h.set_tag(tags::TEAM,
                  TagValue::from(owner_team_string(world, owner)));

        // 6. C1 origin stamp — the campaign identity this aircraft
        //    flies under, as DATA. The result sink reads it to
        //    attribute kills (EntityId -> which squadron lost / which
        //    one scored). All three VUs come from the entities' own
        //    PropertyBag residue; the team slot is the flight's owner
        //    (the CAMPAIGN vocabulary — the sim's blue/red/green string
        //    above is a different thing entirely).
        auto& origin = h.add<CampaignOriginComponent>();
        origin.flight_vu = entity_vu_id(world, flight_entity);
        origin.squadron_vu = (fp->squadron.value != 0)
            ? entity_vu_id(world, fp->squadron) : 0;
        origin.home_airbase_vu = home_airbase_vu;
        origin.team_slot = owner;
        origin.callsign_id = fp->callsign_id;
        origin.callsign_num = fp->callsign_num;
    }

    return h.id();
}

std::vector<f4::entities::EntityId>
spawn_aircraft_from_flights(f4::entities::EntityWorld& world,
                             const f4::world_convert::ClassTable& ct,
                             const f4::models::ModelDatabase& db,
                             const f4::data::AircraftConfig& cfg,
                             const ScenarioAirfield& airfield,
                             const ScenarioAircraft& scenario_aircraft,
                             const FlightSpawnFilter& filter,
                             const AirbaseAirfieldMap* airbase_airfields,
                             const std::unordered_map<std::uint32_t,
                                 f4::entities::EntityId>* objective_id_map,
                             const weapons::WeaponClassTable* weapon_table) {
    using namespace f4::entities;

    // Find every entity with a FlightPlanComponent. f4-world::populate_units
    // created these from the campaign's Flight units — each one represents
    // a single aircraft mission element.
    const auto flight_ids = world.with_component<FlightPlanComponent>();
    if (flight_ids.empty()) return {};

    std::vector<EntityId> spawned;

    // B.3: per-airbase parking counter. The pre-B.3 code used one global
    // index, which put flight #400 at 16,000 ft (3 miles) from its field —
    // on a real save (TestCamp: 449 flights over ~40 fields) that produced
    // a satellite ring of aircraft around every busy airbase. Counting per
    // airbase keeps each field's ramp compact.
    std::unordered_map<uint64_t, int> per_airbase_index;

    for (const auto flight_id : flight_ids) {
        EntityHandle flight_h(flight_id, &world);
        const auto* fp = flight_h.get<FlightPlanComponent>();
        if (!fp) continue;

        // --- B.3 flight filter ---
        if (filter.team >= 0) {
            const auto team_tag = flight_h.get_tag(tags::TEAM);
            const uint8_t owner = (team_tag && team_tag->as_int())
                ? static_cast<uint8_t>(*team_tag->as_int()) : 0;
            if (static_cast<int>(owner) != filter.team) continue;
        }
        if (filter.mission >= 0 &&
            static_cast<int>(fp->mission) != filter.mission) {
            continue;
        }
        if (filter.max_flights > 0 &&
            static_cast<int>(spawned.size()) >= filter.max_flights) {
            break;
        }

        // Per-airbase parking slot for this flight.
        EntityId airbase_id;  // key: 0 = the shared fallback airfield
        if (fp->squadron.value != 0) {
            auto* sq = EntityHandle(fp->squadron, &world).get<SquadronComponent>();
            if (sq && sq->airbase.value != 0) {
                airbase_id = sq->airbase;
            }
        }
        const int slot = per_airbase_index[airbase_id.value]++;

        if (auto spawned_id = spawn_aircraft_for_flight(
                world, flight_id, ct, db, cfg, airfield,
                scenario_aircraft, slot, airbase_airfields,
                objective_id_map, weapon_table)) {
            spawned.push_back(*spawned_id);
        }
    }

    return spawned;
}

// ============================================================================
// A-G employment tranche — campaign weapon map + strike arming
// ============================================================================

std::uint32_t campaign_weapon_handle(const weapons::WeaponClassTable& table,
                                     std::uint16_t wire_id) {
    for (const auto& e : kCampaignWeaponMap) {
        if (e.wire_id == wire_id) {
            return table.find_by_name(e.engine_name);
        }
    }
    return weapons::kInvalidWeapon;
}

std::string campaign_weapon_label(const weapons::WeaponClassTable& table,
                                  std::uint16_t wire_id) {
    const auto handle = campaign_weapon_handle(table, wire_id);
    if (handle != weapons::kInvalidWeapon) {
        if (const auto* rec = table.get(handle)) return rec->name;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "WPN-%u",
                  static_cast<unsigned>(wire_id));
    return std::string(buf);
}

namespace {

/// Doctrine MK-82 fill: 2 stations x `kDoctrineRoundsPerStation` bombs.
/// Mirrors FreeFalcon's LoadWeapons squadron-stores fallback (a strike
/// flight with unmappable wire stores still delivers ordnance); the QC
/// summary separates the doctrine count from the wire count.
constexpr int kDoctrineStations = 2;
constexpr int kDoctrineRoundsPerStation = 2;
/// Cap on the release stick (the StrikeModule's salvo_max) — a QC run
/// wants a representative stick, not the whole wing's worth of iron.
constexpr int kDoctrineSalvoMax = 4;

/// Compute the vacuum->dragged range scale for the trigger from the bomb
/// card's own ballistics: fly one release at the doctrine delivery point
/// (5,000 ft AGL, 675 fps — ~400 kts) with and without drag, ratio them.
/// One number per weapon class, deterministic, and it tracks the SAME ODE
/// the bomb entity flies (bomb.cpp), so the trigger and the flyout agree
/// on where the bomb lands.
double bomb_drag_factor_for(const weapons::WeaponClassTable& table,
                            std::uint32_t bomb_handle) {
    const auto* rec = table.get(bomb_handle);
    if (rec == nullptr) return 0.85;   // safe default

    constexpr double kAltFt = 5000.0;
    constexpr double kSpeedFps = 675.0;
    constexpr double kDt = 0.1;        // coarse: the ratio is smooth

    weapons::Bomb dragged;
    dragged.release(weapons::BombConfig::from_record(*rec),
                    f4::geo::WorldPosition{0.0, 0.0, kAltFt},
                    f4::math::Vec3<double>{kSpeedFps, 0.0, 0.0}, 0.0);
    weapons::BombConfig vac = weapons::BombConfig::from_record(*rec);
    vac.cd = 0.0;
    vac.ref_area_ft2 = 0.0;
    weapons::Bomb vacuum;
    vacuum.release(vac,
                    f4::geo::WorldPosition{0.0, 0.0, kAltFt},
                    f4::math::Vec3<double>{kSpeedFps, 0.0, 0.0}, 0.0);
    auto fly = [](weapons::Bomb& b) {
        double t = 0.0;
        while (!b.terminal() && t < 120.0) { b.tick(kDt); t += kDt; }
    };
    fly(dragged);
    fly(vacuum);
    if (dragged.status() != weapons::BombStatus::Impact ||
        vacuum.status() != weapons::BombStatus::Impact ||
        vacuum.ground_range_ft() < 1.0) {
        return 0.85;
    }
    const double factor = dragged.ground_range_ft() /
                          vacuum.ground_range_ft();
    // Guard pathological cards (drag > vacuum never happens, but a card
    // with silly fields could invert the ratio).
    return std::clamp(factor, 0.3, 1.0);
}

} // namespace

StrikeArmament arm_flight_strike(
    const weapons::WeaponClassTable& table,
    f4::entities::EntityHandle& aircraft,
    const std::vector<f4::entities::LoadoutStationState>& loadout_stations,
    std::uint8_t mission_byte) {
    using namespace f4::entities;
    using f4::campaign::MissionCategory;

    StrikeArmament out;

    // Only delivery-category missions arm the strike trigger. The store
    // is attached regardless (bookkeeping for every flight — QC renders
    // what the flight carries).
    const auto category = f4::campaign::mission_category(mission_byte);
    const bool delivery_mission =
        category == MissionCategory::Strike ||
        category == MissionCategory::SEAD ||
        category == MissionCategory::CAS;

    // --- The store from the decoded wire loadout ---------------------------
    auto& store = aircraft.add<weapons::WeaponStoreComponent>();
    std::uint32_t droppable_handle = weapons::kInvalidWeapon;
    for (const auto& st : loadout_stations) {
        const auto handle = campaign_weapon_handle(table, st.weapon_id);
        const auto* rec = table.get(handle);
        if (rec != nullptr &&
            rec->category == weapons::WeaponCategory::Bomb) {
            // Mapped droppable station.
            store.add_station(handle, st.count,
                              campaign_weapon_label(table, st.weapon_id));
            ++out.wire_stations;
            ++out.droppable_stations;
            out.droppable_rounds += st.count;
            if (droppable_handle == weapons::kInvalidWeapon) {
                droppable_handle = handle;
            }
        } else {
            // Wire-faithful bookkeeping station: droppable nothing, but
            // the QC surface can render what the flight carried.
            store.add_station(handle, st.count,
                              campaign_weapon_label(table, st.weapon_id));
            ++out.wire_stations;
        }
    }

    // --- Doctrine fill for delivery flights with no droppable wire bombs --
    if (delivery_mission && out.droppable_stations == 0) {
        const auto mk82 = table.find_by_name("MK-82");
        if (mk82 != weapons::kInvalidWeapon) {
            for (int i = 0; i < kDoctrineStations; ++i) {
                store.add_station(mk82, kDoctrineRoundsPerStation,
                                  "MK-82");
                ++out.doctrine_stations;
                ++out.droppable_stations;
                out.droppable_rounds += kDoctrineRoundsPerStation;
            }
            droppable_handle = mk82;
        }
    }

    // --- The strike fire control --------------------------------------------
    if (delivery_mission && droppable_handle != weapons::kInvalidWeapon &&
        aircraft.has<f4::ai::BrainComponent>()) {
        auto& brain = *aircraft.get<f4::ai::BrainComponent>();
        auto& strike = brain.strike();
        strike.config.drag_factor =
            bomb_drag_factor_for(table, droppable_handle);
        strike.config.salvo_max = std::min(out.droppable_rounds,
                                           kDoctrineSalvoMax);
        if (const auto* rec = table.get(droppable_handle)) {
            // CCIP tolerance from the weapon's lethal radius (half — the
            // pipper must sit well inside the blast footprint to start
            // the stick; the committed stick then walks across the rest).
            strike.config.impact_tolerance_ft =
                std::max(50.0, 0.5 * rec->lethal_radius_ft);
        }
        // A/G releases ride the intent surface regardless of the A/A
        // combat flag (the brain's strike rung runs un-gated; the host
        // driver's bomb path is likewise independent). combat_enabled
        // stays OFF for campaign flights — the A/A rungs (omniscient-GCI
        // sensor fusion, BVR maneuvering) would break route-following.
        out.strike_armed = true;
    }
    return out;
}

// ============================================================================
// B.3 tranche — route building + team string mapping
// ============================================================================

namespace {

/// FreeFalcon WP_ACTION names (campwp.h) — used for waypoint display names
/// in traces and QC summaries. Values > 26 render "ACTION".
const char* wp_action_text(std::uint8_t action) {
    switch (action) {
        case 0:  return "NOTHING";
        case 1:  return "TAKEOFF";
        case 2:  return "ASSEMBLE";
        case 3:  return "POSTASSEMBLE";
        case 4:  return "REFUEL";
        case 5:  return "REARM";
        case 6:  return "PICKUP";
        case 7:  return "LAND";
        case 8:  return "TIMING";
        case 9:  return "CASCP";
        case 10: return "ESCORT";
        case 11: return "CA";
        case 12: return "CAP";
        case 13: return "INTERCEPT";
        case 14: return "GNDSTRIKE";
        case 15: return "NAVSTRIKE";
        case 16: return "SAD";
        case 17: return "STRIKE";
        case 18: return "BOMB";
        case 19: return "SEAD";
        case 20: return "ELINT";
        case 21: return "RECON";
        case 22: return "RESCUE";
        case 23: return "ASW";
        case 24: return "TANKER";
        case 25: return "AIRDROP";
        case 26: return "JAM";
        default: return "ACTION";
    }
}

/// Default cruise CAS for derived route legs. The saved plan carries no
/// per-leg speeds; 400 kts is a representative cruise value for the
/// fighter-types that dominate a campaign's air tasking. Per-action
/// speed profiles arrive with the M4.5 route tranche.
constexpr double kDefaultLegSpeedKts = 400.0;

/// Altitude floor for derived route legs. Ramp/taxi legs store z = 0;
/// the NavigationModule would command a descent into terrain. 500 ft is
/// above pattern but below terrain features in most theaters.
constexpr double kMinWaypointAltFt = 500.0;

/// Altitude floor for A-G delivery waypoints (see the route builder).
constexpr double kMinDeliveryWaypointAltFt = 1500.0;

/// Waypoint count where a route stops being "the flight's real plan" and
/// starts being noise: single-waypoint plans (just a takeoff point, the
/// shape every untasked flight carries in some saves) provide no enroute
/// legs, so they're not worth attaching.
constexpr std::size_t kMinUsableWaypoints = 2;

} // namespace

std::optional<f4::ai::MissionPlan>
build_mission_plan_from_flight(
    const f4::entities::EntityWorld& world,
    f4::entities::EntityId flight_entity,
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        objective_id_map) {
    using namespace f4::entities;
    using f4::ai::modules::NavigationModule;

    EntityHandle h(flight_entity, const_cast<EntityWorld*>(&world));
    const auto* wp = h.get<WaypointPlanComponent>();
    if (!wp || wp->waypoints.size() < kMinUsableWaypoints) return std::nullopt;

    f4::ai::MissionPlan plan;

    // Route: skip leading WP_TAKEOFF waypoints (action 1) — the
    // TakeoffModule owns departure from the airbase; the NavigationModule
    // receives the route it should fly AFTER wheels-up. Everything else
    // (strike points, CAP anchors, the terminal WP_LAND back at home
    // plate) is kept in wire order.
    std::size_t first = 0;
    while (first < wp->waypoints.size() &&
           wp->waypoints[first].action == 1 /* WP_TAKEOFF */) {
        ++first;
    }
    if (wp->waypoints.size() - first < kMinUsableWaypoints - 1) {
        // Nothing but a takeoff point (or takeoff + single point that is
        // itself the ramp) — no usable enroute legs.
        return std::nullopt;
    }

    // A-G tranche: the flight's own resolved target — the fallback for
    // delivery waypoints whose own target_num resolves nothing (the save
    // stores the same objective on both the flight and the waypoint).
    const auto* fp = h.get<FlightPlanComponent>();
    const std::uint64_t flight_target =
        (fp != nullptr && fp->target.valid()) ? fp->target.value : 0;

    plan.route.reserve(wp->waypoints.size() - first);
    int n = 1;
    for (std::size_t i = first; i < wp->waypoints.size(); ++i) {
        const auto& w = wp->waypoints[i];
        auto pos = grid_to_enu(w.x, w.y, w.z);
        // A-G tranche: delivery waypoints floor HIGHER (1,500 ft) than
        // ordinary legs — the release envelope grows with altitude above
        // the target, and a 500-ft delivery floor would hand the trigger
        // a knife-edge dz right at the module's min-release gate. The
        // saved INTSTRIKE routes carry z=10 at the strike point (the
        // campaign stores the low-level ingress number, not a release
        // altitude); the M4.5 per-action profile tranche replaces this
        // floor with the mission profile's altitude bands.
        const double floor_ft =
            f4::ai::modules::is_ag_delivery_action(w.action)
                ? kMinDeliveryWaypointAltFt
                : kMinWaypointAltFt;
        pos.z = std::max(static_cast<double>(pos.z), floor_ft);
        char name[32];
        std::snprintf(name, sizeof(name), "WP%d:%s", n++,
                      wp_action_text(w.action));

        // A-G tranche: delivery-action waypoints carry their strike
        // target's EntityId::value (the brain's StrikeModule arms on
        // this). Resolution: the waypoint's own target_num through the
        // objective map, else the flight's resolved target.
        std::uint64_t target_id = 0;
        if (f4::ai::modules::is_ag_delivery_action(w.action)) {
            if (objective_id_map != nullptr && w.target_num != 0) {
                const auto it = objective_id_map->find(w.target_num);
                if (it != objective_id_map->end() && it->second.valid()) {
                    target_id = it->second.value;
                }
            }
            if (target_id == 0) {
                target_id = flight_target;
            }
        }

        NavigationModule::Waypoint route_wp{name, pos,
                                            kDefaultLegSpeedKts};
        route_wp.action = w.action;
        route_wp.target_id = target_id;
        plan.route.push_back(std::move(route_wp));
    }

    // start_phase stays Ground: the campaign flight departs from its
    // airbase (taxi → takeoff → enroute), which is exactly the brain's
    // default sequencing.
    return plan;
}

// ============================================================================
// C3 route tranche — synthetic-intent missions: route → MissionPlan,
// and the aircraft that flies it
// ============================================================================

std::optional<f4::ai::MissionPlan>
build_mission_plan_from_route(
        const std::vector<f4::campaign::RouteWaypoint>& route,
        std::uint32_t target_objective_vu,
        const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
            objective_id_map) {
    using namespace f4::entities;
    using f4::ai::modules::NavigationModule;

    if (route.size() < 2) return std::nullopt;
    f4::ai::MissionPlan plan;

    // The route's own takeoff waypoint is dropped (the TakeoffModule
    // owns departure — the same rule the saved-flight path applies);
    // everything after it (ingress corners, IP, target, turn point,
    // egress, the terminal WP_LAND) becomes the brain's route.
    std::size_t first = 0;
    while (first < route.size() &&
           route[first].action == 1 /* WP_TAKEOFF */) {
        ++first;
    }
    if (route.size() - first < 1) return std::nullopt;

    // The strike target: the intent's target objective resolved through
    // the id map (the fallback for delivery waypoints that carry no
    // target_num of their own — the builder stamps them, but a stale
    // map deserves the flight-level fallback too).
    std::uint64_t flight_target = 0;
    if (objective_id_map != nullptr && target_objective_vu != 0) {
        const auto it = objective_id_map->find(target_objective_vu);
        if (it != objective_id_map->end() && it->second.valid()) {
            flight_target = it->second.value;
        }
    }

    plan.route.reserve(route.size() - first);
    int n = 1;
    for (std::size_t i = first; i < route.size(); ++i) {
        const auto& w = route[i];
        auto pos = grid_to_enu(w.x, w.y, static_cast<float>(w.altitude_ft));
        const double floor_ft =
            f4::ai::modules::is_ag_delivery_action(w.action)
                ? kMinDeliveryWaypointAltFt
                : kMinWaypointAltFt;
        pos.z = std::max(pos.z, floor_ft);
        char name[32];
        std::snprintf(name, sizeof(name), "WP%d:%s", n++,
                      wp_action_text(w.action));

        std::uint64_t target_id = 0;
        if (f4::ai::modules::is_ag_delivery_action(w.action)) {
            if (objective_id_map != nullptr && w.target_num != 0) {
                const auto it = objective_id_map->find(w.target_num);
                if (it != objective_id_map->end() && it->second.valid()) {
                    target_id = it->second.value;
                }
            }
            if (target_id == 0) target_id = flight_target;
        }

        NavigationModule::Waypoint route_wp{name, pos, kDefaultLegSpeedKts};
        route_wp.action = w.action;
        route_wp.target_id = target_id;
        plan.route.push_back(std::move(route_wp));
    }

    return plan;
}

std::optional<f4::entities::EntityId>
spawn_aircraft_for_intent(
        f4::entities::EntityWorld& world,
        const f4::campaign::MissionIntent& intent,
        const std::unordered_map<std::uint32_t, f4::entities::EntityId>&
            unit_id_map,
        const f4::world_convert::ClassTable& ct,
        const f4::models::ModelDatabase& db,
        const f4::data::AircraftConfig& cfg,
        const ScenarioAirfield& airfield,
        const ScenarioAircraft& scenario_aircraft,
        int parking_slot,
        const AirbaseAirfieldMap* airbase_airfields,
        const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
            objective_id_map,
        const weapons::WeaponClassTable* weapon_table) {
    using namespace f4::entities;
    using namespace f4::flight;
    using namespace f4::ai;
    using namespace f4::simulation;

    if (intent.route.empty()) return std::nullopt;

    // Resolve the intent's squadron entity (the roster the campaign
    // drew from — a real unit in the populated world, keyed by VU num).
    const auto sq_it = unit_id_map.find(intent.squadron_id);
    EntityId squadron_entity;
    const SquadronComponent* sq = nullptr;
    if (sq_it != unit_id_map.end() && sq_it->second.valid()) {
        squadron_entity = sq_it->second;
        sq = EntityHandle(squadron_entity, &world).get<SquadronComponent>();
    }

    // Parking spot: the squadron's airbase objective when resolvable
    // (position + per-base airfield data), else the route's takeoff
    // waypoint (its grid IS the airbase the planner launched from),
    // else the caller's fallback threshold. Same shape and the same
    // fallback ladder as the saved-flight spawn path.
    f4::geo::WorldPosition parking_spot = airfield.threshold_position;
    std::uint64_t home_airbase_vu = 0;
    const ScenarioAirfield* base_af = nullptr;
    EntityId airbase_entity;
    if (sq && sq->airbase.value != 0) {
        airbase_entity = sq->airbase;
        home_airbase_vu = airbase_vu_id(world, sq->airbase);
        if (airbase_airfields && home_airbase_vu != 0) {
            const auto it = airbase_airfields->find(
                static_cast<std::uint32_t>(home_airbase_vu));
            if (it != airbase_airfields->end()) base_af = &it->second;
        }
        auto* tf = EntityHandle(sq->airbase, &world).get<TransformComponent>();
        if (tf && (base_af || !airbase_airfields)) {
            parking_spot = tf->position;
        }
    }
    if (parking_spot == airfield.threshold_position && !airbase_entity.valid()) {
        // Fall back to the route's own takeoff waypoint (grid → ENU).
        const auto& tw = intent.route.front();
        if (tw.action == 1 /* WP_TAKEOFF */) {
            parking_spot = grid_to_enu(tw.x, tw.y, 0.0f);
        }
    }
    const ScenarioAirfield& field = base_af ? *base_af : airfield;

    // Per-flight lateral offset — the same parking spread the flight
    // path uses (alternate sides, 80 ft steps).
    constexpr double OFFSET_STEP_FT = 80.0;
    const double offset = (parking_slot % 2 == 0 ? 1.0 : -1.0)
                        * (static_cast<double>(parking_slot / 2) + 1.0)
                        * OFFSET_STEP_FT;
    parking_spot.x += offset;

    // The model: the SQUADRON's entity_type (the aircraft type the
    // campaign drew from), the same resolution the flight path uses.
    int16_t vis_type_index = 0;
    if (sq) {
        auto* sq_uc =
            EntityHandle(squadron_entity, &world).get<UnitCoreComponent>();
        if (sq_uc) {
            vis_type_index = ct.vis_type_for(
                static_cast<uint16_t>(sq_uc->class_table_index), 0);
        }
    }
    if (vis_type_index <= 0) {
        vis_type_index = scenario_aircraft.vis_type_index;
    }

    // Compose the aircraft — the same component set the flight path
    // composes, fed from the intent instead of a flight entity.
    auto h = world.create();

    auto& tf = h.add<TransformComponent>();
    tf.position = parking_spot;
    const double hdg = field.runway_heading_rad;
    const auto q0 = f4::simulation::enu_quat_from_compass(hdg);
    tf.qw = q0.w;  tf.qx = q0.x;  tf.qy = q0.y;  tf.qz = q0.z;

    auto& fm = h.add<FlightModelComponent>();
    fm.init(cfg,
            /*alt_ft=*/parking_spot.z,
            /*vt_ftps=*/0.0,
            /*hdg_rad=*/hdg,
            /*inAir=*/false,
            /*north_ft=*/parking_spot.y,
            /*east_ft=*/parking_spot.x);
    fm.set_ground(parking_spot.z, f4::math::Vec3d{0.0, 0.0, -1.0});

    auto& vis = h.add<VisualModelComponent>();
    if (db.valid()) {
        vis.model_record = db.model(vis_type_index);
    }
    vis.active_lod = 0;
    f4::models::SwitchState gear_switch;
    gear_switch.switch_number = 10;
    gear_switch.active_child  = 0;
    vis.model_state.switches.push_back(gear_switch);

    auto& brain = h.add<BrainComponent>();
    brain.module().rotate_speed_kts = 140.0;
    brain.module().gear_up_alt_ft = 200.0;
    brain.module().departure_alt_ft = field.departure_altitude_ft;
    brain.module().taxi_speed_kts = 15.0;
    brain.module().airbase_id = home_airbase_vu;
    if (auto plan = build_mission_plan_from_route(
            intent.route, intent.target_objective_id, objective_id_map)) {
        brain.set_mission_plan(std::move(*plan));
    }

    // Ordnance: no wire loadout exists for a synthetic draw, so the
    // doctrine fill arms the delivery-category missions (the same
    // rule the flight path's fallback uses — MK-82 stations for A/G
    // missions, bookkeeping-only otherwise).
    if (weapon_table != nullptr) {
        (void)arm_flight_strike(*weapon_table, h, {}, intent.mission_byte);
    }

    // TEAM tag + C1 origin stamp: the campaign identity the aircraft
    // flies under. The flight_vu is the intent's synthetic flight id
    // (no live flight entity exists — the identity IS the ledger's
    // draw record); the squadron/home-airbase VUs and the team slot
    // come from the intent, so kills over the target write back to
    // the squadron that was tasked (the C1 loop, closed for
    // generated missions too).
    h.set_tag(tags::TEAM,
              TagValue::from(owner_team_string(world, intent.team)));
    auto& origin = h.add<CampaignOriginComponent>();
    origin.flight_vu = intent.flight_id;
    origin.squadron_vu = intent.squadron_id;
    origin.home_airbase_vu = static_cast<std::uint32_t>(home_airbase_vu);
    origin.team_slot = intent.team;
    origin.callsign_id = 0;
    origin.callsign_num = 0;

    return h.id();
}

std::string owner_team_string(const f4::entities::EntityWorld& world,
                              std::uint8_t owner) {
    using namespace f4::entities;

    // Find the campaign singleton (ROLE "campaign") for the player team.
    int player_slot = -1;
    const auto camp_ids = world.with_tag(
        tags::ROLE, TagValue::from(std::string("campaign")));
    if (!camp_ids.empty()) {
        EntityHandle h(camp_ids[0], const_cast<EntityWorld*>(&world));
        if (auto* cs = h.get<CampaignStateComponent>()) {
            player_slot = cs->te_team;
        }
    }

    // No campaign data (synthetic test world): slot 0 is the de-facto
    // friendly, everything else hostile.
    if (player_slot < 0) {
        return owner == 0 ? "blue" : "red";
    }
    if (static_cast<int>(owner) == player_slot) return "blue";

    // Look up the owner team's stance toward the player slot. RelType
    // enum (cmpglobl.h): red = at WAR with the player (5) — the same
    // rule the campaign's belligerence/target selection uses. The
    // pre-C3 "< 0" sign test misread the garbage columns real saves
    // carry toward unused slots (e.g. -5141) as war; the enum decode
    // maps out-of-range values to NoRelations, so phantom slots stay
    // green.
    const auto team_ids = world.with_tag(
        tags::ROLE, TagValue::from(std::string("team")));
    for (const auto eid : team_ids) {
        EntityHandle h(eid, const_cast<EntityWorld*>(&world));
        auto* tc = h.get<TeamComponent>();
        if (!tc || tc->slot != static_cast<int>(owner)) continue;
        if (player_slot < static_cast<int>(tc->stance.size()) &&
            f4::world::relation_from_wire(
                tc->stance[static_cast<std::size_t>(player_slot)]) ==
                f4::world::Relation::War) {
            return "red";
        }
        return "green";
    }
    return "green";
}

// ============================================================================
// Mode B: Unit Deaggregation
// ============================================================================

namespace {

// --- SYNTHETIC FORMATION LAYOUTS -------------------------------------------
//
// FreeFalcon's ground-vehicle formation tables (SquadFormations /
// PlatoonFormations / CompanyFormations, defined in gndai.cpp:110-282 of
// the original source) are NOT ported into this tree. They are flagged as
// future work in worklog.md:857. Until they are ported, we use a small
// set of synthetic layouts:
//
//   • wedge4  — 4-vehicle wedge (lead + 2 wingmen + trail). Used for any
//                unit with ≤4 live vehicles (most battalions: 4 groups ×
//                1-3 live vehicles collapses to ≤4 after aggregation).
//   • grid    — N>4 vehicles arranged in a 4-wide grid, 50 ft spacing.
//                Used for larger aggregations (brigades deaggregated to
//                their component vehicles).
//
// Both layouts are rotated by the unit's heading (from
// GroundTacticalComponent::heading, uint8_t 0-255 × 1.4°/step) before
// being added to the unit's TransformComponent::position.
//
// All offsets are in FEET, ENU frame, relative to the unit center.
//   +x = east, +y = north. The unit's heading 0 = facing north (+y);
//   heading π/2 = facing east (+x). Rotation: standard 2D CCW rotation
//   of the offset by the heading angle.
//
// When the real FreeFalcon formation tables are ported, replace
// `formation_offset()` with a lookup into the ported tables. The
// spawn_vehicles_from_unit() contract (offset is in unit-local feet,
// rotated by unit heading) doesn't change.

constexpr double WEDGE_SPACING_FT = 30.0;  // ~tank length, plausible wedge spacing

/// 4-vehicle wedge offsets (unit-local, unrotated):
///   slot 0: lead     at ( 0, +30)
///   slot 1: wing-L   at (-30,  0)
///   slot 2: wing-R   at (+30,  0)
///   slot 3: trail    at ( 0, -30)
/// Lead faces forward (+y); wingmen trail by 30 ft; trail brings up the rear.
struct Offset { double dx; double dy; };
constexpr std::array<Offset, 4> WEDGE4{{
    {  0.0,  30.0 },
    { -30.0,  0.0 },
    {  30.0,  0.0 },
    {  0.0, -30.0 },
}};

constexpr double GRID_SPACING_FT = 50.0;
constexpr int    GRID_COLS       = 4;

/// Compute the (dx, dy) offset for the i-th vehicle in a synthetic
/// formation. Wedge for i < 4, grid for i >= 4.
/// (When real FreeFalcon formation tables are ported, replace this body
/// with `return ported_table[unit_class][i]` or similar.)
Offset formation_offset(int vehicle_index) {
    if (vehicle_index < 4) {
        return WEDGE4[static_cast<std::size_t>(vehicle_index)];
    }
    // Grid extension: rows of 4, indexed from vehicle_index=4 onward.
    const int grid_i = vehicle_index - 4;
    const int row = grid_i / GRID_COLS;
    const int col = grid_i % GRID_COLS;
    // Center the grid: col 0..3 → dx -75..+75 (4 * 50 / 2 = 100, half = 50, center -25).
    // Push rows behind the wedge (negative y).
    const double dx = (col - (GRID_COLS - 1) * 0.5) * GRID_SPACING_FT;
    const double dy = -90.0 - static_cast<double>(row) * GRID_SPACING_FT;
    return { dx, dy };
}

/// Rotate a unit-local (dx, dy) offset by a compass heading (radians,
/// 0 = +y / north, CW positive) into world ENU.
///
/// Compass heading θ rotates the +y axis (north) toward +x (east). So a
/// unit-local offset (dx, dy) becomes world offset:
///   world_dx =  dx · cos θ + dy · sin θ
///   world_dy = -dx · sin θ + dy · cos θ
Offset rotate_offset(Offset local, double heading_rad) {
    const double ch = std::cos(heading_rad);
    const double sh = std::sin(heading_rad);
    return { local.dx * ch + local.dy * sh,
            -local.dx * sh + local.dy * ch };
}

/// Resolve a VEHICLE entity_type → ModelRecord* via the ClassTable.
/// Returns nullptr if the lookup fails at any stage (entity_type out of
/// range, visType[0] == 0, model DB has no record for the vis_type).
const f4::models::ModelRecord*
resolve_vehicle_model(const f4::world_convert::ClassTable& ct,
                       const f4::models::ModelDatabase& db,
                       int16_t vehicle_entity_type) {
    if (vehicle_entity_type < 100) return nullptr;  // not a valid entity_type
    const auto vis_type = ct.vis_type_for(
        static_cast<uint16_t>(vehicle_entity_type), 0);
    if (vis_type <= 0) return nullptr;
    if (!db.valid()) return nullptr;
    return db.model(vis_type);
}

} // namespace

std::vector<f4::entities::EntityId>
spawn_vehicles_from_unit(f4::entities::EntityWorld& world,
                          const f4::world_convert::ClassTable& ct,
                          const f4::models::ModelDatabase& db,
                          f4::entities::EntityId unit_id) {
    using namespace f4::entities;
    using namespace f4::simulation;

    std::vector<EntityId> spawned;

    EntityHandle unit_h(unit_id, &world);
    if (!unit_h.valid()) return spawned;

    const auto* vc = unit_h.get<VehicleCompositionComponent>();
    const auto* tf = unit_h.get<TransformComponent>();
    if (!vc || !tf) return spawned;

    // Unit heading: GroundTacticalComponent::heading is uint8_t 0-255,
    // ×1.4° per step (entity.hpp:483). Falls back to 0 (north-facing) when
    // the unit has no GroundTacticalComponent (e.g. TaskForce whose GT
    // component is populated with zeros, or Squadron which doesn't have one).
    double heading_rad = 0.0;
    if (const auto* gt = unit_h.get<GroundTacticalComponent>()) {
        heading_rad = static_cast<double>(gt->heading) * (360.0 / 256.0)
                    * 0.017453292519943295;  // DEG_TO_RAD
    }
    const auto q0 = f4::simulation::enu_quat_from_compass(heading_rad);

    // Spawn one entity per live vehicle per group.
    // vehicle_index increments across ALL groups so the formation is
    // contiguous (the wedge is filled by group 0's vehicles, then group
    // 1's, etc.).
    int vehicle_index = 0;
    for (const auto& g : vc->groups) {
        if (g.live_count <= 0) continue;

        // Resolve the vehicle model once per group (all vehicles in a
        // group share the same vehicle_type → same ModelRecord).
        const auto* model_rec = resolve_vehicle_model(ct, db, g.vehicle_type);
        if (!model_rec) {
            // No model for this vehicle_type — skip the whole group.
            // We still advance vehicle_index so subsequent groups land in
            // distinct formation slots (otherwise two groups could overlap).
            vehicle_index += g.live_count;
            continue;
        }

        for (int i = 0; i < g.live_count; ++i) {
            const Offset local = formation_offset(vehicle_index);
            const Offset world_off = rotate_offset(local, heading_rad);

            auto h = world.create();

            // 1. TransformComponent — position = unit center + formation offset.
            auto& vtf = h.add<TransformComponent>();
            vtf.position = f4::geo::WorldPosition(
                tf->position.x + world_off.dx,
                tf->position.y + world_off.dy,
                tf->position.z);
            // All vehicles in the formation face the unit's heading.
            vtf.qw = q0.w;  vtf.qx = q0.x;  vtf.qy = q0.y;  vtf.qz = q0.z;

            // 2. VisualModelComponent — the renderable handle. No FM, no
            // brain — vehicles are static for now. Ground AI (DigitalBrain
            // for vehicles) is a separate porting task.
            auto& vis = h.add<VisualModelComponent>();
            vis.model_record = model_rec;
            vis.active_lod = 0;
            // model_state defaults — the renderer's draw_entity_meshes()
            // doesn't consult switches/DOFs today.

            spawned.push_back(h.id());
            ++vehicle_index;
        }
    }

    return spawned;
}

std::vector<f4::entities::EntityId>
spawn_vehicles_from_units(f4::entities::EntityWorld& world,
                           const f4::world_convert::ClassTable& ct,
                           const f4::models::ModelDatabase& db) {
    using namespace f4::entities;

    const auto unit_ids = world.with_component<VehicleCompositionComponent>();
    std::vector<EntityId> spawned;
    for (const auto unit_id : unit_ids) {
        auto batch = spawn_vehicles_from_unit(world, ct, db, unit_id);
        spawned.insert(spawned.end(), batch.begin(), batch.end());
    }
    return spawned;
}

// --- Squadron → parked-aircraft deaggregation ------------------------------
//
// For each Squadron entity:
//   1. Resolve home airbase via SquadronComponent::airbase (now works thanks
//      to the positional fallback in world_loader.cpp).
//   2. Count active Flights (FlightPlanComponent::squadron == this squadron).
//      The active flights produce their own aircraft via
//      spawn_aircraft_from_flights(); we only spawn the remainder so we
//      don't duplicate them.
//   3. Pick parking spots from the airbase's ScenarioAirfield.parking_spots
//      (built by derive_airfield_from_objective; either real PLT_PARK or
//      the synthesized 8-spot row). Cycle through spots if more aircraft
//      than spots — multiple aircraft per spot is wrong but better than
//      dropping them silently (and the airfield's parking-spots list will
//      grow when real PHD data is loaded).
//   4. Compose each parked aircraft: Transform + FM + VMC + Brain (same
//      shape as spawn_aircraft_from_flights, but the brain is dormant).

namespace {

/// Count Flights whose FlightPlanComponent::squadron points at the given
/// Squadron EntityId. Used to suppress parked-aircraft duplicates.
int count_active_flights_for_squadron(const f4::entities::EntityWorld& world,
                                        f4::entities::EntityId squadron_id) {
    using namespace f4::entities;
    int count = 0;
    const auto flight_ids = world.with_component<FlightPlanComponent>();
    for (const auto fid : flight_ids) {
        EntityHandle fh(fid, const_cast<EntityWorld*>(&world));
        const auto* fp = fh.get<FlightPlanComponent>();
        if (fp && fp->squadron.value == squadron_id.value) ++count;
    }
    return count;
}

/// Pick the i-th parking spot, cycling through the available spots if i
/// exceeds the spot count. (Multiple aircraft per spot is wrong but
/// better than dropping aircraft — and the spot list will grow when real
/// PHD parking data is loaded for non-Korea theaters.)
const ScenarioParkingSpot*
pick_parking_spot(const std::vector<ScenarioParkingSpot>& spots, int i) {
    if (spots.empty()) return nullptr;
    const auto idx = static_cast<std::size_t>(i) % spots.size();
    return &spots[idx];
}

} // namespace

std::vector<f4::entities::EntityId>
spawn_aircraft_from_squadrons(f4::entities::EntityWorld& world,
                                const f4::world_convert::ClassTable& ct,
                                const f4::models::ModelDatabase& db,
                                const f4::data::AircraftConfig& cfg,
                                const ScenarioAirfield& airfield,
                                const ScenarioAircraft& scenario_aircraft) {
    using namespace f4::entities;
    using namespace f4::flight;
    using namespace f4::ai;
    using namespace f4::simulation;

    const auto squadron_ids = world.with_component<SquadronComponent>();
    if (squadron_ids.empty()) return {};

    std::vector<EntityId> spawned;

    for (const auto squadron_id : squadron_ids) {
        EntityHandle sq_h(squadron_id, &world);
        const auto* sq = sq_h.get<SquadronComponent>();
        const auto* sq_uc = sq_h.get<UnitCoreComponent>();
        if (!sq || !sq_uc) continue;

        // 1. Resolve the aircraft visType from the squadron's class_table_index.
        //    This is the aircraft type the squadron flies (e.g. F-16 entity_type
        //    273 → visType 1052 → F-16 ModelRecord).
        int16_t vis_type_index = ct.vis_type_for(
            static_cast<uint16_t>(sq_uc->class_table_index), 0);
        if (vis_type_index <= 0) {
            vis_type_index = scenario_aircraft.vis_type_index;
        }
        const auto* model_rec = db.valid() ? db.model(vis_type_index) : nullptr;

        // 2. Count active Flights — only spawn the un-tasked remainder.
        const int active_flights = count_active_flights_for_squadron(world, squadron_id);
        const int n_pilots = static_cast<int>(sq->pilots.size());
        int n_to_spawn = n_pilots - active_flights;
        if (n_to_spawn <= 0) continue;

        // 3. Pick parking spots from the airbase. When the airbase has
        //    parking spots (real PLT_PARK or the synthesized 8-spot row),
        //    use them. Otherwise fall back to the airfield threshold (the
        //    same fallback spawn_aircraft_from_flights uses).
        const auto& spots = airfield.parking_spots;
        const bool has_spots = !spots.empty();

        for (int i = 0; i < n_to_spawn; ++i) {
            f4::geo::WorldPosition parking_pos = airfield.threshold_position;
            double heading_rad = airfield.runway_heading_rad;

            if (has_spots) {
                const auto* spot = pick_parking_spot(spots, i);
                if (spot) {
                    parking_pos = spot->position;
                    heading_rad = spot->heading_rad;
                }
            } else {
                // No parking spots: fall back to a per-aircraft lateral
                // offset from the threshold (same pattern as
                // spawn_aircraft_from_flights, but per-squadron not per-flight).
                constexpr double OFFSET_STEP_FT = 80.0;
                const double offset = (i % 2 == 0 ? 1.0 : -1.0)
                                    * (static_cast<double>(i / 2) + 1.0)
                                    * OFFSET_STEP_FT;
                parking_pos.x += offset;
            }

            // 4. Compose the parked aircraft: Transform + FM + VMC + Brain.
            //    Same shape as spawn_aircraft_from_flights, but the brain
            //    is dormant (no active mission — the aircraft is parked).
            auto h = world.create();

            auto& tf = h.add<TransformComponent>();
            tf.position = parking_pos;
            const auto q0 = f4::simulation::enu_quat_from_compass(heading_rad);
            tf.qw = q0.w;  tf.qx = q0.x;  tf.qy = q0.y;  tf.qz = q0.z;

            auto& fm = h.add<FlightModelComponent>();
            fm.init(cfg,
                    /*alt_ft=*/parking_pos.z,
                    /*vt_ftps=*/0.0,
                    /*hdg_rad=*/heading_rad,
                    /*inAir=*/false);
            fm.set_ground(parking_pos.z, f4::math::Vec3d{0.0, 0.0, -1.0});

            auto& vis = h.add<VisualModelComponent>();
            vis.model_record = model_rec;
            vis.active_lod = 0;
            f4::models::SwitchState gear_switch;
            gear_switch.switch_number = 10;
            gear_switch.active_child  = 0;  // gear down
            vis.model_state.switches.push_back(gear_switch);

            auto& brain = h.add<BrainComponent>();
            brain.module().rotate_speed_kts = 140.0;
            brain.module().gear_up_alt_ft = 200.0;
            brain.module().departure_alt_ft = airfield.departure_altitude_ft;
            brain.module().taxi_speed_kts = 15.0;

            // Dormant: parked inventory, not simulated. A populated save
            // spawns ~1,000 of these; without the flag each one ticks a
            // full brain + FCS/EOM every tick (~25 ms/tick at campaign
            // scale — the QC harness caught this). The airframe renders
            // (passive Transform/VisualModel) and stays available for a
            // future launch, which materializes a fresh non-dormant
            // entity through the flight spawner.
            brain.set_dormant(true);
            fm.set_dormant(true);

            spawned.push_back(h.id());
        }
    }

    return spawned;
}

} // namespace f4::simulation

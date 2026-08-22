// f4-scenario-player/src/airfield_overlays.cpp
//
// Builds AirfieldOverlays from a Scenario. See header for rationale.
//
// The runway/taxiway/parking geometry goes through the shared
// f4::renderer::build_airfield_geometry_3d() in both cases:
//
//   1. Real campaign airbase (scenario.layout_lists non-empty):
//        use those lists directly. The scenario's layout_center places
//        the objective-local geometry in the world.
//
//   2. Hand-authored scenario (no layout_lists):
//        synthesize GroundLayoutList entries (CL + LT + RT + PARK) from
//        the scenario's threshold/end/parking + a runway width, then run
//        them through the same shared builder. The renderer doesn't care
//        whether the lists came from a PHD file or were synthesized here.
//
// The scenario-specific overlays (taxi route, flight plan, approach,
// taxi-in, compass, hold-short, runway-end markers) use the shared
// f4::renderer::LayoutLine / LayoutMarker types so they render through
// the same draw_layout_line / draw_layout_marker code path.

#include "f4/scenario_player/airfield_overlays.hpp"

#include <f4/entities/types.hpp>             // GroundLayoutList, GroundLayoutPoint
#include <f4/renderer/ground_layout_models.hpp>  // build_airfield_geometry_3d

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace f4::scenario_player {

namespace {

// ── Tunables (match the previous airport_geometry.cpp constants) ────────────

// Standard runway width for a fighter base (Kunsan's Rwy 36/18 is 150 ft;
// we use 100 ft as a reasonable default — exact width comes from the
// airbase's GroundLayoutList in the real-layout path).
constexpr double RUNWAY_WIDTH_FT = 100.0;

// Parking-spot marker cube size (small, just for visibility).
constexpr float PARKING_MARKER_SIZE_FT = 15.0f;

// Hold-short / runway-end marker cube size.
constexpr float HOLD_SHORT_MARKER_SIZE_FT = 10.0f;

// Compass rose extent (feet from the parking spot center).
constexpr double COMPASS_EXTENT_FT = 200.0;

// Approach reference extent: how far before the threshold the extended
// centerline + glide-slope lines extend (4 NM in feet).
constexpr double APPROACH_EXTENT_FT = 4.0 * 6076.12;

// 3-deg glide slope.
constexpr double GLIDE_SLOPE_DEG = 3.0;
constexpr double PI = 3.14159265358979323846;

// ── Color helpers (RGBA 0..255 — matches LayoutLine/Marker convention) ──────
//
// The shared primitives use uint8_t RGBA. The previous scenario-player
// types used float 0..1; the values below are the 0..255 equivalents so
// the visual output is identical.

struct Color8 { uint8_t r, g, b, a; };

constexpr Color8 C_WHITE   = {255, 255, 255, 255};
constexpr Color8 C_YELLOW  = {255, 217,   0, 255};
constexpr Color8 C_GREEN   = { 51, 230,  51, 255};
constexpr Color8 C_RED    = {230,  51,  51, 255};
constexpr Color8 C_NWAY   = {179, 179, 191, 255};   // pale grey compass
constexpr Color8 C_CYAN    = { 38, 217, 230, 255};
constexpr Color8 C_DIMCYAN = { 38, 115, 128, 153};  // dim, semi-transparent
constexpr Color8 C_ORANGE  = {255, 140,  26, 255};
constexpr Color8 C_PURPLE  = {191, 102, 242, 255};

inline void set_line_color(f4::renderer::LayoutLine& l, const Color8& c) {
    l.r = c.r; l.g = c.g; l.b = c.b; l.a = c.a;
}

inline void set_marker_color(f4::renderer::LayoutMarker& m, const Color8& c) {
    m.r = c.r; m.g = c.g; m.b = c.b; m.a = c.a;
}

// ── Real-layout path: use scenario.layout_lists directly ─────────────────────

void build_real_layout(const f4::simulation::Scenario& s, AirfieldOverlays& g) {
    g.geometry = f4::renderer::build_airfield_geometry_3d(s.layout_lists);
    g.has_real_layout = true;
    g.origin_enu_x = static_cast<float>(s.layout_center.x);
    g.origin_enu_y = static_cast<float>(s.layout_center.y);
    g.origin_enu_z = static_cast<float>(s.layout_center.z);
}

// ── Synthetic-layout path: synthesize GroundLayoutLists and use the
//    shared builder. The synthesized lists mimic real PHD data:
//
//      CL  (type 1)  — threshold → end (drives surface + dashes + end marker)
//      LT  (type 12) — left edge threshold → left edge end   ┐ drives threshold
//      RT  (type 13) — right edge threshold → right edge end ┘ bars (need both)
//      PARK (type 11) — parking spot                          → parking marker
//
//    The runway surface's actual width (used by build_threshold_bars) is
//    derived from LT[0]→RT[0] distance, so it matches RUNWAY_WIDTH_FT.

void build_synthetic_layout(const f4::simulation::Scenario& s, AirfieldOverlays& g) {
    const auto& threshold = s.airfield.threshold_position;
    const auto& end = s.airfield.runway_end_position;

    // Along-runway unit vector and perpendicular (CCW 90°).
    const double dx = end.x - threshold.x;
    const double dy = end.y - threshold.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0) {
        // Degenerate — emit nothing. The overlay code below still runs.
        return;
    }
    const double fx = dx / len, fy = dy / len;     // along-runway
    const double px = -fy,        py = fx;          // perpendicular (CCW)

    // Runway width: prefer scenario-provided, else default.
    const double width = s.airfield.runway_width_ft > 0.0
                          ? s.airfield.runway_width_ft : RUNWAY_WIDTH_FT;
    const double half = width * 0.5;

    std::vector<f4::entities::GroundLayoutList> layouts;
    layouts.reserve(4);

    // ── CL (type 1) — threshold + end ─────────────────────────────────
    // heading_deg is the compass heading of the threshold→end direction
    // (atan2 of east-component over north-component, in degrees, [0, 360)).
    {
        f4::entities::GroundLayoutList cl;
        cl.type = 1;  // PLT_RUNWAY
        cl.runway_num = 1;
        // Compass heading: atan2(east, north) — matches how the world
        // bridge derives heading_deg for real CL lists.
        const double heading_rad = std::atan2(fx, fy);
        cl.heading_deg = static_cast<float>(heading_rad * 180.0 / PI);
        if (cl.heading_deg < 0.0f) cl.heading_deg += 360.0f;
        f4::entities::GroundLayoutPoint p0;
        p0.x = static_cast<float>(threshold.x);
        p0.y = static_cast<float>(threshold.y);
        p0.type = 1;  // PT_RUNWAY
        f4::entities::GroundLayoutPoint p1;
        p1.x = static_cast<float>(end.x);
        p1.y = static_cast<float>(end.y);
        p1.type = 1;
        cl.points = {p0, p1};
        cl.count = 2;
        layouts.push_back(std::move(cl));
    }

    // ── LT (type 12) — left edge threshold → left edge end ────────────
    {
        f4::entities::GroundLayoutList lt;
        lt.type = 12;  // PLT_RUNWAY_LT
        lt.runway_num = 1;
        lt.heading_deg = layouts.front().heading_deg;
        f4::entities::GroundLayoutPoint p0;
        p0.x = static_cast<float>(threshold.x - px * half);
        p0.y = static_cast<float>(threshold.y - py * half);
        f4::entities::GroundLayoutPoint p1;
        p1.x = static_cast<float>(end.x - px * half);
        p1.y = static_cast<float>(end.y - py * half);
        lt.points = {p0, p1};
        lt.count = 2;
        layouts.push_back(std::move(lt));
    }

    // ── RT (type 13) — right edge threshold → right edge end ──────────
    {
        f4::entities::GroundLayoutList rt;
        rt.type = 13;  // PLT_RUNWAY_RT
        rt.runway_num = 1;
        rt.heading_deg = layouts.front().heading_deg;
        f4::entities::GroundLayoutPoint p0;
        p0.x = static_cast<float>(threshold.x + px * half);
        p0.y = static_cast<float>(threshold.y + py * half);
        f4::entities::GroundLayoutPoint p1;
        p1.x = static_cast<float>(end.x + px * half);
        p1.y = static_cast<float>(end.y + py * half);
        rt.points = {p0, p1};
        rt.count = 2;
        layouts.push_back(std::move(rt));
    }

    // ── PARK (type 11) — parking spot ──────────────────────────────────
    if (!s.aircraft.empty()) {
        const auto& parking = s.aircraft.front().parking_spot;
        f4::entities::GroundLayoutList park;
        park.type = 11;  // PLT_PARK
        park.runway_num = 255;  // not a runway
        f4::entities::GroundLayoutPoint p;
        p.x = static_cast<float>(parking.x);
        p.y = static_cast<float>(parking.y);
        p.type = 11;  // PT_SMALL_PARK
        park.points = {p};
        park.count = 1;
        layouts.push_back(std::move(park));
    }

    g.geometry = f4::renderer::build_airfield_geometry_3d(layouts);
    g.has_real_layout = false;
    // Synthetic layout is already in world ENU (scenario positions are
    // ENU feet); origin offset is zero.
    g.origin_enu_x = 0.0f;
    g.origin_enu_y = 0.0f;
    g.origin_enu_z = 0.0f;
}

// ── Scenario-specific overlays ──────────────────────────────────────────────
//
// These don't have PHD equivalents (or aren't worth modeling as lists):
//   - taxi route (yellow line strip — a navigation aid, not a painted mark)
//   - flight plan (cyan route + drop lines + waypoint markers)
//   - approach reference (orange extended centerline + 3° glide slope)
//   - taxi-in route (purple line strip)
//   - compass rose (N/E/S/W lines + tick marks)
//   - parking-spot / hold-short / runway-end markers (these duplicate the
//     geometry builder's markers but carry scenario-specific labels and
//     sizes; the originals from build_airfield_geometry_3d are also drawn
//     but they're small red cubes — these overlays are the larger
//     colored cubes the user actually sees).

void build_taxi_route(const std::vector<f4::geo::WorldPosition>& route,
                       AirfieldOverlays& g) {
    if (route.size() < 2) return;
    g.taxi_route_lines.reserve(route.size() - 1);
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
        f4::renderer::LayoutLine l;
        l.x0 = static_cast<float>(route[i].x);
        l.y0 = static_cast<float>(route[i].y);
        l.z  = static_cast<float>(route[i].z + 1.0);  // lift 1 ft to avoid z-fighting
        l.x1 = static_cast<float>(route[i + 1].x);
        l.y1 = static_cast<float>(route[i + 1].y);
        l.z  = static_cast<float>(route[i + 1].z + 1.0);
        set_line_color(l, C_YELLOW);
        g.taxi_route_lines.push_back(l);
    }
}

void build_flightplan(const std::vector<f4::simulation::ScenarioWaypoint>& wps,
                       double ground_z,
                       AirfieldOverlays& g) {
    if (wps.empty()) return;
    for (std::size_t i = 0; i < wps.size(); ++i) {
        const auto& p = wps[i].position;

        // Drop line to the ground so the waypoint's altitude is readable
        // from any 3D viewing angle.
        f4::renderer::LayoutLine drop;
        drop.x0 = static_cast<float>(p.x);
        drop.y0 = static_cast<float>(p.y);
        drop.z  = static_cast<float>(p.z);
        drop.x1 = static_cast<float>(p.x);
        drop.y1 = static_cast<float>(p.y);
        drop.z  = static_cast<float>(ground_z + 1.0);
        set_line_color(drop, C_DIMCYAN);
        g.flightplan_drop_lines.push_back(drop);

        // Waypoint marker (cyan cube).
        f4::renderer::LayoutMarker m;
        m.x = static_cast<float>(p.x);
        m.y = static_cast<float>(p.y);
        m.z = static_cast<float>(p.z);
        m.size_ft = 25.0f;
        set_marker_color(m, C_CYAN);
        g.flightplan_waypoints.push_back(m);

        // Route line to the next waypoint at altitude.
        if (i + 1 < wps.size()) {
            const auto& q = wps[i + 1].position;
            f4::renderer::LayoutLine l;
            l.x0 = static_cast<float>(p.x);
            l.y0 = static_cast<float>(p.y);
            l.z  = static_cast<float>(p.z);
            l.x1 = static_cast<float>(q.x);
            l.y1 = static_cast<float>(q.y);
            l.z  = static_cast<float>(q.z);
            set_line_color(l, C_CYAN);
            g.flightplan_lines.push_back(l);
        }
    }
}

void build_approach(const f4::geo::WorldPosition& threshold,
                    const f4::geo::WorldPosition& end,
                    AirfieldOverlays& g) {
    const double dx = end.x - threshold.x;
    const double dy = end.y - threshold.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0) return;
    const double fx = dx / len, fy = dy / len;

    // Far end of the approach reference (~4 NM before the threshold).
    const f4::geo::WorldPosition far_end(
        threshold.x - fx * APPROACH_EXTENT_FT,
        threshold.y - fy * APPROACH_EXTENT_FT,
        threshold.z);

    // Extended centerline (flat on the ground, lifted 1 ft).
    {
        f4::renderer::LayoutLine cl;
        cl.x0 = static_cast<float>(threshold.x);
        cl.y0 = static_cast<float>(threshold.y);
        cl.z  = static_cast<float>(threshold.z + 1.0);
        cl.x1 = static_cast<float>(far_end.x);
        cl.y1 = static_cast<float>(far_end.y);
        cl.z  = static_cast<float>(far_end.z + 1.0);
        set_line_color(cl, C_ORANGE);
        g.approach_lines.push_back(cl);
    }

    // 3-deg glide slope rising from the threshold along the same course.
    {
        const double slope = std::tan(GLIDE_SLOPE_DEG * PI / 180.0);
        f4::renderer::LayoutLine gs;
        gs.x0 = static_cast<float>(threshold.x);
        gs.y0 = static_cast<float>(threshold.y);
        gs.z  = static_cast<float>(threshold.z);
        gs.x1 = static_cast<float>(far_end.x);
        gs.y1 = static_cast<float>(far_end.y);
        gs.z  = static_cast<float>(threshold.z + APPROACH_EXTENT_FT * slope);
        set_line_color(gs, C_ORANGE);
        g.approach_lines.push_back(gs);
    }

    // Marker at the far end (the outer approach reference point).
    {
        f4::renderer::LayoutMarker m;
        m.x = static_cast<float>(far_end.x);
        m.y = static_cast<float>(far_end.y);
        m.z = static_cast<float>(far_end.z);
        m.size_ft = 20.0f;
        set_marker_color(m, C_ORANGE);
        g.approach_markers.push_back(m);
    }
}

void build_taxi_in_route(const std::vector<f4::geo::WorldPosition>& route,
                          AirfieldOverlays& g) {
    if (route.size() < 2) return;
    g.taxi_in_route_lines.reserve(route.size() - 1);
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
        f4::renderer::LayoutLine l;
        l.x0 = static_cast<float>(route[i].x);
        l.y0 = static_cast<float>(route[i].y);
        l.z  = static_cast<float>(route[i].z + 1.0);
        l.x1 = static_cast<float>(route[i + 1].x);
        l.y1 = static_cast<float>(route[i + 1].y);
        l.z  = static_cast<float>(route[i + 1].z + 1.0);
        set_line_color(l, C_PURPLE);
        g.taxi_in_route_lines.push_back(l);
    }
}

void build_compass_rose(const f4::geo::WorldPosition& center,
                         double extent_ft,
                         AirfieldOverlays& g) {
    const float z = static_cast<float>(center.z + 1.0);
    const float ext = static_cast<float>(extent_ft);
    const float cx = static_cast<float>(center.x);
    const float cy = static_cast<float>(center.y);

    // Cardinal directions (ENU: +Y = North, +X = East).
    auto add_line = [&](float x0, float y0, float x1, float y1) {
        f4::renderer::LayoutLine l;
        l.x0 = x0; l.y0 = y0; l.z = z;
        l.x1 = x1; l.y1 = y1; l.z = z;
        set_line_color(l, C_NWAY);
        g.compass_rose.push_back(l);
    };

    // Center → N/E/S/W lines.
    add_line(cx, cy, cx,         cy + ext);   // N
    add_line(cx, cy, cx,         cy - ext);   // S
    add_line(cx, cy, cx + ext,   cy);          // E
    add_line(cx, cy, cx - ext,   cy);          // W

    // Tick marks at the cardinal points (perpendicular short segments).
    const float tick_half = 15.0f;
    add_line(cx,         cy + ext, cx - tick_half, cy + ext);   // N tick (W half)
    add_line(cx + tick_half, cy + ext, cx,         cy + ext);   // N tick (E half)
    add_line(cx,         cy - ext, cx - tick_half, cy - ext);   // S tick
    add_line(cx + tick_half, cy - ext, cx,         cy - ext);
    add_line(cx + ext,   cy,        cx + ext,      cy - tick_half);  // E tick
    add_line(cx + ext,   cy + tick_half, cx + ext, cy);
    add_line(cx - ext,   cy,        cx - ext,      cy - tick_half);  // W tick
    add_line(cx - ext,   cy + tick_half, cx - ext, cy);
}

void build_overlay_markers(const f4::simulation::Scenario& s,
                            AirfieldOverlays& g) {
    const auto& threshold = s.airfield.threshold_position;
    const auto& end = s.airfield.runway_end_position;
    const auto& parking = s.aircraft.empty()
        ? f4::geo::WorldPosition{}
        : s.aircraft.front().parking_spot;

    // Parking-spot marker (green cube at the aircraft spawn position).
    g.parking_spot.x = static_cast<float>(parking.x);
    g.parking_spot.y = static_cast<float>(parking.y);
    g.parking_spot.z = static_cast<float>(parking.z);
    g.parking_spot.size_ft = PARKING_MARKER_SIZE_FT;
    set_marker_color(g.parking_spot, C_GREEN);

    // Hold-short marker (yellow cube at the end of the taxi route — the
    // route's last waypoint IS the hold-short point).
    if (!s.airfield.taxi_route.empty()) {
        const auto& last_wp = s.airfield.taxi_route.back();
        g.hold_short.x = static_cast<float>(last_wp.x);
        g.hold_short.y = static_cast<float>(last_wp.y);
        g.hold_short.z = static_cast<float>(last_wp.z);
        g.hold_short.size_ft = HOLD_SHORT_MARKER_SIZE_FT;
        set_marker_color(g.hold_short, C_YELLOW);
    }

    // Runway-end marker (red cube at the far end of the runway).
    g.runway_end.x = static_cast<float>(end.x);
    g.runway_end.y = static_cast<float>(end.y);
    g.runway_end.z = static_cast<float>(end.z);
    g.runway_end.size_ft = HOLD_SHORT_MARKER_SIZE_FT;
    set_marker_color(g.runway_end, C_RED);

    build_compass_rose(parking, COMPASS_EXTENT_FT, g);
    (void)threshold;  // not used directly here — approach uses it
}

} // namespace

AirfieldOverlays build_airfield_overlays(const f4::simulation::Scenario& s) {
    AirfieldOverlays g;

    // If the scenario has no airfield, return an empty geometry. The
    // renderer will just draw the aircraft on an empty field.
    if (s.airfield.taxi_route.empty()) {
        g.built = true;
        return g;
    }

    // Build the shared AirfieldGeometry3D (runway/taxiway/parking).
    if (!s.layout_lists.empty()) {
        build_real_layout(s, g);
    } else {
        build_synthetic_layout(s, g);
    }

    // Build the scenario-specific overlays (taxi route, flight plan,
    // approach, taxi-in, markers, compass rose).
    build_taxi_route(s.airfield.taxi_route, g);
    build_flightplan(s.waypoints, s.airfield.threshold_position.z, g);
    build_approach(s.airfield.threshold_position, s.airfield.runway_end_position, g);
    build_taxi_in_route(s.airfield.taxi_in_route, g);
    build_overlay_markers(s, g);

    g.built = true;
    return g;
}

} // namespace f4::scenario_player

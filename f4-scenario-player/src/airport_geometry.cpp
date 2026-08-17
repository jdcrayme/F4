// f4-scenario-player/src/airport_geometry.cpp
//
// Builds an AirportGeometry from a Scenario. See header for rationale.

#include "f4/scenario_player/airport_geometry.hpp"

#include <cmath>

namespace f4::scenario_player {

namespace {

// Standard runway width for a fighter base (Kunsan's Rwy 36/18 is 150 ft /
// 45 m wide; we use 100 ft as a reasonable default — exact width comes from
// the airbase's GroundLayoutList in Phase 2).
constexpr double RUNWAY_WIDTH_FT = 100.0;

// Centerline dash length and spacing (typical FAA markings).
constexpr double DASH_LENGTH_FT = 120.0;
constexpr double DASH_GAP_FT    = 80.0;

// Threshold bar dimensions (the wide painted bars at the runway threshold).
constexpr double THRESHOLD_BAR_WIDTH_FT = 30.0;
constexpr double THRESHOLD_BAR_LENGTH_FT = 50.0;
constexpr int    N_THRESHOLD_BARS = 8;  // 4 bars per side, total 8

// Parking-spot marker cube size (small, just for visibility).
constexpr double PARKING_MARKER_SIZE_FT = 15.0;

// Hold-short / runway-end marker cube size.
constexpr double HOLD_SHORT_MARKER_SIZE_FT = 10.0;

// Compass rose extent (feet from the parking spot center).
constexpr double COMPASS_EXTENT_FT = 200.0;

// Color helpers (RGBA 0..1).
constexpr float WHITE[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr float YELLOW[4] = {1.0f, 0.85f, 0.0f, 1.0f};
constexpr float GREEN[4]  = {0.2f, 0.9f, 0.2f, 1.0f};
constexpr float RED[4]    = {0.9f, 0.2f, 0.2f, 1.0f};
constexpr float GREY[4]   = {0.20f, 0.20f, 0.22f, 1.0f};  // runway surface
constexpr float NWAY[4]   = {0.7f, 0.7f, 0.75f, 1.0f};    // compass rose

void set_color(float out[4], const float in[4]) {
    out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
}

// Build a quad from two edge points (p0,p1) on one side and (p2,p3) on
// the other, in order. The renderer treats the quad as two triangles
// (p0,p1,p2) and (p0,p2,p3).
GeoQuad make_quad(const f4::geo::WorldPosition& p0,
                  const f4::geo::WorldPosition& p1,
                  const f4::geo::WorldPosition& p2,
                  const f4::geo::WorldPosition& p3,
                  const float color[4]) {
    GeoQuad q;
    q.p[0] = p0; q.p[1] = p1; q.p[2] = p2; q.p[3] = p3;
    set_color(&q.r, color);
    return q;
}

// Given a runway centerline (threshold → end) and a width, compute the
// four corners of the runway surface quad. The width is perpendicular
// to the centerline, in the ENU horizontal plane.
GeoQuad build_runway_surface(const f4::geo::WorldPosition& threshold,
                             const f4::geo::WorldPosition& end,
                             double width_ft) {
    // Direction vector along the runway (ENU).
    const double dx = end.x - threshold.x;
    const double dy = end.y - threshold.y;
    const double len = std::sqrt(dx*dx + dy*dy);
    if (len < 1.0) {
        // Degenerate — return a tiny zero-size quad at the threshold.
        return make_quad(threshold, threshold, threshold, threshold, GREY);
    }
    // Perpendicular (rotate 90° in the XY plane): (-dy, dx) / len.
    const double px = -dy / len * (width_ft * 0.5);
    const double py =  dx / len * (width_ft * 0.5);

    // Corners (counter-clockwise when viewed from above, +Z up):
    //   p0 = threshold - perp
    //   p1 = threshold + perp
    //   p2 = end + perp
    //   p3 = end - perp
    const f4::geo::WorldPosition p0{threshold.x - px, threshold.y - py, threshold.z};
    const f4::geo::WorldPosition p1{threshold.x + px, threshold.y + py, threshold.z};
    const f4::geo::WorldPosition p2{end.x + px, end.y + py, threshold.z};
    const f4::geo::WorldPosition p3{end.x - px, end.y - py, threshold.z};
    return make_quad(p0, p1, p2, p3, GREY);
}

// Build threshold bars: a row of N painted bars perpendicular to the
// runway, just past the threshold. Each bar is a small quad.
std::vector<GeoQuad> build_threshold_bars(const f4::geo::WorldPosition& threshold,
                                          const f4::geo::WorldPosition& end,
                                          double runway_width_ft,
                                          int n_bars) {
    std::vector<GeoQuad> out;
    out.reserve(static_cast<std::size_t>(n_bars));

    const double dx = end.x - threshold.x;
    const double dy = end.y - threshold.y;
    const double len = std::sqrt(dx*dx + dy*dy);
    if (len < 1.0) return out;

    // Unit vectors: along-runway (fwd) and perpendicular (perp).
    const double fx = dx / len, fy = dy / len;
    const double px = -fy, py = fx;

    // Bars are placed symmetrically across the runway width, each one
    // a small rectangle (THRESHOLD_BAR_LENGTH_FT along the runway,
    // THRESHOLD_BAR_WIDTH_FT across). The bars start a few feet past
    // the threshold and span the runway width with small gaps between.
    const double total_width = runway_width_ft * 0.9;  // leave a small margin
    const double gap = (total_width - n_bars * THRESHOLD_BAR_WIDTH_FT) / (n_bars - 1);
    const double start_offset = 10.0;  // feet past the threshold

    for (int i = 0; i < n_bars; ++i) {
        // Cross-runway position: -total_width/2 + i*(width+gap) + width/2
        const double cross = -total_width * 0.5 + i * (THRESHOLD_BAR_WIDTH_FT + gap)
                             + THRESHOLD_BAR_WIDTH_FT * 0.5;
        // Center of this bar (in ENU, relative to threshold):
        const double cx = threshold.x + fx * start_offset + px * cross;
        const double cy = threshold.y + fy * start_offset + py * cross;

        // Four corners of the bar:
        const double half_l = THRESHOLD_BAR_LENGTH_FT * 0.5;
        const double half_w = THRESHOLD_BAR_WIDTH_FT * 0.5;
        const f4::geo::WorldPosition p0{cx - fx*half_l - px*half_w,
                                        cy - fy*half_l - py*half_w,
                                        threshold.z + 0.5};  // +0.5 to z-fight above runway
        const f4::geo::WorldPosition p1{cx - fx*half_l + px*half_w,
                                        cy - fy*half_l + py*half_w,
                                        threshold.z + 0.5};
        const f4::geo::WorldPosition p2{cx + fx*half_l + px*half_w,
                                        cy + fy*half_l + py*half_w,
                                        threshold.z + 0.5};
        const f4::geo::WorldPosition p3{cx + fx*half_l - px*half_w,
                                        cy + fy*half_l - py*half_w,
                                        threshold.z + 0.5};
        out.push_back(make_quad(p0, p1, p2, p3, WHITE));
    }
    return out;
}

// Build centerline dashes: a sequence of small white quads spaced along
// the runway length. Skips the first/last few feet to leave room for
// the threshold bars and the runway-end markings.
std::vector<GeoQuad> build_centerline_dashes(const f4::geo::WorldPosition& threshold,
                                             const f4::geo::WorldPosition& end,
                                             double dash_len, double gap_len) {
    std::vector<GeoQuad> out;
    const double dx = end.x - threshold.x;
    const double dy = end.y - threshold.y;
    const double len = std::sqrt(dx*dx + dy*dy);
    if (len < dash_len + gap_len) return out;

    const double fx = dx / len, fy = dy / len;
    const double px = -fy, py = fx;
    const double half_w = 2.0;  // 4 ft wide centerline

    // Start past the threshold bars; stop short of the far end.
    double start = 80.0;
    double stop = len - 80.0;
    for (double s = start; s + dash_len <= stop; s += dash_len + gap_len) {
        const double s_mid = s + dash_len * 0.5;
        const double cx = threshold.x + fx * s_mid;
        const double cy = threshold.y + fy * s_mid;
        const double half_l = dash_len * 0.5;
        const f4::geo::WorldPosition p0{cx - fx*half_l - px*half_w,
                                        cy - fy*half_l - py*half_w,
                                        threshold.z + 0.5};
        const f4::geo::WorldPosition p1{cx - fx*half_l + px*half_w,
                                        cy - fy*half_l + py*half_w,
                                        threshold.z + 0.5};
        const f4::geo::WorldPosition p2{cx + fx*half_l + px*half_w,
                                        cy + fy*half_l + py*half_w,
                                        threshold.z + 0.5};
        const f4::geo::WorldPosition p3{cx + fx*half_l - px*half_w,
                                        cy + fy*half_l - py*half_w,
                                        threshold.z + 0.5};
        out.push_back(make_quad(p0, p1, p2, p3, WHITE));
    }
    return out;
}

// Build a line strip through the scenario's taxi route waypoints.
std::vector<GeoLine> build_taxi_route(const std::vector<f4::geo::WorldPosition>& route) {
    std::vector<GeoLine> out;
    if (route.size() < 2) return out;
    out.reserve(route.size() - 1);
    for (std::size_t i = 0; i + 1 < route.size(); ++i) {
        GeoLine l;
        l.a = route[i];
        l.b = route[i + 1];
        // Lift slightly above the ground to avoid z-fighting with the runway.
        l.a.z += 1.0;
        l.b.z += 1.0;
        set_color(&l.r, YELLOW);
        out.push_back(l);
    }
    return out;
}

// Build a compass rose centered on the parking spot — 4 line segments
// pointing N/E/S/W with the cardinal direction extending further.
std::vector<GeoLine> build_compass_rose(const f4::geo::WorldPosition& center,
                                        double extent_ft) {
    std::vector<GeoLine> out;
    out.reserve(8);
    const double z = center.z + 1.0;  // just above ground

    // Cardinal directions (ENU: +Y = North, +X = East)
    const f4::geo::WorldPosition n{center.x, center.y + extent_ft, z};
    const f4::geo::WorldPosition s{center.x, center.y - extent_ft, z};
    const f4::geo::WorldPosition e{center.x + extent_ft, center.y, z};
    const f4::geo::WorldPosition w{center.x - extent_ft, center.y, z};

    // Each direction gets a short segment + a longer segment for N
    auto add = [&](const f4::geo::WorldPosition& from,
                   const f4::geo::WorldPosition& to) {
        GeoLine l;
        l.a = from; l.b = to;
        set_color(&l.r, NWAY);
        out.push_back(l);
    };
    add(center, n);
    add(center, s);
    add(center, e);
    add(center, w);

    // Add tick marks at the cardinal directions (perpendicular short segments)
    const double tick_half = 15.0;
    auto add_tick = [&](const f4::geo::WorldPosition& at, double dx, double dy) {
        GeoLine l;
        l.a = f4::geo::WorldPosition{at.x - dx * tick_half, at.y - dy * tick_half, z};
        l.b = f4::geo::WorldPosition{at.x + dx * tick_half, at.y + dy * tick_half, z};
        set_color(&l.r, NWAY);
        out.push_back(l);
    };
    add_tick(n, 1.0, 0.0);  // E-W tick at the N point
    add_tick(s, 1.0, 0.0);
    add_tick(e, 0.0, 1.0);  // N-S tick at the E point
    add_tick(w, 0.0, 1.0);

    return out;
}

} // namespace

AirportGeometry build_airport_geometry(const f4::simulation::Scenario& s) {
    AirportGeometry g;

    // If the scenario has no airfield, return an empty geometry. The
    // renderer will just draw the aircraft on an empty field.
    if (s.airfield.taxi_route.empty()) return g;

    const auto& threshold = s.airfield.threshold_position;
    const auto& end = s.airfield.runway_end_position;
    const auto& parking = s.aircraft.empty()
        ? f4::geo::WorldPosition{}
        : s.aircraft.front().parking_spot;

    // Runway surface (one big dark-grey quad).
    g.runway_surface = build_runway_surface(threshold, end, RUNWAY_WIDTH_FT);

    // Threshold bars at the threshold end.
    g.threshold_bars = build_threshold_bars(threshold, end, RUNWAY_WIDTH_FT,
                                             N_THRESHOLD_BARS);

    // Centerline dashes along the runway.
    g.centerline_dashes = build_centerline_dashes(threshold, end,
                                                   DASH_LENGTH_FT, DASH_GAP_FT);

    // Taxi route line strip.
    g.taxi_route_lines = build_taxi_route(s.airfield.taxi_route);

    // Parking-spot marker (green cube at the aircraft spawn position).
    g.parking_spot.center = parking;
    g.parking_spot.size_ft = static_cast<float>(PARKING_MARKER_SIZE_FT);
    set_color(&g.parking_spot.r, GREEN);

    // Hold-short marker (yellow cube at the end of the taxi route — the
    // route's last waypoint IS the hold-short point where the aircraft
    // stops and requests takeoff clearance).
    if (!s.airfield.taxi_route.empty()) {
        const auto& last_wp = s.airfield.taxi_route.back();
        g.hold_short.center = last_wp;
        g.hold_short.size_ft = static_cast<float>(HOLD_SHORT_MARKER_SIZE_FT);
        set_color(&g.hold_short.r, YELLOW);
    }

    // Runway-end marker (red cube at the far end of the runway).
    g.runway_end.center = end;
    g.runway_end.size_ft = static_cast<float>(HOLD_SHORT_MARKER_SIZE_FT);
    set_color(&g.runway_end.r, RED);

    // Compass rose centered on the parking spot.
    g.compass_rose = build_compass_rose(parking, COMPASS_EXTENT_FT);

    return g;
}

} // namespace f4::scenario_player

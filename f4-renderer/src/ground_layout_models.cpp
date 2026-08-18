// f4-renderer/src/ground_layout_models.cpp
//
// Pure layout-to-geometry conversion. See ground_layout_models.hpp for the
// full rationale + coordinate convention.
//
// The function walks the GroundLayoutComponent::layouts vector and groups
// lists by type:
//
//   - PLT_RUNWAY_LT (12) + PLT_RUNWAY_RT (13) + PLT_RUNWAY (1) +
//     PLT_RUNWAY_DIM (8) lists sharing the same runway_num are grouped
//     together and become one runway surface + threshold bars +
//     centerline dashes + runway-end marker.
//
//   - PLT_PARK (11) lists become labeled parking markers (P1, P2, ...).
//   - PLT_HELICOPTER (14) lists become labeled helipad markers (H1, ...).
//   - Any other list type (15, 17, 0, unknown) becomes a filled taxiway
//     strip + a yellow centerline strip (per the user's "infer from any
//     list" choice).
//
// Feature placements (FeatureSetComponent::features) become flat
// building-footprint quads, colored by damage_state.
//
// The output is an AirfieldGeometry3D struct. The 3D panel
// (ground_layout_3d.cpp) consumes it and renders each primitive via
// Raylib's BeginMode3D + DrawPlane/DrawCube/DrawLine3D.

#include <f4/renderer/ground_layout_models.hpp>

#include <f4/entities/types.hpp>
#include <f4/math/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::renderer {

namespace {

// ---------------------------------------------------------------------------
// Tunable rendering constants. The values match typical real-world
// airfield markings (FAA AC 150/5340-1L & ICAO Annex 14 vol 1).
// ---------------------------------------------------------------------------

constexpr float RUNWAY_CENTERLINE_HALF_WIDTH_FT  = 2.0f;     // 4 ft painted centerline
constexpr float RUNWAY_THRESHOLD_BAR_WIDTH_FT    = 30.0f;    // each bar's across-runway width
constexpr float RUNWAY_THRESHOLD_BAR_LENGTH_FT  = 50.0f;    // each bar's along-runway length
constexpr int   RUNWAY_N_THRESHOLD_BARS         = 8;        // 4 per side
constexpr float RUNWAY_THRESHOLD_BAR_OFFSET_FT  = 10.0f;    // first bar past the threshold
constexpr float RUNWAY_DASH_LENGTH_FT            = 120.0f;   // painted centerline dash
constexpr float RUNWAY_DASH_GAP_FT               = 80.0f;    // gap between dashes
constexpr float RUNWAY_DASH_START_MARGIN_FT      = 80.0f;    // skip first/last N ft
constexpr float RUNWAY_END_MARKER_SIZE_FT        = 12.0f;

constexpr float TAXIWAY_DEFAULT_HALF_WIDTH_FT    = 25.0f;    // 50 ft taxiway width
constexpr float TAXIWAY_CENTERLINE_HALF_WIDTH_FT = 1.5f;     // 3 ft painted yellow line

constexpr float PARKING_MARKER_SIZE_FT           = 18.0f;    // cube half-side
constexpr float HELIPAD_MARKER_SIZE_FT           = 25.0f;

constexpr float FEATURE_FOOTPRINT_HALF_FT       = 18.0f;    // generic building footprint

// Lift painted markings slightly above the surface to avoid z-fighting.
constexpr float Z_BIAS_MARKING = 0.5f;
constexpr float Z_BIAS_MARKER  = 0.5f;

// ---------------------------------------------------------------------------
// Color helpers — packed RGBA bytes
// ---------------------------------------------------------------------------

constexpr uint8_t C_RUNWAY_SURFACE[4]   = { 50,  50,  55, 255};  // dark grey asphalt
constexpr uint8_t C_THRESHOLD_BAR[4]    = {235, 235, 235, 255};  // white
constexpr uint8_t C_CENTERLINE_DASH[4]   = {235, 235, 235, 255};  // white
constexpr uint8_t C_RUNWAY_DIM[4]        = {200, 200, 200, 200};  // pale grey, dimmer
constexpr uint8_t C_TAXIWAY_SURFACE[4]   = { 60,  55,  45, 230};  // dark brown (asphalt)
constexpr uint8_t C_TAXIWAY_CENTERLINE[4] = {220, 200,  40, 255};  // yellow
constexpr uint8_t C_PARKING[4]           = { 60, 200,  80, 255};  // green
constexpr uint8_t C_HELIPAD[4]           = { 80, 200, 220, 255};  // cyan
constexpr uint8_t C_RUNWAY_END[4]        = {220,  60,  60, 255};  // red
constexpr uint8_t C_RUNWAY_LABEL[4]      = {255, 255, 255, 255};  // white label

// Damage-state colors for feature footprints (mirrors the 2D panel).
constexpr uint8_t C_FEATURE_INTACT[4]    = { 80, 200,  80, 220};
constexpr uint8_t C_FEATURE_DAMAGED[4]   = {220, 200,  40, 220};
constexpr uint8_t C_FEATURE_DESTROYED[4]  = {220,  80,  40, 220};
constexpr uint8_t C_FEATURE_RUBBLE[4]    = {140,  40,  20, 220};

inline void set_color(uint8_t out[4], const uint8_t in[4]) {
    out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; out[3] = in[3];
}

inline void set_quad_color(LayoutQuad& q, const uint8_t c[4]) {
    q.r = c[0]; q.g = c[1]; q.b = c[2]; q.a = c[3];
}

inline void set_line_color(LayoutLine& l, const uint8_t c[4]) {
    l.r = c[0]; l.g = c[1]; l.b = c[2]; l.a = c[3];
}

inline void set_marker_color(LayoutMarker& m, const uint8_t c[4]) {
    m.r = c[0]; m.g = c[1]; m.b = c[2]; m.a = c[3];
}

// ---------------------------------------------------------------------------
// Small vector helpers (2D, ENU feet) — now using f4::math::Vec2f
// ---------------------------------------------------------------------------

using Vec2f = f4::math::Vec2f;

/// Unit perpendicular (rotate 90° CCW) of the segment a→b.
/// Returns {0,0} if the segment is degenerate.
inline Vec2f unit_perp_ccw(Vec2f a, Vec2f b) {
    const Vec2f d = b - a;
    const float L = d.length();
    if (L < 1e-6f) return {0.0f, 0.0f};
    return d.perp_ccw().normalized();
}

// ---------------------------------------------------------------------------
// Bbox accumulator
// ---------------------------------------------------------------------------

struct Bbox2 {
    float min_x = 1e30f, min_y = 1e30f;
    float max_x = -1e30f, max_y = -1e30f;
    bool any = false;

    void add(float x, float y) {
        if (x < min_x) min_x = x;
        if (y < min_y) min_y = y;
        if (x > max_x) max_x = x;
        if (y > max_y) max_y = y;
        any = true;
    }

    void add_point(const f4::entities::GroundLayoutPoint& p) {
        add(p.x, p.y);
    }
};

// ---------------------------------------------------------------------------
// Runway-group builder
// ---------------------------------------------------------------------------

/// References to the lists that describe one runway. Indexed by
/// PointListType — at most one of each per runway_num (in practice
/// there is exactly one LT, one RT, one CL, one Dim, but the data is
/// forgiving and any missing piece just degrades gracefully).
///
/// IMPORTANT: Real Falcon4 PHD data uses MULTIPLE CL lists per runway
/// — one per threshold (the same physical runway described from each
/// end, at headings 180° apart). We store all of them in `cl_lists`
/// so we can emit a runway-end marker for each. The surface itself is
/// built from the FIRST CL list (or LT+RT edges if present, which is
/// rare in real data).
struct RunwayGroup {
    int    runway_num = -1;   // from GroundLayoutList::runway_num
    float  heading_deg = 0.0f;
    const f4::entities::GroundLayoutList* lt = nullptr;
    const f4::entities::GroundLayoutList* rt = nullptr;
    std::vector<const f4::entities::GroundLayoutList*> cl_lists;  // 1+ per runway
    const f4::entities::GroundLayoutList* dim = nullptr;
};

/// Build a runway surface quad from the LT/RT edge lists.
///
/// If both lists have ≥ 2 points, we use LT[0], RT[0], RT[last], LT[last]
/// as the four corners (CCW from above with +Z up — assuming the lists
/// are ordered threshold→end). If only one list is present, we fall back
/// to building the surface from the centerline list (CL) with a default
/// width. If neither is present, we skip the surface entirely.
void build_runway_surface(const RunwayGroup& g,
                          AirfieldGeometry3D& out,
                          Bbox2& bbox) {
    if (g.lt && g.rt && g.lt->points.size() >= 2 && g.rt->points.size() >= 2) {
        // Common case: left + right edge lists, each with 2+ points.
        // Use the first and last points on each side. (If the lists have
        // more than 2 points — intermediate samples along the edge —
        // we'd ideally build a polygon strip, but in practice the PHD
        // data only stores threshold + end. We fall through to the
        // simplest 4-corner quad.)
        const auto& lp0 = g.lt->points.front();
        const auto& lp1 = g.lt->points.back();
        const auto& rp0 = g.rt->points.front();
        const auto& rp1 = g.rt->points.back();

        LayoutQuad q;
        q.x[0] = lp0.x; q.y[0] = lp0.y;
        q.x[1] = rp0.x; q.y[1] = rp0.y;
        q.x[2] = rp1.x; q.y[2] = rp1.y;
        q.x[3] = lp1.x; q.y[3] = lp1.y;
        q.z = 0.0f;
        set_quad_color(q, C_RUNWAY_SURFACE);
        out.runway_surfaces.push_back(q);

        bbox.add(lp0.x, lp0.y); bbox.add(lp1.x, lp1.y);
        bbox.add(rp0.x, rp0.y); bbox.add(rp1.x, rp1.y);
        return;
    }

    if (!g.cl_lists.empty()) {
        // Fallback: only a centerline (or several). Real Falcon4 PHD data
        // has NO LT/RT lists — every runway is described by 1+ CL lists,
        // one per threshold. We use the FIRST CL list for the surface;
        // additional CL lists each get their own runway-end marker.
        //
        // IMPORTANT — real CL list shape (verified against 02_20 Airbase 2):
        //   pts[0]      (PT_RUNWAY,     type=1)  — this threshold
        //   pts[1]      (PT_TAKEOFF,    type=2)  — OPPOSITE threshold (~runway
        //                                          length away, not a hold-short)
        //   pts[2]      (PT_TAKE_RUNWAY,type=15) — runway access point (just
        //                                          off the opposite threshold)
        //   pts[3..N-1] (PT_TAXI,       type=3)  — taxiway exit path back to
        //                                          the apron
        // The runway's two endpoints are pts[0] and pts[1]. pts[2..N-1] is
        // the embedded taxiway exit, handled separately by the third pass
        // (see build_airfield_geometry_3d). Using points.back() here would
        // place the surface's far end at the LAST taxiway node — in the
        // middle of the airbase, not at the runway end.
        const auto* cl = g.cl_lists.front();
        if (cl && cl->points.size() >= 2) {
            const auto& p0 = cl->points[0];   // this threshold
            const auto& p1 = cl->points[1];   // opposite threshold
            const Vec2f perp = unit_perp_ccw({p0.x, p0.y}, {p1.x, p1.y});
            if (perp.length() >= 0.5f) {
                // Default half-width = 50 ft (100 ft runway — typical fighter base).
                constexpr float DEFAULT_HALF = 50.0f;
                const Vec2f off = perp * DEFAULT_HALF;
                // Corner order: left@start, right@start, right@end, left@end
                // (matches the L+R edge convention used above).
                LayoutQuad q;
                q.x[0] = p0.x + off.x; q.y[0] = p0.y + off.y;
                q.x[1] = p0.x - off.x; q.y[1] = p0.y - off.y;
                q.x[2] = p1.x - off.x; q.y[2] = p1.y - off.y;
                q.x[3] = p1.x + off.x; q.y[3] = p1.y + off.y;
                q.z = 0.0f;
                set_quad_color(q, C_RUNWAY_SURFACE);
                out.runway_surfaces.push_back(q);

                bbox.add(q.x[0], q.y[0]); bbox.add(q.x[1], q.y[1]);
                bbox.add(q.x[2], q.y[2]); bbox.add(q.x[3], q.y[3]);
            }
        }
        return;
    }
    // No edges, no centerline — nothing to draw.
}

/// Build N threshold bars perpendicular to the runway, just past the
/// threshold end. Skipped if we don't have both LT[0] and RT[0] (we
/// need to know the actual runway width to space the bars).
void build_threshold_bars(const RunwayGroup& g,
                          AirfieldGeometry3D& out,
                          Bbox2& bbox) {
    if (!g.lt || !g.rt || g.lt->points.empty() || g.rt->points.empty()) return;
    const auto& lp0 = g.lt->points.front();
    const auto& rp0 = g.rt->points.front();
    const Vec2f lt0{lp0.x, lp0.y};
    const Vec2f rt0{rp0.x, rp0.y};
    const Vec2f across = rt0 - lt0;
    const float  width = across.length();
    if (width < 10.0f) return;

    // Along-runway direction (LT[1] - LT[0]). Fall back to RT[1] - RT[0]
    // if LT has only 1 point.
    Vec2f along{0.0f, 0.0f};
    if (g.lt->points.size() >= 2) {
        along = Vec2f{g.lt->points[1].x, g.lt->points[1].y} - lt0;
    } else if (g.rt->points.size() >= 2) {
        along = Vec2f{g.rt->points[1].x, g.rt->points[1].y} - rt0;
    }
    const float along_len = along.length();
    if (along_len < 1.0f) return;
    const Vec2f fwd = along * (1.0f / along_len);

    // Bars span 90% of the runway width with small gaps between.
    const float usable = width * 0.9f;
    const int n = RUNWAY_N_THRESHOLD_BARS;
    const float gap = (usable - n * RUNWAY_THRESHOLD_BAR_WIDTH_FT) /
                     static_cast<float>(n - 1);
    const float start_off = RUNWAY_THRESHOLD_BAR_OFFSET_FT;

    for (int i = 0; i < n; ++i) {
        // Cross-runway position relative to the runway centerline.
        const float cross = -usable * 0.5f +
                            static_cast<float>(i) *
                            (RUNWAY_THRESHOLD_BAR_WIDTH_FT + gap) +
                            RUNWAY_THRESHOLD_BAR_WIDTH_FT * 0.5f;
        // Center of this bar (in ENU feet, relative to objective center).
        const Vec2f center_dir_lt_to_rt = across * (1.0f / width);
        const Vec2f mid{lt0.x + across.x * 0.5f,
                        lt0.y + across.y * 0.5f};
        const Vec2f c = mid + center_dir_lt_to_rt * cross + fwd * start_off;

        const float hl = RUNWAY_THRESHOLD_BAR_LENGTH_FT * 0.5f;
        const float hw = RUNWAY_THRESHOLD_BAR_WIDTH_FT * 0.5f;
        const Vec2f fwd_hl  = fwd * hl;
        const Vec2f side_hw = center_dir_lt_to_rt * hw;

        LayoutQuad q;
        q.x[0] = c.x - fwd_hl.x - side_hw.x; q.y[0] = c.y - fwd_hl.y - side_hw.y;
        q.x[1] = c.x - fwd_hl.x + side_hw.x; q.y[1] = c.y - fwd_hl.y + side_hw.y;
        q.x[2] = c.x + fwd_hl.x + side_hw.x; q.y[2] = c.y + fwd_hl.y + side_hw.y;
        q.x[3] = c.x + fwd_hl.x - side_hw.x; q.y[3] = c.y + fwd_hl.y - side_hw.y;
        q.z = Z_BIAS_MARKING;
        set_quad_color(q, C_THRESHOLD_BAR);
        out.threshold_bars.push_back(q);

        bbox.add(q.x[0], q.y[0]); bbox.add(q.x[2], q.y[2]);
    }
}

/// Build centerline dashes along the runway. Uses the FIRST CL list
/// (type 1) if present; otherwise falls back to the LT list's first/last
/// points. Each dash is a small quad perpendicular to the runway,
/// spaced along the length.
void build_centerline_dashes(const RunwayGroup& g,
                              AirfieldGeometry3D& out,
                              Bbox2& bbox) {
    Vec2f thr{0.0f, 0.0f}, end{0.0f, 0.0f};
    bool have_pts = false;

    if (!g.cl_lists.empty() && g.cl_lists.front()->points.size() >= 2) {
        // Runway endpoints are pts[0] (this threshold) and pts[1] (opposite
        // threshold). See build_runway_surface for the full CL-list shape
        // comment. Using points.back() would draw the dashes all the way
        // down the embedded taxiway exit, not along the runway.
        const auto* cl = g.cl_lists.front();
        thr = {cl->points[0].x, cl->points[0].y};
        end = {cl->points[1].x, cl->points[1].y};
        have_pts = true;
    } else if (g.lt && g.lt->points.size() >= 2) {
        // Fall back to the midpoint of LT[0]→RT[0] and LT[1]→RT[1].
        if (g.rt && g.rt->points.size() >= 2) {
            const auto& lp0 = g.lt->points[0];
            const auto& lp1 = g.lt->points[1];
            const auto& rp0 = g.rt->points[0];
            const auto& rp1 = g.rt->points[1];
            thr = {(lp0.x + rp0.x) * 0.5f, (lp0.y + rp0.y) * 0.5f};
            end = {(lp1.x + rp1.x) * 0.5f, (lp1.y + rp1.y) * 0.5f};
            have_pts = true;
        }
    }
    if (!have_pts) return;

    const Vec2f d = end - thr;
    const float L = d.length();
    if (L < RUNWAY_DASH_LENGTH_FT + RUNWAY_DASH_GAP_FT) return;

    const Vec2f fwd = d * (1.0f / L);
    const Vec2f side = fwd.perp_ccw();  // already unit length

    const float start = RUNWAY_DASH_START_MARGIN_FT;
    const float stop  = L - RUNWAY_DASH_START_MARGIN_FT;
    const float hw = RUNWAY_CENTERLINE_HALF_WIDTH_FT;

    for (float s = start; s + RUNWAY_DASH_LENGTH_FT <= stop;
         s += RUNWAY_DASH_LENGTH_FT + RUNWAY_DASH_GAP_FT) {
        const float s_mid = s + RUNWAY_DASH_LENGTH_FT * 0.5f;
        const Vec2f c = thr + fwd * s_mid;
        const float hl = RUNWAY_DASH_LENGTH_FT * 0.5f;

        LayoutQuad q;
        q.x[0] = c.x - fwd.x * hl - side.x * hw;
        q.y[0] = c.y - fwd.y * hl - side.y * hw;
        q.x[1] = c.x - fwd.x * hl + side.x * hw;
        q.y[1] = c.y - fwd.y * hl + side.y * hw;
        q.x[2] = c.x + fwd.x * hl + side.x * hw;
        q.y[2] = c.y + fwd.y * hl + side.y * hw;
        q.x[3] = c.x + fwd.x * hl - side.x * hw;
        q.y[3] = c.y + fwd.y * hl - side.y * hw;
        q.z = Z_BIAS_MARKING;
        set_quad_color(q, C_CENTERLINE_DASH);
        out.centerline_dashes.push_back(q);

        bbox.add(q.x[0], q.y[0]); bbox.add(q.x[2], q.y[2]);
    }
}

/// Build a runway-end marker (red cube) at the far end of EACH CL list,
/// with a label like "RWY 09" derived from the heading. Real PHD data
/// has 1+ CL lists per runway (one per threshold at headings 180°
/// apart); each gets its own marker.
void build_runway_end_marker(const RunwayGroup& g,
                             AirfieldGeometry3D& out,
                             Bbox2& bbox) {
    // Prefer CL lists (one marker per CL list — covers both thresholds).
    // Marker is placed at pts[1] — the OPPOSITE threshold from the CL's own
    // heading. See build_runway_surface for the full CL-list shape comment.
    // Using points.back() here would place the marker at the end of the
    // embedded taxiway exit (in the middle of the airbase), causing all
    // four markers of a two-runway airbase to cluster at the centroid and
    // look like "converging runways".
    if (!g.cl_lists.empty()) {
        for (const auto* cl : g.cl_lists) {
            if (!cl || cl->points.size() < 2) continue;
            const auto& end_pt = cl->points[1];
            LayoutMarker m;
            m.x = end_pt.x; m.y = end_pt.y; m.z = Z_BIAS_MARKER;
            m.size_ft = RUNWAY_END_MARKER_SIZE_FT;
            m.heading_deg = cl->heading_deg;
            m.shape = 0;  // cube
            set_marker_color(m, C_RUNWAY_END);

            // Runway number label (e.g. "RWY 09" for a 90° heading, "RWY 18"
            // for 180°). Round to nearest 10°, divide by 10, zero-pad to 2
            // digits. Heading comes from the CL list's own heading_deg
            // (which the JSON encoder emits from PHD's `data` field).
            int rwy_num = static_cast<int>(std::round(cl->heading_deg / 10.0f)) % 36;
            if (rwy_num == 0) rwy_num = 36;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "RWY %02d", rwy_num);
            m.label = buf;

            out.runway_ends.push_back(m);
            bbox.add(end_pt.x, end_pt.y);
        }
        return;
    }

    // Fallback when only LT (no CL): single marker at LT's last point.
    if (g.lt && g.lt->points.size() >= 2) {
        const auto& end_pt = g.lt->points.back();
        LayoutMarker m;
        m.x = end_pt.x; m.y = end_pt.y; m.z = Z_BIAS_MARKER;
        m.size_ft = RUNWAY_END_MARKER_SIZE_FT;
        m.heading_deg = g.heading_deg;
        m.shape = 0;
        set_marker_color(m, C_RUNWAY_END);

        int rwy_num = static_cast<int>(std::round(g.heading_deg / 10.0f)) % 36;
        if (rwy_num == 0) rwy_num = 36;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "RWY %02d", rwy_num);
        m.label = buf;

        out.runway_ends.push_back(m);
        bbox.add(end_pt.x, end_pt.y);
    }
}

/// Dispatch all the runway-group builders for one runway_num.
void build_runway_group(const RunwayGroup& g,
                        AirfieldGeometry3D& out,
                        Bbox2& bbox) {
    build_runway_surface(g, out, bbox);
    build_threshold_bars(g, out, bbox);
    build_centerline_dashes(g, out, bbox);
    build_runway_end_marker(g, out, bbox);
}

// ---------------------------------------------------------------------------
// Taxiway strip builder
// ---------------------------------------------------------------------------

/// Build a filled strip with width perpendicular to the path through
/// the given points. Each segment becomes one quad (overlap at joints
/// is acceptable visually and avoids needing to miter the corners).
void build_taxiway_strip(const f4::entities::GroundLayoutList& list,
                         AirfieldGeometry3D& out,
                         Bbox2& bbox) {
    if (list.points.size() < 2) return;

    const float half_w = TAXIWAY_DEFAULT_HALF_WIDTH_FT;
    for (std::size_t i = 0; i + 1 < list.points.size(); ++i) {
        const auto& p0 = list.points[i];
        const auto& p1 = list.points[i + 1];
        const Vec2f a{p0.x, p0.y};
        const Vec2f b{p1.x, p1.y};
        const Vec2f side = unit_perp_ccw(a, b);
        if (side.length() < 0.5f) continue;
        const Vec2f off = side * half_w;
        // Corner order: left@start, right@start, right@end, left@end
        // (matches build_runway_surface — CCW from above with +Z up).
        LayoutQuad q;
        q.x[0] = a.x + off.x; q.y[0] = a.y + off.y;
        q.x[1] = a.x - off.x; q.y[1] = a.y - off.y;
        q.x[2] = b.x - off.x; q.y[2] = b.y - off.y;
        q.x[3] = b.x + off.x; q.y[3] = b.y + off.y;
        q.z = 0.0f;
        set_quad_color(q, C_TAXIWAY_SURFACE);
        out.taxiway_strips.push_back(q);

        bbox.add(q.x[0], q.y[0]); bbox.add(q.x[2], q.y[2]);
    }

    // Yellow centerline strip along the path (one LayoutLine per segment).
    for (std::size_t i = 0; i + 1 < list.points.size(); ++i) {
        const auto& p0 = list.points[i];
        const auto& p1 = list.points[i + 1];
        LayoutLine l;
        l.x0 = p0.x; l.y0 = p0.y;
        l.x1 = p1.x; l.y1 = p1.y;
        l.z = Z_BIAS_MARKING;
        l.dashed = false;
        set_line_color(l, C_TAXIWAY_CENTERLINE);
        out.taxiway_centerlines.push_back(l);

        bbox.add(p0.x, p0.y); bbox.add(p1.x, p1.y);
    }
}

// ---------------------------------------------------------------------------
// Parking / helipad marker builders
// ---------------------------------------------------------------------------

void build_parking_markers(const f4::entities::GroundLayoutList& list,
                           AirfieldGeometry3D& out,
                           Bbox2& bbox,
                           int& counter) {
    for (const auto& pt : list.points) {
        LayoutMarker m;
        m.x = pt.x; m.y = pt.y; m.z = Z_BIAS_MARKER;
        m.size_ft = PARKING_MARKER_SIZE_FT;
        m.shape = 0;  // cube
        // Heading: use the list's heading if non-zero, else 0.
        m.heading_deg = list.heading_deg;
        set_marker_color(m, C_PARKING);

        char buf[16];
        std::snprintf(buf, sizeof(buf), "P%d", ++counter);
        m.label = buf;

        out.parking_spots.push_back(m);
        bbox.add(pt.x, pt.y);
    }
}

void build_helipad_markers(const f4::entities::GroundLayoutList& list,
                           AirfieldGeometry3D& out,
                           Bbox2& bbox,
                           int& counter) {
    for (const auto& pt : list.points) {
        LayoutMarker m;
        m.x = pt.x; m.y = pt.y; m.z = Z_BIAS_MARKER;
        m.size_ft = HELIPAD_MARKER_SIZE_FT;
        m.shape = 1;  // cylinder
        m.heading_deg = 0.0f;
        set_marker_color(m, C_HELIPAD);

        char buf[16];
        std::snprintf(buf, sizeof(buf), "H%d", ++counter);
        m.label = buf;

        out.helipads.push_back(m);
        bbox.add(pt.x, pt.y);
    }
}

// ---------------------------------------------------------------------------
// Feature footprint builder
// ---------------------------------------------------------------------------

void build_feature_footprints(const std::vector<f4::entities::FeatureEntryState>& features,
                              AirfieldGeometry3D& out,
                              Bbox2& bbox) {
    for (const auto& f : features) {
        // Skip empty placeholder features (the bridge emits these when
        // the FED entry is unused).
        if (f.index == 0 && f.offset_x == 0.0f && f.offset_y == 0.0f) continue;

        // Pick a color based on the damage state.
        const uint8_t* color = C_FEATURE_INTACT;
        switch (f.damage_state) {
            case 0:  color = C_FEATURE_INTACT;    break;
            case 1:  color = C_FEATURE_DAMAGED;   break;
            case 2:  color = C_FEATURE_DESTROYED; break;
            default: color = C_FEATURE_RUBBLE;    break;
        }

        // Build a small oriented quad (footprint) at the feature's
        // offset, oriented by `facing` (degrees). Half-extent is fixed
        // (FeatureEntryState doesn't carry building dimensions).
        const float rad = -f.facing * 3.14159265358979f / 180.0f;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        const float hl = FEATURE_FOOTPRINT_HALF_FT;
        const float hw = FEATURE_FOOTPRINT_HALF_FT;

        // Local (dx, dy) → world (wx, wy)
        auto rot = [&](float dx, float dy) {
            return std::make_pair(
                f.offset_x + dx * c - dy * s,
                f.offset_y + dx * s + dy * c);
        };

        LayoutQuad q;
        const auto [x0, y0] = rot(-hl, -hw);
        const auto [x1, y1] = rot(+hl, -hw);
        const auto [x2, y2] = rot(+hl, +hw);
        const auto [x3, y3] = rot(-hl, +hw);
        q.x[0] = x0; q.y[0] = y0;
        q.x[1] = x1; q.y[1] = y1;
        q.x[2] = x2; q.y[2] = y2;
        q.x[3] = x3; q.y[3] = y3;
        q.z = Z_BIAS_MARKING;
        set_quad_color(q, color);
        out.feature_footprints.push_back(q);

        bbox.add(f.offset_x, f.offset_y);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

AirfieldGeometry3D build_airfield_geometry_3d(
    const std::vector<f4::entities::GroundLayoutList>& layouts,
    const std::vector<f4::entities::FeatureEntryState>* features) {

    AirfieldGeometry3D out;
    Bbox2 bbox;
    int park_counter = 0;
    int heli_counter = 0;

    // --- Group runway-related lists by runway_num ---------------------------
    //
    // We use a small map keyed by runway_num. The map is cleared per
    // call (no caching) so the function stays pure.
    //
    // IMPORTANT: Real Falcon4 PHD data uses runway_num=0 for the FIRST
    // runway (not -1 as the comment in theater_data.hpp suggests). The
    // int8_t value -1 (which becomes uint8_t 255 after JSON round-trip)
    // is the actual "not a runway" sentinel. We must NOT skip runway
    // lists with runway_num==0 — that was the previous bug, which
    // caused the entire first runway to be silently dropped.
    //
    // Within a runway group, real PHD data typically has 2 CL lists
    // (one per threshold, at headings 180° apart). We collect them all
    // into cl_lists so build_runway_end_marker can emit one marker per
    // threshold. The surface itself uses just the first CL list (or
    // LT+RT edges if present, which is rare in real data).
    std::unordered_map<int, RunwayGroup> runway_groups;

    // First pass: collect runway lists into groups.
    for (const auto& list : layouts) {
        const uint8_t t = list.type;
        if (is_runway_edge_type(t) || is_runway_centerline_type(t) || is_runway_dim_type(t)) {
            // Skip the "not a runway" sentinel (int8_t -1 → uint8_t 255).
            // Real runways use 0-based indices (0, 1, 2, ...).
            if (list.runway_num == 255u) {
                continue;
            }
            auto& g = runway_groups[static_cast<int>(list.runway_num)];
            g.runway_num = static_cast<int>(list.runway_num);
            // Heading: only trust the centerline list's heading (PLT_RUNWAY,
            // type=1). Other runway-related lists (PLT_RUNWAY_DIM type=8,
            // PLT_RUNWAY_LT/RT type=12/13) carry type-specific `data` values
            // that are NOT runway headings — letting them overwrite
            // g.heading_deg would silently corrupt the LT-only fallback
            // path in build_runway_end_marker.
            if (t == 1 && list.heading_deg != 0.0f) g.heading_deg = list.heading_deg;
            if (t == 12)      g.lt  = &list;
            else if (t == 13) g.rt  = &list;
            else if (t == 1)  g.cl_lists.push_back(&list);
            else if (t == 8)  g.dim = &list;
        }
    }

    // Second pass: build each runway group.
    for (const auto& [num, g] : runway_groups) {
        (void)num;
        build_runway_group(g, out, bbox);
    }

    // Third pass: build taxiway strips + placement-point markers.
    //
    // IMPORTANT — real PLT_RUNWAY (type=1) lists are HYBRID: they contain
    // the runway endpoints (pts[0], pts[1]) AND an embedded taxiway exit
    // path (pts[2..N-1], the PT_TAXI nodes leading from the runway access
    // point back to the apron). The runway portion is consumed in pass 1
    // via the runway_groups; the taxiway portion must be extracted here
    // and fed to build_taxiway_strip, otherwise the entire taxiway network
    // connected to runway exits is silently dropped (Bug: "no taxiways
    // displayed"). See build_runway_surface for the full CL-list shape.
    for (const auto& list : layouts) {
        const uint8_t t = list.type;
        if (is_runway_edge_type(t) || is_runway_dim_type(t)) {
            // Already handled above (runway group). No embedded taxiway
            // portion to extract for LT/RT or DIM lists.
            continue;
        }
        if (is_runway_centerline_type(t)) {
            // CL list — runway portion (pts[0], pts[1]) was consumed in
            // pass 1. Extract the embedded taxiway exit (pts[2..N-1]) and
            // render it as a taxiway strip. Skip if the list is too short
            // to have a taxiway portion (e.g. the synthetic 2-point test
            // shape used by RealPhdShapeRunwayNumZeroIsAccepted).
            if (list.points.size() >= 3) {
                f4::entities::GroundLayoutList taxi;
                taxi.type       = 17;     // PLT_TRACK — generic path
                taxi.runway_num = 255;    // not a runway
                taxi.heading_deg = 0.0f;
                taxi.ltrt       = 0;
                taxi.count      = static_cast<uint8_t>(list.points.size() - 2);
                taxi.points.assign(list.points.begin() + 2, list.points.end());
                build_taxiway_strip(taxi, out, bbox);
            }
            continue;
        }
        if (is_parking_type(t)) {
            build_parking_markers(list, out, bbox, park_counter);
            continue;
        }
        if (is_helipad_type(t)) {
            build_helipad_markers(list, out, bbox, heli_counter);
            continue;
        }
        if (is_placement_point_type(t)) {
            // SAM (4), Artillery (5), AAA (6), Static radar (10), Dock (16).
            // These are single-point placements — render as small markers
            // (without labels) so the user can see them in 3D, but don't
            // treat them as taxiway strips.
            for (const auto& pt : list.points) {
                LayoutMarker m;
                m.x = pt.x; m.y = pt.y; m.z = Z_BIAS_MARKER;
                m.size_ft = PARKING_MARKER_SIZE_FT * 0.7f;
                m.shape = 2;  // cone — visually distinct from parking
                m.heading_deg = 0.0f;
                // Color by type:
                //   SAM (4) → red, Artillery (5) → orange, AAA (6) → yellow,
                //   Static radar (10) → magenta, Dock (16) → blue.
                switch (t) {
                    case 4:  set_marker_color(m, C_RUNWAY_END); break;        // red
                    case 5:  set_marker_color(m, C_TAXIWAY_CENTERLINE); break; // yellow-orange
                    case 6:  {
                        uint8_t c[4] = {220, 200, 40, 255};
                        set_marker_color(m, c);
                        break;
                    }
                    case 10: {
                        uint8_t c[4] = {180, 60, 220, 255};
                        set_marker_color(m, c);
                        break;
                    }
                    case 16: {
                        uint8_t c[4] = {60, 120, 220, 255};
                        set_marker_color(m, c);
                        break;
                    }
                    default: set_marker_color(m, C_FEATURE_INTACT); break;
                }
                out.helipads.push_back(m);  // reuse helipads vector for placement markers
                bbox.add(pt.x, pt.y);
            }
            continue;
        }
        // Anything else: taxiway/path strip.
        build_taxiway_strip(list, out, bbox);
    }

    // Fourth pass: building footprints from FeatureSetComponent.
    if (features) {
        build_feature_footprints(*features, out, bbox);
    }

    // Finalize bbox.
    if (bbox.any) {
        out.min_x = bbox.min_x;
        out.min_y = bbox.min_y;
        out.max_x = bbox.max_x;
        out.max_y = bbox.max_y;
        out.empty = false;
    } else {
        out.empty = true;
    }

    return out;
}

} // namespace f4::renderer

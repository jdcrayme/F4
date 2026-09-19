// f4-simulation/include/f4/simulation/formation_layout.hpp
//
// Deaggregation formation layouts — the ONE definition of how individual
// vehicles/aircraft are placed around their aggregate's center.
//
// Consumers:
//   - f4-simulation (campaign_bridge.cpp): spawn_vehicles_from_unit /
//     derive_airfield_from_objective / spawn_aircraft_from_squadrons.
//   - f4-world-viewer (class_table_browser.cpp): the class-table row
//     previews, so what the browser draws is literally what the session
//     spawns. When the real FreeFalcon tables land, both update from
//     this file.
//
// --- SYNTHETIC GROUND FORMATIONS -------------------------------------------
//
// FreeFalcon's ground-vehicle formation tables (SquadFormations /
// PlatoonFormations / CompanyFormations, defined in gndai.cpp:110-282 of
// the original source) are NOT ported into this tree. Until they are, we
// use a small set of synthetic layouts:
//
//   • wedge4  — 4-vehicle wedge (lead + 2 wingmen + trail). Used for any
//                unit with ≤4 live vehicles (most battalions: 4 groups ×
//                1-3 live vehicles collapses to ≤4 after aggregation).
//   • grid    — N>4 vehicles arranged in a 4-wide grid, 50 ft spacing.
//                Used for larger aggregations (brigades deaggregated to
//                their component vehicles).
//
// All ground offsets are in FEET, ENU frame, RELATIVE TO THE UNIT CENTER,
// unrotated (unit-local). The caller rotates by the unit's heading
// (GroundTacticalComponent::heading, uint8_t 0-255 × 1.4°/step) via
// rotate_offset() before adding to the unit's position.
//
//   +x = east, +y = north. The unit's heading 0 = facing north (+y);
//   heading π/2 = facing east (+x). rotate_offset is the standard
//   compass rotation of the offset by the heading angle.
//
// When the real FreeFalcon formation tables are ported, replace
// formation_offset()'s body with a lookup into the ported tables. The
// spawn_vehicles_from_unit() contract (offset is in unit-local feet,
// rotated by unit heading) doesn't change.

#pragma once

#include <array>
#include <cmath>

namespace f4::simulation::formation {

struct Offset { double dx; double dy; };

constexpr double WEDGE_SPACING_FT = 30.0;  // ~tank length, plausible wedge spacing

/// 4-vehicle wedge offsets (unit-local, unrotated):
///   slot 0: lead     at ( 0, +30)
///   slot 1: wing-L   at (-30,  0)
///   slot 2: wing-R   at (+30,  0)
///   slot 3: trail    at ( 0, -30)
/// Lead faces forward (+y); wingmen trail by 30 ft; trail brings up the rear.
constexpr std::array<Offset, 4> WEDGE4{{
    {  0.0,  30.0 },
    { -30.0,  0.0 },
    {  30.0,  0.0 },
    {  0.0, -30.0 },
}};

constexpr double GRID_SPACING_FT = 50.0;
constexpr int    GRID_COLS       = 4;
constexpr double GRID_FIRST_ROW_Y_FT = -90.0;  // first grid row, behind the wedge

/// Compute the (dx, dy) offset for the i-th vehicle in a synthetic
/// formation. Wedge for i < 4, grid for i >= 4.
/// (When real FreeFalcon formation tables are ported, replace this body
/// with `return ported_table[unit_class][i]` or similar.)
[[nodiscard]] inline Offset formation_offset(int vehicle_index) {
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
    const double dy = GRID_FIRST_ROW_Y_FT - static_cast<double>(row) * GRID_SPACING_FT;
    return { dx, dy };
}

/// Rotate a unit-local (dx, dy) offset by a compass heading (radians,
/// 0 = +y / north, CW positive) into world ENU.
///
/// Compass heading θ rotates the +y axis (north) toward +x (east). So a
/// unit-local offset (dx, dy) becomes world offset:
///   world_dx =  dx · cos θ + dy · sin θ
///   world_dy = -dx · sin θ + dy · cos θ
[[nodiscard]] inline Offset rotate_offset(Offset local, double heading_rad) {
    const double ch = std::cos(heading_rad);
    const double sh = std::sin(heading_rad);
    return { local.dx * ch + local.dy * sh,
            -local.dx * sh + local.dy * ch };
}

// --- SYNTHETIC SQUADRON RAMP ROW -------------------------------------------
//
// Without real PLT_PARK data the airfield synthesis lays parking out as a
// single row: 8 spots, 300 ft right of the runway centerline, 80 ft
// along-runway spacing, each spot facing back down the runway. Overflow
// aircraft (more pilots than spots) wrap and step 60 ft to the aircraft's
// right per pass.
//
// The row's axis frame is the runway's: "along" points down the runway
// heading, "right" is right of course. Slot i sits at:
//   along = -i · RAMP_SPACING_FT  (walking back from slot 0)
//   right = +RAMP_ROW_OFFSET_FT   (constant for every slot)
// spawn_aircraft_from_squadrons anchors slot 0 at the objective center;
// preview renderers center the row on their local origin instead.

constexpr int    RAMP_ROW_SPOTS        = 8;
constexpr double RAMP_ROW_OFFSET_FT    = 300.0;  // perpendicular, right of centerline
constexpr double RAMP_SPACING_FT       = 80.0;   // along-runway spot spacing
constexpr double RAMP_OVERFLOW_STEP_FT = 60.0;   // per wrapped pass, to the right

/// Local-frame offset of ramp slot i relative to slot 0: (along, right)
/// in feet. Anchoring (objective center for the sim, row center for a
/// preview) is the caller's job.
[[nodiscard]] inline Offset ramp_slot_offset(int i) {
    return { -static_cast<double>(i) * RAMP_SPACING_FT, 0.0 };
}

/// Rotate an (along, right) runway-frame offset into world ENU given the
/// runway heading (radians, compass). "Along" is the heading direction,
/// "right" is right of course: right-of-course unit = (cos h, -sin h).
[[nodiscard]] inline Offset rotate_runway_offset(Offset local,
                                                  double heading_rad) {
    const double sh = std::sin(heading_rad);
    const double ch = std::cos(heading_rad);
    return { local.dx * sh + local.dy * ch,
             local.dx * ch - local.dy * sh };
}

} // namespace f4::simulation::formation

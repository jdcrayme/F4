// f4-campaign/include/f4/campaign/threat_map.hpp
//
// ThreatMap — the C3 campaign-layer threat model (ScoreThreatFast port).
//
// FreeFalcon pre-computes two campaign-scale maps at rebuild cadence
// (campui/campmap.cpp MakeCampMap, AddToThreatMap): SamMapData — one
// byte per 6x6-grid-unit cell (MAP_RATIO = 6) carrying TWO 2-bit
// counters, low-altitude and high-altitude air-defense density (0..3
// units whose engagement ring covers the cell) — and CampMapData — the
// ownership map (nearest objective's team, 0xF when unowned), packed 2
// cells per byte. ScoreThreatFast(x, y, altlevel, who) then scores any
// grid point from those arrays in O(1): territorial overflight check
// first (a hostile owner returns the lethal 32000), else the 2-bit
// counters weighted per altitude band.
//
// F4 ports the MODEL, not the pixel plumbing:
//
//   * Grid: 1 campaign grid unit is the campaign's native measure
//     (FreeFalcon GRID_SIZE_KM = 1.0 — Distance() between grid points
//     is "kilometers"). One threat cell spans MAP_RATIO = 6 grid units.
//   * Ownership: nearest objective's owner within FreeFalcon's search
//     radii (10 then 80 grid units), else 0xF (unowned). The reference
//     additionally consults terrain cover to leave WATER unowned; the
//     campaign layer cannot see terrain (the IDataSource boundary), so
//     water-only cells with no objective in range resolve 0xF — the
//     same result the reference's nearest-objective search produces
//     for open sea, without the terrain dependency.
//   * Threat density: every air-defense battalion (DOMAIN_LAND,
//     TYPE_BATTALION, STYPE_UNIT_AIR_DEFENSE) paints its two rings —
//     weapon_range[LowAir] and weapon_range[Air] grid units — into the
//     cells they cover, incrementing the 2-bit counter when the unit
//     can hit at that distance (hit_chance[band] nonzero). Counters
//     saturate at 3, the wire's own packing limit. The reference skips
//     MOVING battalions (the map rebuilds between moves); this slice
//     paints them where they stand — saved campaigns' AD battalions
//     are overwhelmingly static, and rebuild cadence is a later host
//     concern.
//   * Team packing: one map serves both belligerents. AD units at war
//     with the map's VIEWER team pack bits 4-7 (the threat TO the
//     viewer); the rest (the viewer's own side's AD) pack bits 0-3.
//     score(x, y, alt, who) reads the half that threatens `who`
//     — exactly the reference's FalconLocalSession bit offset rule,
//     with the viewer passed explicitly instead of taken from a
//     player session.
//
// Altitude bands (FreeFalcon find.cpp MinAltAtLevel/MaxAltAtLevel,
// feet):  GroundAltitude 0-99, LowAltitude 100-4999,
// MediumAltitude 5000-19999, HighAltitude 20000-39999,
// VeryHighAltitude 40000+. Band score formulas (ScoreThreatFast):
//   Low:      low*28 + high*2  (+10 over WAR territory)
//   Medium:   low*10 + high*23
//   High:     high*30
//   VeryHigh: high*15
// where low/high are the 2-bit counters (0..3) → scores 0..100,
// "roughly percent chance to be hit".
//
// RoE and territory (the reference's team.cpp RoEData table, verified
// against the source): stance entries are the RelType ENUM
// (cmpglobl.h: 0 NoRelations, 1 Allied, 2 Friendly, 3 Neutral,
// 4 Hostile, 5 War — see f4/world Relation), NOT a sign convention;
// RoEData[roe][stance] indexes it directly. ROE_AIR_OVERFLY is DENIED
// for exactly the two middle classes — Neutral and Hostile-but-not-war
// — while WAR territory is overflyABLE (that is how strike missions
// reach their targets) and NoRelations/Allied/Friendly allow it too.
// The enum decodes correctly now, so those classes ARE expressible;
// this slice still scores rather than walls — the 32000 lethal
// overflight denial lands with the RoE refinement tranche (score()
// grows the Neutral/Hostile wall and the A*'s >120 impassable test,
// already ported — see path_finder.hpp — becomes reachable). What the
// vocabulary carries TODAY: the +10 general threat over WAR territory
// at low altitude (ROE_AIR_FIRE is granted in war) — implemented.
//
// Determinism: a pure function of (objectives, units, teams) — no RNG,
// no clocks. The same sources build the same map, cell for cell.
//
// Dependencies: f4-world (IDataSource), f4-entities (UnitClass).
// C++20.

#pragma once

#include <f4/world/data_source.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace f4::campaign {

/// Grid units per threat-map cell (FreeFalcon campmap.h MAP_RATIO).
inline constexpr int kThreatMapRatio = 6;

/// Altitude band a threat score is requested for (FreeFalcon
/// AltitudeType; GroundAltitude scores 0 — nothing on the map threatens
/// a ground-mover this slice).
enum class AltBand : std::uint8_t {
    Ground = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    VeryHigh = 4,
};

/// Resolve an altitude in FEET to its band (GetAltitudeLevel port:
/// the band whose [min, max] range contains the altitude; the boundary
/// convention is < max, matching the reference's MaxAltAtLevel test).
[[nodiscard]] constexpr AltBand alt_band_from_feet(int feet) noexcept {
    if (feet < 100)   return AltBand::Ground;
    if (feet < 5000)  return AltBand::Low;
    if (feet < 20000) return AltBand::Medium;
    if (feet < 40000) return AltBand::High;
    return AltBand::VeryHigh;
}

/// The threat map over one campaign world, viewed from one team.
///
/// Construction snapshots the sources (objectives for ownership, units
/// for air-defense rings, teams for stance); the map is then immutable.
/// Rebuild policy is the host's: FreeFalcon rebuilds on its campaign
/// update cadence when emitters change; our current hosts build once
/// per run (saved campaigns carry static AD dispositions).
class ThreatMap {
public:
    /// Build from the world sources, from `viewer_team`'s perspective
    /// (the team whose ENEMIES pack the high bits — see the class doc).
    /// \param objectives  objective list (positions + owners)
    /// \param units       unit roster (air-defense battalions + ranges)
    /// \param teams       team slots + stance matrix (RoE source)
    /// \param viewer_team slot of the team the map is built for
    ThreatMap(const f4::world::IObjectiveSource& objectives,
              const f4::world::IUnitCoreSource& units,
              const f4::world::ITeamSource& teams,
              std::uint8_t viewer_team);

    /// ScoreThreatFast port: threat to `who`'s aircraft at grid (x, y),
    /// altitude band `alt`. See the header doc for the band formulas.
    /// Coordinates outside the derived map extent score 100 ("off the
    /// map", the reference's out-of-bounds answer).
    ///
    /// RoE note (the reference's own table, team.cpp RoEData):
    /// ROE_AIR_OVERFLY is DENIED for Neutral and Hostile-but-not-at-war
    /// — and ALLOWED for War (that is how strike missions fly through
    /// enemy land) and every friendly class. The stance entries are
    /// the RelType enum (see the header doc), so the denying classes
    /// ARE expressible — but this slice still SCORES rather than
    /// walls: the 32000 lethal denial is the RoE refinement tranche's
    /// (it makes the A*'s >120 impassable test reachable). What IS
    /// scored today: the +10 "general threat" over WAR territory at
    /// low altitude (RoEData grants ROE_AIR_FIRE in war).
    [[nodiscard]] int score(int x, int y, AltBand alt,
                            std::uint8_t who) const noexcept;

    /// Map dimensions in cells (derived from the data extent, rounded
    /// up to whole cells: max grid coordinate seen + one cell margin).
    [[nodiscard]] int cells_x() const noexcept { return cells_x_; }
    [[nodiscard]] int cells_y() const noexcept { return cells_y_; }

    /// Ownership of the cell containing grid (x, y): the objective
    /// owner's slot, or 0xF when unowned/out of extent. (The score()
    /// RoE check consumes this; exposed for QC and the viewer.)
    [[nodiscard]] std::uint8_t owner_at(int x, int y) const noexcept;

    /// Raw per-cell counters for QC + tests: the 2-bit low/high density
    /// that threatens `who` at grid (x, y). Values 0..3.
    [[nodiscard]] int low_band_density(int x, int y,
                                       std::uint8_t who) const noexcept;
    [[nodiscard]] int high_band_density(int x, int y,
                                        std::uint8_t who) const noexcept;

    /// Build statistics (QC summary block): painted air-defense
    /// battalions, cells with any threat, total ring area.
    struct Stats {
        int ad_units = 0;        ///< battalions painted
        int threatened_cells = 0;///< cells carrying any painted density
                                 ///  (either half — the viewer's own
                                 ///  side's rings count: they are what
                                 ///  the viewer's ENEMIES fly through)
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    /// Paint one battalion's rings (AddToThreatMap port).
    void paint_unit_(int x, int y, std::uint8_t owner,
                     int low_range, int high_range,
                     int low_hit, int high_hit);

    /// Resolve nearest-objective ownership for every cell
    /// (MakeCampMap(MAP_OWNERSHIP) port, radius 10 then 80).
    void build_ownership_(const f4::world::IObjectiveSource& objectives);

    /// Hostility test: is team `a` AT WAR with team `b`? (RelType 5 —
    /// the vocabulary's top rung; out-of-range wire values decode as
    /// NoRelations, so garbage toward unused slots never reads as war.
    /// Engagement, territory bonuses, and target enmity all key on it.)
    [[nodiscard]] bool war_(std::uint8_t a, std::uint8_t b) const;

    int cells_x_ = 0;
    int cells_y_ = 0;

    /// Per-cell ownership nibble (0xF = unowned). Index cy*cells_x+cx.
    std::vector<std::uint8_t> owner_;

    /// Per-cell packed byte, FreeFalcon layout: bits 0-1 low-alt and
    /// 2-3 high-alt density for units NOT hostile to the viewer (the
    /// viewer's own side), bits 4-5 / 6-7 for units that CAN ENGAGE the
    /// viewer (its enemies).
    std::vector<std::uint8_t> sam_;

    /// Team slot the map was built for (bit-offset rule).
    std::uint8_t viewer_team_ = 0;

    /// Stance rows by slot (slot → row) for the hostility test.
    std::vector<std::vector<int16_t>> stance_by_slot_;

    Stats stats_;
};

} // namespace f4::campaign

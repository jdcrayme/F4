// f4-campaign/src/threat_map.cpp
//
// ThreatMap implementation — see threat_map.hpp for the FreeFalcon
// correspondence map (MakeCampMap / AddToThreatMap / ScoreThreatFast).

#include "f4/campaign/threat_map.hpp"

#include <algorithm>
#include <cmath>

namespace f4::campaign {

namespace {

// MoveType indices into the UCD arrays (falcent.h): the threat model
// consumes the two air bands.
constexpr int kMoveLowAir = 4;
constexpr int kMoveAir = 5;

// FreeFalcon's ownership search radii (campmap.cpp MAP_OWNERSHIP):
// nearest objective within 10 grid units, else within 80.
constexpr double kOwnerSearchNear = 10.0;
constexpr double kOwnerSearchFar = 80.0;

// The unowned marker (campmap.cpp writes 0xF when no objective is in
// range — land or sea without a nearby objective).
constexpr std::uint8_t kUnowned = 0x0F;

} // namespace

ThreatMap::ThreatMap(const f4::world::IObjectiveSource& objectives,
                     const f4::world::IUnitCoreSource& units,
                     const f4::world::ITeamSource& teams,
                     std::uint8_t viewer_team)
    : viewer_team_(viewer_team) {

    // Stance rows by slot: slot -> that team's stance vector (indexed
    // by slot). Slots without a team row resolve to an empty vector =
    // neutral toward everyone (GetRoE's TeamInfo[a] == 0 case returns
    // ROE_ALLOWED).
    int max_slot = 0;
    for (int t = 0; t < teams.team_count(); ++t) {
        max_slot = std::max(max_slot, teams.slot(t));
    }
    stance_by_slot_.assign(static_cast<std::size_t>(max_slot) + 1, {});
    for (int t = 0; t < teams.team_count(); ++t) {
        const int slot = teams.slot(t);
        if (slot >= 0 &&
            static_cast<std::size_t>(slot) < stance_by_slot_.size()) {
            stance_by_slot_[static_cast<std::size_t>(slot)] = teams.stance(t);
        }
    }

    // Map extent: the data's own extent (objectives + units) rounded up
    // to whole cells plus a one-cell margin, so every source point and
    // its surrounding cells are on the map. FreeFalcon sizes the map
    // from the theater (TheaterSizeX/Y); the campaign layer can only
    // see the data, and paths only ever span data points.
    int max_x = 0, max_y = 0;
    for (int i = 0; i < objectives.objective_count(); ++i) {
        max_x = std::max(max_x, static_cast<int>(objectives.x(i)));
        max_y = std::max(max_y, static_cast<int>(objectives.y(i)));
    }
    for (int i = 0; i < units.unit_count(); ++i) {
        max_x = std::max(max_x, static_cast<int>(units.x(i)));
        max_y = std::max(max_y, static_cast<int>(units.y(i)));
    }
    cells_x_ = max_x / kThreatMapRatio + 2;
    cells_y_ = max_y / kThreatMapRatio + 2;

    owner_.assign(static_cast<std::size_t>(cells_x_) * cells_y_, kUnowned);
    sam_.assign(static_cast<std::size_t>(cells_x_) * cells_y_, 0);

    build_ownership_(objectives);

    // Paint the air-defense rings (MakeCampMap(MAP_SAMCOVERAGE) over the
    // AirDefenseList: battalions of STYPE_UNIT_AIR_DEFENSE, land domain).
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.domain(i) != 3 /* DOMAIN_LAND */) continue;
        if (units.unit_class(i) != f4::entities::UnitClass::Battalion)
            continue;
        if (units.unit_subtype(i) != 1 /* STYPE_LAND_AIR_DEFENSE */) continue;

        const auto& range = units.unit_weapon_range(i);
        const auto& hit = units.unit_hit_chance(i);
        // Both rings zero: a destroyed/bare battalion paints nothing
        // (the reference's GetAproxHitChance gate would reject every
        // cell anyway — skip the loop entirely).
        if (range[kMoveLowAir] == 0 && range[kMoveAir] == 0) continue;

        ++stats_.ad_units;
        paint_unit_(units.x(i), units.y(i), units.owner(i),
                    range[kMoveLowAir], range[kMoveAir],
                    hit[kMoveLowAir], hit[kMoveAir]);
    }

    // Coverage stat: cells carrying any painted density — either
    // half (the viewer's enemies at bits 4-7 OR the viewer's own side
    // at bits 0-3 — the latter is what the viewer's ENEMIES fly
    // through). Both halves are live threat data; a count scoped to
    // one half would read 0 whenever the enemy's AD is unpaintable
    // (fixture theater-db coverage) while routes still bend around
    // the other half's rings.
    for (const std::uint8_t s : sam_) {
        if (s != 0) ++stats_.threatened_cells;
    }
}

void ThreatMap::build_ownership_(
        const f4::world::IObjectiveSource& objectives) {
    // Nearest objective (Euclidean, wire-order tie-break: strictly
    // closer wins) within FreeFalcon's far radius; owner's slot or
    // 0xF. One pass over the cells, one pass over the objectives per
    // cell — objective counts are hundreds and cell counts thousands,
    // and this runs once per map build.
    for (int cy = 0; cy < cells_y_; ++cy) {
        for (int cx = 0; cx < cells_x_; ++cx) {
            const double px = cx * kThreatMapRatio;
            const double py = cy * kThreatMapRatio;
            int best = -1;
            double best_d2 = kOwnerSearchFar * kOwnerSearchFar;
            for (int i = 0; i < objectives.objective_count(); ++i) {
                const double dx = px - objectives.x(i);
                const double dy = py - objectives.y(i);
                const double d2 = dx * dx + dy * dy;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = i;
                }
            }
            const std::size_t idx =
                static_cast<std::size_t>(cy) * cells_x_ + cx;
            owner_[idx] = best >= 0 ? objectives.owner(best) : kUnowned;
        }
    }
}

void ThreatMap::paint_unit_(int x, int y, std::uint8_t owner,
                            int low_range, int high_range,
                            int low_hit, int high_hit) {
    (void)owner;  // ownership lives in owner_; the paint only writes sam_
    const int cx = x / kThreatMapRatio;
    const int cy = y / kThreatMapRatio;

    // Ring radii in cells (AddToThreatMap: GetWeaponRange / MAP_RATIO).
    const double ring_low = static_cast<double>(low_range) / kThreatMapRatio;
    const double ring_high = static_cast<double>(high_range) / kThreatMapRatio;
    const int bd = std::max(static_cast<int>(ring_high),
                            static_cast<int>(ring_low));
    const int fx = std::max(cx - bd - 1, 0);
    const int lx = std::min(cx + bd + 1, cells_x_ - 1);
    const int fy = std::max(cy - bd - 1, 0);
    const int ly = std::min(cy + bd + 1, cells_y_ - 1);

    // Bit offsets (AddToThreatMap): units at war with the viewer pack
    // bits 4-7 (the threat TO the viewer's side); the rest pack bits
    // 0-3. Engagement test: the battalion's stance toward the viewer
    // is wartime (GetRoE(battalion, viewer, ROE_AIR_ENGAGE) — granted
    // for War and Hostile; our vocabulary carries War).
    const bool engages_viewer = war_(owner, viewer_team_);
    const int li = engages_viewer ? 4 : 0;
    const int hi = engages_viewer ? 6 : 2;

    for (int gx = fx; gx <= lx; ++gx) {
        for (int gy = fy; gy <= ly; ++gy) {
            const std::size_t idx =
                static_cast<std::size_t>(gy) * cells_x_ + gx;
            // Distance in cells minus the reference's one-cell fudge.
            const double d = std::sqrt(
                static_cast<double>((gx - cx) * (gx - cx) +
                                    (gy - cy) * (gy - cy))) - 1.0;

            // Low-altitude counter: inside the ring, saturation 3, and
            // the hit-chance gate (GetAproxHitChance nonzero —
            // class-level approximation: hit_chance[band] != 0 and the
            // true range test d*MAP_RATIO < Range[band]).
            const double km = std::max(d, 0.0) * kThreatMapRatio;
            if (ring_low >= d && low_hit != 0 && km < low_range) {
                const int c = (sam_[idx] >> li) & 0x03;
                if (c < 3) {
                    sam_[idx] = static_cast<std::uint8_t>(
                        (sam_[idx] & ~(0x03 << li)) | ((c + 1) << li));
                }
            }
            if (ring_high >= d && high_hit != 0 && km < high_range) {
                const int c = (sam_[idx] >> hi) & 0x03;
                if (c < 3) {
                    sam_[idx] = static_cast<std::uint8_t>(
                        (sam_[idx] & ~(0x03 << hi)) | ((c + 1) << hi));
                }
            }
        }
    }
}

bool ThreatMap::war_(std::uint8_t a, std::uint8_t b) const {
    // RelType::War (5) — the vocabulary's own top rung (cmpglobl.h;
    // the stance arrays are enum values, not signs — see
    // data_source.hpp Relation). Slots out of range / empty rows / the
    // garbage real saves carry toward unused slots: not at war
    // (relation_from_wire maps out-of-range to NoRelations).
    if (a == b) return false;
    if (static_cast<std::size_t>(a) >= stance_by_slot_.size()) return false;
    const auto& row = stance_by_slot_[static_cast<std::size_t>(a)];
    if (static_cast<std::size_t>(b) >= row.size()) return false;
    return f4::world::relation_from_wire(
               row[static_cast<std::size_t>(b)]) ==
           f4::world::Relation::War;
}

std::uint8_t ThreatMap::owner_at(int x, int y) const noexcept {
    const int cx = x / kThreatMapRatio;
    const int cy = y / kThreatMapRatio;
    if (cx < 0 || cy < 0 || cx >= cells_x_ || cy >= cells_y_) return kUnowned;
    return owner_[static_cast<std::size_t>(cy) * cells_x_ + cx];
}

int ThreatMap::score(int x, int y, AltBand alt,
                     std::uint8_t who) const noexcept {
    // No overflight denial this slice: the reference's RoEData denies
    // ROE_AIR_OVERFLY for Neutral and Hostile-but-not-at-war — the two
    // MIDDLE classes of the enum, expressible now that the vocabulary
    // is decoded (3 and 4); the lethal 32000 wall lands with the RoE
    // refinement tranche. What the enum DOES carry today: the +10
    // general threat over WAR territory at low altitude. See the
    // header doc.

    const int cx = x / kThreatMapRatio;
    const int cy = y / kThreatMapRatio;
    if (cx < 0 || cy < 0 || cx >= cells_x_ || cy >= cells_y_) {
        return 100;  // off the map (the reference's out-of-bounds answer)
    }
    const std::size_t idx = static_cast<std::size_t>(cy) * cells_x_ + cx;

    // Bit half: the side that threatens `who`. The map packs the
    // viewer's wartime enemies at bits 4-7; querying as the viewer
    // reads that half. Querying as anyone else reads bits 0-3 — the
    // AD units NOT at war with the viewer, i.e. the viewer's own
    // side's rings (the only possible threat to another belligerent).
    const int ix = (who == viewer_team_) ? 4 : 0;
    const int low = (sam_[idx] >> ix) & 0x03;
    const int high = (sam_[idx] >> (ix + 2)) & 0x03;

    switch (alt) {
        case AltBand::Ground:
            return 0;
        case AltBand::Low: {
            int s = low * 28 + high * 2;
            // General threat for flight over WAR territory
            // (GetRoE(who, own, ROE_AIR_FIRE) — granted in war; own
            // must be a real, known team, not 0 or unowned).
            const std::uint8_t own = owner_[idx];
            if (own != 0 && own != kUnowned && war_(who, own)) s += 10;
            return s;
        }
        case AltBand::Medium:
            return low * 10 + high * 23;
        case AltBand::High:
            return high * 30;
        case AltBand::VeryHigh:
            return high * 15;
    }
    return 0;
}

int ThreatMap::low_band_density(int x, int y,
                                std::uint8_t who) const noexcept {
    const int cx = x / kThreatMapRatio;
    const int cy = y / kThreatMapRatio;
    if (cx < 0 || cy < 0 || cx >= cells_x_ || cy >= cells_y_) return 0;
    const std::size_t idx = static_cast<std::size_t>(cy) * cells_x_ + cx;
    const int ix = (who == viewer_team_) ? 4 : 0;
    return (sam_[idx] >> ix) & 0x03;
}

int ThreatMap::high_band_density(int x, int y,
                                 std::uint8_t who) const noexcept {
    const int cx = x / kThreatMapRatio;
    const int cy = y / kThreatMapRatio;
    if (cx < 0 || cy < 0 || cx >= cells_x_ || cy >= cells_y_) return 0;
    const std::size_t idx = static_cast<std::size_t>(cy) * cells_x_ + cx;
    const int ix = (who == viewer_team_) ? 4 : 0;
    return (sam_[idx] >> (ix + 2)) & 0x03;
}

} // namespace f4::campaign

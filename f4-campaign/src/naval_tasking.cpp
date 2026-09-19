// f4-campaign/src/naval_tasking.cpp
//
// CAMP-DOM-5 — the naval target pool (see naval_tasking.hpp).

#include <f4/campaign/naval_tasking.hpp>

#include <algorithm>
#include <limits>
#include <utility>

#include <f4/entities/types.hpp>

namespace f4::campaign {

std::vector<std::uint32_t> rank_taskforce_targets(
    const f4::world::IUnitCoreSource& units,
    const f4::world::ITeamSource& teams,
    const f4::world::IObjectiveSource& objectives,
    std::uint8_t team) {
    // The symmetric at-war rule (the rank_battalion_targets lambda's
    // own shape: either direction WAR in the wire stance rows).
    const auto at_war = [&](std::uint8_t other) {
        if (other == team) return false;
        for (int t = 0; t < teams.team_count(); ++t) {
            if (teams.slot(t) != static_cast<int>(team)) continue;
            const auto& row = teams.stance(t);
            const auto idx = static_cast<std::size_t>(other);
            if (idx < row.size() &&
                f4::world::relation_from_wire(row[idx]) ==
                    f4::world::Relation::War)
                return true;
        }
        for (int t = 0; t < teams.team_count(); ++t) {
            if (teams.slot(t) != static_cast<int>(other)) continue;
            const auto& row = teams.stance(t);
            const auto idx = static_cast<std::size_t>(team);
            if (idx < row.size() &&
                f4::world::relation_from_wire(row[idx]) ==
                    f4::world::Relation::War)
                return true;
        }
        return false;
    };

    // The requesting team's own-held objective positions (the shore
    // the fleet threatens). Wire order; empty = the degenerate case.
    std::vector<std::pair<std::int32_t, std::int32_t>> own;
    for (int i = 0; i < objectives.objective_count(); ++i) {
        if (objectives.owner(i) != team) continue;
        own.emplace_back(objectives.x(i), objectives.y(i));
    }

    // Squared distance to the nearest own-held objective (squared
    // ranks identically to euclidean — no sqrt, all integer).
    const auto own_dist2 = [&](std::int32_t x, std::int32_t y) {
        std::int64_t best = -1;
        for (const auto& [ox, oy] : own) {
            const std::int64_t dx = x - ox;
            const std::int64_t dy = y - oy;
            const std::int64_t d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < best) best = d2;
        }
        return best;   // -1: no own-held objectives at all
    };

    // Rank: own-shore distance ascending, wire order breaking ties
    // (the wire index folds into the sort key as the tiebreak term —
    // the distance always dominates).
    std::vector<std::pair<std::pair<std::int64_t, int>,
                          std::uint32_t>> keyed;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.domain(i) != 4 /* sea */) continue;
        if (units.unit_class(i) !=
            f4::entities::UnitClass::TaskForce) continue;
        const std::uint8_t owner = units.owner(i);
        if (!at_war(owner)) continue;
        if (units.roster(i) == 0) continue;
        const std::uint32_t vu = units.id_num(i);
        if (vu == 0) continue;
        const std::int64_t d2 = own_dist2(units.x(i), units.y(i));
        keyed.emplace_back(
            std::make_pair(d2 < 0
                               ? std::numeric_limits<std::int64_t>::max()
                               : d2,
                           i),
            vu);
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::uint32_t> out;
    out.reserve(keyed.size());
    for (const auto& k : keyed) out.push_back(k.second);
    return out;
}

} // namespace f4::campaign

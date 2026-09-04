// f4-campaign/src/ground_writeback.cpp
//
// apply_ground_to(WorldState) implementation — see ground_writeback.hpp.

#include <f4/campaign/ground_writeback.hpp>

#include <algorithm>

namespace f4::campaign {

GroundWritebackResult apply_ground_to(const GroundWar& war,
                                      f4::world::WorldState& ws) {
    GroundWritebackResult out;

    // --- Battalions ------------------------------------------------------
    // Walk the WORLD's units (wire order) and match against the
    // engine's states. Activity = anything that dirtied the unit in
    // the engine: moved (position != snapshot), attrited (roster
    // decayed), resupplied, or destroyed. The engine's own snapshot
    // fields (strength_initial vs strength, position vs snapshot
    // position) carry the comparison — a unit that never moved and
    // never lost keeps identical numbers and is skipped.
    std::vector<std::uint32_t> matched;
    matched.reserve(war.units().size());
    for (auto& unit : ws.units) {
        if (unit.unit_class != f4::entities::UnitClass::Battalion) continue;
        const GroundUnitState* g = nullptr;
        for (const auto& cand : war.units()) {
            if (cand.vu == unit.id_num) { g = &cand; break; }
        }
        if (g == nullptr) continue;
        matched.push_back(g->vu);

        // The activity test: the engine's own current state differs
        // from what the world would expect of a pristine snapshot.
        const bool active =
            g->strength != g->strength_initial ||
            g->x != unit.x || g->y != unit.y ||
            g->destroyed;
        if (!active) continue;

        unit.x = static_cast<std::int16_t>(g->x);
        unit.y = static_cast<std::int16_t>(g->y);
        unit.roster = g->roster;
        // losses: the save's seed + this run's kills, uchar-saturating
        // (the wire's own field limit — defense in depth for
        // hand-built fixtures, the engine never overruns it).
        unit.losses = static_cast<std::uint8_t>(std::min(
            255, static_cast<int>(unit.losses) + g->run_losses));
        unit.supply = g->supply;
        unit.morale = g->morale;
        unit.fatigue = g->fatigue;
        unit.heading = g->heading;
        unit.last_move = static_cast<std::int32_t>(
            std::min<std::int64_t>(g->last_move, 2147483647));
        unit.last_combat = static_cast<std::int32_t>(
            std::min<std::int64_t>(g->last_combat, 2147483647));
        unit.dest_x = static_cast<std::int16_t>(g->dest_x);
        unit.dest_y = static_cast<std::int16_t>(g->dest_y);
        ++out.battalions_written;
    }

    // Unmatched engine battalions (attrition or destruction but no
    // world unit): loud. (A position-only mover without a world unit
    // has nothing to write and nothing to corrupt — not a mismatch
    // worth failing a run over; attrition without a home IS.)
    for (const auto& g : war.units()) {
        const bool active =
            g.strength != g.strength_initial || g.destroyed;
        if (!active) continue;
        if (std::find(matched.begin(), matched.end(), g.vu) !=
            matched.end()) {
            continue;
        }
        out.unmatched_battalions.push_back(g.vu);
    }

    // --- Objectives ------------------------------------------------------
    // Owner flips only (first_owner untouched — the wire's "owner at
    // save start" semantics).
    for (auto& obj : ws.objectives) {
        const GroundObjectiveState* g = nullptr;
        for (const auto& cand : war.objectives()) {
            if (cand.vu == obj.id_num) { g = &cand; break; }
        }
        if (g == nullptr) continue;
        if (g->owner == g->initial_owner) continue;
        if (obj.owner != g->initial_owner) {
            // The world moved under us (a foreign write-back order):
            // last write wins is the C1 discipline, but a mismatched
            // base is loud, not silent.
            out.unmatched_objectives.push_back(obj.id_num);
        }
        obj.owner = g->owner;
        ++out.objectives_flipped;
    }

    return out;
}

} // namespace f4::campaign

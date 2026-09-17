// f4-world/src/airbase_synthesis.cpp
//
// Squadron airbase synthesis — see airbase_synthesis.hpp for the
// stock-save problem, the acceptance rule, and the determinism contract.

#include <f4/world/airbase_synthesis.hpp>

#include <f4/world/detail/world_state.hpp>

#include <cstdint>
#include <vector>

namespace f4::world {

namespace {

// ObjectiveType 1=AIRBASE, 2=AIRSTRIP, 3=ARMYBASE — the parking-capable
// vocabulary world_loader's positional fallback uses (raw ints here for
// the same reason it uses them: f4-world precedes f4-world-types in the
// build graph and owns no dependency on it).
constexpr std::uint8_t kTypeAirbase  = 1;
constexpr std::uint8_t kTypeAirstrip = 2;
constexpr std::uint8_t kTypeArmyBase = 3;


/// One candidate home base — the fields the pick needs, pre-filtered to
/// airbase-capable objectives with a usable VU (wire order preserved).
struct BaseCandidate {
    int16_t x = 0;
    int16_t y = 0;
    std::uint8_t owner = 0;
    std::uint32_t id_num = 0;
};

/// The stance row of team `from` toward team `to` (slot-indexed; .tea
/// enrichment — absent rows decode NoRelations, the honest default).
[[nodiscard]] Relation relation_toward(const WorldState& ws,
                                       std::uint8_t from,
                                       std::uint8_t to) {
    for (const auto& t : ws.teams) {
        if (t.slot != static_cast<int>(from)) continue;
        const auto idx = static_cast<std::size_t>(to);
        if (idx < t.stance.size()) {
            return relation_from_wire(t.stance[idx]);
        }
        break;
    }
    return Relation::NoRelations;
}

/// Basing acceptance: own team, or the squadron team's stance toward the
/// base's owner is ALLIED or FRIENDLY (the US wing basing on RK-owned
/// fields — the Korea norm). Hostile/War/NoRelations owners never.
[[nodiscard]] bool base_owner_acceptable(const WorldState& ws,
                                         std::uint8_t squad_owner,
                                         std::uint8_t base_owner) {
    if (base_owner == squad_owner) return true;
    const Relation rel = relation_toward(ws, squad_owner, base_owner);
    return rel == Relation::Allied || rel == Relation::Friendly;
}

} // namespace

AirbaseSynthesisReport synthesize_squadron_airbases(WorldState& ws) {
    AirbaseSynthesisReport report;

    // The candidate index: airbase-capable objectives in wire order.
    std::vector<BaseCandidate> bases;
    bases.reserve(ws.objectives.size());
    for (const auto& obj : ws.objectives) {
        const bool capable =
            obj.objective_type == kTypeAirbase ||
            obj.objective_type == kTypeAirstrip ||
            obj.objective_type == kTypeArmyBase;
        if (!capable || obj.id_num == 0) continue;
        bases.push_back(BaseCandidate{obj.x, obj.y, obj.owner, obj.id_num});
    }

    for (auto& u : ws.units) {
        if (u.unit_class != f4::entities::UnitClass::Squadron) continue;
        ++report.squadrons;
        if (u.airbase_id != 0) continue;   // the wire based this one

        // Pass 1 — exact grid: squadrons sit on their base's grid (the
        // same first-chance the entity-side positional fallback takes).
        // Pass 2 — nearest acceptable, uncapped (an unbased wing anywhere
        // beats no wing; see the header's rule note). Strict < keeps the
        // wire-order-first candidate on ties — deterministic.
        const BaseCandidate* pick = nullptr;
        for (const auto& b : bases) {
            if (b.x == u.x && b.y == u.y &&
                base_owner_acceptable(ws, u.owner, b.owner)) {
                pick = &b;
                break;
            }
        }
        double best_d2 = 0.0;
        if (pick == nullptr) {
            for (const auto& b : bases) {
                if (!base_owner_acceptable(ws, u.owner, b.owner)) continue;
                const double dx = static_cast<double>(b.x - u.x);
                const double dy = static_cast<double>(b.y - u.y);
                const double d2 = dx * dx + dy * dy;
                if (pick == nullptr || d2 < best_d2) {
                    pick = &b;
                    best_d2 = d2;
                }
            }
        }

        if (pick != nullptr) {
            u.airbase_id = pick->id_num;
            ++report.assigned;
        } else {
            ++report.unresolved;
        }
    }
    return report;
}

} // namespace f4::world

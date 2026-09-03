// f4-campaign/src/squadron_snapshot.hpp
//
// INTERNAL — shared by campaign.cpp and result_ledger.cpp. Not installed,
// not in the umbrella header; the two consumers must NEVER drift apart,
// so the squadron-force snapshot lives in exactly one place.
//
// WHAT IT SNAPSHOTS (a pure function of the read-only sources):
//   * Per squadron (wire order): identity (VU, owner, specialty, name),
//     available aircraft, and the wire's own reinforcement budget.
//   * Availability rule (the M4.7 rule, unchanged since B.3 except the
//     roster decode below): the squadron's own roster when it carries
//     one; else an even share of the team's te_number_aircraft pool
//     (the remainder stays with the team pool).
//
// THE ROSTER DECODE (the C2 fix): the wire's u32 `roster` is the same
// 2-bit-per-group packing flights and battalions use — 16 groups, each
// 0..3. TestCamp's squadrons carry 0x5555aaaa-style values whose group
// sum is 20/24/19/... — exactly the aircraft counts a Falcon squadron
// carries (0x5555aaaa = 8 groups of 2 + 8 groups of 1 = 24). The
// pre-C2 Campaign read the RAW u32 as the count (1,431,655,786 for a
// 24-ship wing) — harmless on kunsan (rosters are 0 there, the shared
// team-pool path) but astronomic on any real v71 save. One shared
// decode fixes both consumers at once; the kunsan golden summaries are
// unaffected (roster 0 routes to the team-pool share as before).
//
// THE REINFORCEMENT BUDGET: the unit record's `reinforcement` i16 —
// FreeFalcon's per-squadron replacement allowance ("aircraft on
// order"). TestCamp: 26 of 94 squadrons carry 24..168. The C2
// reinforcement tick delivers against this budget (see
// CampaignResultLedger::apply_reinforcements).
//
// Determinism: pure function, no RNG, no clocks. C++20.

#pragma once

#include <f4/world/data_source.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace f4::campaign::detail {

/// The 2-bit-per-group roster decode (16 groups; 0 when the save
/// carries no roster). Shared by squadrons and flights — the wire uses
/// one packing for all three unit families.
[[nodiscard]] inline int roster_group_aircraft(std::uint32_t roster) {
    if (roster == 0) return 0;
    int total = 0;
    for (int g = 0; g < 16; ++g) {
        total += static_cast<int>((roster >> (g * 2)) & 0x03u);
    }
    return total;
}

/// One squadron's snapshot — the fields both the Campaign's SquadronRef
/// and the ledger's SquadronLedger need at construction.
struct SquadronSnapshot {
    std::uint32_t vu = 0;          ///< VU_ID.num (the campaign key)
    std::uint8_t owner = 0;        ///< team slot
    std::uint8_t specialty = 0;    ///< aro role byte
    std::string name;              ///< class_name (display)
    int available = 0;             ///< roster-decoded, or team-pool share
    int reinforce_pending = 0;     ///< wire reinforcement (aircraft on order)
};

/// The team pools after the shared-out distribution (slot-indexed,
/// fixed 8 slots — the wire's own team slot vocabulary).
using TeamPoolRemainder = std::array<int, 8>;

struct SquadronForceSnapshot {
    std::vector<SquadronSnapshot> squadrons;   // wire order (deterministic)
    TeamPoolRemainder team_pool{0, 0, 0, 0, 0, 0, 0, 0};
};

/// Snapshot the air force exactly as Campaign::Campaign does: team
/// pools seeded from te_number_aircraft (slot-indexed), squadrons with
/// rosters keep them, squadrons without share their team's pool evenly
/// (remainder stays in team_pool). Called by both the Campaign and the
/// result ledger so their numbers agree BY CONSTRUCTION.
[[nodiscard]] inline SquadronForceSnapshot
snapshot_squadron_force(const f4::world::ICampaignSource& camp,
                        const f4::world::ITeamSource& teams,
                        const f4::world::IUnitCoreSource& units) {
    SquadronForceSnapshot out;

    // Seed the per-team pools (te_number_aircraft, slot-indexed).
    const auto& team_pools = camp.te_number_aircraft();
    for (int t = 0; t < teams.team_count() && t < 8; ++t) {
        const int slot = teams.slot(t);
        if (slot < 0 || slot >= 8) continue;
        if (static_cast<std::size_t>(slot) < team_pools.size()) {
            out.team_pool[static_cast<std::size_t>(slot)] =
                team_pools[static_cast<std::size_t>(slot)];
        }
    }

    // The squadron roster, wire order.
    const int n = units.unit_count();
    for (int i = 0; i < n; ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::Squadron) continue;
        const auto* sq = units.as_squadron(i);
        if (!sq) continue;   // inconsistent source; skip defensively

        SquadronSnapshot s;
        s.vu = units.id_num(i);
        s.owner = units.owner(i);
        s.specialty = sq->specialty(i);
        s.name = units.class_name(i);
        s.available = roster_group_aircraft(units.roster(i));
        s.reinforce_pending = units.reinforcement(i);
        out.squadrons.push_back(std::move(s));
    }

    // Squadrons without a roster share their team's pool evenly; the
    // remainder stays in team_pool. Deterministic (wire order).
    for (int t = 0; t < teams.team_count() && t < 8; ++t) {
        const int slot = teams.slot(t);
        if (slot < 0 || slot >= 8) continue;
        std::vector<SquadronSnapshot*> shared;
        for (auto& s : out.squadrons) {
            if (s.owner == static_cast<std::uint8_t>(slot) &&
                s.available == 0) {
                shared.push_back(&s);
            }
        }
        if (shared.empty()) continue;
        const int share = out.team_pool[static_cast<std::size_t>(slot)] /
                          static_cast<int>(shared.size());
        for (auto* s : shared) s->available = share;
        out.team_pool[static_cast<std::size_t>(slot)] -=
            share * static_cast<int>(shared.size());
    }

    return out;
}

} // namespace f4::campaign::detail

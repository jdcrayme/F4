// f4-campaign/include/f4/campaign/world_writeback.hpp
//
// apply_to(WorldState) — the ledger's save-side write-back (Phase C1).
//
// OPT-IN header, the same pattern as f4-world's world_adapters.hpp:
// this is deliberately NOT included by the ledger's core header, because
// it pulls in the full WorldState layout (<f4/world/detail/
// world_state.hpp>). Consumers that only feed events and read counters
// never need WorldState; only the loop-closing consumer does.
//
// What it does: writes the ledger's post-run state back into the typed
// WorldState the run started from —
//
//   * campaign.te_number_aircraft[slot]  := post-loss team pools
//   * squadron units: aa_kills / ag_kills / total_losses := absolutes
//     (clamped to the wire's own ranges — the ledger already saturates,
//     the clamp here is defense in depth for hand-built fixtures)
//   * objectives: fstatus := the synced damage bitmap (VU_ID match)
//
// After this, the standard pipeline applies: a world JSON written from
// the mutated state (the open-format save), populate_world into a fresh
// EntityWorld, and the campaign's NEXT cycle sees the attrition. That
// is the C1 acceptance: decode → run → fight → apply → reload → the
// damage and the pools are there.
//
// Mismatches are counted, never silently dropped: squadron VUs and
// objective VUs the WorldState doesn't carry are returned in the result
// struct (a fixture built without the unit, a stale world, a wrong save
// — loud, the project rule).
//
// C++20.

#pragma once

#include <f4/campaign/result_ledger.hpp>
#include <f4/world/detail/world_state.hpp>

namespace f4::campaign {

/// What apply_to() did — the QC tool prints it, the tests assert it.
struct WorldWritebackResult {
    /// Team pools written (slot existed in campaign.te_number_aircraft).
    int team_pools_written = 0;
    /// Squadron counters written (VU matched a Squadron unit).
    int squadrons_written = 0;
    /// Objective fstatus bitmaps written (VU matched an objective).
    int objectives_written = 0;
    /// Ledger squadron VUs with activity but no matching unit.
    std::vector<std::uint32_t> unmatched_squadrons;
    /// Ledger objective VUs with damage but no matching objective.
    std::vector<std::uint32_t> unmatched_objectives;
};

/// Apply the ledger to the WorldState (see header comment). Only
/// ledger entries with ACTIVITY are written: pristine teams/squadrons
/// keep the WorldState's own values untouched (a zero-event ledger must
/// round-trip a world byte-for-identically on these fields).
[[nodiscard]] WorldWritebackResult
apply_to(const CampaignResultLedger& ledger, f4::world::WorldState& ws);

} // namespace f4::campaign

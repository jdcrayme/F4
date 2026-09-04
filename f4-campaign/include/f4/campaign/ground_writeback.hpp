// f4-campaign/include/f4/campaign/ground_writeback.hpp
//
// apply_ground_to(WorldState) — the ground war's save-side write-back
// (G1; the world_writeback.hpp pattern, one layer over).
//
// OPT-IN header for the same reason world_writeback.hpp is: it pulls
// the full WorldState layout, which only the loop-closing consumer
// needs.
//
// What it does: writes the ENGINE's post-run ground state back into
// the typed WorldState the run started from —
//
//   * battalion units: x / y (grid, moved), roster (decayed, the
//     wire's own 2-bit group packing), losses (uchar-saturating, the
//     save's seed + this run's kills), supply / morale / fatigue,
//     heading, last_move / last_combat (absolute campaign times, the
//     wire's own convention), dest_x / dest_y (the live target)
//   * objectives: owner := the engine's live owner when it flipped
//     (first_owner untouched — the wire's own "owner at save start"
//     semantics)
//
// ACTIVITY-gated like the C1 write-back: a unit that never moved,
// attrited, or was resupplied (dirty flag never set — equivalently:
// position, roster, and state equal the snapshot) is not written. A
// ground-quiet war round-trips the WorldState byte-identically on
// these fields. In practice the engine only *changes* mobile
// war-pair battalions and captured objectives, so neutral armies and
// untouched rear areas stay exactly as the save left them.
//
// Mismatches are counted, never dropped silently: battalions whose vu
// has no WorldState unit are returned in the result (the loud rule).
//
// C++20.

#pragma once

#include <f4/campaign/ground_war.hpp>
#include <f4/world/detail/world_state.hpp>

#include <cstdint>
#include <vector>

namespace f4::campaign {

/// What apply_ground_to() did — the QC tool prints it, the tests
/// assert it.
struct GroundWritebackResult {
    /// Battalion state blocks written (vu matched a Battalion unit
    /// with activity).
    int battalions_written = 0;
    /// Objective owners flipped (engine owner != snapshot owner).
    int objectives_flipped = 0;
    /// Battalion VUs with activity but no matching WorldState unit.
    std::vector<std::uint32_t> unmatched_battalions;
    /// Objectives with a flipped owner but no matching WorldState
    /// objective.
    std::vector<std::uint32_t> unmatched_objectives;
};

/// Apply the ground war's state to the WorldState (see header
/// comment). Only ACTIVE units/objectives are written: a pristine
/// war leaves the world byte-identical on these fields (the
/// zero-event round-trip contract, the C1 write-back's own rule).
[[nodiscard]] GroundWritebackResult
apply_ground_to(const GroundWar& war, f4::world::WorldState& ws);

} // namespace f4::campaign

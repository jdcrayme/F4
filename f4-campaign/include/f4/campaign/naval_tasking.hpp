// f4-campaign/include/f4/campaign/naval_tasking.hpp
//
// CAMP-DOM-5 — the naval tasking wrap. The upstream
// NavalTaskingManagerClass is a 15-byte flag shell on the wire (the
// .tea's NTM record: manager header + flags, captured verbatim by the
// world pass) — the reference's actual naval tasking IS the air ATM
// filing anti-ship missions at naval targets (Core Systems Reference:
// "for each carrier: generates AMIS_ASHIP"). So the wrap maps the
// naval face onto the ATM pipeline's REQUEST VOCABULARY instead of
// building a sibling manager: the enemy's task forces (the wire's
// domain-4 units) join the target pools the request generator already
// rotates across, their filings ride the same events (mission_filed),
// and the query face (taskforces) serves the wire's own rows.
//
// This header owns the naval TARGET POOL — the deterministic ranking
// that decides which task force an anti-ship package visits. It is
// the sibling of rank_battalion_targets (ground_war.hpp): same shape,
// sea metric.
//
// WHAT STAYS OUT (the "how deep" record — plan §8 DOM-5 as-built):
// task-force MOVEMENT (the wire's dest_x/dest_y is decoded but never
// consumed — a naval GroundWar sibling is its own tranche), naval
// threat-map painting (MoveType Naval=6 arrays exist on the wire, the
// map paints land AD only), carrier airbases (the reference's naval
// airbase scoring), task groups / CVN ops. The wrap makes the naval
// targets REAL to the tasking pipeline; it does not make them MOVE.

#pragma once

#include <cstdint>
#include <vector>

#include <f4/world/data_source.hpp>

namespace f4::campaign {

/// DOM-5 — the naval-target ranking for anti-ship tasking: the task
/// forces of teams at WAR with `team` (the symmetric belligerence
/// rule, sea domain 4, the aggregate TaskForce class, non-empty
/// roster), ordered by squared distance to the requesting team's
/// nearest OWN-HELD objective ascending — the fleet off your coast is
/// the fleet you strike first — wire order breaking ties (std::sort is
/// not stable; the wire index folds into the sort key as the
/// tiebreak term, the rank_battalion_targets pattern). No own-held
/// objectives: every candidate ties at INT64_MAX and wire order
/// decides (the honest degenerate case, the front rule's own shape).
///
/// No ledger filter: the ledger's ground-unit books are battalion-
/// worded (GroundWar books only the land aggregate) and the engine
/// holds no naval-loss source — a spent task force is a later
/// tranche's book. Deterministic (squared distance ranks the same as
/// distance; no sqrt, no RNG).
[[nodiscard]] std::vector<std::uint32_t> rank_taskforce_targets(
    const f4::world::IUnitCoreSource& units,
    const f4::world::ITeamSource& teams,
    const f4::world::IObjectiveSource& objectives,
    std::uint8_t team);

} // namespace f4::campaign

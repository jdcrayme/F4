// f4-world/include/f4/world/airbase_synthesis.hpp
//
// Squadron airbase synthesis — the stock-save bridge.
//
// PROBLEM: the Squadron binary tail stores the home airbase as a raw
// VU_ID, and the stock campaign saves shipped with the game (the Steam
// install's save0/1/2.cam + Instant.cam, .ver 63/65) carry ZERO for every
// squadron. The link is established by the game's own campaign engine on
// first load — after the save was written — so a decoded stock save is a
// war whose air force has no home fields. populate_units' positional
// fallback (world_loader.cpp) already recovers the ENTITY-side link, but
// the campaign side reads the raw wire value: the ATM's FindBestAir bases
// every flight on its squadron's airbase (atm.cpp), the route builder
// gates on it (campaign.cpp's airbase_vu != 0), and the spawner only
// materializes synthetic intents that carry a route. With every squadron
// unbased, the ladder still generates intents (hundreds per cycle on a
// stock Korea save) and nothing ever flies.
//
// FIX: assign each unbased squadron a home base from the objective list —
// the same thing the game's campaign init does. The pass MUTATES the
// WorldState in place, so every consumer (adapters → ladder/ATM/ledger,
// populate_world → SquadronComponent::airbase) sees one consistent
// assignment, and is pure/deterministic: wire order, wire-order tie-breaks,
// no RNG, no clocks.
//
// RULE (per squadron with airbase_id == 0):
//   Candidates: objectives with objective_type in {TYPE_AIRBASE,
//   TYPE_AIRSTRIP, TYPE_ARMYBASE} (the same parking-capable vocabulary
//   world_loader's positional fallback uses) and id_num != 0.
//   Acceptance: the candidate's owner is the squadron's OWN team, or the
//   squadron team's stance toward the base owner decodes ALLIED or
//   FRIENDLY. Same-team first-class; allied basing is the Korea norm (the
//   US wing's 24 squadrons sit on RK-owned Kunsan/Osan — the US owns no
//   airbase objectives at all in save0/1). Hostile/War owners are never
//   accepted; NoRelations (the -5141 garbage toward unused slots) never.
//   Pick: an exact (x, y) grid match wins (squadrons sit on their base's
//   grid); otherwise the nearest ACCEPTABLE candidate by squared grid
//   distance. Unlike the entity-side fallback there is NO radius cap —
//   an unbased wing anywhere in the theater beats no wing at all, and
//   the entity fallback (5 grids) still refines parking when this pass
//   leaves a squadron unresolved. Ties: wire order (strict < keeps the
//   earlier objective).

#pragma once

#include <f4/world/data_source.hpp>

namespace f4::world {

// Forward declaration — WorldState is an implementation detail defined in
// <f4/world/detail/world_state.hpp>. Include that header explicitly to
// call the synthesis.
struct WorldState;

struct AirbaseSynthesisReport {
    int squadrons = 0;   ///< examined (unit_class == Squadron)
    int assigned = 0;    ///< wire carried 0, an acceptable base was found
    int unresolved = 0;  ///< wire carried 0, no acceptable candidate
};

/// Assign a home airbase to every squadron whose wire airbase VU is 0.
/// Already-based squadrons are left untouched (TestCamp's 26 zero-VU
/// carrier-wing entries stay as the wire carries them when based
/// squadrons exist alongside). Returns the counts.
[[nodiscard]] AirbaseSynthesisReport
synthesize_squadron_airbases(WorldState& ws);

} // namespace f4::world

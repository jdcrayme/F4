// f4-campaign/include/f4/campaign/flight_writeback.hpp
//
// flight_writeback — the FID-2 flight write-back (the ground twin's
// air side): fold the FlightAggregateEngine's aggregate truth into the
// session's WorldState, so a tiered session's WorldState carries the
// war's live flight positions/fuel the same way apply_ground_to
// carries the battalions'. In-memory only — the .cam re-encoder is the
// save-write tranche (Docs/FIDELITY_TIERS_PLAN.md §4.2).

#pragma once

#include <f4/campaign/flight_aggregate.hpp>
#include <f4/world/detail/world_state.hpp>

namespace f4::campaign {

/// What the write-back did (the GroundWritebackResult shape, air side).
struct FlightWritebackResult {
    int flights_synced = 0;   ///< engine flights found in the world
    int positions_moved = 0;  ///< x/y actually changed
    int fuels_updated = 0;    ///< fuel_burnt actually changed
};

/// Write every DIRTY aggregate flight back into `ws` (the same
/// activity rule apply_ground_to uses — a clean flight costs nothing).
/// Per flight: UnitState.x/y (rounded grid, clamped to the wire's
/// int16), flight_altitude, fuel_burnt. The wire's z stays untouched
/// ("always 0 at v63" — the decoder's own rule).
FlightWritebackResult apply_flights_to(const FlightAggregateEngine& engine,
                                       f4::world::WorldState& ws);

} // namespace f4::campaign

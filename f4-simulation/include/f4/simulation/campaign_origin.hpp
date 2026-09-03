// f4-simulation/include/f4/simulation/campaign_origin.hpp
//
// CampaignOriginComponent — the sim entity's link BACK to its campaign
// identity (Phase C1, the result-sink tranche).
//
// FreeFalcon never needed this: a sim entity IS a campaign entity there
// (SimVehicleClass derives from FalconEntity, which carries its VU_ID
// and its campaign object pointer). The engine-agnostic split — sim
// entities in one world, campaign units in another, bridged at spawn —
// deliberately severs that identity, which is exactly why nothing could
// flow results back: a kill landed on EntityId 47 of the sim world and
// NOTHING could say which squadron just lost an aircraft.
//
// This component is the restored link, as data: stamped at spawn by
// spawn_aircraft_for_flight (the shared core every campaign spawn path
// goes through — bus-driven, bulk, and scenario-driven alike), read by
// CampaignResultSink when a kill or an impact has to be attributed.
//
// Why it lives in f4-simulation (not f4-entities): same rule as
// VisualModelComponent — the component carries campaign-domain identity
// (VU_IDs + team slot) but exists ONLY on sim-spawned entities; the
// campaign units themselves already carry their VUs in PropertyBag
// residue. f4-entities stays dependency-free; any library can define
// new components via the Component<T> CRTP.
//
// Why passive: the origin never changes after spawn. A re-tasked
// flight is a new flight in the campaign; the component is set once.
//
// Scenario-list aircraft carry NO origin component — the sink counts
// them as non-campaign kills (visible, unattributed). That is the
// compatibility contract: scenarios without campaign data behave
// exactly as before this tranche.
//
// Dependencies: f4-entities (Component<T>). C++20.

#pragma once

#include <f4/entities/entity.hpp>

#include <cstdint>

namespace f4::simulation {

/// The campaign identity of a sim-spawned aircraft. Stamped once at
/// spawn; read by the result sink to attribute kills and losses.
struct CampaignOriginComponent : entities::Component<CampaignOriginComponent> {
    /// The flight unit's VU_ID.num (0 when the flight carried none).
    std::uint32_t flight_vu = 0;
    /// The owning squadron's VU_ID.num (0 when unresolvable).
    std::uint32_t squadron_vu = 0;
    /// The home airbase objective's VU_ID.num (0 = none).
    std::uint32_t home_airbase_vu = 0;
    /// The campaign owner slot (the flight unit's TEAM tag int — the
    /// CAMPAIGN vocabulary, not the sim's blue/red/green strings).
    std::uint8_t team_slot = 0;
    /// The flight's callsign pair, wire-faithful (callsign pool index +
    /// slot within pool). Kept as the raw bytes — display resolution is
    /// a viewer concern, and fabricating strings here would be a
    /// fidelity lie.
    std::uint8_t callsign_id = 0;
    std::uint8_t callsign_num = 0;
};

} // namespace f4::simulation

// f4-ai/include/f4/ai/atc/messages.hpp
//
// ATC message types — the protocol between AI modules and ATC.
//
// These messages flow over the MessageBus. The AI publishes requests
// (TaxiRequest, TakeoffRequest, etc.) and subscribes to responses
// (TaxiClearance, TakeoffClearance, etc.). This decouples the AI from
// any particular ATC implementation — a StubATC for testing, a real
// ATC module for production.
//
// Design: messages are plain structs (per f4-messaging convention).
// No inheritance, no virtual dispatch, no pack pragmas. Just data.
//
// Dependencies: f4-geo (WorldPosition). C++20.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <f4/geo/position.hpp>

namespace f4::ai::atc {

// ============================================================================
// Ground / Taxi messages
// ============================================================================

struct TaxiRequest {
    std::uint64_t aircraft_id{0};
    // The airbase the aircraft is parked at (VU_ID.num of the airbase
    // objective; 0 = unknown — the ATC falls back to its default airfield).
    // Campaign saves park flights at many different bases; without this
    // the ATC answers every request with ONE field's taxi route and the
    // aircraft taxis across the theater toward a runway it will never
    // reach. (LandingRequest has carried the same field since the
    // protocol was defined.)
    std::uint64_t airbase_id{0};
    // Intent: "I want to taxi from my current position to the runway."
};

struct TaxiClearance {
    std::uint64_t aircraft_id{0};
    // Echo of the request's airbase (diagnostic only — the route is the
    // authoritative content).
    std::uint64_t airbase_id{0};
    // The taxi route: sequence of taxiway waypoints to follow.
    std::vector<geo::WorldPosition> taxi_route;
    // The runway the aircraft is cleared to taxi to.
    int runway_id{0};
    std::string runway_name;  // e.g. "Rwy 36L"
};

struct HoldShortRequest {
    std::uint64_t aircraft_id{0};
    std::uint64_t airbase_id{0};
    int runway_id{0};
};

struct HoldShortClearance {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
    // Position of the hold-short point.
    geo::WorldPosition hold_position;
};

// ============================================================================
// Takeoff messages
// ============================================================================

struct TakeoffRequest {
    std::uint64_t aircraft_id{0};
    std::uint64_t airbase_id{0};
    int runway_id{0};
};

struct TakeoffClearance {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
    // Runway heading in radians.
    double runway_heading_rad{0.0};
    // Runway threshold position.
    geo::WorldPosition threshold_position;
    // Departure altitude (pattern altitude or higher).
    double departure_altitude_ft{0.0};
};

struct LineUpAndWait {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
};

// ============================================================================
// Landing / Approach messages
// ============================================================================

struct LandingRequest {
    std::uint64_t aircraft_id{0};
    // The airbase/objective the aircraft wants to land at.
    std::uint64_t airbase_id{0};
};

struct LandingClearance {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
    std::string runway_name;
    double runway_heading_rad{0.0};
    // Runway threshold position (approach reference point).
    geo::WorldPosition threshold_position;
    // Threshold elevation (MSL feet).
    double threshold_altitude_ft{0.0};
    // Glide slope angle (typically 3°).
    double glide_slope_angle_rad{0.0};
    // Pattern altitude (MSL feet).
    double pattern_altitude_ft{0.0};
    // Decision height (AGL feet).
    double decision_height_ft{0.0};
    // Runway dimensions (feet). Lateral bounds guard (Tranche A2):
    // the landing module fires GoAround when |course_lateral_ft| exceeds
    // runway_width_ft/2 in the near-runway environment. Zero = unknown
    // (the guard is disabled — backward-compatible default).
    double runway_width_ft{0.0};
    double runway_length_ft{0.0};
};

struct ApproachClearance {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
    std::string approach_type;  // "ILS", "VOR", "VISUAL"
};

struct ClearedToLand {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
};

struct GoAroundMessage {
    std::uint64_t aircraft_id{0};
    int runway_id{0};
    std::string reason;  // "not_cleared", "runway_occupied", "below_DH"
};

struct TaxiOffClearance {
    std::uint64_t aircraft_id{0};
    std::vector<geo::WorldPosition> taxi_route;
};

// ============================================================================
// Air Refueling messages
// ============================================================================
//
// The full USAF boom AAR procedure (see Docs/AAR_REDESIGN_PLAN.md §1):
//   rendezvous -> pre-contact -> (tanker clears contact) -> contact ->
//   hold -> (receiver requests disconnect) -> back to pre-contact ->
//   (tanker reports fuel + clears departure) -> descend 1000 ft below
//   tanker -> resume route.
//
// The protocol is full-duplex: the receiver reports its position state
// (PrecontactReport, ContactRequest, DisconnectRequest), and the tanker
// responds with clearances + status (TankerAssigned, ClearToContact,
// ContactMade, ContactLost, DisconnectApproved, FuelTransferred).

struct RefuelRequest {
    std::uint64_t aircraft_id{0};   // receiver
};

struct TankerAssigned {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
    // Tanker's current position (for vectoring).
    geo::WorldPosition tanker_position;
    // Tanker's track heading.
    double tanker_heading_rad{0.0};
    // AR altitude (MSL feet).
    double ar_altitude_ft{0.0};
};

// Receiver -> Tanker: "I'm at the pre-contact position, stabilized"
// (the USAF "Precontact" call). The tanker responds with ClearToContact
// when the boom operator is ready.
struct PrecontactReport {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

// Tanker -> Receiver: "Cleared to contact" — the boom operator clears
// the receiver to close from pre-contact into the contact position.
struct ClearToContact {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

struct ContactRequest {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

struct ContactMade {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

struct ContactLost {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
    std::string reason;  // "drifted", "receiver_disconnected"
};

// Receiver -> Tanker: "Disconnect" — the receiver requests disconnect
// when refueling is complete. The tanker responds with DisconnectApproved.
struct DisconnectRequest {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

// Tanker -> Receiver: "Cleared to depart" — the tanker acknowledges the
// disconnect, reports the fuel transferred (via FuelTransferred), and
// clears the receiver to depart (descend 1000 ft below the tanker, resume
// own navigation).
struct DisconnectApproved {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

// Tanker -> Receiver: "X lbs received" — the tanker reports the fuel
// offloaded during the contact. Sent alongside DisconnectApproved.
struct FuelTransferred {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
    double fuel_lbs{0.0};
};

struct RefuelComplete {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
    double fuel_transferred_lbs{0.0};
};

struct DisconnectMessage {
    std::uint64_t receiver_id{0};
    std::uint64_t tanker_id{0};
};

// ============================================================================
// Formation / Wingman messages
// ============================================================================

struct FormationCommand {
    std::uint64_t wingman_id{0};
    std::uint64_t lead_id{0};
    std::string formation_type;  // "Wedge", "Trail", "Echelon", etc.
    // Desired position offset relative to lead.
    double lateral_offset_ft{0.0};
    double vertical_offset_ft{0.0};
    double longitudinal_offset_ft{0.0};
};

struct JoinUp {
    std::uint64_t wingman_id{0};
    std::uint64_t lead_id{0};
};

// ============================================================================
// General purpose
// ============================================================================

struct EmergencyDeclare {
    std::uint64_t aircraft_id{0};
    std::string emergency_type;  // "engine_out", "fuel_critical", "hydraulics"
};

} // namespace f4::ai::atc

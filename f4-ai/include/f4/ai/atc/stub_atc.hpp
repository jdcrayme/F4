// f4-ai/include/f4/ai/atc/stub_atc.hpp
//
// StubATC — a minimal ATC that grants every request immediately.
//
// This is NOT a real air traffic controller. It exists so that AI modules
// (TakeoffModule, LandingModule, RefuelModule) can progress through their
// state machines in tests and demos without a full ATC implementation.
//
// The StubATC:
//   - Subscribes to all ATC request messages on the MessageBus.
//   - Responds immediately with the corresponding clearance message.
//   - Uses preconfigured runway/airbase data from the ground layout.
//   - Never denies a request, never issues holds, never spaces aircraft.
//
// SUBSCRIPTION ORDER: The StubATC subscribes in its constructor. It MUST
// be constructed BEFORE any AI module calls initialize() (which publishes
// a TaxiRequest). If the StubATC is constructed after the TaxiRequest is
// published, the request will have no handler and the AI will stall in
// RequestTaxi forever. The recommended wiring order is:
//   1. Create MessageBus
//   2. Create StubATC (subscribes to request types)
//   3. Create AI module
//   4. Call ai_module.initialize() (publishes TaxiRequest)
//
// When a real ATC module is built later, it implements the SAME message
// types — so the AI modules never know the difference. The StubATC is
// swapped out at the MessageBus wiring level, not in the AI code.
//
// Dependencies: f4-messaging, f4-geo. C++20.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <f4/messaging/bus.hpp>
#include <f4/geo/position.hpp>

#include "f4/ai/atc/messages.hpp"

namespace f4::ai::atc {

// ============================================================================
// AirfieldConfig — ground layout data the StubATC needs.
// ============================================================================
struct AirfieldConfig {
    int active_runway_id{36};
    std::string active_runway_name{"Rwy 36L"};
    double runway_heading_rad{0.0};              // magnetic heading, radians
    geo::WorldPosition threshold_position;        // runway threshold
    double threshold_altitude_ft{0.0};            // threshold elevation MSL
    double pattern_altitude_ft{2500.0};           // traffic pattern altitude
    double glide_slope_angle_rad{0.05235988};     // 3 degrees in radians
    double decision_height_ft{200.0};             // DH for ILS
    double departure_altitude_ft{2500.0};         // initial departure altitude

    // Taxi route: parking -> hold short -> runway
    std::vector<geo::WorldPosition> taxi_route;

    // Runway centerline end (for takeoff roll reference)
    geo::WorldPosition runway_end_position;
};

// ============================================================================
// TankerConfig — data for a scripted tanker.
// ============================================================================
struct TankerConfig {
    std::uint64_t tanker_entity_id{0};
    geo::WorldPosition position;              // tanker's orbit position
    double heading_rad{4.71238898};           // 270 degrees (westbound AR track)
    double altitude_ft{20000.0};             // AR altitude MSL
    double speed_kts{250.0};                 // AR speed
    // Boom envelope (from FreeFalcon digi_refuel.cpp):
    double lateral_tolerance_ft{5.0};        // ±5ft lateral
    double vertical_tolerance_ft{10.0};      // ±10ft vertical
    double longitudinal_tolerance_ft{30.0};  // ±30ft longitudinal (boom length)
};

// ============================================================================
// StubATC — grants everything immediately.
// ============================================================================
class StubATC {
public:
    explicit StubATC(messaging::MessageBus& bus)
        : bus_(bus)
    {
        subscribe_all();
    }

    // Configure the airfield the stub will clear aircraft for.
    void set_airfield(const AirfieldConfig& config) {
        airfield_ = config;
    }

    // Configure a PER-AIRBASE airfield (campaign path). Requests carrying
    // `airbase_id` (the VU_ID.num of the airbase objective the aircraft is
    // parked at) are answered from THAT airfield; requests with an
    // unknown/zero id fall back to the default set_airfield() config.
    // Campaign saves park flights at dozens of different bases — one
    // global airfield would hand every aircraft the same taxi route and
    // runway, sending them taxiing across the theater (the B.3 QC
    // harness caught exactly that). Registering per-base configs keeps
    // every aircraft's ground ops local to its own field.
    void set_airbase_airfield(std::uint64_t airbase_id, const AirfieldConfig& config) {
        airbase_airfields_[airbase_id] = config;
    }

    // How many per-airbase airfields are registered (test access).
    [[nodiscard]] std::size_t airbase_airfield_count() const noexcept {
        return airbase_airfields_.size();
    }

    // Configure a tanker for AR.
    void set_tanker(const TankerConfig& config) {
        tanker_ = config;
    }

    // Accessors for test verification.
    [[nodiscard]] const AirfieldConfig& airfield() const noexcept { return airfield_; }
    [[nodiscard]] const TankerConfig& tanker() const noexcept { return tanker_; }

private:
    // Resolve the airfield to answer a request with: the per-airbase
    // registration when present, else the default.
    [[nodiscard]] const AirfieldConfig& resolve_airfield(std::uint64_t airbase_id) const noexcept {
        const auto it = airbase_airfields_.find(airbase_id);
        return (it != airbase_airfields_.end()) ? it->second : airfield_;
    }

    void subscribe_all() {
        // --- Ground / Taxi ---
        bus_.subscribe<TaxiRequest>([this](const TaxiRequest& msg) {
            const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
            TaxiClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.airbase_id = msg.airbase_id;
            clearance.taxi_route = af.taxi_route;
            clearance.runway_id = af.active_runway_id;
            clearance.runway_name = af.active_runway_name;
            bus_.publish(clearance);
        });

        bus_.subscribe<HoldShortRequest>([this](const HoldShortRequest& msg) {
            const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
            HoldShortClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.runway_id = msg.runway_id;
            // Hold-short position is the last waypoint in the taxi route
            if (!af.taxi_route.empty()) {
                clearance.hold_position = af.taxi_route.back();
            }
            bus_.publish(clearance);
        });

        // --- Takeoff ---
        bus_.subscribe<TakeoffRequest>([this](const TakeoffRequest& msg) {
            const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
            TakeoffClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.runway_id = af.active_runway_id;
            clearance.runway_heading_rad = af.runway_heading_rad;
            clearance.threshold_position = af.threshold_position;
            clearance.departure_altitude_ft = af.departure_altitude_ft;
            bus_.publish(clearance);
        });

        // --- Landing ---
        bus_.subscribe<LandingRequest>([this](const LandingRequest& msg) {
            // LandingRequest has always carried airbase_id — resolve it
            // through the same registry (a per-base landing config beats
            // the default when registered).
            const AirfieldConfig& af = resolve_airfield(msg.airbase_id);
            LandingClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.runway_id = af.active_runway_id;
            clearance.runway_name = af.active_runway_name;
            clearance.runway_heading_rad = af.runway_heading_rad;
            clearance.threshold_position = af.threshold_position;
            clearance.threshold_altitude_ft = af.threshold_altitude_ft;
            clearance.glide_slope_angle_rad = af.glide_slope_angle_rad;
            clearance.pattern_altitude_ft = af.pattern_altitude_ft;
            clearance.decision_height_ft = af.decision_height_ft;
            bus_.publish(clearance);
        });

        bus_.subscribe<ApproachClearance>([this](const ApproachClearance& msg) {
            // Grant cleared-to-land immediately
            ClearedToLand cleared;
            cleared.aircraft_id = msg.aircraft_id;
            cleared.runway_id = msg.runway_id;
            bus_.publish(cleared);
        });

        // --- Refueling ---
        bus_.subscribe<RefuelRequest>([this](const RefuelRequest& msg) {
            TankerAssigned assigned;
            assigned.receiver_id = msg.aircraft_id;
            assigned.tanker_id = tanker_.tanker_entity_id;
            assigned.tanker_position = tanker_.position;
            assigned.tanker_heading_rad = tanker_.heading_rad;
            assigned.ar_altitude_ft = tanker_.altitude_ft;
            bus_.publish(assigned);
        });

        bus_.subscribe<ContactRequest>([this](const ContactRequest& msg) {
            // Stub grants contact immediately (real ATC would check geometry)
            ContactMade contact;
            contact.receiver_id = msg.receiver_id;
            contact.tanker_id = msg.tanker_id;
            bus_.publish(contact);
        });
    }

    messaging::MessageBus& bus_;
    AirfieldConfig airfield_{};
    TankerConfig tanker_{};

    // Per-airbase airfield registry (campaign path — see
    // set_airbase_airfield). Key: airbase VU_ID.num.
    std::unordered_map<std::uint64_t, AirfieldConfig> airbase_airfields_;
};

} // namespace f4::ai::atc

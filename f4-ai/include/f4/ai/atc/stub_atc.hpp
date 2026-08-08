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
// When a real ATC module is built later, it implements the SAME message
// types — so the AI modules never know the difference. The StubATC is
// swapped out at the MessageBus wiring level, not in the AI code.
//
// Dependencies: f4-messaging, f4-geo. C++20.

#pragma once

#include <cstdint>
#include <string>

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

    // Configure a tanker for AR.
    void set_tanker(const TankerConfig& config) {
        tanker_ = config;
    }

    // Accessors for test verification.
    [[nodiscard]] const AirfieldConfig& airfield() const noexcept { return airfield_; }
    [[nodiscard]] const TankerConfig& tanker() const noexcept { return tanker_; }

private:
    void subscribe_all() {
        // --- Ground / Taxi ---
        bus_.subscribe<TaxiRequest>([this](const TaxiRequest& msg) {
            TaxiClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.taxi_route = airfield_.taxi_route;
            clearance.runway_id = airfield_.active_runway_id;
            clearance.runway_name = airfield_.active_runway_name;
            bus_.publish(clearance);
        });

        bus_.subscribe<HoldShortRequest>([this](const HoldShortRequest& msg) {
            HoldShortClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.runway_id = msg.runway_id;
            // Hold-short position is the last waypoint in the taxi route
            if (!airfield_.taxi_route.empty()) {
                clearance.hold_position = airfield_.taxi_route.back();
            }
            bus_.publish(clearance);
        });

        // --- Takeoff ---
        bus_.subscribe<TakeoffRequest>([this](const TakeoffRequest& msg) {
            TakeoffClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.runway_id = airfield_.active_runway_id;
            clearance.runway_heading_rad = airfield_.runway_heading_rad;
            clearance.threshold_position = airfield_.threshold_position;
            clearance.departure_altitude_ft = airfield_.departure_altitude_ft;
            bus_.publish(clearance);
        });

        // --- Landing ---
        bus_.subscribe<LandingRequest>([this](const LandingRequest& msg) {
            LandingClearance clearance;
            clearance.aircraft_id = msg.aircraft_id;
            clearance.runway_id = airfield_.active_runway_id;
            clearance.runway_name = airfield_.active_runway_name;
            clearance.runway_heading_rad = airfield_.runway_heading_rad;
            clearance.threshold_position = airfield_.threshold_position;
            clearance.threshold_altitude_ft = airfield_.threshold_altitude_ft;
            clearance.glide_slope_angle_rad = airfield_.glide_slope_angle_rad;
            clearance.pattern_altitude_ft = airfield_.pattern_altitude_ft;
            clearance.decision_height_ft = airfield_.decision_height_ft;
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
};

} // namespace f4::ai::atc

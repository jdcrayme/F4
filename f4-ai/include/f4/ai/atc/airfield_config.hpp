// f4-ai/include/f4/ai/atc/airfield_config.hpp
//
// AirfieldConfig / TankerConfig — the data the ATC implementations answer
// requests with. Extracted from stub_atc.hpp (where they originated) so
// both ATC implementations and the IAirTrafficControl interface can depend
// on them without dragging a controller implementation along. stub_atc.hpp
// still includes this header, so existing includers are unaffected.
//
// Dependencies: f4-geo. C++20.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <f4/geo/position.hpp>

namespace f4::ai::atc {

// ============================================================================
// AirfieldConfig — ground layout data an ATC needs to clear aircraft.
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

    // Runway dimensions (feet). Tranche A2: threaded through to
    // LandingClearance so the landing module's lateral bounds guard can
    // fire GoAround when an approach is outside the pavement. Zero =
    // unknown (guard disabled, the pre-A2 behavior).
    double runway_width_ft{0.0};
    double runway_length_ft{0.0};
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

} // namespace f4::ai::atc

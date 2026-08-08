// f4-recorder/include/f4/recorder/path_geometry.hpp
//
// Intended path geometry — the reference paths that AI modules steer toward.
//
// The key diagnostic question for AI validation is: "Is the aircraft within
// tolerance of its intended path?" This file defines the data structures
// that represent intended paths and the methods to compute deviation.
//
// Path segments are the "reference lines" the AI is trying to follow:
//   - Taxiway centerlines (from ground layout data)
//   - Runway centerlines
//   - Flight plan legs (waypoint-to-waypoint great circles)
//   - Glide slopes (3° descent from runway threshold)
//   - Traffic pattern legs (downwind, base, final)
//   - Holding patterns
//   - Formation offset positions
//   - AR boom envelope
//
// Each segment has a type, a name, a sequence of waypoints defining the
// path geometry, and optional tolerance bounds. The cross_track_error
// in FlightSnapshot is computed relative to the current active segment.
//
// Dependencies: f4-geo. C++20.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include <f4/geo/position.hpp>

namespace f4::recorder {

// ============================================================================
// PathSegment — one piece of an intended flight path.
// ============================================================================
enum class PathSegmentType {
    Taxiway,            // Ground taxi route
    RunwayCenterline,   // Aligned on runway for takeoff/landing
    FlightPlanLeg,      // Waypoint-to-waypoint leg
    GlideSlope,         // 3° (or other) descent path
    TrafficPatternLeg,  // Downwind, base, or departure leg
    HoldingPattern,     // Racetrack hold
    FormationOffset,    // Position relative to formation lead
    AREnvelope,         // Boom/refueling envelope position
    InitialClimb,       // Post-takeoff climb to pattern altitude
    GoAround,           // Missed approach path
};

struct PathSegment {
    PathSegmentType type{PathSegmentType::FlightPlanLeg};
    std::string name;                         // "Taxiway A", "Rwy 36L", "WP1->WP2"
    std::vector<geo::WorldPosition> waypoints;// defining geometry

    // Tolerance bounds (nullopt = use module default)
    std::optional<double> lateral_tolerance_ft;   // cross-track
    std::optional<double> vertical_tolerance_ft;  // vertical (glide slope)
    std::optional<double> speed_tolerance_kts;    // speed

    // Segment metadata
    double start_tick{0.0};     // tick when this segment became active
    double end_tick{0.0};       // tick when segment was completed/exited
    bool completed{false};      // aircraft reached the end of this segment

    // Computed statistics (filled after the run)
    double max_cross_track_error_ft{0.0};
    double max_vertical_error_ft{0.0};
    double avg_cross_track_error_ft{0.0};
    double avg_vertical_error_ft{0.0};
};

// ============================================================================
// FlightPath — the complete intended + actual path for one aircraft.
// ============================================================================
struct FlightPath {
    std::uint64_t entity_id{0};
    std::string callsign;

    // The intended path: sequence of segments the AI plans to fly.
    // This is set before/during the simulation based on the flight plan,
    // ATC clearances, and AI module decisions.
    std::vector<PathSegment> intended_segments;

    // The actual trajectory: sampled positions from the recording.
    // This is what the aircraft actually flew.
    std::vector<geo::WorldPosition> actual_positions;

    // Per-segment deviation stats (computed post-hoc by compute_deviations).
    // Index corresponds to intended_segments index.
    struct SegmentStats {
        double max_cross_track_ft{0.0};
        double max_vertical_ft{0.0};
        double rms_cross_track_ft{0.0};
        double rms_vertical_ft{0.0};
        bool within_tolerance{true};
    };
    std::vector<SegmentStats> segment_stats;
};

// ============================================================================
// MultiAircraftScenario — paths and relationships for multiple aircraft.
// ============================================================================
enum class RelationshipType {
    Formation,       // Wingman holding formation on lead
    ARContact,       // Receiver in contact with tanker
    BFMEngage,       // Fighter engaging bandit
    WingmanLead,     // Generic wingman-lead relationship
};

struct RelativeGeometry {
    std::uint64_t tick{0};
    double range_ft{0.0};           // 3D distance
    double bearing_rad{0.0};        // horizontal bearing from A to B
    double elevation_rad{0.0};      // vertical angle from A to B
    double closure_rate_kts{0.0};   // closing velocity
    double lateral_offset_ft{0.0};  // perpendicular to A's heading
    double vertical_offset_ft{0.0}; // altitude difference
    bool in_tolerance{false};       // within formation/AR envelope
};

struct Relationship {
    RelationshipType type{RelationshipType::Formation};
    std::uint64_t entity_a{0};      // e.g., receiver / wingman
    std::uint64_t entity_b{0};      // e.g., tanker / lead
    std::string description;        // "Viper1 wingman on ViperLead"
    std::vector<RelativeGeometry> geometry_over_time;
};

struct MultiAircraftScenario {
    std::string scenario_name;
    std::vector<FlightPath> aircraft;
    std::vector<Relationship> relationships;
};

// ============================================================================
// Path deviation computation.
// ============================================================================

// Compute the cross-track error from a point to a line segment
// defined by two endpoints. Returns the signed perpendicular distance
// (positive = right of the line looking from start to end).
[[nodiscard]] double cross_track_error(
    const geo::WorldPosition& point,
    const geo::WorldPosition& line_start,
    const geo::WorldPosition& line_end) noexcept;

// Compute the along-track distance: how far along the line segment
// the closest point is, as a fraction [0, 1] of the segment length.
[[nodiscard]] double along_track_fraction(
    const geo::WorldPosition& point,
    const geo::WorldPosition& line_start,
    const geo::WorldPosition& line_end) noexcept;

// Compute glide slope altitude at a given distance from the threshold.
// Standard 3° glide slope: altitude = distance * tan(3°).
[[nodiscard]] double glide_slope_altitude_ft(
    double distance_from_threshold_ft,
    double glide_slope_angle_rad = 3.0 * 3.14159265358979323846 / 180.0,
    double threshold_altitude_ft = 0.0) noexcept;

} // namespace f4::recorder

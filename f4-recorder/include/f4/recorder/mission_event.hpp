// f4-recorder/include/f4/recorder/mission_event.hpp
//
// MissionEvent — one observation from the mission event stream (MC-1,
// MISSION_CONTRACT_PLAN §8): the join-point events the contract
// verifier evaluates — waypoint captures, station entry/exit, brain
// phase changes. Where CombatEvent carries the FIGHT (launches,
// impacts, kills), MissionEvent carries the MISSION SHAPE (the
// route's progress, the station's dwell, the phase machine's arc).
//
// Producers: the Simulation layer (the per-tick walk that already
// reads the brain for snapshots) — detected as STATE TRANSITIONS of
// the nav cursor / the station-hold flag / the brain phase, so no
// engine-agnostic module needs to know a recorder exists. Consumers:
// the recording JSON ("mission_events", emitted only when present —
// old documents load unchanged) and, downstream, the MC-2 contract
// verifier + report card.
//
// Field conventions mirror CombatEvent: tick/sim_time_s stamped by the
// producer at detection time; entity_id is the aircraft's sim
// EntityId::value; identity strings (callsign, mission) are resolved
// at capture time so the card never re-joins.
//
// Dependencies: standard types + std::string only. C++20.

#pragma once

#include <cstdint>
#include <string>

namespace f4::recorder {

// ============================================================================
// MissionEvent — one mission-shape observation.
// ============================================================================
struct MissionEvent {
    enum class Kind {
        WaypointCaptured,   // the nav cursor advanced off waypoint wp_index
        StationEntered,     // the station hold armed (racetrack anchor)
        StationExited,      // the station hold released
        PhaseChanged        // the brain's mission phase machine moved
    };

    // --- Timing (stamped by the producer at detection) ---
    std::uint64_t tick{0};
    double sim_time_s{0.0};

    Kind kind{Kind::PhaseChanged};

    // --- Who ---
    std::uint64_t entity_id{0};
    std::string callsign;      ///< resolved at capture (CS%03u-%u or roster)
    std::string mission;       ///< AMIS_* name ("" = non-campaign)

    // --- WaypointCaptured payload ---
    int wp_index{-1};          ///< the waypoint LEFT (captured)
    std::string wp_name;       ///< its name ("" = unnamed)
    std::uint8_t wp_action{0}; ///< its wire action (delivery: 14/15/17/18/19)
    double cross_track_ft{0.0};///< miss distance at capture (ft)

    // --- PhaseChanged payload ---
    std::string from_phase;
    std::string to_phase;      ///< e.g. Ground -> Enroute (wheels up),
                               ///< Enroute -> Approach, -> Complete
};

/// Kind name for JSON ("waypoint_captured" etc.).
inline const char* mission_event_kind_name(MissionEvent::Kind k) noexcept {
    switch (k) {
        case MissionEvent::Kind::WaypointCaptured: return "waypoint_captured";
        case MissionEvent::Kind::StationEntered:   return "station_entered";
        case MissionEvent::Kind::StationExited:    return "station_exited";
        case MissionEvent::Kind::PhaseChanged:     return "phase_changed";
    }
    return "phase_changed";
}

} // namespace f4::recorder

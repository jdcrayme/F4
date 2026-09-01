// f4-recorder/include/f4/recorder/combat_event.hpp
//
// CombatEvent — a discrete, timestamped combat TRANSITION captured from the
// sim MessageBus: detections, RWR warnings, launches, detonations, damage,
// kills. Together with the per-tick FlightSnapshots (which gain missile
// tracks), the event stream makes a recorded FIGHT replayable headless:
// a replay host can scrub the kinematics AND narrate what happened, and
// regression tests can assert the whole engagement chain survived the
// JSON round-trip (COMBAT_CHAIN_PLAN.md M4, "every shot/detection/kill
// is replayable").
//
// Design principles (mirroring FlightSnapshot):
//   - Value type: copyable, storable, default-constructible.
//   - Engine-agnostic: entity ids are raw uint64s, kinds/ends are this
//     header's own enum / plain strings — f4-recorder does NOT link
//     f4-weapons or f4-sensors (the CMakeLists documents that stance;
//     the bridge that converts bus messages to CombatEvents lives in
//     f4-simulation, which links everything).
//   - One fat struct for all kinds rather than a variant: recordings are
//     append-mostly and serialized field-by-field; per-kind unions would
//     complicate the JSON round-trip for no size win at fight scale
//     (tens of events, not tens of thousands).
//   - sim_time_s is the message's own stamp (authoritative); tick is the
//     sim tick the event belongs to (aligned with FlightSnapshot::tick by
//     the capture bridge).
//   - weapon_name is resolved at CAPTURE time (the bridge owns the weapon
//     table) so a replay never needs the table to interpret a launch.
//
// Dependencies: f4-geo (WorldPosition). C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/geo/position.hpp>

namespace f4::recorder {

// ============================================================================
// CombatEventKind — which bus transition the event represents.
// ============================================================================
enum class CombatEventKind : std::uint8_t {
    TrackAcquired  = 0,  // a radar gained a track          (sensors)
    TrackDropped   = 1,  // a radar lost a track            (sensors)
    RwrLock        = 2,  // victim first painted by a lock  (sensors)
    RwrLaunch      = 3,  // victim first saw a launch       (sensors)
    MissileLaunched= 4,  // a weapon left the rail          (weapons)
    MissileDetonated=5,  // a missile reached a terminal state (weapons)
    DamageApplied  = 6,  // hit points were deducted        (weapons)
    EntityKilled   = 7,  // live -> killed transition       (weapons)
    GunFired       = 8,  // a gun burst's first round       (weapons)
};

/// Stable wire names ("track_acquired", ...). Emitted as the JSON "kind"
/// and safe to grep in replay tooling.
[[nodiscard]] inline const char* combat_event_kind_name(CombatEventKind k) noexcept {
    switch (k) {
        case CombatEventKind::TrackAcquired:   return "track_acquired";
        case CombatEventKind::TrackDropped:    return "track_dropped";
        case CombatEventKind::RwrLock:         return "rwr_lock";
        case CombatEventKind::RwrLaunch:       return "rwr_launch";
        case CombatEventKind::MissileLaunched: return "missile_launched";
        case CombatEventKind::MissileDetonated:return "missile_detonated";
        case CombatEventKind::DamageApplied:   return "damage_applied";
        case CombatEventKind::EntityKilled:    return "entity_killed";
        case CombatEventKind::GunFired:        return "gun_fired";
    }
    return "unknown";
}

// ============================================================================
// CombatEvent — one combat transition, with per-kind payload fields.
// ============================================================================
struct CombatEvent {
    // --- Timing (both stamped by the capture bridge) ---
    std::uint64_t tick{0};         // sim tick the event belongs to (1-based,
                                   // aligned with FlightSnapshot::tick)
    double sim_time_s{0.0};        // the message's own stamp, seconds

    CombatEventKind kind{CombatEventKind::TrackAcquired};

    // --- Participants (raw EntityId::value; 0 = not applicable) ---
    // subject: whose radar acquired / the RWR victim / the shooter /
    //          the target that took damage or died
    // object:  the tracked target / the RWR emitter / the missile's target
    std::uint64_t subject_id{0};
    std::uint64_t object_id{0};
    std::uint64_t missile_id{0};   // launch + detonation events

    // --- Launch payload (MissileLaunched) ---
    std::uint32_t weapon_handle{0xFFFFFFFFu};  // raw table handle
    std::string weapon_name;                   // resolved at capture time
    geo::WorldPosition position{};             // launch / burst point (ENU ft)
    double speed_ft_s{0.0};                    // missile speed at launch

    // --- Gun burst payload (GunFired) ---
    int rounds{0};                             // rounds in the burst

    // --- Detonation payload (MissileDetonated) ---
    std::string end_cause;                     // "target_hit", "closest_approach",
                                               // "self_destruct", "no_target"
    double miss_distance_ft{0.0};
    double flight_time_s{0.0};

    // --- Damage payload (DamageApplied) ---
    double damage{0.0};
    double hit_points_after{0.0};
    bool killed{false};

    // --- RWR payload (RwrLock / RwrLaunch) ---
    double range_ft{0.0};                      // emitter -> victim slant range
};

} // namespace f4::recorder

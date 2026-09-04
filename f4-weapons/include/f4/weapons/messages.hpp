// f4-weapons/include/f4/weapons/messages.hpp
//
// Combat message types published on the sim MessageBus.
//
// Same pattern as f4-flight-model's messages.hpp (the project's established
// bus convention): plain structs, public fields, no inheritance, no
// virtuals, one struct per logical event. Per-frame telemetry does NOT go
// on the bus — these are the rare events other subsystems react to
// (RWR, debrief, kill tracking, radio calls, campaign attrition).
//
// All entity ids are raw EntityId::value (uint64) so the message layer stays
// decoupled from f4-entities types at the handler site.

#pragma once

#include <f4/geo/position.hpp>

#include <cstdint>

namespace f4::weapons {

/// Published by launch_missile() after the store was debited and the missile
/// entity was created.
struct MissileLaunchedMessage {
    std::uint64_t missile_id{0};      // EntityId of the new missile entity
    std::uint64_t shooter_id{0};
    std::uint64_t target_id{0};
    std::uint32_t weapon_handle{0xFFFFFFFFu};
    f4::geo::WorldPosition position{};   // launch point
    double        speed_fts{0.0};
    double        sim_time_s{0.0};
};

/// Why a missile stopped flying.
enum class MissileEndCause : std::uint8_t {
    TargetHit,          // fuze fired within fuze_radius of the target
    ClosestApproach,    // proximity fuze at closest approach (miss distance > fuze radius)
    SelfDestruct,       // time-of-flight limit
    NoTarget,           // never had a track; expired ballistically
};

[[nodiscard]] inline const char* missile_end_cause_name(MissileEndCause c) noexcept {
    switch (c) {
        case MissileEndCause::TargetHit:       return "target_hit";
        case MissileEndCause::ClosestApproach: return "closest_approach";
        case MissileEndCause::SelfDestruct:    return "self_destruct";
        case MissileEndCause::NoTarget:        return "no_target";
    }
    return "unknown";
}

/// Published when a missile reaches a terminal state (Detonated or Expired).
/// One message per missile, ever.
struct MissileDetonatedMessage {
    std::uint64_t missile_id{0};
    std::uint64_t shooter_id{0};
    std::uint64_t target_id{0};       // 0 if the missile expired without a track
    MissileEndCause cause{MissileEndCause::SelfDestruct};
    f4::geo::WorldPosition position{};   // burst point
    double        miss_distance_ft{0.0}; // closest range achieved to the target
    double        flight_time_s{0.0};
    double        sim_time_s{0.0};
};

/// Published after apply_damage() deducted hit points from a live target.
/// NOT published for zero-effect hits (damage == 0).
struct DamageAppliedMessage {
    std::uint64_t target_id{0};
    std::uint64_t shooter_id{0};      // entity that delivered the effect
    std::uint64_t missile_id{0};      // 0 for gun hits
    double        damage{0.0};
    double        hit_points_after{0.0};
    bool          killed{false};
    double        sim_time_s{0.0};
};

/// Published once per target, on the transition live -> killed. Death
/// handling (removal, wreckage, campaign attrition) is the subscriber's job.
struct EntityKilledMessage {
    std::uint64_t target_id{0};
    std::uint64_t shooter_id{0};
    double        sim_time_s{0.0};
};

/// Published when a gun burst begins (one per burst, not per round).
struct GunFiredMessage {
    std::uint64_t shooter_id{0};
    std::uint64_t target_id{0};       // aim hint (0 = none)
    int           rounds{0};          // rounds in this burst
    std::uint32_t weapon_handle{0};   // Gun-class record (name resolution)
    f4::geo::WorldPosition position{};   // muzzle position
    double        sim_time_s{0.0};
};

// ============================================================================
// A-G employment (the M5 strike slice). Bombs get their own release and
// impact events: a gravity weapon's terminal event carries the OBJECTIVE
// damage summary, not a hit-point delta against an airframe, so it does
// not ride DamageAppliedMessage.
// ============================================================================

/// Published by release_bomb() after the store was debited and the bomb
/// entity was created (the A-G mirror of MissileLaunchedMessage).
struct BombReleasedMessage {
    std::uint64_t bomb_id{0};         // EntityId of the new bomb entity
    std::uint64_t shooter_id{0};
    std::uint64_t target_id{0};       // the strike target (objective)
    std::uint32_t weapon_handle{0xFFFFFFFFu};
    f4::geo::WorldPosition position{};   // release point
    double        speed_fts{0.0};
    double        sim_time_s{0.0};
};

/// Why a bomb stopped flying.
enum class BombEndCause : std::uint8_t {
    Impact,       // reached the impact plane
    Expired,      // time-of-flight limit while still above the plane
};

[[nodiscard]] inline const char* bomb_end_cause_name(BombEndCause c) noexcept {
    switch (c) {
        case BombEndCause::Impact:  return "impact";
        case BombEndCause::Expired: return "expired";
    }
    return "unknown";
}

/// Published when a bomb reaches a terminal state. One message per bomb.
/// The damage fields are the OBJECTIVE feature damage summary (see
/// bomb_battery.hpp) — zero when the impact hit no targeted objective.
struct BombImpactMessage {
    std::uint64_t bomb_id{0};
    std::uint64_t shooter_id{0};
    std::uint64_t target_id{0};       // the strike target (0 = none assigned)
    BombEndCause  cause{BombEndCause::Impact};
    f4::geo::WorldPosition position{};   // impact point
    double        miss_distance_ft{0.0}; // impact -> aim point (horizontal)
    double        flight_time_s{0.0};
    // Objective damage summary (valid when target_id != 0 and it resolved
    // an objective with features).
    int           features_damaged{0};
    int           features_destroyed{0};
    double        destroyed_pct{0.0};
    double        sim_time_s{0.0};
};

// ============================================================================
// G2 — the interdiction link. A campaign battalion is an AGGREGATE point
// entity (the reference deaggregates it into vehicles inside the bubble;
// that mapping is the viewer tranche's), so a blast against it is not a
// hit-point event against one damageable thing — it is an integer count of
// vehicles the warhead removed. The count rides its own message: NOT N
// EntityKilledMessages for a unit that is not dead (the battalion's life
// stays the ground-war engine's — the ledger books the loss, the engine
// pulls it and decays the roster, the mirror syncs). One message per bomb
// whose blast killed at least one vehicle.
// ============================================================================
struct GroundUnitLossMessage {
    std::uint64_t target_id{0};       // the battalion entity (EntityId)
    std::uint64_t shooter_id{0};      // the aircraft that released
    int           vehicles_killed{0}; // 1..N (capped at remaining strength)
    double        sim_time_s{0.0};
};

} // namespace f4::weapons

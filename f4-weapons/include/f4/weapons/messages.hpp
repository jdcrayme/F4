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
    f4::geo::WorldPosition position{};   // muzzle position
    double        sim_time_s{0.0};
};

} // namespace f4::weapons

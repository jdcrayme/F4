// f4-weapons/include/f4/weapons/bomb_battery.hpp
//
// The ECS binding for gravity bombs: bombs are ENTITIES, exactly like
// missiles (FreeFalcon's model — every weapon in the air is a VuEntity).
//
//   BombComponent     — passive state (the pure Bomb + bookkeeping)
//   BombSimComponent  — behavioral (physics pass, priority 41): ticks the
//                       ballistic flyout, mirrors state into the entity's
//                       TransformComponent, runs the impact plane check,
//                       applies OBJECTIVE FEATURE damage, publishes the
//                       terminal messages.
//
// release_bomb() is the only sanctioned way to create a bomb entity: it
// validates + debits the shooter's WeaponStoreComponent, captures the aim
// point (the target objective's transform + the impact plane), creates the
// entity (Transform + Bomb + BombSim + CampaignIdentity + team/role tags),
// and publishes BombReleasedMessage. Returns a zero EntityId when the
// release was refused (no store, no loaded Bomb-category station, ...).
//
// Objective damage — the campaign-side endpoint of the A-G chain
// (FreeFalcon: SimFeatureClass::ApplyDamage -> Objective::SetFeatureStatus,
// f4vu.h VIS states). apply_objective_feature_damage() walks the objective's
// FeatureSetComponent, applies the blast model per feature (distance from
// the impact point to the feature's placement offset), maintains the
// per-feature hit-point ledger + the 2-bit VIS damage state, and syncs the
// DamageBitmapComponent's fstatus bits (the wire the campaign save format
// itself uses). objective_damage_summary() reads the same ledger back out
// for QC summaries and viewer rendering.

#pragma once

#include <cstdint>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/bomb.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>

namespace f4::weapons {

// ============================================================================
// BombComponent — per-bomb state (passive).
// ============================================================================
struct BombComponent : public entities::Component<BombComponent> {
    Bomb           bomb{};
    std::uint32_t  weapon_handle = kInvalidWeapon;
    std::uint64_t  shooter_id = 0;
    std::uint64_t  target_id  = 0;   // EntityId::value of the strike target
                                     // (an objective, or 0 for a ballistic
                                     // release with no target assigned)
    double         sim_time_at_release_s = 0.0;
    std::uint64_t  tick_counter = 0;  // ticks since release (diagnostics)

    // Damage potential snapshot (copied from the WeaponClassRecord so the
    // impact never needs the table — same rule as MissileComponent).
    double warhead_power_lb = 0.0;
    double lethal_radius_ft = 0.0;

    // The aim point captured at release (ENU feet): the objective center
    // (or the targeted feature's placement when target_building is known).
    // The impact's miss distance reports against THIS point.
    f4::geo::WorldPosition aim_point{};
};

// ============================================================================
// BombSimComponent — per-bomb behavior (physics pass, right after the
// missile sim so mixed strike packages sweep in a stable order).
// ============================================================================
class BombSimComponent : public entities::BehavioralComponent<BombSimComponent> {
public:
    int priority() const noexcept override { return 41; }  // physics pass

    void on_attached(entities::EntityHandle& self) override { owner_ = self; }

    void update(double dt, messaging::MessageBus& bus) override;

    /// Simulation time, set by the host each tick (same static-clock
    /// contract as MissileSimComponent — the host owns time).
    static void set_sim_time(double t) { sim_time_s() = t; }
    static double sim_time() { return sim_time_s(); }

private:
    entities::EntityHandle owner_{};
    static double& sim_time_s() {
        static double t = 0.0;
        return t;
    }
};

// ============================================================================
// Objective feature damage — the campaign-side damage ledger.
// ============================================================================

/// What one bomb impact did to one objective (reported in
/// BombImpactMessage and the QC summary).
struct ObjectiveDamageResult {
    std::uint64_t objective_id = 0;
    int    features_total = 0;        // features on the objective
    int    features_damaged = 0;      // newly DAMAGED this impact (VIS 2)
    int    features_destroyed = 0;    // newly DESTROYED this impact (VIS 3)
    int    features_destroyed_total = 0;  // destroyed after this impact
    double destroyed_pct = 0.0;       // value-weighted, 0..100
    double damage_applied = 0.0;      // total hp damage across features
    bool   objective_found = false;   // false: target id resolved no objective
};

/// Apply one bomb's blast to every feature on the objective near the
/// impact point. Blast model: damage.hpp's apply_damage per feature, burst
/// range = HORIZONTAL distance from the impact point to the feature's
/// placement (objective transform + offset). The per-feature ledger
/// (FeatureSetComponent::feature_hp) is initialized lazily from the
/// feature's FCD hit_points, or a 100-hp default when the class data is
/// missing (the fixture FCD covers a subset of classes — documented).
/// VIS states follow FreeFalcon's f4vu.h: 0 normal, 1 repaired, 2 damaged,
/// 3 destroyed. The fstatus bitmap (2 bits per feature, byte i/4, shift
/// (i%4)*2 — the save format's own packing) is kept in sync; a missing
/// DamageBitmapComponent is created on first damage.
[[nodiscard]] ObjectiveDamageResult
apply_objective_feature_damage(entities::EntityWorld& world,
                               std::uint64_t objective_id,
                               const f4::geo::WorldPosition& impact_point,
                               double warhead_power_lb,
                               double lethal_radius_ft,
                               double roll01 = 0.5);

/// Read the damage ledger back out (no mutation): value-weighted destroyed
/// percentage + counts. Used by QC summaries and viewer damage overlays.
[[nodiscard]] ObjectiveDamageResult
objective_damage_summary(const entities::EntityWorld& world,
                         std::uint64_t objective_id);

// ============================================================================
// Free functions — release / sweep
// ============================================================================

/// Release one bomb of `weapon_handle` from `shooter` against `target`
/// (an objective entity; may be invalid for a ballistic-only release).
///
/// Requires on the shooter: TransformComponent (position/velocity),
/// WeaponStoreComponent (with a loaded Bomb-category station of the
/// weapon). The impact plane is the AIM POINT's ground elevation: the
/// target objective's transform z when the target resolves, else the
/// shooter's current z (a same-altitude plane — the release is ballistic
/// and falls at least that far).
///
/// Creates on the bomb: TransformComponent, BombComponent,
/// BombSimComponent, CampaignIdentityComponent + team tag (IFF) +
/// ROLE="bomb" (what the recorder/replay keys bomb tracks on).
/// Returns the bomb's EntityId, or a default (zero) EntityId when the
/// release was refused. On success the store IS debited and
/// BombReleasedMessage IS published.
[[nodiscard]] entities::EntityId release_bomb(
    entities::EntityWorld& world,
    messaging::MessageBus& bus,
    const entities::EntityHandle& shooter,
    entities::EntityId target,
    const WeaponClassTable& table,
    std::uint32_t weapon_handle,
    double sim_time_s);

/// Destroy every bomb entity whose flyout is terminal (Impact/Expired).
/// Hosts call it between ticks (never inside world.update_all()).
std::size_t sweep_spent_bombs(entities::EntityWorld& world);

/// Count live (falling) bomb entities.
[[nodiscard]] std::size_t count_live_bombs(const entities::EntityWorld& world);

} // namespace f4::weapons

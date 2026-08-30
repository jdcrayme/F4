// f4-weapons/include/f4/weapons/missile_battery.hpp
//
// The ECS binding for guided missiles: missiles are ENTITIES, matching
// FreeFalcon's model (every missile is a VuEntity with its own id, team,
// and lifecycle).
//
//   MissileComponent   — passive state (the pure Missile + bookkeeping)
//   MissileSimComponent— behavioral (physics pass, priority 40): ticks the
//                        flyout, mirrors position/velocity into the entity's
//                        TransformComponent, runs the fuze, applies damage,
//                        publishes terminal messages.
//
// launch_missile() is the ONLY sanctioned way to create a missile: it
// validates + debits the shooter's WeaponStoreComponent, creates the
// entity (Transform + Missile + MissileSim), and publishes
// MissileLaunchedMessage. If the shooter has no loaded station of the
// requested weapon the function returns a zero EntityId and nothing happens.
//
// sweep_spent_missiles() destroys terminal missile entities. It is a
// separate sweep (not world.destroy() inside update()) so we never mutate
// the world mid-iteration; hosts call it between ticks.

#pragma once

#include <cstdint>
#include <functional>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/missile.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/weapons/weapon_store.hpp>

namespace f4::weapons {

// ============================================================================
// MissileComponent — per-missile state (passive).
// ============================================================================
struct MissileComponent : public entities::Component<MissileComponent> {
    Missile        missile{};
    std::uint32_t  weapon_handle = kInvalidWeapon;
    std::uint64_t  shooter_id = 0;
    std::uint64_t  target_id  = 0;   // EntityId::value of the assigned target
    double         sim_time_at_launch_s = 0.0;
    std::uint64_t  tick_counter = 0;  // ticks this missile has flown (diagnostics)

    // Damage potential snapshot (copied from the WeaponClassRecord at launch
    // so detonation never needs the table — the table may be a temporary).
    double warhead_power_lb = 0.0;
    double lethal_radius_ft = 0.0;

    // --- M2 sensor integration point (COMBAT_CHAIN_PLAN.md M2) ---------------
    // Optional seeker-source override. When SET, the missile sim asks THIS
    // callable for the target picture every tick instead of reading the
    // target's TransformComponent directly. That is the hook through which
    // track quality, seeker gimbal error, or jamming enter the flyout: an
    // implementation can return a degraded snapshot (offset position), a
    // stale one, or valid=false (seeker lost the target -> Ballistic).
    // When EMPTY (the default), guidance reads the target transform
    // directly — the M1 behavior, and the current production path until
    // f4-sensors' track model is wired in by the host.
    using SeekerSourceFn = std::function<TargetSnapshot(
        const entities::EntityWorld& world, std::uint64_t target_id)>;
    SeekerSourceFn seeker_source;
};

// ============================================================================
// MissileSimComponent — per-missile behavior (physics pass).
// ============================================================================
class MissileSimComponent : public entities::BehavioralComponent<MissileSimComponent> {
public:
    int priority() const noexcept override { return 40; }  // physics pass

    void on_attached(entities::EntityHandle& self) override { owner_ = self; }

    void update(double dt, messaging::MessageBus& bus) override;

    /// Simulation time, set by the host each tick (used for message stamps).
    /// Cheap and stateless enough that the component carries it per entity;
    /// a future world-level clock source can replace it without API change.
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
// Free functions — launch / sweep
// ============================================================================

/// Launch a missile of `weapon_handle` from `shooter` at `target`.
///
/// Requires on the shooter: TransformComponent (position/velocity),
/// WeaponStoreComponent (with a loaded station of the weapon).
/// Creates on the missile: TransformComponent, MissileComponent,
/// MissileSimComponent, CampaignIdentityComponent (copied team tag so IFF
/// filters work), plus the "team" tag copied from the shooter.
///
/// Returns the missile's EntityId, or a default (zero-value) EntityId if
/// the launch was refused (no store, no loaded station, bad weapon handle,
/// shooter/target missing). On success the store IS debited and
/// MissileLaunchedMessage IS published.
[[nodiscard]] entities::EntityId launch_missile(
    entities::EntityWorld& world,
    messaging::MessageBus& bus,
    const entities::EntityHandle& shooter,
    entities::EntityId target,
    const WeaponClassTable& table,
    std::uint32_t weapon_handle,
    double sim_time_s);

/// Destroy every missile entity whose flyout is terminal (Detonated/Expired).
/// Returns the number destroyed. Call between ticks, never inside
/// world.update_all().
std::size_t sweep_spent_missiles(entities::EntityWorld& world);

/// Count live (non-terminal) missile entities — host/debug convenience.
[[nodiscard]] std::size_t count_live_missiles(const entities::EntityWorld& world);

} // namespace f4::weapons

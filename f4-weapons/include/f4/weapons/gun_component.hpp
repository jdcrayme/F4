// f4-weapons/include/f4/weapons/gun_component.hpp
//
// GunComponent — an aircraft's cannon, as an ECS component.
//
// The component is a PASSIVE data holder (the RwrComponent pattern, not
// the BehavioralComponent one): the GunStream it owns advances only when
// the HOST calls update_guns() between ECS ticks, because a moving gun
// must emit its rounds from the aircraft's FRESH muzzle pose — the
// transform the per-tick FM sync just wrote — and hit detection both
// iterates and (on hits) mutates the world, which update_all's contract
// forbids mid-iteration. Missiles get the same treatment from
// sweep_spent_missiles + the brain-intent driver; guns get it here.
//
// The burst itself starts in the combat driver
// (f4-simulation/combat_bridge.cpp: a brain gun-trigger intent becomes
// GunStream::start_burst + a WeaponStoreComponent debit); this sweep only
// flies what is already in the air.
//
// GunConfig is filled from the Gun-category WeaponClassRecord (M61A1) at
// attach time (attach_combat_loadout); the store's gun station is the
// rounds ledger the driver debits.
//
// Dependencies: f4-entities, f4-messaging, f4-geo, f4-math. C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/weapons/gun.hpp>

#include <vector>

namespace f4::weapons {

/// One aircraft's gun: the ballistic stream + its identity. Configured at
/// attach (GunConfig from the M61A1 record, handle + seed from the host);
/// driven by update_guns() + the combat driver's start_burst calls.
class GunComponent : public entities::Component<GunComponent> {
public:
    /// The ballistic state (tracers in flight, burst bookkeeping).
    GunStream stream{};

    /// The weapon-class handle (M61A1) — carried on GunFiredMessage for
    /// name resolution by the recorder / transcript.
    std::uint32_t weapon_handle{0};

    /// Muzzle offset ahead of the aircraft origin along the boresight
    /// (ft) — keeps rounds out of the shooter's own hit sphere, the same
    /// clearance launch_missile gives a missile.
    double muzzle_offset_ft{15.0};
};

/// World-level sweep: advance every GunComponent's stream one tick.
/// Call AFTER the host's FM->Transform sync (fresh muzzle pose; the
/// stream emits from the muzzle + boresight computed from the owner's
/// transform velocity each tick — a firing aircraft sweeping its gun
/// across the sky is the point) and OUTSIDE update_all (hit detection
/// mutates the world: damage, kills).
///
/// The boresight is the velocity unit vector (a gun fires where the
/// aircraft is going; the AI's job is to make that point at the lead).
/// Stationary guns (a parked strafing test) fire east — the same
/// degenerate-direction convention launch_missile uses.
///
/// Per gun per tick: bus/sim-time/weapon-handle are (re)stamped, then
/// stream.tick(dt, world, owner, muzzle, direction). Returns every hit
/// applied this tick (already applied to the world — bookkeeping only).
std::vector<GunHit> update_guns(entities::EntityWorld& world,
                                messaging::MessageBus& bus,
                                double dt,
                                double sim_time_s);

} // namespace f4::weapons

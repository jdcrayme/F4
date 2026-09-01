// f4-weapons/src/missile_battery.cpp — ECS binding for missiles.
//
// Missiles are entities (FreeFalcon's VuEntity model): MissileComponent
// holds the flyout state, MissileSimComponent (behavioral, physics pass)
// ticks it each world update, mirrors state into TransformComponent, runs
// the fuze, applies damage, and publishes the terminal messages.

#include <f4/weapons/missile_battery.hpp>

#include <f4/weapons/damage.hpp>
#include <f4/weapons/messages.hpp>

#include <algorithm>
#include <cmath>

namespace f4::weapons {

namespace {

/// Distance between two positions (feet).
[[nodiscard]] double distance(const f4::geo::WorldPosition& a,
                              const f4::geo::WorldPosition& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// Build this tick's seeker picture from the target entity, if it is alive
/// and has a TransformComponent. A dead/destroyed target reads as an
/// invalid track (the seeker loses lock — see Missile::tick).
[[nodiscard]] TargetSnapshot snapshot_target(const entities::EntityWorld& world,
                                             std::uint64_t target_id) {
    TargetSnapshot snap;
    if (target_id == 0) {
        return snap;
    }
    const entities::EntityHandle target(entities::EntityId{target_id},
                                        const_cast<entities::EntityWorld*>(&world));
    if (auto* t = target.get<entities::TransformComponent>()) {
        snap.valid = true;
        snap.position = t->position;
        snap.velocity = f4::math::Vec3<double>{t->vx, t->vy, t->vz};
    }
    return snap;
}

} // namespace

void MissileSimComponent::update(double dt, messaging::MessageBus& bus) {
    if (!owner_.valid()) {
        return;
    }
    auto* world = owner_.world();
    auto* mc = owner_.get<MissileComponent>();
    if (world == nullptr || mc == nullptr || mc->missile.terminal()) {
        return;  // already done — sweep_spent_missiles() removes the entity
    }

    // --- Tick the flyout with the current seeker picture ----------------------
    // A seeker_source override (see MissileComponent) replaces the direct
    // transform read — that is where track quality / jamming enter at M2.
    const TargetSnapshot snap = mc->seeker_source
        ? mc->seeker_source(*world, mc->target_id)
        : snapshot_target(*world, mc->target_id);
    mc->missile.tick(dt, snap);
    ++mc->tick_counter;

    // Mirror state into TransformComponent (renderer / spatial queries /
    // future RWR see the missile like any other entity).
    auto* tc = owner_.get<entities::TransformComponent>();
    if (tc != nullptr) {
        tc->position = mc->missile.position();
        tc->vx = mc->missile.velocity().x;
        tc->vy = mc->missile.velocity().y;
        tc->vz = mc->missile.velocity().z;
    }

    // --- Terminal handling (runs exactly once per missile) ----------------------
    if (!mc->missile.terminal()) {
        return;
    }

    const MissileStatus st = mc->missile.status();
    const double miss_distance =
        mc->missile.min_range_ft() >= 0.0
            ? mc->missile.min_range_ft()
            : distance(mc->missile.position(), snap.position);

    // Map the terminal state to a cause for the bus.
    MissileEndCause cause = MissileEndCause::SelfDestruct;
    if (st == MissileStatus::Detonated) {
        cause = (miss_distance <= mc->missile.fuze_radius_ft())
                    ? MissileEndCause::TargetHit
                    : MissileEndCause::ClosestApproach;
    } else if (mc->target_id == 0) {
        cause = MissileEndCause::NoTarget;
    }

    // --- Damage against the target (only on a real detonation) ------------------
    if (st == MissileStatus::Detonated && mc->target_id != 0) {
        const entities::EntityHandle target(entities::EntityId{mc->target_id}, world);
        auto* dmg = target.get<entities::DamageStateComponent>();
        if (dmg != nullptr && !dmg->killed && dmg->hit_points > 0.0) {
            const auto out = apply_damage(dmg->hit_points, dmg->max_hit_points,
                                          mc->warhead_power_lb, miss_distance,
                                          mc->lethal_radius_ft,
                                          /*roll01=*/0.5);
            dmg->hit_points = out.hit_points_after;
            if (out.killed) {
                dmg->killed = true;
                dmg->killed_by = mc->shooter_id;
                dmg->killed_at_tick = mc->tick_counter;
            }
            if (out.damage_applied > 0.0) {
                bus.publish(DamageAppliedMessage{
                    mc->target_id, mc->shooter_id, owner_.id().value,
                    out.damage_applied, out.hit_points_after, out.killed,
                    sim_time()});
            }
            if (out.killed) {
                bus.publish(EntityKilledMessage{mc->target_id, mc->shooter_id,
                                                sim_time()});
            }
        }
    }

    // --- Terminal message ----------------------------------------------------------
    bus.publish(MissileDetonatedMessage{
        owner_.id().value, mc->shooter_id, mc->target_id, cause,
        mc->missile.position(), miss_distance, mc->missile.flight_time_s(),
        sim_time()});
}

// ============================================================================
// launch_missile — the only sanctioned way to create a missile entity.
// ============================================================================
entities::EntityId launch_missile(entities::EntityWorld& world,
                                  messaging::MessageBus& bus,
                                  const entities::EntityHandle& shooter,
                                  entities::EntityId target,
                                  const WeaponClassTable& table,
                                  std::uint32_t weapon_handle,
                                  double sim_time_s) {
    const auto* rec = table.get(weapon_handle);
    if (rec == nullptr) {
        return entities::EntityId{};
    }
    if (rec->category != WeaponCategory::AirToAirMissile &&
        rec->category != WeaponCategory::AirToGroundMissile) {
        // Missiles only. Guns have their own path; bombs are M2+.
        return entities::EntityId{};
    }
    auto* shooter_transform = shooter.get<entities::TransformComponent>();
    auto* store = shooter.get<WeaponStoreComponent>();
    if (shooter_transform == nullptr || store == nullptr) {
        return entities::EntityId{};
    }

    // Find a loaded station of THIS weapon and debit it (before creating
    // anything — a dry launch changes nothing).
    std::size_t station_idx = WeaponStoreComponent::npos;
    for (std::size_t i = 0; i < store->station_count(); ++i) {
        const auto* s = store->station(i);
        if (s != nullptr && s->weapon_handle == weapon_handle && s->rounds > 0) {
            station_idx = i;
            break;
        }
    }
    if (station_idx == WeaponStoreComponent::npos) {
        return entities::EntityId{};
    }
    store->expend(station_idx, 1);

    // --- Create the missile entity ---------------------------------------------
    auto missile_handle = world.create();

    // Muzzle offset: ~15 ft ahead of the shooter's origin along its velocity
    // (or east if stationary) — keeps the missile out of the shooter's own
    // proximity-fuze volume.
    f4::math::Vec3<double> fwd{shooter_transform->vx, shooter_transform->vy,
                               shooter_transform->vz};
    const double shooter_speed = fwd.length();
    fwd = shooter_speed > 0.0 ? fwd / shooter_speed
                              : f4::math::Vec3<double>{1.0, 0.0, 0.0};
    const f4::geo::WorldPosition muzzle{
        shooter_transform->position.x + fwd.x * 15.0,
        shooter_transform->position.y + fwd.y * 15.0,
        shooter_transform->position.z + fwd.z * 15.0};

    auto& tc = missile_handle.add<entities::TransformComponent>();
    tc.position = muzzle;
    tc.vx = shooter_transform->vx;
    tc.vy = shooter_transform->vy;
    tc.vz = shooter_transform->vz;

    auto& mc = missile_handle.add<MissileComponent>();
    mc.weapon_handle = weapon_handle;
    mc.shooter_id = shooter.id().value;
    mc.target_id = target.value;
    mc.sim_time_at_launch_s = sim_time_s;
    mc.warhead_power_lb = rec->warhead_power_lb;
    mc.lethal_radius_ft = rec->lethal_radius_ft;
    mc.missile.launch(MissileConfig::from_record(*rec), muzzle,
                      f4::math::Vec3<double>{tc.vx, tc.vy, tc.vz});

    // Copy IFF: team tag + CampaignIdentityComponent (shooter's team).
    if (auto shooter_team = shooter.get_tag(entities::tags::TEAM);
        shooter_team.has_value()) {
        missile_handle.set_tag(entities::tags::TEAM, *shooter_team);
    }
    // ROLE="missile": what makes the missile an EMITTER in the RWR's eyes
    // (update_rwr's launch detection) and is_missile in SensorFusion's
    // target list. Without it the victim's RWR never sees the launch and
    // its AI never defends — the M3 tactics E2E caught exactly that.
    missile_handle.set_tag(entities::tags::ROLE,
                           entities::TagValue::from(std::string("missile")));
    if (auto* ident = shooter.get<entities::CampaignIdentityComponent>()) {
        missile_handle.add<entities::CampaignIdentityComponent>(*ident);
    }

    missile_handle.add<MissileSimComponent>();

    bus.publish(MissileLaunchedMessage{
        missile_handle.id().value, shooter.id().value, target.value,
        weapon_handle, muzzle, shooter_speed, sim_time_s});

    return missile_handle.id();
}

std::size_t sweep_spent_missiles(entities::EntityWorld& world) {
    std::size_t removed = 0;
    // with_component() returns a snapshot (by value), so destroying during
    // the loop is safe.
    for (const auto id : world.with_component<MissileComponent>()) {
        const entities::EntityHandle h(id, &world);
        const auto* mc = h.get<MissileComponent>();
        if (mc != nullptr && mc->missile.terminal()) {
            world.destroy(id);
            ++removed;
        }
    }
    return removed;
}

std::size_t count_live_missiles(const entities::EntityWorld& world) {
    std::size_t live = 0;
    for (const auto id : world.with_component<MissileComponent>()) {
        const entities::EntityHandle h(id, const_cast<entities::EntityWorld*>(&world));
        const auto* mc = h.get<MissileComponent>();
        if (mc != nullptr && !mc->missile.terminal()) {
            ++live;
        }
    }
    return live;
}

} // namespace f4::weapons

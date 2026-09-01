// f4-weapons/src/gun_component.cpp — see gun_component.hpp.

#include <f4/weapons/gun_component.hpp>

namespace f4::weapons {

std::vector<GunHit> update_guns(entities::EntityWorld& world,
                                messaging::MessageBus& bus,
                                double dt,
                                double sim_time_s) {
    std::vector<GunHit> hits;

    // with_component is a snapshot id list — safe while tick() mutates
    // component state (tracers, damage) without creating/destroying
    // entities. (A gun kill never removes the victim here; the corpse
    // sweep belongs to the host, like sweep_spent_missiles.)
    for (const auto eid : world.with_component<GunComponent>()) {
        entities::EntityHandle owner(eid, &world);
        auto* gun = owner.get<GunComponent>();
        if (gun == nullptr) continue;

        const auto* tf = owner.get<entities::TransformComponent>();
        if (tf == nullptr) continue;  // no pose -> nothing to fly from

        // Boresight: the velocity unit vector (fallback east when
        // stationary — never a NaN).
        f4::math::Vec3<double> fwd{tf->vx, tf->vy, tf->vz};
        const double speed = fwd.length();
        fwd = speed > 0.0 ? fwd / speed
                          : f4::math::Vec3<double>{1.0, 0.0, 0.0};

        const f4::geo::WorldPosition muzzle{
            tf->position.x + fwd.x * gun->muzzle_offset_ft,
            tf->position.y + fwd.y * gun->muzzle_offset_ft,
            tf->position.z + fwd.z * gun->muzzle_offset_ft};

        gun->stream.set_message_bus(&bus);
        gun->stream.set_sim_time(sim_time_s);
        gun->stream.set_weapon_handle(gun->weapon_handle);

        auto tick_hits = gun->stream.tick(dt, world, eid.value, muzzle, fwd);
        hits.insert(hits.end(), tick_hits.begin(), tick_hits.end());
    }
    return hits;
}

} // namespace f4::weapons

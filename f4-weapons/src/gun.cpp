// f4-weapons/src/gun.cpp — see gun.hpp.

#include <f4/weapons/gun.hpp>

#include <f4/weapons/messages.hpp>
#include <f4/weapons/missile.hpp>   // kGravityFps2 (shared sim constant)

#include <algorithm>
#include <cmath>

namespace f4::weapons {

namespace {

[[nodiscard]] double distance(const f4::geo::WorldPosition& a,
                              const f4::geo::WorldPosition& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

GunStream::GunStream(GunConfig config, std::uint32_t seed)
    : cfg_(config), rng_(seed) {}

void GunStream::start_burst(int rounds) {
    burst_remaining_ = static_cast<double>(std::max(0, rounds));
    burst_size_ = std::max(0, rounds);
    emit_carry_ = 0.0;
    burst_announced_ = false;
}

void GunStream::emit_round(const f4::geo::WorldPosition& muzzle,
                           const f4::math::Vec3<double>& unit_direction) {
    GunTracer t;
    t.position = muzzle;
    t.velocity = unit_direction * cfg_.muzzle_velocity_fps;

    // Dispersion: random angular offset up to dispersion_rad, uniform in
    // disc (r = R*sqrt(u) for uniform area), applied on a perpendicular
    // basis of the firing direction.
    if (cfg_.dispersion_rad > 0.0) {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        const double r = cfg_.dispersion_rad * std::sqrt(unit(rng_));
        const double theta = 6.283185307179586476925 * unit(rng_);

        f4::math::Vec3<double> helper{0.0, 0.0, 1.0};
        if (std::abs(unit_direction.z) > 0.9) {
            helper = f4::math::Vec3<double>{1.0, 0.0, 0.0};
        }
        const f4::math::Vec3<double> side = unit_direction.cross(helper).normalized();
        const f4::math::Vec3<double> up = side.cross(unit_direction);

        const f4::math::Vec3<double> offset =
            (side * std::cos(theta) + up * std::sin(theta)) * std::tan(r);
        t.velocity = (unit_direction + offset).normalized() * cfg_.muzzle_velocity_fps;
    }

    tracers_.push_back(t);
}

std::vector<GunHit> GunStream::tick(double dt,
                                    entities::EntityWorld& world,
                                    std::uint64_t shooter_id,
                                    const f4::geo::WorldPosition& muzzle,
                                    const f4::math::Vec3<double>& direction) {
    std::vector<GunHit> hits;

    // --- Emit scheduled rounds (rate-based with a fractional carry) ---------
    if (burst_remaining_ > 0.0 && dt > 0.0) {
        emit_carry_ += cfg_.rounds_per_minute / 60.0 * dt;
        const f4::math::Vec3<double> unit_dir = direction.normalized();
        while (emit_carry_ >= 1.0 && burst_remaining_ >= 1.0) {
            emit_carry_ -= 1.0;
            burst_remaining_ -= 1.0;
            emit_round(muzzle, unit_dir);

            if (!burst_announced_ && bus_ != nullptr) {
                burst_announced_ = true;
                bus_->publish(GunFiredMessage{
                    shooter_id, /*target_id=*/0, burst_size_, muzzle,
                    /*sim_time_s=*/0.0});
            }
        }
        if (burst_remaining_ < 1.0) {
            burst_remaining_ = 0.0;  // burst exhausted
        }
    }

    // --- Integrate tracers -----------------------------------------------------
    for (auto& t : tracers_) {
        t.velocity.z -= kGravityFps2 * dt;
        t.position.x += t.velocity.x * dt;
        t.position.y += t.velocity.y * dt;
        t.position.z += t.velocity.z * dt;
        t.age_s += dt;
    }

    // --- Hit detection -----------------------------------------------------------
    // with_component() returns a snapshot id list per call; tracers are
    // removed after processing, never the world.
    for (auto it = tracers_.begin(); it != tracers_.end(); /* manual */) {
        bool consumed = false;
        for (const auto target_id : world.with_component<entities::TransformComponent>()) {
            if (target_id.value == shooter_id) {
                continue;
            }
            const entities::EntityHandle h(target_id, &world);
            const auto* tc = h.get<entities::TransformComponent>();
            if (tc == nullptr) {
                continue;
            }
            const double d = distance(it->position, tc->position);
            if (d > kGunHitRadiusFt) {
                continue;
            }
            // Hit: apply damage if the target can take it.
            GunHit hit;
            hit.target_id = target_id.value;
            hit.shooter_id = shooter_id;
            if (auto* dmg = h.get<entities::DamageStateComponent>();
                dmg != nullptr && !dmg->killed && dmg->hit_points > 0.0) {
                std::uniform_real_distribution<double> unit(0.0, 1.0);
                const auto out = apply_damage(dmg->hit_points, dmg->max_hit_points,
                                              cfg_.round_power_lb, d,
                                              cfg_.lethal_radius_ft, unit(rng_));
                dmg->hit_points = out.hit_points_after;
                hit.damage = out.damage_applied;
                hit.killed = out.killed;
                if (out.killed) {
                    dmg->killed = true;
                    dmg->killed_by = shooter_id;
                }
                if (bus_ != nullptr && out.damage_applied > 0.0) {
                    bus_->publish(DamageAppliedMessage{
                        target_id.value, shooter_id, /*missile_id=*/0,
                        out.damage_applied, out.hit_points_after, out.killed,
                        /*sim_time_s=*/0.0});
                }
                if (bus_ != nullptr && out.killed) {
                    bus_->publish(EntityKilledMessage{target_id.value, shooter_id,
                                                      /*sim_time_s=*/0.0});
                }
            }
            hits.push_back(hit);
            consumed = true;  // tracer spent on first proximity hit
            break;
        }

        // --- Lifetime cleanup -----------------------------------------------------
        if (!consumed && it->age_s >= cfg_.max_flight_s) {
            consumed = true;  // tracer expires, no hit
        }
        if (consumed) {
            it = tracers_.erase(it);
        } else {
            ++it;
        }
    }

    return hits;
}

} // namespace f4::weapons

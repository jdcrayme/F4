// f4-weapons/src/bomb_battery.cpp — ECS binding for gravity bombs.
//
// Mirrors missile_battery.cpp structure-for-structure: the pure flyout
// lives in Bomb (bomb.hpp), this file owns the entity lifecycle (release →
// fall → impact), the objective feature-damage endpoint, and the bus
// messages. The impact-plane check + the per-feature blast walk are the
// two things a missile never does — everything else follows the missile
// contract (store debit before entity creation, transform mirroring,
// terminal handling exactly once, host-side sweep).

#include <f4/weapons/bomb_battery.hpp>

#include <f4/weapons/damage.hpp>
#include <f4/weapons/messages.hpp>

#include <algorithm>
#include <cmath>

namespace f4::weapons {

namespace {

/// Default feature hit points when the FCD class data is missing (the
/// fixture FCD covers a subset of the game's feature classes; FreeFalcon
/// feature hit points typically run 20-200, 100 is the documented default).
constexpr double kDefaultFeatureHp = 100.0;

/// Horizontal (2D) distance between two positions, feet. Feature damage
/// keys on ground distance: both the impact point and the feature
/// placements sit on the objective's ground plane, and vertical offsets
/// (bomb z at the plane, feature z from FED) would add noise, not signal.
[[nodiscard]] double horizontal_distance(
    const f4::geo::WorldPosition& a, const f4::geo::WorldPosition& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

/// VIS damage states (f4vu.h): 0 normal, 1 repaired, 2 damaged, 3 destroyed.
constexpr uint8_t kVisDestroyed = 3;
constexpr uint8_t kVisDamaged   = 2;

/// Write the 2-bit fstatus entry for feature `i` (grow the vector as
/// needed — objectives whose save carried no fstatus start empty).
void write_fstatus(std::vector<uint8_t>& fstatus, std::size_t i,
                   uint8_t vis) noexcept {
    const std::size_t byte_idx = i / 4;
    if (fstatus.size() <= byte_idx) fstatus.resize(byte_idx + 1, 0);
    const std::size_t shift = (i % 4) * 2;
    fstatus[byte_idx] = static_cast<uint8_t>(
        (fstatus[byte_idx] & ~(0x03 << shift)) | (vis << shift));
}

} // namespace

// ============================================================================
// Objective feature damage
// ============================================================================

ObjectiveDamageResult
apply_objective_feature_damage(entities::EntityWorld& world,
                               std::uint64_t objective_id,
                               const f4::geo::WorldPosition& impact_point,
                               double warhead_power_lb,
                               double lethal_radius_ft,
                               double roll01) {
    using namespace entities;

    ObjectiveDamageResult out;
    out.objective_id = objective_id;

    EntityHandle h(EntityId{objective_id}, &world);
    auto* fs = h.get<FeatureSetComponent>();
    if (fs == nullptr || fs->features.empty()) {
        out.objective_found = (h.get<TransformComponent>() != nullptr);
        return out;  // not an objective / no features — nothing to damage
    }
    out.objective_found = true;
    out.features_total = static_cast<int>(fs->features.size());

    // The objective's ENU position: features place relative to this.
    const auto* obj_tf = h.get<TransformComponent>();
    f4::geo::WorldPosition center{};
    if (obj_tf != nullptr) center = obj_tf->position;

    // Lazily initialize the live hp ledger (pristine world until now).
    if (fs->feature_hp.size() != fs->features.size()) {
        fs->feature_hp.assign(fs->features.size(), kDefaultFeatureHp);
        for (std::size_t i = 0; i < fs->features.size(); ++i) {
            const double hp = fs->features[i].hit_points;
            if (hp > 0.0) fs->feature_hp[i] = hp;
            // A feature the save already carries as destroyed starts at 0
            // hp (damage_state is the loaded wire state).
            if (fs->features[i].damage_state == kVisDestroyed) {
                fs->feature_hp[i] = 0.0;
            }
        }
    }

    // The fstatus bitmap: create-on-first-damage when the save carried none.
    auto* bitmap = h.get<DamageBitmapComponent>();
    if (bitmap == nullptr) {
        bitmap = &h.add<DamageBitmapComponent>();
    }

    // Walk the features: one blast per feature in radius.
    int destroyed_before = 0;
    int destroyed_after = 0;
    double value_total = 0.0;
    double value_destroyed_after = 0.0;
    for (std::size_t i = 0; i < fs->features.size(); ++i) {
        auto& feature = fs->features[i];

        // The feature's placement: objective center + FED offset
        // (offset_x east, offset_y north — the same convention the
        // viewer's 2D feature dots use).
        const f4::geo::WorldPosition fp{
            center.x + feature.offset_x,
            center.y + feature.offset_y,
            center.z + feature.offset_z};
        const double burst_range = horizontal_distance(impact_point, fp);

        // Value weighting (FED "value" = % loss in operational status for
        // destruction; 0 in the partial fixture data — count each feature
        // equally then).
        const double weight = (feature.value > 0) ? feature.value : 1.0;
        value_total += weight;

        const uint8_t vis_before = feature.damage_state;
        if (vis_before == kVisDestroyed) {
            ++destroyed_before;
            ++destroyed_after;
            value_destroyed_after += weight;
            continue;  // rubble takes no further damage
        }

        // Apply the blast (same model as airframe damage; nominal roll —
        // f4-weapons never rolls its own dice).
        const double hp_before = fs->feature_hp[i];
        const double max_hp = (feature.hit_points > 0)
            ? static_cast<double>(feature.hit_points)
            : kDefaultFeatureHp;
        const auto dmg = apply_damage(hp_before, max_hp,
                                      warhead_power_lb, burst_range,
                                      lethal_radius_ft, roll01);
        fs->feature_hp[i] = dmg.hit_points_after;
        out.damage_applied += dmg.damage_applied;

        // Map the ledger back to the wire VIS state: killed -> destroyed,
        // any applied damage -> damaged (FreeFalcon marks a feature
        // VIS_DAMAGED the first blast it survives — there is no partial
        // threshold in SetFeatureStatus), else unchanged (normal).
        uint8_t vis_after = vis_before;
        if (dmg.killed) {
            vis_after = kVisDestroyed;
        } else if (dmg.damage_applied > 0.0) {
            vis_after = kVisDamaged;
        }
        if (vis_after != vis_before) {
            feature.damage_state = vis_after;
            write_fstatus(bitmap->fstatus, i, vis_after);
            if (vis_after == kVisDestroyed) {
                ++out.features_destroyed;
                ++destroyed_after;
                value_destroyed_after += weight;
            } else {
                ++out.features_damaged;
            }
        } else if (vis_before == kVisDamaged) {
            // stays damaged — counts for the summary's totals below
        }
    }

    out.features_destroyed_total = destroyed_after;
    out.destroyed_pct = (value_total > 0.0)
        ? 100.0 * value_destroyed_after / value_total
        : 0.0;
    (void)destroyed_before;
    return out;
}

ObjectiveDamageResult
objective_damage_summary(const entities::EntityWorld& world,
                         std::uint64_t objective_id) {
    using namespace entities;

    ObjectiveDamageResult out;
    out.objective_id = objective_id;

    const EntityHandle h(EntityId{objective_id},
                         const_cast<EntityWorld*>(&world));
    const auto* fs = h.get<FeatureSetComponent>();
    if (fs == nullptr || fs->features.empty()) {
        out.objective_found = (h.get<TransformComponent>() != nullptr);
        return out;
    }
    out.objective_found = true;
    out.features_total = static_cast<int>(fs->features.size());

    double value_total = 0.0;
    double value_destroyed = 0.0;
    for (std::size_t i = 0; i < fs->features.size(); ++i) {
        const auto& f = fs->features[i];
        const double weight = (f.value > 0) ? f.value : 1.0;
        value_total += weight;
        if (f.damage_state == kVisDestroyed) {
            ++out.features_destroyed_total;
            value_destroyed += weight;
        }
    }
    out.destroyed_pct = (value_total > 0.0)
        ? 100.0 * value_destroyed / value_total
        : 0.0;
    return out;
}

// ============================================================================
// BombSimComponent — the fall, the impact, the terminal messages
// ============================================================================

void BombSimComponent::update(double dt, messaging::MessageBus& bus) {
    if (!owner_.valid()) return;
    auto* world = owner_.world();
    auto* bc = owner_.get<BombComponent>();
    if (world == nullptr || bc == nullptr || bc->bomb.terminal()) return;

    bc->bomb.tick(dt);
    ++bc->tick_counter;

    // Mirror state into the transform (the recorder + viewer track bombs
    // through the same snapshot stream missiles use).
    auto* tc = owner_.get<entities::TransformComponent>();
    if (tc != nullptr) {
        tc->position = bc->bomb.position();
        tc->vx = bc->bomb.velocity().x;
        tc->vy = bc->bomb.velocity().y;
        tc->vz = bc->bomb.velocity().z;
    }

    if (!bc->bomb.terminal()) return;

    // --- Terminal handling (exactly once per bomb) -------------------------
    const BombStatus st = bc->bomb.status();
    const BombEndCause cause = (st == BombStatus::Impact)
        ? BombEndCause::Impact : BombEndCause::Expired;
    const f4::geo::WorldPosition impact = bc->bomb.position();
    const double miss = horizontal_distance(impact, bc->aim_point);

    // Objective feature damage: the campaign-side endpoint. Only a real
    // impact against a resolvable target applies damage; an Expired bomb
    // (never reached the plane) harmlessly records its end.
    if (st == BombStatus::Impact && bc->target_id != 0) {
        auto result = apply_objective_feature_damage(
            *world, bc->target_id, impact, bc->warhead_power_lb,
            bc->lethal_radius_ft, /*roll01=*/0.5);
        if (result.objective_found) {
            bus.publish(BombImpactMessage{
                owner_.id().value, bc->shooter_id, bc->target_id, cause,
                impact, miss, bc->bomb.flight_time_s(),
                result.features_damaged, result.features_destroyed,
                result.destroyed_pct, sim_time()});
            return;
        }
        // Target id resolved no objective (a unit target, or stale): the
        // impact still reports — with the damage summary zeroed.
    }
    bus.publish(BombImpactMessage{
        owner_.id().value, bc->shooter_id, bc->target_id, cause,
        impact, miss, bc->bomb.flight_time_s(),
        0, 0, 0.0, sim_time()});
}

// ============================================================================
// release_bomb — the only sanctioned way to create a bomb entity.
// ============================================================================

entities::EntityId release_bomb(entities::EntityWorld& world,
                                messaging::MessageBus& bus,
                                const entities::EntityHandle& shooter,
                                entities::EntityId target,
                                const WeaponClassTable& table,
                                std::uint32_t weapon_handle,
                                double sim_time_s) {
    const auto* rec = table.get(weapon_handle);
    if (rec == nullptr) return entities::EntityId{};
    if (rec->category != WeaponCategory::Bomb) {
        // Bombs only. Missiles have launch_missile; guns have GunStream.
        return entities::EntityId{};
    }
    auto* shooter_transform = shooter.get<entities::TransformComponent>();
    auto* store = shooter.get<WeaponStoreComponent>();
    if (shooter_transform == nullptr || store == nullptr) {
        return entities::EntityId{};
    }

    // Find a loaded station of THIS weapon and debit it (before creating
    // anything — a dry release changes nothing).
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

    // --- The aim point + impact plane ---------------------------------------
    // The aim point: the target objective's position when it resolves (and
    // carries a transform). The aim point drives the impact plane (its z)
    // and the reported miss distance.
    f4::geo::WorldPosition aim_point = shooter_transform->position;
    bool have_aim = false;
    if (target.valid()) {
        const entities::EntityHandle tgt(target, &world);
        if (const auto* tf = tgt.get<entities::TransformComponent>()) {
            aim_point = tf->position;
            have_aim = true;
        }
    }
    // No target (or the target carries no transform): the impact plane
    // falls back to MSL 0 — a ballistic-only release still falls until it
    // reaches sea level (the tof limit bounds a release made above it).
    // NOT the shooter's own z: a same-altitude plane would terminal the
    // bomb at the instant of release.
    const double impact_plane_z = have_aim ? aim_point.z : 0.0;

    // --- Create the bomb entity ---------------------------------------------
    auto bomb_handle = world.create();

    auto& tc = bomb_handle.add<entities::TransformComponent>();
    tc.position = shooter_transform->position;
    tc.vx = shooter_transform->vx;
    tc.vy = shooter_transform->vy;
    tc.vz = shooter_transform->vz;

    auto& bc = bomb_handle.add<BombComponent>();
    bc.weapon_handle = weapon_handle;
    bc.shooter_id = shooter.id().value;
    bc.target_id = target.value;
    bc.sim_time_at_release_s = sim_time_s;
    bc.warhead_power_lb = rec->warhead_power_lb;
    bc.lethal_radius_ft = rec->lethal_radius_ft;
    bc.aim_point = aim_point;
    bc.bomb.release(BombConfig::from_record(*rec),
                    shooter_transform->position,
                    f4::math::Vec3<double>{tc.vx, tc.vy, tc.vz},
                    impact_plane_z);

    // IFF copy: team tag + identity (same rule as launch_missile).
    if (auto shooter_team = shooter.get_tag(entities::tags::TEAM);
        shooter_team.has_value()) {
        bomb_handle.set_tag(entities::tags::TEAM, *shooter_team);
    }
    // ROLE="bomb": what the recorder keys bomb tracks on (missiles carry
    // "missile" — the replay discriminates the two weapon streams by role).
    bomb_handle.set_tag(entities::tags::ROLE,
                        entities::TagValue::from(std::string("bomb")));
    if (auto* ident = shooter.get<entities::CampaignIdentityComponent>()) {
        bomb_handle.add<entities::CampaignIdentityComponent>(*ident);
    }

    bomb_handle.add<BombSimComponent>();

    bus.publish(BombReleasedMessage{
        bomb_handle.id().value, shooter.id().value, target.value,
        weapon_handle, shooter_transform->position,
        f4::math::Vec3<double>{tc.vx, tc.vy, tc.vz}.length(), sim_time_s});

    return bomb_handle.id();
}

std::size_t sweep_spent_bombs(entities::EntityWorld& world) {
    std::size_t removed = 0;
    // with_component() returns a snapshot (by value) — safe to destroy
    // during the loop.
    for (const auto id : world.with_component<BombComponent>()) {
        const entities::EntityHandle h(id, &world);
        const auto* bc = h.get<BombComponent>();
        if (bc != nullptr && bc->bomb.terminal()) {
            world.destroy(id);
            ++removed;
        }
    }
    return removed;
}

std::size_t count_live_bombs(const entities::EntityWorld& world) {
    std::size_t live = 0;
    for (const auto id : world.with_component<BombComponent>()) {
        const entities::EntityHandle h(id, const_cast<entities::EntityWorld*>(&world));
        const auto* bc = h.get<BombComponent>();
        if (bc != nullptr && !bc->bomb.terminal()) ++live;
    }
    return live;
}

} // namespace f4::weapons

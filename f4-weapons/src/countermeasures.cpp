// f4-weapons/src/countermeasures.cpp — dispensers, decoy motion, and
// the seeker-seduction model. See countermeasures.hpp for the design.

#include <f4/weapons/countermeasures.hpp>

#include <f4/weapons/messages.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <unordered_set>

namespace f4::weapons {

namespace {

/// Distance between two positions (feet) — missile_battery's helper shape.
[[nodiscard]] inline double distance(const f4::geo::WorldPosition& a,
                                     const f4::geo::WorldPosition& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// The direct target read — EXACTLY missile_battery.cpp's
/// snapshot_target (a dead/missing target reads invalid; a dead but
/// still-rendered corpse paints — the M1 contract).
[[nodiscard]] TargetSnapshot read_target_snapshot(
    const entities::EntityWorld& world, std::uint64_t target_id) {
    TargetSnapshot snap;
    if (target_id == 0) return snap;
    const entities::EntityHandle target(entities::EntityId{target_id},
                                        const_cast<entities::EntityWorld*>(
                                            &world));
    if (auto* t = target.get<entities::TransformComponent>()) {
        snap.valid = true;
        snap.position = t->position;
        snap.velocity = f4::math::Vec3<double>{t->vx, t->vy, t->vz};
    }
    return snap;
}

[[nodiscard]] std::string team_of(const entities::EntityHandle& h) {
    if (auto tag = h.get_tag(entities::tags::TEAM)) {
        if (const auto* s = tag->as_string()) return *s;
    }
    return {};
}

} // namespace

// ============================================================================
// DecoySimComponent — the decoy's one-tick physics.
// ============================================================================
void DecoySimComponent::update(double dt, messaging::MessageBus& bus) {
    if (!owner_.valid()) return;
    auto* world = owner_.world();
    auto* tc = owner_.get<entities::TransformComponent>();
    const auto* decoy = owner_.get<DecoyComponent>();
    if (world == nullptr || tc == nullptr || decoy == nullptr) return;

    (void)bus;
    f4::math::Vec3<double> vel{tc->vx, tc->vy, tc->vz};

    if (decoy->kind == DecoyKind::Flare) {
        // A falling candle: gravity, light aerodynamic drag.
        if (flare_gravity) vel.z -= kGravityFps2 * dt;
        const double keep = std::max(0.0, 1.0 - flare_drag_per_s * dt);
        vel = vel * keep;
    } else {
        // A chaff bloom: the bundle shatters and the cloud stalls into
        // the airstream — hard drag toward rest.
        const double keep = std::max(0.0, 1.0 - chaff_drag_per_s * dt);
        vel = vel * keep;
    }

    tc->position = f4::geo::WorldPosition{
        tc->position.x + vel.x * dt,
        tc->position.y + vel.y * dt,
        tc->position.z + vel.z * dt};
    tc->vx = vel.x;
    tc->vy = vel.y;
    tc->vz = vel.z;
}

// ============================================================================
// deploy_countermeasure — the dispenser.
// ============================================================================
int deploy_countermeasure(entities::EntityWorld& world,
                          messaging::MessageBus& bus,
                          const entities::EntityHandle& aircraft,
                          DecoyKind kind,
                          double sim_time_s,
                          std::uint32_t rng_seed) {
    auto* tc = aircraft.get<entities::TransformComponent>();
    auto* cm = aircraft.get<CountermeasureComponent>();
    if (tc == nullptr || cm == nullptr) return 0;

    // The salvo interval paces the (per-tick) defeat intents.
    double& last = (kind == DecoyKind::Chaff) ? cm->last_chaff_s
                                              : cm->last_flare_s;
    if (sim_time_s - last < cm->salvo_interval_s) return 0;

    const int salvo = std::max(
        0, (kind == DecoyKind::Chaff) ? cm->chaff_salvo : cm->flare_salvo);
    int& rounds = (kind == DecoyKind::Chaff) ? cm->chaff_rounds
                                             : cm->flare_rounds;
    const int count = std::min(salvo, rounds);
    if (count <= 0) return 0;
    rounds -= count;
    last = sim_time_s;

    // Dispersion: seeded per call (owner + kind + salvo time) so the
    // same world state produces the same bloom pattern.
    std::mt19937 rng(rng_seed ^
                     static_cast<std::uint32_t>(aircraft.id().value) ^
                     static_cast<std::uint32_t>(
                         static_cast<int>(kind)) ^
                     static_cast<std::uint32_t>(sim_time_s * 1024.0));
    std::uniform_real_distribution<double> spread{-60.0, 60.0};
    std::uniform_real_distribution<double> drop{-60.0, 20.0};

    // Release point: ~15 ft BEHIND the aircraft (the airframe is ahead
    // of its own dispensers by the time the bundle inflates).
    f4::math::Vec3<double> vel{tc->vx, tc->vy, tc->vz};
    const double speed = vel.length();
    const f4::math::Vec3<double> fwd =
        speed > 1.0 ? vel / speed : f4::math::Vec3<double>{0.0, 0.0, 0.0};
    const f4::geo::WorldPosition release{
        tc->position.x - fwd.x * 15.0,
        tc->position.y - fwd.y * 15.0,
        tc->position.z - fwd.z * 15.0};

    for (int i = 0; i < count; ++i) {
        auto decoy = world.create();

        auto& dtf = decoy.add<entities::TransformComponent>();
        dtf.position = release;

        // Initial velocity: a slice of the airframe's, plus the
        // dispenser's eject. Chaff scatters wide and stays with the
        // airstream; flares eject down-and-out and then fall.
        f4::math::Vec3<double> dv;
        if (kind == DecoyKind::Chaff) {
            dv = vel * 0.55 +
                 f4::math::Vec3<double>{spread(rng), spread(rng),
                                        drop(rng) * 0.5};
        } else {
            dv = vel * 0.85 +
                 f4::math::Vec3<double>{spread(rng) * 0.5, spread(rng) * 0.5,
                                        -40.0 + drop(rng) * 0.5};
        }
        dtf.vx = dv.x;
        dtf.vy = dv.y;
        dtf.vz = dv.z;

        auto& dc = decoy.add<DecoyComponent>();
        dc.kind = kind;
        dc.owner_id = aircraft.id().value;
        dc.born_s = sim_time_s;
        dc.ttl_s = (kind == DecoyKind::Chaff) ? 5.0 : 3.5;

        decoy.add<DecoySimComponent>();

        // IFF: the decoy flies the OWNER's colors (a red flare never
        // baits a red missile). ROLE "decoy" keeps the observability
        // convention (missiles carry "missile").
        if (auto team = aircraft.get_tag(entities::tags::TEAM);
            team.has_value()) {
            decoy.set_tag(entities::tags::TEAM, *team);
        }
        decoy.set_tag(entities::tags::ROLE,
                      entities::TagValue::from(std::string("decoy")));
    }

    bus.publish(CountermeasureDeployedMessage{
        aircraft.id().value, static_cast<std::uint32_t>(kind), count,
        release, sim_time_s});

    return count;
}

std::size_t sweep_expired_decoys(entities::EntityWorld& world,
                                 double sim_time_s) {
    std::size_t removed = 0;
    // with_component() returns a snapshot (by value) — destroy-safe.
    for (const auto id : world.with_component<DecoyComponent>()) {
        const entities::EntityHandle h(id, &world);
        const auto* dc = h.get<DecoyComponent>();
        if (dc != nullptr && dc->expired_at(sim_time_s)) {
            world.destroy(id);
            ++removed;
        }
    }
    return removed;
}

std::size_t count_live_decoys(const entities::EntityWorld& world,
                              DecoyKind kind, double sim_time_s) {
    std::size_t n = 0;
    for (const auto id : world.with_component<DecoyComponent>()) {
        const entities::EntityHandle h(
            id, const_cast<entities::EntityWorld*>(&world));
        const auto* dc = h.get<DecoyComponent>();
        if (dc != nullptr && dc->kind == kind &&
            !dc->expired_at(sim_time_s)) {
            ++n;
        }
    }
    return n;
}

// ============================================================================
// The seduction model.
// ============================================================================
MissileComponent::SeekerSourceFn
make_decoy_aware_seeker_source(const SeekerCountermeasureConfig& cfg) {
    struct State {
        std::mt19937 rng;
        std::uint64_t seduced_id = 0;
        std::unordered_set<std::uint64_t> failed;
        explicit State(std::uint32_t seed) : rng(seed) {}
    };
    auto state = std::make_shared<State>(cfg.rng_seed);

    const SeekerCountermeasureConfig c = cfg;   // captured by value

    return [state, c](const entities::EntityWorld& world,
                      std::uint64_t target_id) -> TargetSnapshot {
        const double now = MissileSimComponent::sim_time();

        // --- The seduced decoy (sticky while it lives) -------------------
        if (state->seduced_id != 0) {
            const entities::EntityHandle decoy(
                entities::EntityId{state->seduced_id},
                const_cast<entities::EntityWorld*>(&world));
            const auto* dc = decoy.get<DecoyComponent>();
            const auto* tf = decoy.get<entities::TransformComponent>();
            if (dc != nullptr && tf != nullptr &&
                !dc->expired_at(now)) {
                TargetSnapshot snap;
                snap.valid = true;
                snap.position = tf->position;
                snap.velocity =
                    f4::math::Vec3<double>{tf->vx, tf->vy, tf->vz};
                return snap;
            }
            state->seduced_id = 0;   // burnt out / swept — re-arm
        }

        // --- Candidate decoys inside the seeker envelope -----------------
        const entities::EntityHandle missile(
            entities::EntityId{c.missile_id},
            const_cast<entities::EntityWorld*>(&world));
        const auto* mtf = missile.get<entities::TransformComponent>();
        if (mtf != nullptr) {
            const f4::math::Vec3<double> mvel{mtf->vx, mtf->vy, mtf->vz};
            const double mvel_len = mvel.length();

            std::uniform_real_distribution<double> uniform01{0.0, 1.0};

            for (const auto& [eid, dc] :
                 world.with_component_ref<DecoyComponent>()) {
                if (dc->expired_at(now)) continue;
                if (dc->kind == DecoyKind::Chaff &&
                    (c.guidance != GuidanceKind::SemiActiveRadar &&
                     c.guidance != GuidanceKind::ActiveRadar)) {
                    continue;
                }
                if (dc->kind == DecoyKind::Flare &&
                    c.guidance != GuidanceKind::Ir) {
                    continue;
                }
                // IFF: same-team decoys never seduce.
                const entities::EntityHandle decoy(
                    eid, const_cast<entities::EntityWorld*>(&world));
                if (team_of(decoy) == c.shooter_team) continue;

                // Envelope: range + seeker cone off the velocity axis.
                const auto* dtf = decoy.get<entities::TransformComponent>();
                if (dtf == nullptr) continue;
                const double dx = dtf->position.x - mtf->position.x;
                const double dy = dtf->position.y - mtf->position.y;
                const double dz = dtf->position.z - mtf->position.z;
                const double rng_ft = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (rng_ft > c.seeker_max_range_ft) continue;
                if (mvel_len > 1.0 && rng_ft > 1.0) {
                    const f4::math::Vec3<double> los{
                        dx / rng_ft, dy / rng_ft, dz / rng_ft};
                    const double cos_half =
                        mvel.dot(los) / mvel_len;
                    const double half =
                        std::cos(c.seeker_half_angle_rad);
                    if (cos_half < half) continue;   // outside the cone
                }

                // One honest roll per decoy; a failed bloom never
                // re-rolls (60 Hz re-rolls would be certain seduction).
                if (state->failed.count(eid.value) != 0) continue;
                const double p = (c.guidance == GuidanceKind::Ir)
                                     ? c.flare_chance
                                     : c.chaff_transfer_p;
                if (uniform01(state->rng) < p) {
                    state->seduced_id = eid.value;
                    TargetSnapshot snap;
                    snap.valid = true;
                    snap.position = dtf->position;
                    snap.velocity =
                        f4::math::Vec3<double>{dtf->vx, dtf->vy, dtf->vz};
                    return snap;
                }
                state->failed.insert(eid.value);
            }
        }

        // --- No bait taken: the direct target read ------------------------
        return read_target_snapshot(world, target_id);
    };
}

} // namespace f4::weapons

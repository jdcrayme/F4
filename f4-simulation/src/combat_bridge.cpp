// f4-simulation/src/combat_bridge.cpp
//
// Implementation of the M3 combat integration bridge (combat_bridge.hpp):
// the spawn-side component attachment + the SensorFusion detection policy
// backed by f4-sensors state. See the header for the design notes.

#include "f4/simulation/combat_bridge.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/entities/types.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/sensors/track_store.hpp>

namespace f4::simulation {

namespace {

constexpr double FEET_PER_NM = 6076.11548;

/// The BVR weapon: the LONGEST-RANGE air-to-air missile class in the
/// table (AIM-120C over AIM-9M). find_by_category would return the FIRST
/// A/A record — the Sidewinder — and the whole BVR doctrine (envelope,
/// launch station) would be scoped to a heat-seeker's 10 NM boundary.
const weapons::WeaponClassRecord* best_aa_missile(
    const weapons::WeaponClassTable& table) {
    const weapons::WeaponClassRecord* best = nullptr;
    for (const auto& rec : table.records()) {
        if (rec.category != weapons::WeaponCategory::AirToAirMissile)
            continue;
        if (best == nullptr || rec.max_range_ft > best->max_range_ft) {
            best = &rec;
        }
    }
    return best;
}

} // anonymous namespace

void configure_brain_combat(f4::ai::BrainComponent& brain,
                            const weapons::WeaponClassTable& table) {
    brain.set_combat_enabled(true);

    // Employment envelope from the BVR weapon class. The 0.5 factor turns
    // the AIM-120C's 40 NM aerodynamic boundary into a ~20 NM
    // doctrine-safe R_ne (the plan's default max_pk_range).
    const auto* rec = best_aa_missile(table);
    if (rec == nullptr || rec->max_range_ft <= 0.0) return;
    const double min_nm =
        std::max(rec->min_range_ft, 0.5 * FEET_PER_NM) / FEET_PER_NM;
    const double max_nm = 0.5 * rec->max_range_ft / FEET_PER_NM;
    brain.bvr().fire().set_envelope_nm(min_nm, max_nm);
}

void attach_combat_loadout(entities::EntityHandle& aircraft,
                           const weapons::WeaponClassTable& table,
                           const ScenarioAircraft& ac,
                           std::uint32_t seed_base,
                           std::size_t aircraft_index,
                           double hit_points) {
    // Identity first: the TEAM tag drives IFF (TrackStore), RWR emitter
    // role checks, and launch_missile's team copy. CampaignIdentity
    // carries the callsign the radar's NCTR resolves after a few scans.
    aircraft.set_tag(entities::tags::TEAM,
                     entities::TagValue::from(ac.team));
    auto& id = aircraft.add<entities::CampaignIdentityComponent>();
    id.team_id = (ac.team == "red") ? 1 : 0;
    id.callsign = ac.callsign;

    // Stores: the standard fighter loadout from the (built-in placeholder)
    // weapon class table. AIM-9M x2, AIM-7M x2, AIM-120C x8-ish — see
    // weapon_class_table.cpp for the exact station fill.
    aircraft.add<weapons::WeaponStoreComponent>(
        weapons::WeaponStoreComponent::standard_fighter(table));

    // Observability: default fighter RCS.
    aircraft.add<sensors::SignatureComponent>();

    // The radar: default parameter card + scan volume; per-aircraft seed
    // derived from the scenario base seed so the whole scenario stays
    // deterministic while co-mounted radars don't roll identical sequences.
    auto& radar = aircraft.add<sensors::RadarSimComponent>();
    radar.rng_seed = seed_base + static_cast<std::uint32_t>(aircraft_index);
    radar.own_team = ac.team;

    // The RWR (passive — Simulation::tick's update_rwr sweep fills it).
    aircraft.add<sensors::RwrComponent>();

    // Damage endpoint: hit points, not yet killed.
    auto& dmg = aircraft.add<entities::DamageStateComponent>();
    dmg.hit_points = hit_points;
    dmg.max_hit_points = hit_points;
}

f4::ai::SensorFusion::DetectionPolicy::Verdict
RadarBackedDetectionPolicy::classify(const f4::ai::TargetInfo& t) {
    Verdict v{};

    // The ownship must still exist — a dead AI's policy answers nothing.
    // (EntityHandle on a dead id yields null component lookups.)
    entities::EntityHandle ownship(entities::EntityId{ownship_id_}, world_);

    // CORPSES DON'T PAINT (the M3 host decision radar_component.hpp
    // defers to this layer): an entity whose DamageStateComponent is
    // killed answers all-false, so the shooter's SensorFusion drops it
    // from visibility the next refresh — the BVR module sees LostTarget,
    // stops engaging, and goes home instead of pumping the remaining
    // shoot-shoot allotment into a still-flying airframe. The corpse
    // itself keeps its FM (documented M2 simplification); only the
    // TARGET picture is filtered here.
    entities::EntityHandle candidate(entities::EntityId{t.entity_id}, world_);
    if (const auto* dmg = candidate.get<entities::DamageStateComponent>()) {
        if (dmg->killed) return v;
    }

    // radar: live track in the ownship's radar?
    if (const auto* radar = ownship.get<sensors::RadarSimComponent>()) {
        const auto* tf = radar->tracks().find(t.entity_id);
        if (tf != nullptr && tf->state != sensors::TrackState::Dropped) {
            v.radar = true;
        }
    }

    // rwr: is the candidate painting US?
    if (const auto* rwr = ownship.get<sensors::RwrComponent>()) {
        for (const auto& w : rwr->warnings) {
            if (w.emitter_id == t.entity_id) {
                v.rwr = true;
                break;
            }
        }
    }

    // visual + gci stay false: the flip off GCI-omniscience is the point.
    return v;
}

// ============================================================================
// Brain combat-intent driver
// ============================================================================

std::size_t execute_brain_combat_intents(entities::EntityWorld& world,
                                         messaging::MessageBus& bus,
                                         const weapons::WeaponClassTable& table,
                                         double sim_time_s) {
    std::size_t launches = 0;

    // Every entity with a combat-enabled brain. (with_component is a
    // copy of the id list — safe to create entities while iterating.)
    for (const auto eid : world.with_component<f4::ai::BrainComponent>()) {
        entities::EntityHandle shooter(eid, &world);
        auto* brain = shooter.get<f4::ai::BrainComponent>();
        if (!brain || !brain->combat_enabled()) continue;

        const auto& intent = brain->combat_intent();

        // Killed aircraft hold no lock and fire nothing (corpses fly, they
        // do not fight — see classify above).
        const auto* dmg = shooter.get<entities::DamageStateComponent>();
        const bool dead = dmg != nullptr && dmg->killed;

        // --- Lock intent: STT through the shooter's own radar. ---------
        if (intent.radar_lock && intent.lock_target_id != 0 && !dead) {
            if (auto* radar = shooter.get<sensors::RadarSimComponent>()) {
                radar->command_track(intent.lock_target_id);
            }
        }

        // --- Release intent: one missile through the sim's table. ------
        if (intent.weapon_release && intent.release_target_id != 0 && !dead) {
            auto* store = shooter.get<weapons::WeaponStoreComponent>();
            if (store) {
                // BVR doctrine: among the loaded A/A stations, fire the
                // LONGEST-RANGE weapon first (AMRAAM before Sidewinder —
                // the same class the envelope was configured from).
                const auto* bvr_rec = best_aa_missile(table);
                std::size_t best_station =
                    weapons::WeaponStoreComponent::npos;
                double best_score = -1.0;
                for (std::size_t s = 0; s < store->station_count(); ++s) {
                    const auto* st = store->station(s);
                    if (!st || st->rounds <= 0) continue;
                    const auto* rec = table.get(st->weapon_handle);
                    if (rec == nullptr ||
                        rec->category !=
                            weapons::WeaponCategory::AirToAirMissile) {
                        continue;
                    }
                    // Range first; a +1 ft tie-break prefers the BVR class
                    // when two stations carry equal-range missiles.
                    const double score = rec->max_range_ft +
                        ((bvr_rec != nullptr && rec->id == bvr_rec->id)
                             ? 1.0 : 0.0);
                    if (score > best_score) {
                        best_score = score;
                        best_station = s;
                    }
                }
                if (best_station != weapons::WeaponStoreComponent::npos) {
                    const auto* st = store->station(best_station);
                    const auto missile = weapons::launch_missile(
                        world, bus, shooter,
                        entities::EntityId{intent.release_target_id},
                        table, st->weapon_handle, sim_time_s);
                    if (missile.valid()) ++launches;
                }
            }
        }
    }
    return launches;
}

} // namespace f4::simulation

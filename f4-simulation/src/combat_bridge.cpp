// f4-simulation/src/combat_bridge.cpp
//
// Implementation of the M3 combat integration bridge (combat_bridge.hpp):
// the spawn-side component attachment + the SensorFusion detection policy
// backed by f4-sensors state. See the header for the design notes.

#include "f4/simulation/combat_bridge.hpp"

#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/entities/types.hpp>
#include <f4/recorder/flight_recorder.hpp>
#include <f4/sensors/messages.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/missile_battery.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/weapons/weapon_types.hpp>
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

/// The WVR weapon: the IR-guided air-to-air missile class (AIM-9M —
/// the close-in heater). Scopes the WVR fire-control envelope; the
/// driver's WVR station preference keys on the same guidance kind.
const weapons::WeaponClassRecord* ir_aa_missile(
    const weapons::WeaponClassTable& table) {
    const weapons::WeaponClassRecord* best = nullptr;
    for (const auto& rec : table.records()) {
        if (rec.category != weapons::WeaponCategory::AirToAirMissile)
            continue;
        if (rec.guidance != weapons::GuidanceKind::Ir)
            continue;
        if (best == nullptr || rec.max_range_ft > best->max_range_ft) {
            best = &rec;
        }
    }
    return best;
}

/// The gun class (M61A1 — the only Gun-category card in the table).
/// Scopes the gun fire control's envelope + muzzle velocity and the
/// GunComponent's ballistic config.
const weapons::WeaponClassRecord* gun_class(
    const weapons::WeaponClassTable& table) {
    for (const auto& rec : table.records()) {
        if (rec.category == weapons::WeaponCategory::Gun) return &rec;
    }
    return nullptr;
}

} // anonymous namespace

void configure_brain_combat(f4::ai::BrainComponent& brain,
                            const weapons::WeaponClassTable& table,
                            bool hold_fire,
                            bool bvr_hold,
                            bool missiles_hold,
                            bool guns_hold,
                            int gun_rounds) {
    brain.set_combat_enabled(true);
    brain.set_hold_fire(hold_fire);
    brain.set_bvr_hold(bvr_hold || missiles_hold);

    // ROE goes to the FIRE CONTROLS (module level), not just the brain's
    // intent gate: a module-level hold means should_fire() answers no, so
    // no pulse, no phantom shot counted, and no doctrine separation ever
    // triggers on shots that never left the rail. hold_fire disarms every
    // module; bvr_hold disarms the BVR fire control alone (heaters free);
    // missiles_hold disarms BVR + IR (the guns-dogfight ROE); guns_hold
    // disarms the trigger alone (the no-surprise default for pre-gun
    // scenarios).
    const bool missiles_tight = hold_fire || missiles_hold;
    brain.bvr().fire().config().hold_fire = missiles_tight || bvr_hold;
    brain.wvr().fire().config().hold_fire = missiles_tight;
    brain.wvr().guns().config().hold_fire = hold_fire || guns_hold;

    // Employment envelope from the BVR weapon class. The 0.5 factor turns
    // the AIM-120C's 40 NM aerodynamic boundary into a ~20 NM
    // doctrine-safe R_ne (the plan's default max_pk_range).
    const auto* rec = best_aa_missile(table);
    if (rec != nullptr && rec->max_range_ft > 0.0) {
        const double min_nm =
            std::max(rec->min_range_ft, 0.5 * FEET_PER_NM) / FEET_PER_NM;
        const double max_nm = 0.5 * rec->max_range_ft / FEET_PER_NM;
        brain.bvr().fire().set_envelope_nm(min_nm, max_nm);
    }

    // WVR (Step 9): the IR fire-control envelope from the heater class.
    // The floor is the max of the class arming distance and 0.5 NM (the
    // plan's minimum heater employment); the ceiling 0.8x the class
    // boundary — inside the seeker's real wheelhouse, well short of the
    // aerodynamic limit.
    const auto* ir = ir_aa_missile(table);
    if (ir != nullptr && ir->max_range_ft > 0.0) {
        const double min_nm =
            std::max(ir->min_range_ft, 0.5 * FEET_PER_NM) / FEET_PER_NM;
        const double max_nm = 0.8 * ir->max_range_ft / FEET_PER_NM;
        brain.wvr().fire().set_envelope_nm(min_nm, max_nm);
    }

    // GUNS (Steps 11-12): envelope + muzzle velocity from the Gun class
    // card, ammo from the store's gun station (the caller read it after
    // attach). The card's effective range is an aerodynamic statement
    // about the BULLET; the employment doctrine caps it at ~0.35 NM —
    // the range where this fire control's boresight tracking error
    // still projects inside the hit footprint (see GunModule::Config).
    const auto* gun = gun_class(table);
    if (gun != nullptr && gun->max_range_ft > 0.0) {
        constexpr double kGunDoctrineMaxNm = 0.35;
        const double min_nm =
            std::max(0.9 * gun->min_range_ft, 0.08 * FEET_PER_NM) /
            FEET_PER_NM;
        const double max_nm = std::min(0.9 * gun->max_range_ft / FEET_PER_NM,
                                       kGunDoctrineMaxNm);
        brain.wvr().guns().set_envelope_nm(min_nm, max_nm);
        if (gun->max_speed_fts > 0.0) {
            brain.wvr().guns().config().muzzle_velocity_fps =
                gun->max_speed_fts;
        }
    }
    brain.wvr().guns().set_rounds_budget(
        gun_rounds > 0 ? gun_rounds : 0);
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

    // The cannon (Steps 11-12): ballistic config from the Gun class card
    // (M61A1), seeded like the radar (base + index: deterministic per
    // scenario, distinct per shooter — dispersion rolls differ). Rounds
    // ledger = the store's gun station; the brain's gun fire control is
    // pointed at the same number by the spawn loop's
    // configure_brain_combat call.
    const auto* gun_rec = gun_class(table);
    if (gun_rec != nullptr) {
        auto& gun = aircraft.add<weapons::GunComponent>();
        gun.weapon_handle = gun_rec->id;
        weapons::GunConfig gcfg{};
        gcfg.muzzle_velocity_fps =
            gun_rec->max_speed_fts > 0.0 ? gun_rec->max_speed_fts
                                         : 3400.0;
        gcfg.round_power_lb = gun_rec->warhead_power_lb;
        gcfg.lethal_radius_ft = gun_rec->lethal_radius_ft;
        gun.stream = weapons::GunStream(
            gcfg, seed_base +
                static_cast<std::uint32_t>(0x1000 + aircraft_index));
    }

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
                // Station doctrine. BVR: among the loaded A/A stations,
                // fire the LONGEST-RANGE weapon first (AMRAAM before
                // Sidewinder — the class the BVR envelope was configured
                // from). WVR (the brain's combat mode this tick): the
                // IR-guided heaters first, then the shortest-range A/A —
                // a FOX 2 off the wingtip before an AMRAAM out of the
                // trench, and never a radar missile while a heater
                // remains when the fight is inside 3 NM.
                const bool wvr_shot =
                    brain->combat_mode() ==
                    f4::ai::BrainComponent::CombatMode::WVR;
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
                    double score;
                    if (wvr_shot) {
                        score = (rec->guidance == weapons::GuidanceKind::Ir)
                            ? 1.0e12 + rec->max_range_ft  // heaters first
                            : -rec->max_range_ft;         // then shortest
                    } else {
                        // Range first; a +1 ft tie-break prefers the BVR
                        // class when two stations carry equal-range
                        // missiles.
                        score = rec->max_range_ft +
                            ((bvr_rec != nullptr && rec->id == bvr_rec->id)
                                 ? 1.0 : 0.0);
                    }
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

        // --- Gun intent: start a burst through the gun station. ---------
        // The burst size is the brain's gun doctrine (the module's
        // burst_rounds), clipped to the drum; the store debits what
        // actually left the muzzle. The tracers fly in the update_guns
        // sweep later this tick — fresh muzzle pose, real ballistics.
        if (intent.gun_trigger && intent.gun_target_id != 0 && !dead) {
            auto* gun = shooter.get<weapons::GunComponent>();
            auto* store = shooter.get<weapons::WeaponStoreComponent>();
            if (gun != nullptr && store != nullptr) {
                const std::size_t station =
                    store->find_with_category(
                        table, weapons::WeaponCategory::Gun);
                if (station != weapons::WeaponStoreComponent::npos) {
                    const int in_drum =
                        store->station(station)->rounds;
                    const int burst = std::min(
                        brain->wvr().guns().config().burst_rounds, in_drum);
                    if (burst > 0) {
                        store->expend(station, burst);
                        gun->stream.start_burst(
                            burst, intent.gun_target_id);
                    }
                }
            }
        }
    }
    return launches;
}

// ── combat event recording (M4) ────────────────────────────────────────────

void attach_combat_event_recorder(Simulation& sim) {
    auto* rec = sim.recorder();
    if (rec == nullptr) return;  // scenario disabled recording — nothing to do

    // Captured by every handler below: the recorder (owned by the sim,
    // alive as long as the bus that holds these subscriptions), the weapon
    // table (for launch-name resolution), and the sim itself (for tick
    // stamping). All three outlive the bus's subscription list — the bus
    // and its lambdas are destroyed together with the Simulation.
    auto& bus = sim.bus();
    const auto* table = &sim.weapon_table();
    Simulation* sim_ptr = &sim;

    // Bus events publish mid-tick, BEFORE Simulation::tick() increments
    // its counter; the snapshots the SAME tick() call records carry the
    // post-increment tick. +1 aligns the two.
    const auto event_tick = [sim_ptr]() {
        return sim_ptr->tick_count() + 1;
    };

    bus.subscribe<sensors::RadarTrackAcquiredMessage>(
        [rec, event_tick](const sensors::RadarTrackAcquiredMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.time_s;
            e.kind = recorder::CombatEventKind::TrackAcquired;
            e.subject_id = m.radar_entity_id;
            e.object_id = m.target_entity_id;
            rec->record(std::move(e));
        });

    bus.subscribe<sensors::RadarTrackDroppedMessage>(
        [rec, event_tick](const sensors::RadarTrackDroppedMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.time_s;
            e.kind = recorder::CombatEventKind::TrackDropped;
            e.subject_id = m.radar_entity_id;
            e.object_id = m.target_entity_id;
            rec->record(std::move(e));
        });

    bus.subscribe<sensors::RwrWarningMessage>(
        [rec, event_tick](const sensors::RwrWarningMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.time_s;
            e.kind = (m.type == sensors::RwrWarningType::Launch)
                ? recorder::CombatEventKind::RwrLaunch
                : recorder::CombatEventKind::RwrLock;
            e.subject_id = m.victim_id;
            e.object_id = m.emitter_id;
            e.range_ft = m.range_ft;
            rec->record(std::move(e));
        });

    bus.subscribe<weapons::MissileLaunchedMessage>(
        [rec, event_tick, table](const weapons::MissileLaunchedMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.sim_time_s;
            e.kind = recorder::CombatEventKind::MissileLaunched;
            e.subject_id = m.shooter_id;
            e.object_id = m.target_id;
            e.missile_id = m.missile_id;
            e.weapon_handle = m.weapon_handle;
            e.position = m.position;
            e.speed_ft_s = m.speed_fts;
            if (const auto* w = table->get(m.weapon_handle)) {
                e.weapon_name = w->name;
            }
            rec->record(std::move(e));
        });

    bus.subscribe<weapons::MissileDetonatedMessage>(
        [rec, event_tick](const weapons::MissileDetonatedMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.sim_time_s;
            e.kind = recorder::CombatEventKind::MissileDetonated;
            e.subject_id = m.shooter_id;
            e.object_id = m.target_id;
            e.missile_id = m.missile_id;
            e.end_cause = weapons::missile_end_cause_name(m.cause);
            e.position = m.position;
            e.miss_distance_ft = m.miss_distance_ft;
            e.flight_time_s = m.flight_time_s;
            rec->record(std::move(e));
        });

    bus.subscribe<weapons::DamageAppliedMessage>(
        [rec, event_tick](const weapons::DamageAppliedMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.sim_time_s;
            e.kind = recorder::CombatEventKind::DamageApplied;
            e.subject_id = m.target_id;
            e.object_id = m.shooter_id;
            e.missile_id = m.missile_id;
            e.damage = m.damage;
            e.hit_points_after = m.hit_points_after;
            e.killed = m.killed;
            rec->record(std::move(e));
        });

    bus.subscribe<weapons::EntityKilledMessage>(
        [rec, event_tick](const weapons::EntityKilledMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.sim_time_s;
            e.kind = recorder::CombatEventKind::EntityKilled;
            e.subject_id = m.target_id;
            e.object_id = m.shooter_id;
            rec->record(std::move(e));
        });

    // GUNS (Steps 11-12): one event per burst (the stream publishes on
    // the burst's first emitted round — muzzle + rounds + the aim
    // target). Gun damage/kill events ride the DamageApplied/
    // EntityKilled subscriptions above with missile_id == 0 ("gun
    // hits", the message contract documents it).
    bus.subscribe<weapons::GunFiredMessage>(
        [rec, event_tick, table](const weapons::GunFiredMessage& m) {
            recorder::CombatEvent e;
            e.tick = event_tick();
            e.sim_time_s = m.sim_time_s;
            e.kind = recorder::CombatEventKind::GunFired;
            e.subject_id = m.shooter_id;
            e.object_id = m.target_id;
            e.weapon_handle = m.weapon_handle;
            e.position = m.position;
            e.rounds = m.rounds;
            if (const auto* w = table->get(m.weapon_handle)) {
                e.weapon_name = w->name;
            }
            rec->record(std::move(e));
        });
}

} // namespace f4::simulation

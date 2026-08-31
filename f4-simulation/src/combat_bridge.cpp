// f4-simulation/src/combat_bridge.cpp
//
// Implementation of the M3 combat integration bridge (combat_bridge.hpp):
// the spawn-side component attachment + the SensorFusion detection policy
// backed by f4-sensors state. See the header for the design notes.

#include "f4/simulation/combat_bridge.hpp"

#include <f4/entities/types.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/sensors/track_store.hpp>

namespace f4::simulation {

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

} // namespace f4::simulation

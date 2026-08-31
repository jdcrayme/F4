// f4-simulation/include/f4/simulation/combat_bridge.hpp
//
// Combat bridge — the M3 integration layer that wires f4-weapons and
// f4-sensors into the simulation's aircraft entities (COMBAT_CHAIN_PLAN.md
// M3, "integration first").
//
// Two pieces:
//
//   1. attach_combat_loadout() — the spawn-side half. Adds the combat
//      component set to an aircraft entity so the ECS tick drives it:
//        WeaponStoreComponent   (what the jet carries; launch_missile debits it)
//        SignatureComponent     (RCS — what other radars see)
//        RadarSimComponent      (behavioral, priority 45: scans + track files)
//        RwrComponent           (passive; update_rwr() fills it each tick)
//        DamageStateComponent   (hit points; the endpoint of every weapon)
//      plus the identity plumbing every consumer needs: the TEAM tag and
//      CampaignIdentityComponent (callsign for NCTR).
//
//   2. RadarBackedDetectionPolicy — the M2 SensorFusion hook made real.
//      f4-ai's SensorFusion::DetectionPolicy is a pure virtual; f4-ai must
//      NOT link f4-sensors (tactics consume sensors, never the reverse),
//      so the adapter that feeds SensorFusion from the radar's track store
//      lives HERE, at the host layer. When M3's BVRModule constructs its
//      SensorFusion, it installs one of these per aircraft and the AI
//      picture becomes radar-truth instead of GCI-omniscience.
//
// Dependencies: f4-weapons, f4-sensors, f4-ai (interface only), f4-entities.
// C++20.

#pragma once

#include <cstdint>
#include <string>

#include <f4/ai/sensor_fusion.hpp>
#include <f4/entities/entity.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/sensors/radar_component.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/sensors/signature.hpp>

#include "f4/simulation/scenario.hpp"

namespace f4::simulation {

/// Add the combat component set to a spawned aircraft entity. Idempotent
/// per component type (EntityWorld::add replaces? No — it appends; callers
/// must call this exactly once per entity, which the spawn paths do).
///
/// `seed_base` is the scenario's CombatConfig::radar_rng_seed; the per-aircraft
/// seed is derived from it (index mixed in) so two radars don't roll identical
/// detection sequences while the whole scenario stays deterministic.
///
/// The radar starts in Search mode with the default parameter card and scan
/// volume (120-degree bar centered north — bearings are absolute CW from
/// north, so a north-flying fighter covers its nose). The AI (or a test)
/// steers the bar via the component's public fields and locks via
/// command_track(); nothing here hard-codes a doctrine.
///
/// `hit_points` sets DamageStateComponent (light-fighter strength default).
void attach_combat_loadout(entities::EntityHandle& aircraft,
                           const weapons::WeaponClassTable& table,
                           const ScenarioAircraft& ac,
                           std::uint32_t seed_base,
                           std::size_t aircraft_index,
                           double hit_points);

/// SensorFusion::DetectionPolicy backed by the ownship's radar tracks and
/// RWR picture. This is the M2 integration point (SensorFusion::
/// set_detection_policy) implemented against f4-sensors state:
///
///   radar  -> true when the ownship's RadarSimComponent holds a live
///             (non-Dropped) track on the candidate.
///   rwr    -> true when the ownship's RWR picture carries any warning
///             whose emitter IS the candidate (lock or search strobe —
///             both mean "that emitter is painting me").
///   visual -> false (no eyeball model yet — arrives with WVR tactics).
///   gci    -> false. THE FLIP: unlike the legacy rules, GCI-omniscience
///             is OFF under this policy. The AI sees what its radar and
///             RWR see, nothing else. Installing this policy on every
///             blue brain is what replaces the omniscient picture.
///
/// Non-owning: the policy borrows the EntityWorld (the sim owns it) and
/// must be kept alive by the caller for the SensorFusion's lifetime —
/// same contract as SensorFusion::set_detection_policy.
class RadarBackedDetectionPolicy final
    : public f4::ai::SensorFusion::DetectionPolicy {
public:
    RadarBackedDetectionPolicy(entities::EntityWorld& world,
                               std::uint64_t ownship_id) noexcept
        : world_(&world), ownship_id_(ownship_id) {}

    Verdict classify(const f4::ai::TargetInfo& t) override;

private:
    entities::EntityWorld* world_;
    std::uint64_t ownship_id_;
};

} // namespace f4::simulation

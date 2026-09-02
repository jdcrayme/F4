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
//   3. GunComponent — the aircraft's cannon (Steps 11-12): attach adds
//      it (config from the Gun-category class card, M61A1), the brain's
//      gun intent starts bursts, and Simulation::tick's update_guns
//      sweep flies them from the aircraft's fresh muzzle pose.
//
// Dependencies: f4-weapons, f4-sensors, f4-ai (interface only), f4-entities.
// C++20.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <f4/ai/sensor_fusion.hpp>
#include <f4/entities/entity.hpp>
#include <f4/weapons/gun_component.hpp>
#include <f4/weapons/weapon_class_table.hpp>
#include <f4/sensors/radar_component.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/sensors/signature.hpp>

#include "f4/simulation/scenario.hpp"

namespace f4::ai { class BrainComponent; }  // spawn wiring below

namespace f4::simulation {

class Simulation;  // combat event recording (attach_combat_event_recorder)

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
///   visual -> false (no eyeball model yet — arrives with the WVR
///             skill/visual-detection layer).
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

/// Execute every combat brain's intents against the real hardware, one
/// tick after the brains produced them (the host calls this from tick(),
/// between world.update_all() and the update_rwr sweep):
///
///   radar_lock       -> RadarSimComponent::command_track() on the
///                       shooter's own radar (refused harmlessly until
///                       the track store holds a live track on the target).
///   weapon_release   -> weapons::launch_missile() through the table the
///                       simulation owns: selects a loaded air-to-air
///                       station — BVR doctrine fires the LONGEST-RANGE
///                       class first (AMRAAM before Sidewinder), WVR
///                       doctrine (brain combat mode == WVR) fires the
///                       IR-guided stations first, then the shortest
///                       range — a heater off the wingtip before an
///                       AMRAAM in the trench. Debits the store,
///                       publishes MissileLaunchedMessage. Killed aircraft
///                       (per DamageStateComponent) never fire — the M2
///                       simplification keeps corpses flying, but not
///                       fighting.
///   gun_trigger      -> GunComponent::stream.start_burst() through the
///                       store's gun station: the burst size is the
///                       WVRModule's gun doctrine (guns().config()
///                       .burst_rounds), clipped to the rounds actually
///                       in the drum; the station is debited by what left
///                       the muzzle. The burst's tracers fly in the
///                       update_guns sweep later this tick (fresh muzzle
///                       pose — a firing aircraft sweeps its gun across
///                       the sky); GunFiredMessage publishes on the
///                       burst's first emitted round.
///
/// Returns the number of missiles launched this tick.
///
/// `active_aircraft` (optional): the host's roster of SIMULATED aircraft
/// (Simulation::aircraft_entities_). When present, the intents pass
/// visits exactly that roster — at campaign scale the world also holds
/// thousands of DORMANT parked-inventory brains (squadron deaggregation)
/// whose per-entity lookups cost more than the entire intents pass;
/// the roster is also the honest statement of "who is being simulated"
/// (FreeFalcon's fidelity-tiering rule: no simulation work outside the
/// active set). Nullptr walks with_component<BrainComponent>() — the
/// legacy shape for hand-built test worlds (and dormant brains are
/// skipped there too).
std::size_t execute_brain_combat_intents(
    entities::EntityWorld& world,
    messaging::MessageBus& bus,
    const weapons::WeaponClassTable& table,
    double sim_time_s,
    const std::vector<entities::EntityId>* active_aircraft = nullptr);

/// Turn a spawned brain into a fighting brain: enables the combat ladder
/// and configures the fire-control envelopes from the weapon class
/// table:
///
///   BVR  — employment envelope [min range, 0.5 * max range] in NM from
///          the LONGEST-RANGE A/A class (the AIM-120C's 40 NM
///          aerodynamic boundary becomes a ~20 NM doctrine-safe R_ne;
///          when the WST import replaces the table the envelope follows
///          the real cards).
///   WVR  — employment envelope from the IR-guided A/A class (AIM-9M):
///          [max(arming range, 0.5 NM), 0.8 * max range] — the close-in
///          heater shot. When the WST import replaces the table the
///          envelope follows the real cards the same way.
///   GUNS — employment envelope from the Gun-category class (M61A1):
///          [0.9 * min range, 0.9 * max range] in NM (the card's
///          effective range IS the gun's envelope — there is no
///          aerodynamic Rmax to safety-factor), muzzle velocity from the
///          card's max_speed_fts, and the ammo budget from the store's
///          gun-station rounds (the caller passes it; the spawn loop
///          reads it after attach_combat_loadout).
///
/// ROE wiring (module-level, so a held weapon produces no pulse, no
/// phantom count, and no doctrine flip on shots that never happened):
///   hold_fire     — every fire control tight (BVR + IR + guns).
///   bvr_hold      — the BVR fire control alone (radar missiles tight).
///   missiles_hold — BVR + IR tight, guns free (the guns-dogfight ROE;
///                    subsumes bvr_hold).
///   guns_hold     — the gun fire control alone (the no-surprise default
///                    for pre-gun scenarios).
///
/// `hold_fire` (per-aircraft scenario option) suppresses every release
/// intent — the aircraft fights geometry only; `bvr_hold` (scenario
/// combat block) suppresses the BVR release alone — SPINS-style
/// "radar missiles tight, heaters free". The host then installs its
/// detection policy on brain->sensors() (typically a
/// RadarBackedDetectionPolicy it owns).
void configure_brain_combat(f4::ai::BrainComponent& brain,
                            const weapons::WeaponClassTable& table,
                            bool hold_fire = false,
                            bool bvr_hold = false,
                            bool missiles_hold = false,
                            bool guns_hold = false,
                            int gun_rounds = 511);

/// Subscribe the sim's FlightRecorder to every combat bus transition so a
/// recorded fight carries its event stream alongside the kinematic tracks
/// (COMBAT_CHAIN_PLAN.md M4 — "every shot/detection/kill is replayable").
///
/// Converts each message into a recorder::CombatEvent (the recorder stays
/// decoupled from f4-weapons/f4-sensors — this bridge is where the message
/// structs are flattened into plain ids/strings), stamps the event with the
/// sim tick it belongs to (tick_count()+1: bus events publish mid-tick,
/// BEFORE Simulation::tick increments its counter, so +1 aligns them with
/// the FlightSnapshots the same tick() call records), and resolves weapon
/// names through the sim's table at capture time (a replay never needs the
/// table to interpret a launch).
///
/// No-op when the scenario disabled recording (sim.recorder() == nullptr).
/// GunFiredMessage is captured (GunFired: the burst's first round —
/// subject, aim target, weapon name, muzzle, rounds) now that the WVR
/// module produces bursts; gun DAMAGE rides the DamageApplied event
/// stream with missile_id == 0, which the recorder already carried.
void attach_combat_event_recorder(Simulation& sim);

} // namespace f4::simulation

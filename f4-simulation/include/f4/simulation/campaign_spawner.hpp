// f4-simulation/include/f4/simulation/campaign_spawner.hpp
//
// CampaignSimSpawner — the B.3 sim-side spawner.
//
// This is the class the B.3 milestone has pointed at since the M4.7 slice:
// "a sim-side spawner that subscribes to f4::campaign::MissionIntent on the
// message bus and materializes flights through the Milestone-A campaign
// spawn path." It closes the campaign→sim loop:
//
//     WorldState (live save, e.g. TestCamp.cam)
//       → f4::campaign::emit_flight_intents()         [campaign side]
//       → MessageBus.publish(MissionIntent)
//       → CampaignSimSpawner::handle()                [sim side, THIS FILE]
//       → spawn_aircraft_for_flight()                 [campaign bridge]
//       → aircraft entities with SAVED ROUTES attached (digi brains fly them)
//
// The spawner resolves each intent's flight_id (the flight's VU_ID.num) to
// its entity through the PopulatedWorld's unit_id_map, then delegates to
// spawn_aircraft_for_flight — the same core spawn_aircraft_from_flights()
// uses — so bus-driven and bulk-driven spawns produce identical aircraft.
// Intents whose flight_id doesn't resolve (synthetic-ladder intents, which
// carry no live flight) are counted and skipped: the synthetic path will
// grow its own materialization when the ATM tranche gives it routes.
//
// Duplicate protection: the same flight_id arriving twice (a re-publish,
// or two emission passes over one world) spawns ONE aircraft — the second
// intent is counted as a duplicate skip.
//
// Threading/ownership: the bus must outlive the spawner (or detach() must
// be called first). handle() mutates the EntityWorld — the same
// single-thread discipline the whole simulation uses.
//
// Dependencies: f4-entities, f4-world (unit_id_map), f4-campaign
// (MissionIntent), f4-messaging. C++20.

#pragma once

#include <f4/simulation/campaign_bridge.hpp>   // spawn_aircraft_for_flight, FlightSpawnFilter
#include <f4/campaign/campaign.hpp>            // MissionIntent
#include <f4/messaging/bus.hpp>

#include <f4/weapons/weapon_class_table.hpp>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace f4::simulation {

class CampaignSimSpawner {
public:
    /// What the spawner did — the QC tool's summary block and the tests'
    /// assertions read exactly these counters.
    struct Stats {
        /// MissionIntents delivered to handle() (bus-fed or manual).
        int intents_seen = 0;
        /// Aircraft entities actually spawned.
        int aircraft_spawned = 0;
        /// Spawned aircraft that carry a saved-route MissionPlan.
        int routes_attached = 0;
        /// Intents whose flight_id resolved to no entity in the world.
        int unknown_flight_ids = 0;
        /// Intents for a flight that already spawned (skipped).
        int duplicate_skips = 0;
        /// C3: aircraft spawned from SYNTHETIC-ladder intents (routes
        /// the campaign built — generation-to-spawn).
        int synthetic_spawned = 0;
        /// C3: synthetic intents that carried a route but spawned no
        /// aircraft (route unbuildable / unresolvable) — the QC gate's
        /// route-loss counter.
        int synthetic_failed = 0;
    };

    /// Construct over the SAME EntityWorld the campaign flight units live
    /// in. All references (ct/db/cfg/airfield/template) must outlive the
    /// spawner — same rule as spawn_aircraft_from_flights().
    ///
    /// \param world         the populated campaign world (mutated on spawn)
    /// \param unit_id_map    VU_ID.num → flight EntityId (PopulatedWorld::unit_id_map;
    ///                       copied — the spawner must survive populate
    ///                       scope changes)
    /// \param ct             class table (entity_type → vis_type)
    /// \param db             model database (vis_type → ModelRecord)
    /// \param cfg            aircraft config for the flight model
    /// \param airfield       the active scenario airfield
    /// \param scenario_aircraft  template (callsign prefix, vis fallback, config path)
    /// \param filter         intent-side filter: team/mission restrict which
    ///                       intents spawn; max_flights caps the total. The
    ///                       default filter spawns every resolved intent.
    CampaignSimSpawner(f4::entities::EntityWorld& world,
                       std::unordered_map<std::uint32_t, f4::entities::EntityId> unit_id_map,
                       const f4::world_types::ClassTable& ct,
                       const f4::data::AircraftConfig& cfg,
                       const ScenarioAirfield& airfield,
                       const ScenarioAircraft& scenario_aircraft,
                       FlightSpawnFilter filter = {});

    /// A-G tranche: the objective VU_ID → EntityId map (waypoint strike
    /// targets) + the weapon table (loadout arming). Both optional — set
    /// either/both BEFORE the intents arrive; spawns without them behave
    /// exactly as before this tranche (unarmed, route-only).
    void set_objective_id_map(
        const std::unordered_map<std::uint32_t, f4::entities::EntityId>* m) {
        objective_id_map_ = m;
    }
    void set_weapon_table(
        const f4::weapons::WeaponClassTable* t) {
        weapon_table_ = t;
    }

    /// C3: per-airbase airfield data for SYNTHETIC-intent spawns
    /// (runway heading + departure altitude from the squadron's own
    /// base; unknown bases fall back to the constructor's airfield).
    /// The saved-flight bus path keeps its pre-C3 behavior exactly
    /// (parking at the base objective, fallback airfield for heading).
    void set_airbase_airfields(const AirbaseAirfieldMap* m) {
        airbase_airfields_ = m;
    }

    /// Subscribe to MissionIntent on `bus` (returns the subscription id).
    /// The bus must outlive the spawner unless detach() is called first —
    /// the handler is a raw this-capture.
    std::size_t attach(f4::messaging::MessageBus& bus);

    /// Unsubscribe from a previously attached bus. No-op when the
    /// subscription id doesn't match. Safe to call repeatedly.
    void detach(f4::messaging::MessageBus& bus);

    /// Process one intent directly (tests, QC tools without a bus, or a
    /// host that drains its own queue). Idempotent per flight_id.
    void handle(const f4::campaign::MissionIntent& intent);

    /// Every spawned aircraft entity, in spawn order.
    [[nodiscard]] const std::vector<f4::entities::EntityId>& spawned() const noexcept {
        return spawned_;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    f4::entities::EntityWorld& world_;
    std::unordered_map<std::uint32_t, f4::entities::EntityId> unit_id_map_;
    const f4::world_types::ClassTable& ct_;
    const f4::data::AircraftConfig& cfg_;
    const ScenarioAirfield& airfield_;
    const ScenarioAircraft& tpl_;
    FlightSpawnFilter filter_;

    /// A-G tranche (optional inputs; see the setters).
    const std::unordered_map<std::uint32_t, f4::entities::EntityId>*
        objective_id_map_ = nullptr;
    const f4::weapons::WeaponClassTable* weapon_table_ = nullptr;

    /// C3 (optional input; see the setter) — per-base airfield data
    /// for synthetic-intent spawns.
    const AirbaseAirfieldMap* airbase_airfields_ = nullptr;

    /// Per-airbase parking counters — same bookkeeping as the bulk path,
    /// so bus-fed and bulk-fed fleets park identically.
    std::unordered_map<std::uint64_t, int> per_airbase_index_;
    /// flight VU_ID.nums already materialized (duplicate guard).
    std::unordered_set<std::uint32_t> spawned_flight_ids_;

    std::vector<f4::entities::EntityId> spawned_;
    Stats stats_;
    std::size_t subscription_ = static_cast<std::size_t>(-1);
};

} // namespace f4::simulation

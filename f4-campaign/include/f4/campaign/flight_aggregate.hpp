// f4-campaign/include/f4/campaign/flight_aggregate.hpp
//
// FlightAggregateEngine — the FID-2 aggregate flight propagator
// (Docs/FIDELITY_TIERS_PLAN.md). The campaign-side twin of the G1
// GroundWar engine: a headless, deterministic state machine that moves
// the war's FLIGHTS along their routes at a coarse cadence, so a
// session under the Tiered fidelity policy does not run a flight model
// per aircraft to know where the war is.
//
// WHAT THIS IS. The session's Tier-B path (spawn_aircraft_for_flight →
// FlightModelComponent at 60 Hz) is per-aircraft truth and costs 449
// FMs on TestCamp — "a headless-budget run, not a UI"
// (campaign_session.hpp). This engine is the Tier-A truth: each flight
// advances along ITS OWN waypoints (the decoded save's arrive/depart
// schedule when the save carries one, a cruise-speed walk when it does
// not), burning fuel at a cruise rate, on the same one-clock cadence
// the ground war moves battalions. No FM, no AI, no entities — the
// f4-simulation session mirrors the engine into the world's flight
// entities (transforms + FlightPlanComponent fields) and deaggregates
// individual flights into real aircraft when an observer (camera
// bubble), an airfield-ops window, or an explicit request needs them.
//
// ROUTE MODEL (two modes, chosen per flight at construction):
//   * TIME mode — the save's waypoints carry absolute arrive/depart
//     times (WaypointState::arrive/depart, campaign seconds). The
//     flight sits at its save position until the first waypoint's
//     depart, then interpolates linearly between waypoints on the
//     wire's own schedule (hold at a waypoint between its arrive and
//     its depart), and is ARRIVED past the last waypoint's arrive.
//     This reproduces the upstream campaign's own move timing exactly.
//   * SPEED mode — a route with no usable times walks its legs at the
//     cruise speed (cruise_grid_per_min; the ATM's own campaign-move
//     constant), gating on the first waypoint's depart time when one
//     is present. Altitude lerps toward the next waypoint's z by the
//     distance fraction on the current leg.
//
// FUEL. fuel_burnt is PER-AIRCRAFT lbs consumed (the deagg handoff
// turns it into each spawned aircraft's internal fuel: capacity −
// burnt). The engine burns the cruise rate per aircraft per minute
// while the flight is airborne-progressing (after its first depart,
// before arrival). The save's decoded fuel_burnt seeds the counter.
//
// TIER COOPERATION. A deaggregated flight is SUSPENDED: the engine
// stops advancing it and the sim's aircraft own the truth. The session
// folds state back with reaggregate() (lead-aircraft roll-up,
// FIDELITY_TIERS_PLAN §4.4) and the flight resumes as an aggregate. A
// flight whose aircraft all died folds as destroyed (mark_destroyed).
//
// DISCIPLINE (the GroundWar rules, unchanged):
//   * NO EntityWorld, NO flight model here. Campaign identity (VU_IDs,
//     team slots, grid cells) and numbers only; the session does the
//     entity-side mirroring.
//   * NO RNG, NO clocks of its own. tick(delta) on the caller's clock;
//     wire order everywhere; the same sources + tick sequence produce
//     the same state (pinned by test).
//   * The ledger is NOT written by this engine — flight aggregates
//     carry no kill/loss books of their own (losses book at the sim's
//     combat layer through the C1 sink, exactly as today). The
//     write-back to WorldState is the opt-in flight_writeback.hpp.
//
// Dependencies: f4-world (IDataSource), f4-entities (WaypointState
// vocabulary — a value type, not an entity). C++20.
//
// KNOWN GAPS (deliberate, documented — FIDELITY_TIERS_PLAN §7):
//   * Synthetic ATM intents (no world flight) do not ride this engine
//     in v1 — they spawn straight to Tier-B through the spawner as
//     today. The aggregate tier covers the save's own flights (the
//     mass cost).
//   * No per-leg altitude shaping beyond the waypoints' own z, and no
//     tanker/fuel-planning legs (route_builder's deferrals stand).
//   * Formation offsets are discarded at the fold-back (lead roll-up).

#pragma once

#include <f4/campaign/campaign.hpp>       // CampaignTime
#include <f4/entities/types.hpp>           // WaypointState (value vocab)
#include <f4/world/data_source.hpp>        // ICampaignSource, IUnitCoreSource, IFlightSource

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::campaign {

/// Tunables. Defaults documented here; every field is pinned by the
/// unit tests. Mirrors the GroundWarConfig pattern.
struct FlightAggregateConfig {
    /// Aggregate advance cadence (campaign seconds). 60 s = the ground
    /// war's update precedent; a 24-hour war is 1440 updates over the
    /// flight count — trivially cheap (the FID-6 budget's foundation).
    CampaignTime update_sec = 60;

    /// Speed-mode cruise (grid units per minute). 12 = the ATM's own
    /// campaign-move constant (AtmConfig::cruise_grid_per_min), i.e.
    /// ~204.8 ft/s ≈ 121 kts across the 1024-ft grid.
    double cruise_grid_per_min = 12.0;

    /// Cruise fuel burn (lbs per aircraft per minute). The F-16's
    /// cruise-specific figure is data (f4-data) in a later tranche;
    /// this constant is the v1 divergence, documented and pinned.
    int fuel_burn_lbs_per_min = 70;
};

/// Which flights the engine aggregates. Same vocabulary and matching
/// semantics as f4-simulation's FlightSpawnFilter (the tiered session
/// must aggregate exactly the set full-fidelity would have spawned):
/// -1 = no constraint; max_flights caps AFTER the filters, wire order.
struct FlightAggregateFilter {
    int team = -1;
    int mission = -1;
    int max_flights = -1;   // < 0 = unlimited
};

/// FID-5 (Docs/FIDELITY_TIERS_PLAN.md §4.5): one generated mission's
/// aggregate seed — register_synthetic()'s input. The session fills it
/// from the ladder's MissionIntent (the spawner's synthetic path no
/// longer spawns straight to Tier-B); the route is already converted to
/// the engine's waypoint vocabulary.
struct SyntheticFlightSeed {
    std::uint32_t vu = 0;               ///< reserved-namespace flight id
    std::uint8_t team = 0;              ///< owner slot (the intent's)
    std::uint8_t mission = 0;           ///< mission byte (the intent's)
    int aircraft_count = 1;             ///< the intent's package size
    std::int32_t time_on_target = 0;    ///< ABSOLUTE campaign time
    /// The intent's planned route (takeoff → ingress → target → egress →
    /// landing). May be empty — a route-less synthetic flight is a
    /// parked aggregate (has_route false, nothing advances).
    std::vector<f4::entities::WaypointState> route;
};

/// One flight's aggregate state. Public read-only by convention —
/// mutation flows through tick()/set_suspended()/reaggregate()/
/// mark_destroyed() so the invariants hold.
struct FlightAggregateState {
    std::uint32_t vu = 0;               ///< flight VU_ID.num
    std::uint8_t team = 0;              ///< owner slot
    std::uint8_t mission = 0;           ///< mission byte (MissionType)
    int aircraft_count = 0;             ///< vehicles in the flight's groups
    std::int32_t time_on_target = 0;    ///< absolute campaign time (save)
    std::int32_t mission_over_time = 0; ///< absolute campaign time (save)
    double fx = 0.0;                    ///< sub-grid east (grid units)
    double fy = 0.0;                    ///< sub-grid north (grid units)
    float altitude_ft = 0.0f;           ///< MSL feet
    std::int32_t fuel_burnt = 0;        ///< per-aircraft lbs consumed
    std::size_t wp_index = 0;           ///< waypoint being flown TOWARD
    std::int32_t last_move = 0;         ///< absolute time of last advance
    bool dirty = false;                 ///< moved/burned since last sync
    bool suspended = false;             ///< deaggregated: sim owns truth
    bool arrived = false;               ///< reached the last waypoint
    bool destroyed = false;             ///< folded back as all-dead
    bool has_route = false;             ///< the save carried waypoints
};

/// The aggregate flight propagator. Construction snapshots the world's
/// flights (the GroundWar pattern: engine state is sim-side truth, the
/// sources are read once).
class FlightAggregateEngine {
public:
    /// Snapshot the world's Flight units. `campaign` anchors the clock
    /// (current_time = the save epoch, the same anchor the ladder and
    /// the ground war use). `units` and `flights` index the SAME unit
    /// vector (WorldStateAdapters' UnitAdapter implements both over
    /// one index space; the flight-tail reads are valid on Flight rows).
    FlightAggregateEngine(const f4::world::ICampaignSource& campaign,
                          const f4::world::IUnitCoreSource& units,
                          const f4::world::IFlightSource& flights,
                          const FlightAggregateConfig& cfg = {},
                          const FlightAggregateFilter& filter = {});

    /// Advance the clock and fire whole update ticks at the configured
    /// cadence (the GroundWar accumulator shape). Suspended, arrived,
    /// and destroyed flights are skipped (suspended: the sim owns the
    /// truth until the session folds it back).
    void tick(CampaignTime delta_sec);

    // --- Tier cooperation (the session's deagg/reagg contract) -------

    /// Suspend/resume aggregate advancement for one flight (the sim
    /// materialized it). Unknown vu = no-op. Suspending does not touch
    /// position/fuel — the fold-back does.
    void set_suspended(std::uint32_t vu, bool suspended);

    /// Fold the sim's truth back into the aggregate (the reagg handoff,
    /// FIDELITY_TIERS_PLAN §4.4): position/altitude/fuel from the lead
    /// aircraft, the waypoint cursor reset to the next upcoming
    /// waypoint, suspension lifted, dirty set. Unknown vu = no-op.
    void reaggregate(std::uint32_t vu, double fx, double fy,
                     float altitude_ft, std::int32_t fuel_burnt);

    /// Fold an all-dead flight (its aircraft were killed in-sim): the
    /// flight stops advancing permanently. Losses themselves book at
    /// the C1 sink as today. Unknown vu = no-op.
    void mark_destroyed(std::uint32_t vu);

    /// FID-5: register a generated mission as an AGGREGATE (the
    /// synthetic-intent tiering — the FID-6 certificate's "the war's
    /// live aircraft are all synthetic" lever). The flight starts at
    /// its route's first waypoint (the base the planner launched from),
    /// holds there until that waypoint's depart (the session sets it to
    /// the intent's TOT-anchored takeoff gate), then walks SPEED mode at
    /// the cruise. Bypasses the construction filter: the generated
    /// war's own budget is the spawner's, not the save-flight cap.
    /// Returns the new flight's index; \c size_t(-1) when the seed is
    /// unusable (vu 0, or a duplicate — the reserved namespace makes
    /// that a caller bug, so the refusal is loud).
    [[nodiscard]] std::size_t register_synthetic(
        const SyntheticFlightSeed& seed);

    // --- Read access ---------------------------------------------------

    /// The engine's flights, wire order (parallel with routes()).
    [[nodiscard]] const std::vector<FlightAggregateState>& flights()
        const noexcept {
        return flights_;
    }
    /// Each flight's decoded route (parallel with flights()).
    [[nodiscard]] const std::vector<std::vector<f4::entities::WaypointState>>&
    routes() const noexcept {
        return routes_;
    }
    /// Flight lookup by VU_ID.num (null when absent).
    [[nodiscard]] const FlightAggregateState* find(std::uint32_t vu) const;
    /// Index of a flight by VU_ID.num (size_t(-1) when absent).
    [[nodiscard]] std::size_t index_of(std::uint32_t vu) const;

    /// Absolute campaign time (the save epoch + the advanced clock) —
    /// the same anchor the waypoints' arrive/depart and the ledger's
    /// CampaignTimes use.
    [[nodiscard]] std::int64_t now() const noexcept {
        return epoch_ + clock_;
    }

    /// The configured speed-mode cruise (grid units per minute) — the
    /// deagg spawn pose's cruise speed (× 1024 ft / 60 s = ft/s).
    [[nodiscard]] double cruise_grid_per_min() const noexcept {
        return cfg_.cruise_grid_per_min;
    }

    /// Seconds until the flight's first waypoint departs (> 0 pending,
    /// <= 0 departed/no time). The takeoff-ops window query.
    [[nodiscard]] std::int32_t seconds_to_depart(std::size_t index) const;
    /// Seconds until the flight's mission-over time (<= 0 past/none).
    /// The recovery-ops window query.
    [[nodiscard]] std::int32_t seconds_to_mission_over(
        std::size_t index) const;
    /// FID-5: seconds until the flight's time-on-target (<= 0 past/none).
    /// The delivery-ops window query: a flight approaching its TOT
    /// deaggregates to fly the attack (the delivery is a per-aircraft
    /// phase — §4.3's mission-phase pinning, the same arms the takeoff
    /// and recovery windows ride).
    [[nodiscard]] std::int32_t seconds_to_time_on_target(
        std::size_t index) const;
    /// Current leg bearing (compass radians, atan2(east, north)) — the
    /// deagg spawn pose's heading. Holds pose at the save position
    /// before departure.
    [[nodiscard]] double current_heading_rad(std::size_t index) const;

    /// One-frame counters (the session's stats panel + the tests).
    struct Stats {
        int updates = 0;      ///< update ticks fired
        int flights = 0;      ///< aggregated flights
        int aggregate = 0;    ///< advancing as aggregates right now
        int suspended = 0;    ///< deaggregated (sim-owned) right now
        int arrived = 0;      ///< reached their last waypoint
        int destroyed = 0;    ///< folded as all-dead
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    /// Recompute the one-frame counters from the state vector.
    void refresh_stats();

    /// Advance one flight one update (the move_phase_ analog): time or
    /// speed mode by route shape, fuel accrual while progressing.
    void advance_flight_(FlightAggregateState& f, std::size_t index,
                         std::vector<f4::entities::WaypointState>& route,
                         std::int64_t now_abs);

    /// Reset the waypoint cursor after a fold-back: the first waypoint
    /// not yet reached (time mode: arrive > now; speed mode: the
    /// nearest waypoint by remaining distance wins its leg).
    void reset_cursor_(FlightAggregateState& f,
                       const std::vector<f4::entities::WaypointState>& route,
                       std::int64_t now_abs);

    FlightAggregateConfig cfg_;
    std::vector<FlightAggregateState> flights_;
    std::vector<std::vector<f4::entities::WaypointState>> routes_;
    std::int64_t epoch_ = 0;       ///< campaign.current_time at construct
    std::int64_t clock_ = 0;       ///< advanced seconds since construct
    CampaignTime next_update_ = 0; ///< whole-update gate (engine clock)
    Stats stats_{};

    /// True when the route's arrival times are usable (any arrive > 0)
    /// — TIME mode; else SPEED mode. Chosen per flight at construction.
    std::vector<bool> time_mode_;
};

} // namespace f4::campaign

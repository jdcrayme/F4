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
// AGG-4 (Docs/AGGREGATE_CLOCK_PLAN.md §4 — the endgame): a TIME-mode
// row's state is a PURE QUERY f(route, t) computed on read. The
// schedule the save carries IS the truth; the engine no longer steps
// TIME rows through 60-s quanta. What remains discrete — the waypoint
// arrivals (the cursor transitions and the terminal arrival) — fires
// from the deterministic due-queue (due_queue.hpp, the AGG-2a
// primitive's named consumer): one live event per row, armed at the
// next scheduled arrive, re-armed as it fires, invalidated by any
// mutation that touches the row (fold re-anchor, retask, scrub,
// suspend). Fuel is the closed form of the same schedule (the walk's
// own per-update accrual, counted on the update grid without walking
// it); SPEED-mode rows keep the chunk walk until synthetic intents
// carry arrival schedules. Between events the TIME rows' propagation
// cost is ZERO: the serving faces (display_position, fuel_burnt_now,
// waypoint_cursor) derive everything from (route, now).
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
#include <f4/campaign/due_queue.hpp>      // AGG-4: the arrival events
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

    /// The wire's credibility floor (grid/min). A TIME-mode route
    /// whose computable legs ALL imply a crawl below this (the stock
    /// saves' waypoint times are the ATO planner's multi-day horizon —
    /// legs crossing Korea at 0.01 grid/min, weeks per leg) cannot
    /// drive motion; such a route flies SPEED mode at the cruise
    /// instead, and a route with no computable leg at all does too.
    /// Sane wires (hand-authored fixtures, the tests' schedules) keep
    /// the TIME-mode identity. 0 disables the fallback (the wire is
    /// always trusted).
    double min_leg_speed_grid_per_min = 1.0;
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
    /// ABSOLUTE campaign time the mission's recovery books (the ATM's
    /// own mission_over, threaded through the intent). 0 = none — the
    /// documented v1 gap (a seed without it never opens the
    /// recovery-ops window and parks on its last waypoint).
    std::int32_t mission_over_time = 0;
    /// The intent's planned route (takeoff → ingress → target → egress →
    /// landing). May be empty — a route-less synthetic flight is a
    /// parked aggregate (has_route false, nothing advances).
    std::vector<f4::entities::WaypointState> route;
};

/// One flight's aggregate state. Public read-only by convention —
/// mutation flows through tick()/set_suspended()/reaggregate()/
/// mark_destroyed()/retask()/scrub() so the invariants hold.
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
    /// SPEED-mode cruise for THIS row (grid/min). 0 = the config
    /// default (the ATM's campaign-move constant). The fold books the
    /// lead's actual ground speed here — a folded flight keeps the
    /// pace the viewer just watched instead of braking to the global
    /// estimate.
    double cruise_grid_per_min = 0.0;
    bool dirty = false;                 ///< moved/burned since last sync
    bool suspended = false;             ///< deaggregated: sim owns truth
    bool arrived = false;               ///< reached the last waypoint
    bool destroyed = false;             ///< folded back as all-dead
    bool has_route = false;             ///< the save carried waypoints
    /// CAMP-CMD-2: scrubbed by a flight_abort before departure — the
    /// sortie never launches. A distinct terminal state (NOT destroyed,
    /// NOT arrived): the tick skips it, the tier triggers and the air
    /// picture skip it, and the flights view reports it as aborted.
    bool scrubbed = false;
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
    /// destroyed, and scrubbed flights are skipped (suspended: the sim
    /// owns the truth until the session folds it back; scrubbed: the
    /// CAMP-CMD-2 abort that never launches).
    void tick(CampaignTime delta_sec);

    // --- Tier cooperation (the session's deagg/reagg contract) -------

    /// Suspend/resume aggregate advancement for one flight (the sim
    /// materialized it). Unknown vu = no-op. Suspending does not touch
    /// position/fuel — the fold-back does.
    void set_suspended(std::uint32_t vu, bool suspended);

    /// Fold the sim's truth back into the aggregate (the reagg handoff,
    /// FIDELITY_TIERS_PLAN §4.4): position/altitude/fuel from the lead
    /// aircraft, the waypoint cursor reset to the next upcoming
    /// waypoint, suspension lifted, dirty set. A TIME-mode flight's
    /// schedule RE-ANCHORS at the fold (see reanchor_schedule_) so the
    /// wire schedule passes through the folded position at the fold
    /// time — the fold never snaps the flight back to a stale schedule
    /// point. `cruise_grid_per_min` (0 = keep the config default)
    /// books the lead's ACTUAL ground speed as this row's SPEED-mode
    /// cruise: a flight that just flew 400 kts past the camera resumes
    /// its aggregate at the pace the viewer watched, not at the global
    /// 121-kt estimate (the visual brake that read as "the flight
    /// stopped"). `lead_airborne` lets an AIRBORNE lead re-anchor even
    /// with the wire's takeoff gate still closed — it already flew off
    /// the wire; a grounded complement keeps the wire. Unknown vu =
    /// no-op.
    void reaggregate(std::uint32_t vu, double fx, double fy,
                     float altitude_ft, std::int32_t fuel_burnt,
                     double cruise_grid_per_min = 0.0,
                     bool lead_airborne = false);

    /// Track a SUSPENDED flight's live position/fuel into its row (the
    /// session calls this per tier pass from the lead aircraft's
    /// transform). Suspension means the sim owns the truth — but the
    /// row is what every tier rule (the reagg bubble, the deagg
    /// triggers) and the fold's not-killed path read, so a stale row
    /// made the machinery judge a live flight by where it MATERIALIZED,
    /// not where it was. Fuel is monotone (the same rule as the fold).
    /// Unknown vu, or a row that is not suspended, = no-op (an
    /// aggregate owns its own kinematics).
    void update_live(std::uint32_t vu, double fx, double fy,
                     float altitude_ft, std::int32_t fuel_burnt);

    /// Fold an all-dead flight (its aircraft were killed in-sim): the
    /// flight stops advancing permanently. Losses themselves book at
    /// the C1 sink as today. Unknown vu = no-op.
    void mark_destroyed(std::uint32_t vu);

    // --- CAMP-CMD-2 — the command writes ------------------------------

    /// Retask one flight (the flight_retask write): the route replaces
    /// whatever the flight flew (the caller builds it FROM the flight's
    /// current position — the head waypoint IS that position), the
    /// mission byte and the two absolute times follow the new plan, and
    /// the flight walks the new route in SPEED mode from the retask
    /// point (the new route carries no leg times — the intent vocabulary;
    /// a TIME-mode save flight retasks INTO speed mode, documented).
    /// The cursor starts at index 1 (flying toward the route's second
    /// waypoint from the head) unless the flight is SUSPENDED — a live
    /// aircraft owns the truth; the fold-back's reset_cursor_ re-derives
    /// the cursor on the new route from the lead aircraft's position.
    /// Refuses (false, nothing written): unknown vu, arrived, destroyed,
    /// scrubbed, suspended-cursor is fine but an empty route never is.
    bool retask(std::uint32_t vu, std::uint8_t mission,
                std::vector<f4::entities::WaypointState> route,
                std::int32_t time_on_target_abs,
                std::int32_t mission_over_abs);

    /// Scrub one flight (the flight_abort write for a sortie that has
    /// not launched): the flight stops advancing permanently WITHOUT
    /// being destroyed or arrived — a distinct terminal state the tier
    /// triggers, the air picture, and every ops window skip. Refuses
    /// (false): unknown vu, arrived, destroyed, already scrubbed. A
    /// SUSPENDED flight scrubs too (the caller folds its live aircraft
    /// back first — the parked complement never launches).
    bool scrub(std::uint32_t vu);

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
    /// A row's EFFECTIVE SPEED-mode cruise: its folded ground speed
    /// when the fold booked one, else the config default. The combat
    /// trigger's convergence prediction reads this per row.
    [[nodiscard]] double effective_cruise_grid_per_min(
        std::size_t index) const noexcept {
        if (index >= flights_.size()) return cfg_.cruise_grid_per_min;
        const double own = flights_[index].cruise_grid_per_min;
        return own > 0.0 ? own : cfg_.cruise_grid_per_min;
    }
    /// The row's mode as constructed (TIME = the wire's own schedule
    /// drives it; SPEED = the cruise walk). A timed route whose legs
    /// all imply a crawl below min_leg_speed_grid_per_min constructs
    /// as SPEED (the stock saves' multi-day wires cannot drive
    /// motion). Unknown index = false.
    [[nodiscard]] bool is_time_mode(std::size_t index) const noexcept {
        return index < time_mode_.size() && time_mode_[index];
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

    /// The flight's DISPLAY position at now_abs — a PURE query (no
    /// state mutation, the GetRealPosition analogue: coarse simulation,
    /// smooth display). TIME mode re-derives the wire's own schedule
    /// interpolation at now_abs; SPEED mode walks the current leg
    /// forward from the last advance at the cruise speed (anchored at
    /// the departure gate for a flight whose first update has not fired
    /// yet), clamped at the route's end. Suspended/arrived/destroyed/
    /// scrubbed flights and route-less rows report their stored
    /// position (a suspended flight's truth is its live aircraft — the
    /// session overlays the lead's transform on top of this).
    void display_position(std::size_t index, std::int64_t now_abs,
                          double& fx, double& fy,
                          float& altitude_ft) const;

    /// AGG-4 — the row's fuel burnt AT NOW (the closed form of the
    /// walk's own accrual: the same per-update burn on the same update
    /// grid, counted from the row's fuel anchor without walking it).
    /// TIME-mode aggregate rows report base + burn × eligible updates
    /// since the anchor (the schedule decides which updates move: the
    /// walk's own predicate — past the takeoff gate, past the first
    /// activation, before the terminal arrival); every other row
    /// (SPEED, suspended, terminal) reports the stored field, which is
    /// materialized at each transition that would otherwise hide the
    /// accrual (suspend, fold, retask). Byte-equal to the pre-AGG-4
    /// walk's totals — pinned by test.
    [[nodiscard]] std::int32_t fuel_burnt_now(std::size_t index) const;

    /// AGG-4 — the row's waypoint cursor AT NOW: the waypoint being
    /// flown TOWARD (or the last one whose arrival passed). TIME-mode
    /// rows derive it from the schedule with the walk's own rule (past
    /// arrivals override in order, the first upcoming leg names its
    /// target, a hold keeps the previous); SPEED rows report the
    /// stored cursor the walk maintains. The stored field on a TIME
    /// row materializes only at the queue's arrival events — the
    /// mid-leg flying-toward value lives in this query.
    [[nodiscard]] std::size_t waypoint_cursor(std::size_t index) const;

    /// One-frame counters (the session's stats panel + the tests).
    struct Stats {
        int updates = 0;      ///< update ticks fired
        int flights = 0;      ///< aggregated flights
        int aggregate = 0;    ///< advancing as aggregates right now
        int suspended = 0;    ///< deaggregated (sim-owned) right now
        int arrived = 0;      ///< reached their last waypoint
        int destroyed = 0;    ///< folded as all-dead
        int scrubbed = 0;     ///< CAMP-CMD-2: aborted before launch
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    /// Recompute the one-frame counters from the state vector.
    void refresh_stats();

    /// Advance one flight one update (the move_phase_ analog). AGG-4:
    /// SPEED-mode rows only — a TIME row's state is the schedule (the
    /// due-queue's arrival events + the query faces serve it), so the
    /// chunk walk never touches one.
    void advance_flight_(FlightAggregateState& f, std::size_t index,
                         std::vector<f4::entities::WaypointState>& route,
                         std::int64_t now_abs);

    /// AGG-4 — one TIME-mode arrival event, fired from the due-queue:
    /// validate the payload against the row's current shape (a
    /// mutation re-armed the row → a stale event drops), materialize
    /// exactly what the update walk used to write when its quanta
    /// crossed the arrival (the waypoint snap + the cursor + the
    /// terminal flag), and arm the row's NEXT scheduled arrival.
    struct ArrivalEvent {
        std::uint32_t vu;    ///< row identity (index + vu agreement)
        std::uint32_t gen;   ///< the row's schedule generation
        std::size_t index;   ///< the row (flights_ never erases — stable)
        std::size_t wp;      ///< the waypoint whose arrive fired
        std::int64_t due;    ///< the event's own due (the last_move stamp)
    };
    void fire_arrival_(const ArrivalEvent& ev, std::int64_t due_abs);

    /// Arm the row's next scheduled arrival strictly after `after_abs`
    /// (the cursor-chase: one live event per row). No eligibility
    /// checks — the callers validate; nothing to arm = no event.
    void arm_next_arrival_(std::size_t index, std::uint32_t gen,
                           std::int64_t after_abs);

    /// Bump the row's schedule generation (retiring its live events)
    /// and re-arm it from now when it is an eligible TIME aggregate.
    /// Every mutation that touches a row's schedule or flight state
    /// funnels through this.
    void reschedule_arrivals_(std::size_t index);

    /// Materialize the derived TIME state INTO the row at now_abs (the
    /// walk's own overrides, cursor and terminal rule) — construction
    /// pastes a mid-war save's schedule state at the epoch, and the
    /// fuel transitions paste the closed-form burn before the stored
    /// field takes over (suspend, fold, retask).
    void materialize_time_state_(std::size_t index, std::int64_t now_abs);

    /// The closed-form count of the walk's moving updates on the grid
    /// (epoch_ + k·update_sec, k ≥ 1) inside (from_abs, to_abs] that
    /// the pre-AGG-4 walk would have burned on: past the takeoff gate,
    /// past the first activation (the first scheduled arrival or the
    /// first leg's departure — the walk's own moved() predicate as one
    /// integer threshold), before the terminal arrival. Exact int64
    /// floor/ceil arithmetic — no walk, no per-update loop.
    [[nodiscard]] std::int64_t count_burn_updates_(
        std::size_t index, std::int64_t from_abs, std::int64_t to_abs) const;

    /// The TIME-mode cursor query's shared body (waypoint_cursor's
    /// walk replica, also the heading target's source).
    [[nodiscard]] std::size_t time_cursor_(std::size_t index,
                                           std::int64_t now_abs) const;

    /// Reset the waypoint cursor after a fold-back: the first waypoint
    /// not yet reached (time mode: arrive > now; speed mode: the
    /// nearest waypoint by remaining distance wins its leg).
    void reset_cursor_(FlightAggregateState& f,
                       const std::vector<f4::entities::WaypointState>& route,
                       std::int64_t now_abs);

    /// FID-P1: catch one flight's stored position up to now_abs along
    /// its CURRENT route (position/altitude only — fuel stays
    /// quanta-granular, booked at the update boundaries). The retask
    /// mutation anchors at the flight's true position — the one the
    /// serving face extrapolates — not at the last 60-s quanta point;
    /// without it a mid-quanta retask would snap the flight BACK to the
    /// quanta point. A no-op at quanta-aligned calls (elapsed 0) and
    /// for suspended flights (the sim owns the truth).
    void catch_up_(FlightAggregateState& f, std::size_t index,
                   std::int64_t now_abs);

    /// Shift a TIME-mode route's arrive/depart times by one constant
    /// so the schedule passes through the folded position (f.fx/fy) at
    /// now_abs. The route SHAPE and every leg duration/dwell are
    /// preserved — the wire schedule simply slides to where the
    /// aircraft actually is, so the fold-back never snaps the flight
    /// back to a stale schedule point (the live window flew at real
    /// speeds while the wire modeled slower progress; the delta can be
    /// tens of grids). A grounded pre-departure fold keeps the wire
    /// (the takeoff gate still owns the schedule); an AIRBORNE lead
    /// re-anchors regardless — it already flew off the wire, and the
    /// shift lands the gate in the past (the sortie is flying). A
    /// no-op for routes without at least two scheduled anchors.
    void reanchor_schedule_(FlightAggregateState& f,
                            std::vector<f4::entities::WaypointState>& route,
                            std::int64_t now_abs, bool lead_airborne);

    FlightAggregateConfig cfg_;
    std::vector<FlightAggregateState> flights_;
    std::vector<std::vector<f4::entities::WaypointState>> routes_;
    std::int64_t epoch_ = 0;       ///< campaign.current_time at construct
    std::int64_t clock_ = 0;       ///< advanced seconds since construct
    CampaignTime next_update_ = 0; ///< whole-update gate (engine clock)
    Stats stats_{};

    /// AGG-4 — the TIME rows' discrete-transition scheduler (one live
    /// arrival event per eligible row; keyed (due, priority, seq) by
    /// the due-queue's own determinism contract).
    DueQueue<ArrivalEvent> arrivals_;
    /// Per row: the schedule generation its live events were armed
    /// under. Any mutation bumps it; a popped event whose generation
    /// disagrees with the row's is stale and drops.
    std::vector<std::uint32_t> arrival_gen_;
    /// Per row: the ABSOLUTE campaign time the stored fuel was last
    /// materialized at (construction seeds the save's burn; suspend,
    /// fold and retask re-materialize). The closed-form query counts
    /// the eligible burn updates since this anchor; the walk-shaped
    /// monotone surfaces (the fold's max) read the materialized field.
    std::vector<std::int64_t> fuel_anchor_;

    /// True when the route's arrival times are usable (any arrive > 0)
    /// — TIME mode; else SPEED mode. Chosen per flight at construction.
    std::vector<bool> time_mode_;

    /// Per flight: the LAST waypoint index carrying an arrival time
    /// (size_t(-1) = none). TIME mode's arrival fires when its arrive
    /// passes — without this, a route whose tail legs are unscheduled
    /// (the walk skips them) never marked the flight arrived and it
    /// sat frozen at the last scheduled waypoint forever. Routes are
    /// only replaced by retask (which flips the flight into SPEED
    /// mode), so a construction-time index stays valid for every
    /// TIME-mode read.
    std::vector<std::size_t> schedule_end_index_;
};

} // namespace f4::campaign

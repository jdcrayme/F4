// f4-campaign/include/f4/campaign/ground_war.hpp
//
// GroundWar — the G1 ground-war engine (the "battalion-level movement +
// front line" tranche; see Docs/GROUND_WAR_PLAN.md).
//
// WHAT THIS IS. The air side of the war closed in C1–C6: tasking draws
// aircraft, aircraft fly and fight, losses write back, the next cycle
// tasks a weaker force. The GROUND side never existed: battalions sat
// frozen at their save-time grid cells, objective ownership never
// changed, and the only ground number the ledger booked was the
// shooter's ag_kills CREDIT — the victim's own attrition was the
// documented gap (CAMPAIGN_LOOP_PLAN §7: "Ground losses book only the
// CREDIT side; battalion roster attrition lands with the ground-war
// tranche").
//
// This engine is the campaign-side twin of the Campaign ladder (the
// AIR tasking engine): a headless, deterministic state machine over the
// same IDataSource interfaces, writing through the same result ledger,
// moved by the same one clock. What it does, per FreeFalcon
// correspondence:
//
//   * WAR PAIR — the same named-slot RelType::War rule
//     Campaign::belligerent_teams() uses (mutual 5 on the stance
//     matrix, garbage decodes to NoRelations). Ground war is a TWO-SIDE
//     machine this slice: the first belligerent pair in slot order.
//     Neutral teams' battalions stand down (a legal, pinned state).
//   * FRONT LINE — per grid column, the boundary between the two
//     belligerents' objective holdings (the FLOT the reference's
//     DistanceToFront queries serve). Column x's band (±kFrontBand)
//     holds: south side's furthest-north objective and north side's
//     furthest-south objective; the front is their midpoint. Sides are
//     assigned by territory centroid (deterministic). Columns where
//     only one side holds territory have no front (the flank rule).
//   * ORDERS (the GTM-lite) — at the orders cadence, every belligerent
//     scores the ENEMY's objectives with the reference's own terms
//     (campobj.cpp DoCalculations, documented in the reference's §13.1):
//     front proximity (200 − distance) × 0.2 capped ±30, priority bonus
//     +50 above 95 / +20 above 90, clamped 0..100. The reference's
//     random(5) term is DROPPED — determinism is this engine's contract
//     (documented deviation, not an omission). Mobile battalions (in
//     wire order) each take the best-scored objective with open
//     garrison slots (score − distance penalty, ties by wire order);
//     artillery trails at half speed; air-defense / SS-missile / supply
//     units are static (the reference's own GTM class distinction:
//     AirDefense/Support/Repair never join the capture ladder).
//   * MOVEMENT — at the update cadence, every mobile battalion with a
//     target walks toward it at its movement speed (the wire's own
//     movement_speed kph when the UCD enrichment carried it, else the
//     subtype's family default — the fixtures' UCD is an 8-entry sample
//     and most saves carry no enrichment). Speeds live in grid units:
//     1 grid unit ≈ 1.024 km, kph → grid/sec. Sub-grid travel
//     accumulates in 1/256 fixed point (the wire's own `position`
//     byte semantics). Fatigue accrues with hours moved (speed halves
//     past 75); supply below 25 halves speed too; an ENGAGED battalion
//     is pinned (it does not advance while in contact — the reference's
//     own movement gate).
//   * ENGAGEMENT — opposing battalions within contact range (2 grid
//     units ≈ 2 km, the reference's battalion firefight distance)
//     exchange attrition every update: a linear exchange where each
//     side's take is proportional to the enemy's combat power
//     (strength × supply × morale), a few vehicles per HOUR per pair
//     at parity. Fractional kills accumulate (fixed point) and land as
//     whole vehicle kills on the ledger. Vehicles leave the roster
//     deterministically (highest group first). Morale erodes with
//     losses, fatigue with combat; a battalion at zero vehicles is
//     destroyed. Spatial bucketing (6-grid cells, the threat map's own
//     MAP_RATIO) keeps the pair walk O(N) per update, not O(N²).
//   * CAPTURE — a battalion at an enemy objective (≤ capture range)
//     with no enemy defender in contact and strength ≥ the threshold
//     flips the objective's owner (the ledger books the capture — the
//     front actually MOVING). The capturing battalion holds as
//     garrison (it is not re-tasked off its prize).
//   * RESUPPLY — the .cmp header's last_resupply anchor (exposed by
//     ICampaignSource since the C2 tranche) drives a ground-supply
//     cadence: each fire refills supply and rests fatigue, exactly the
//     catch-up-once shape the reinforcement cadence uses. DEFAULT OFF
//     (period 0 — the C1 golden-identity discipline).
//   * AIR-CAUSED LOSSES — the C1 sink books AG kills against battalion
//     ENTITIES through apply_ground_loss (air=true); this engine PULLS
//     those events (index-tracked over the ledger's arrival-ordered
//     log) and applies the vehicle kills to its own battalions, so air
//     power thins the ground line the ground war then has to fight
//     with. The loop closes both directions.
//   * LEDGER SYNC — every battalion with activity (moved, attrited,
//     resupplied, destroyed) syncs its final state into the ledger
//     after each update (last write wins): position, strength, supply/
//     morale/fatigue, destroyed. The ledger's ground block is therefore
//     the ground war's byte-stable certificate — the C5 determinism
//     proof covers the GROUND side with zero new machinery.
//
// DISCIPLINE (unchanged from the rest of f4-campaign):
//   * NO EntityWorld, NO f4-world-convert, NO flight model here. The
//     engine sees campaign identity (VU_IDs, team slots, grid cells)
//     and numbers; the f4-simulation session does the entity-side
//     mirroring (transforms, tactical components) where both sides are
//     already linked.
//   * NO RNG, NO clocks of its own. The war advances by tick(delta)
//     on the caller's clock; ordering is wire order everywhere. The
//     same sources + the same tick sequence produce the same ledger
//     bytes (pinned by test, the C5 contract extended to the ground).
//   * ONE WRITER: every state transition flows through the LEDGER's
//     typed apply methods. The engine's internal state is sim-side
//     truth; the ledger is the campaign's book.
//
// Dependencies: f4-world (IDataSource), result_ledger (the write
// model). C++20.
//
// KNOWN GAPS (deliberate, documented — see the plan doc's own §gaps):
//   * Brigade-level doctrine (parent_id cohesion, division reserves)
//     is not modeled — every battalion tasking is independent. The
//     wire's hierarchy is decoded and carried; the doctrine lands with
//     the strategy tranche that owns it in the reference.
//   * Artillery stands IN the line rather than firing over it (no
//     ranged support standoff this slice; the reference's indirect
//     fire model needs the WST/battery data the theater import
//     carries).
//   * Deaggregated vehicle kills (bubble entities) do not decay the
//     parent battalion's roster — the deagg→parent mapping is the
//     viewer tranche's; battalion-ENTITY kills (air=true events) do.

#pragma once

#include <f4/campaign/result_ledger.hpp>
#include <f4/world/data_source.hpp>

#include <cstdint>
#include <vector>

namespace f4::campaign {

/// Tunables. Defaults documented here; every field is pinned by the
/// unit tests through the ledger's ground block. Mirrors the
/// CampaignConfig pattern ( Campaign ladder's own tunables).
struct GroundWarConfig {
    /// Ground update cadence (campaign seconds). FreeFalcon updates
    /// ground units on its campaign update pass; 60 s keeps a 24-hour
    /// war at 1440 updates (672 battalions × bucketed pair walk —
    /// trivially cheap) while sub-grid movement still resolves.
    CampaignTime update_sec = 60;

    /// Orders cadence (campaign seconds) — the GTM cycle. The air
    /// ladder's own tasking cadence defaults to 1800; the ground
    /// commander re-plans on the same rhythm.
    CampaignTime orders_sec = 1800;

    /// Contact range (grid units) — battalions this close are in
    /// engagement (and pinned). 2 grid ≈ 2 km, the reference's
    /// battalion firefight distance.
    int contact_range_grid = 2;

    /// Capture range (grid units) — how close a battalion must be to
    /// an objective to take it.
    int capture_range_grid = 2;

    /// Minimum vehicles to capture (a spent battalion does not take
    /// ground).
    int capture_min_strength = 6;

    /// Garrison slots per objective (how many battalions one objective
    /// attracts per orders cycle).
    int objective_garrison = 2;

    /// Exchange rate: vehicles per HOUR an equal-strength pair loses
    /// (each side, at full supply/morale). 4/h means a 12-vehicle
    /// battalion in a single pinned engagement dies in about 3 hours —
    /// the reference's own tempo (battalions fight for hours, not
    /// minutes).
    int exchange_vehicles_per_hour = 4;

    /// Resupply cadence (campaign seconds; 0 = OFF — the golden-identity
    /// default, exactly like CampaignConfig::reinforcement_period_sec).
    /// Each fire: supply +25 (cap 100), fatigue −25 (floor 0), morale
    /// +10 (cap 100). The anchor is the .cmp header's last_resupply,
    /// bridged through ICampaignSource, catch-up-once.
    CampaignTime resupply_period_sec = 0;
};

/// One battalion's live state inside the engine (the sim-side truth;
/// the LEDGER carries the campaign-side books).
struct GroundUnitState {
    std::uint32_t vu = 0;        ///< VU_ID.num (the campaign key)
    std::uint8_t owner = 0;      ///< team slot
    std::uint8_t subtype = 0;    ///< STYPE_LAND_* (mobility family)
    std::int32_t x = 0;          ///< grid column (current, integer)
    std::int32_t y = 0;          ///< grid row (current, integer)
    std::int32_t fx = 0;         ///< sub-grid fraction (0..255, × grid)
    std::int32_t fy = 0;         ///< sub-grid fraction (0..255)
    std::int32_t dest_x = 0;     ///< movement destination (grid)
    std::int32_t dest_y = 0;
    std::uint32_t target = 0;    ///< target objective VU_ID.num (0 none)
    std::uint32_t roster = 0;    ///< live 2-bit group packing (wire form)
    int strength = 0;            ///< vehicles (sum of roster groups)
    int strength_initial = 0;    ///< vehicles at snapshot
    int run_losses = 0;          ///< vehicles lost this run
    std::uint32_t loss_acc = 0;  ///< fractional attrition accumulator (1/256)
    std::uint8_t supply = 0;
    std::uint8_t morale = 0;
    std::uint8_t fatigue = 0;
    std::uint8_t heading = 0;    ///< wire convention: 0-255, ×1.4 deg
    /// Absolute campaign times (the save's epoch base — the wire's own
    /// last_move/last_combat convention; TestCamp's are ~38.5M s).
    std::int64_t last_move = 0;
    std::int64_t last_combat = 0;
    bool destroyed = false;
    bool mobile = false;         ///< subtype family joins the capture ladder
    bool artillery = false;      ///< trails the line at half speed
    /// Runtime: in contact since the last engage phase (pinned — an
    /// engaged battalion does not advance).
    bool pinned = false;
    /// Movement speed (kph; the wire's movement_speed, else the
    /// subtype family default).
    int speed_kph = 0;
    /// Precomputed movement step per update tick (grid × 1/256).
    int step_fp = 0;
    /// Dirtied since the last ledger sync (movement/attrition/resupply/
    /// destruction) — the sync's activity filter.
    bool dirty = false;
    /// Movement accumulator for supply burn (1/256 of an update).
    std::uint32_t move_ticks = 0;
};

/// One objective's mirrored state (ownership is ENGINE state here; the
/// LEDGER books the capture events, the WRITE-BACK lands the owner).
struct GroundObjectiveState {
    std::uint32_t vu = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t owner = 0;          ///< live owner (flips on capture)
    std::uint8_t initial_owner = 0;  ///< snapshot owner (write-back diff)
    std::uint8_t priority = 0;
};

/// One front-line column (the FLOT sample at grid column x).
struct FrontColumn {
    std::int32_t x = 0;
    /// Midpoint grid row between the sides' forward holdings; valid
    /// only when `contested` (both sides hold objectives in the band).
    std::int32_t y = 0;
    /// The side holding the territory SOUTH of the front.
    std::uint8_t south_owner = 0;
    /// The side holding the territory NORTH of the front.
    std::uint8_t north_owner = 0;
    bool contested = false;
};

// ============================================================================
// G2 — the shared FLOT + unit-target ranking (the interdiction link's
// tasking-side vocabulary; INTERDICTION_PLAN.md §2). The engine computes
// the front from its LIVE objective mirror; the air tasking side needs
// the same columns from the save-time source — the math exists exactly
// once here so the two can never drift.
// ============================================================================

/// One objective's front-relevant fields — the projection BOTH callers
/// of the front computation build (the engine's live mirror, mutated by
/// captures; the tasking side's save-time source view).
struct FrontObjectiveView {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t owner = 0;
};

/// Project an IObjectiveSource into the front view (wire order).
[[nodiscard]] std::vector<FrontObjectiveView>
front_objective_view(const f4::world::IObjectiveSource& objectives);

/// The front line between two belligerents' objective holdings: every
/// grid column in the objectives' x range, the contested ones (both
/// sides hold objectives in the ±3-column band) carrying the midpoint
/// row between the south side's furthest-north and the north side's
/// furthest-south holdings. Sides by held-objective mean y (the smaller
/// holds the south — deterministic). Integer midpoints, wire order.
[[nodiscard]] std::vector<FrontColumn>
front_columns_from_objectives(
    const std::vector<FrontObjectiveView>& objectives,
    std::uint8_t side_a, std::uint8_t side_b);

/// The unit-target ranking for CAS tasking: the battalions of teams at
/// WAR with `team` (the symmetric belligerence rule, land domain, the
/// aggregate Battalion class, non-empty roster), ordered by squared
/// distance to the nearest CONTESTED front column ascending — close air
/// support means the battalions the front fight is made of — wire order
/// breaking ties. Ledger-destroyed battalions are skipped when the
/// ledger is provided (a spent target wastes no package). Deterministic
/// (squared distance ranks the same as distance; no sqrt, no RNG).
[[nodiscard]] std::vector<std::uint32_t>
rank_battalion_targets(const f4::world::IUnitCoreSource& units,
                       const f4::world::ITeamSource& teams,
                       const std::vector<FrontColumn>& front,
                       std::uint8_t team,
                       const CampaignResultLedger* ledger);

/// The belligerent PAIR the FLOT is between (the engine's own rule:
/// named slots, RelType::War toward another named slot, the FIRST
/// at-war pair in slot order — TestCamp's ROK/DPRK). Empty when the
/// world is at peace. The tasking side's unit-target ranking and the
/// engine share this derivation — the shared front math's pair.
[[nodiscard]] std::vector<std::uint8_t>
belligerent_pair(const f4::world::ITeamSource& teams);

/// Telemetry (the QC's ground block and the harness's diary read
/// these; every counter is cumulative unless marked pulse).
struct GroundWarStats {
    int updates = 0;             ///< ground update ticks fired
    int orders_fired = 0;        ///< GTM cycles
    int moved_events = 0;        ///< update ticks in which >= 1 battalion moved
    int engaged_pairs = 0;       ///< cumulative engagement pairs seen
    int update_engaged = 0;      ///< pulse: pairs in the LAST update
    int vehicle_losses = 0;      ///< vehicles lost (mirror of the ledger's)
    int battalions_destroyed = 0;
    int captures = 0;
    int resupply_fires = 0;      ///< ground-supply cadence fires
    /// Distance walked by the whole army, grid units ×256 (integer).
    std::uint64_t army_distance_fp = 0;
    int battalions_alive = 0;    ///< alive NOW
    int battalions_mobile = 0;   ///< mobile & alive NOW
    /// Front line shape: contested columns + mean front row (×100 for
    /// the integer artifact; INT32_MIN when no contested column —
    /// "no front", serialized as 0 by callers that mean none).
    int front_columns = 0;
    std::int64_t front_mean_y_fp = 0;  ///< ×256 fixed point
};

class GroundWar {
public:
    /// Snapshot the sources and bind the ledger (the one writer). All
    /// references are borrowed: the sources for the constructor call
    /// (snapshot taken immediately), the LEDGER for the engine's
    /// lifetime (mutable — the C2 set_result_ledger discipline, except
    /// bound at construction because the ground war has no ledger-less
    /// mode: without books it cannot close its own loop).
    ///
    /// A null ledger is legal but makes the war a pure state machine
    /// (tests drive it that way to inspect internal state); campaign
    /// hosts always bind one.
    GroundWar(const f4::world::ICampaignSource& camp,
              const f4::world::ITeamSource& teams,
              const f4::world::IObjectiveSource& objectives,
              const f4::world::IUnitCoreSource& units,
              CampaignResultLedger* ledger,
              const GroundWarConfig& cfg = {});

    /// Advance the war by `delta_sec` campaign seconds. Accumulates
    /// whole update ticks and fires them in order (one big tick == N
    /// small ones — the C2 pin, same contract). Fires an orders cycle
    /// immediately at clock 0 (the ladder's own first-cycle rule: the
    /// save's own tasking is the LAST cycle's output; the war starts
    /// by planning, not by waiting).
    void tick(CampaignTime delta_sec);

    /// Campaign-relative clock (seconds; 0 = snapshot time).
    [[nodiscard]] CampaignTime clock() const noexcept { return clock_; }

    // --- Queries (the session, harness, QC, and write-back read) -------

    /// The war pair (slot order; empty when nobody is at war — the
    /// engine is then deliberately inert, pinned by test).
    [[nodiscard]] const std::vector<std::uint8_t>&
    belligerents() const noexcept {
        return war_pair_;
    }

    /// Battalion live states, wire order (the write-back's source).
    [[nodiscard]] const std::vector<GroundUnitState>&
    units() const noexcept {
        return units_;
    }

    /// Objective mirror, wire order (owner flips live here).
    [[nodiscard]] const std::vector<GroundObjectiveState>&
    objectives() const noexcept {
        return objectives_;
    }

    /// The front line (columns covering the objectives' x extent).
    /// Rebuilt at every orders cycle.
    [[nodiscard]] const std::vector<FrontColumn>&
    front_line() const noexcept {
        return front_;
    }

    [[nodiscard]] const GroundWarStats& stats() const noexcept {
        return stats_;
    }

private:
    // --- phases (each a deterministic walk; see the header doc) -------
    void fire_orders_();          ///< GTM-lite: score + assign targets
    void rebuild_front_();        ///< FLOT columns from objective holdings
    void move_phase_();           ///< advance mobile battalions
    void engage_phase_();         ///< detect + resolve exchanges
    void capture_phase_();        ///< flip undefended enemy objectives
    void resupply_phase_(CampaignTime t);  ///< the last_resupply cadence
    void pull_air_losses_();      ///< AG kills booked by the sink
    void sync_ledger_();          ///< dirty battalions → ledger state

    // --- helpers --------------------------------------------------------
    [[nodiscard]] bool hostile_(std::uint8_t a, std::uint8_t b) const;
    [[nodiscard]] int objective_score_(const GroundObjectiveState& o) const;
    [[nodiscard]] int distance_(const GroundUnitState& u,
                                const GroundObjectiveState& o) const;
    /// Apply `kills` vehicle losses to a battalion (roster decay,
    /// counters, death) — one code path for exchange and air kills.
    /// `book` = the LEDGER already carries these kills (air events the
    /// sink booked); the engine applies them to its own state without
    /// re-booking.
    void apply_vehicle_loss_(std::size_t idx, int kills,
                             std::uint32_t attacker_battalion,
                             std::uint8_t attacker_team,
                             bool air, std::uint32_t killer_squadron,
                             bool book);
    void mark_destroyed_(std::size_t idx);
    /// Rebuild the spatial buckets (called per engage phase).
    void rebuild_buckets_();
    [[nodiscard]] int bucket_index_(std::int32_t gx, std::int32_t gy) const;

    GroundWarConfig cfg_;

    /// The one writer (borrowed, mutable; may be null in tests).
    CampaignResultLedger* ledger_;

    /// Stance rows by slot (slot → row) for the hostility test.
    std::vector<std::vector<int16_t>> stance_by_slot_;
    std::vector<std::uint8_t> war_pair_;

    std::vector<GroundUnitState> units_;          ///< wire order
    std::vector<GroundObjectiveState> objectives_;///< wire order
    /// vu → index over units_ (wire order preserved for determinism).
    std::vector<std::uint32_t> unit_vus_;

    std::vector<FrontColumn> front_;
    std::int32_t min_x_ = 0;
    std::int32_t max_x_ = 0;

    /// Spatial buckets over the units' extent: cells of
    /// kGroundBucketRatio grid units, battalion indices per cell.
    /// Rebuilt every engage phase (movement invalidates it).
    int bucket_cols_ = 0;
    int bucket_rows_ = 0;
    std::int32_t bucket_x0_ = 0;
    std::int32_t bucket_y0_ = 0;
    std::vector<std::vector<int>> buckets_;

    CampaignTime clock_ = 0;
    CampaignTime next_update_ = 0;
    CampaignTime next_orders_ = 0;

    /// Resupply cadence (absolute campaign time, the reinforcement
    /// cadence's own shape: anchor from the .cmp header, advanced to
    /// "now" on each fire — catch-up-once).
    std::int64_t epoch_ = 0;
    std::int64_t last_resupply_ = 0;

    /// Air-loss pullback cursor: the ledger's ground_loss_log() index
    /// already applied (arrival order, so an index is a cursor).
    std::size_t air_loss_cursor_ = 0;

    /// Engagement scratch (rebuilt per engage phase; member to keep
    /// the update loop allocation-free).
    std::vector<int> engaged_a_;   ///< battalion index
    std::vector<int> engaged_b_;   ///< opposing battalion index

    GroundWarStats stats_;
};

} // namespace f4::campaign

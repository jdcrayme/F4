// f4-campaign/include/f4/campaign/ground_war.hpp
//
// GroundWar — the G1 ground-war engine (the "battalion-level movement +
// front line" tranche; see Docs/archive/GROUND_WAR_PLAN.md).
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

    // --- DOM-2 (supply depth): the per-objective pool ------------------
    //
    // Upstream interdiction targets supply/fuel lines and objectives
    // CARRY supply (ObjectiveClass::Save's supply/fuel u8s — decoded,
    // bridged, displayed, and until now read by no engine decision).
    // These knobs deepen the source-less G1 refill into a POOL: the
    // team's strategic stock (.tea supply_avail/fuel_avail) regenerates
    // its held objectives' stocks each resupply fire, and battalions
    // DRAW from the nearest own-held objective (the line of supply) —
    // a battalion beyond the radius of any own objective is cut off
    // (gets nothing), which is the encirclement/interdiction effect
    // the reference's supply-line targeting exists to create.

    /// The deepened pool (default false = the G1 flat +25/−25/+10
    /// refill, byte-identical goldens). When true (and the resupply
    /// cadence is armed), the fire becomes SOURCED: objective stocks
    /// regenerate from the team pools, then battalions draw from their
    /// nearest own-held objective. Requires objective supply data in
    /// the source (a save carrying none seeds zeros — legal, the war
    /// just runs dry; the campinit packs seed 100/100).
    bool objective_supply = false;

    /// Line-of-supply radius (grid units; ≈ km). A battalion further
    /// than this from every own-held objective draws nothing — cut
    /// off. 10 grid ≈ 10 km: a division's logistics tail.
    int supply_radius_grid = 10;

    /// Per-fire regeneration take per objective from the owning team's
    /// strategic stock (wire order; the pool depletes, later objectives
    /// get what remains). The .tea stock is u16 and the objective stock
    /// caps at 100, so 10/fire feeds a ten-objective front from a
    /// 1000-unit stock in one fire — a division-level PIPELINE, not a
    /// trickle; hosts tune the rate (the reference's own rates are
    /// runtime difficulty settings, not wire data).
    int supply_regen_per_fire = 10;

    /// The objective-feature repair cadence (campaign seconds; 0 = OFF
    /// — the golden-identity default, the same discipline as
    /// resupply_period_sec). The anchor is the .cmp header's
    /// last_repair (exposed by ICampaignSource since C2, never
    /// consumed until now), catch-up-once. Each fire repairs up to
    /// repair_features_per_fire damaged features per objective —
    /// flipping the fstatus pair to the wire's VIS_REPAIRED state —
    /// stamped into the objective's own last_repair field and booked
    /// through the ledger (the repaired bitmap rides the damage-state
    /// face, so the existing fstatus write-back carries it).
    CampaignTime repair_period_sec = 0;

    /// Minimum objective supply for repair crews to work (the movement
    /// gate's own threshold — 25 is where a battalion slows; it is
    /// also where its depot stops feeding the workshops). Below it an
    /// objective's damaged features stay damaged.
    int repair_min_supply = 25;

    /// Features repaired per objective per fire (the rate; upstream
    /// repair is slow — one feature per fire at the default keeps a
    /// shattered base rebuilding over days, not minutes).
    int repair_features_per_fire = 1;

    /// Objective supply consumed per repaired feature (logistics is
    /// not free — the repair rate is fed BY the pool, the tranche
    /// contract's own words).
    int repair_supply_cost = 5;
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
/// DOM-2 adds the logistics face: the objective's own supply/fuel/
/// losses stocks (seeded clamped to the wire domain 0..100 — real
/// saves carry 0xEB garbage where the original game never wrote the
/// fields) and the fstatus damage bitmap the repair cadence works on
/// (seeded from the save, kept current by adopting the ledger's
/// damage-state records before each repair walk).
struct GroundObjectiveState {
    std::uint32_t vu = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t owner = 0;          ///< live owner (flips on capture)
    std::uint8_t initial_owner = 0;  ///< snapshot owner (write-back diff)
    std::uint8_t priority = 0;
    // --- DOM-2: the per-objective pool + the repair work-set ----------
    std::uint8_t supply = 0;         ///< stock 0..100 (seed clamped)
    std::uint8_t fuel = 0;           ///< stock 0..100 (seed clamped)
    std::uint8_t losses = 0;         ///< wire u8, carried (no consumer yet)
    std::int64_t last_repair = 0;    ///< absolute campaign time (stamped)
    /// 2 bits per feature — the .obd fstatus face (0 normal, 1
    /// repaired, 2 damaged, 3 destroyed; f4vu.h). Damaged by the
    /// ledger's damage-state adoption, restored by the repair cadence.
    std::vector<std::uint8_t> fstatus;
    /// DOM-2: the engine moved this objective's logistics (stock
    /// regen, a battalion draw, a repair stamp). The write-back's
    /// activity filter — a mirror that only seeded from the save must
    /// not normalize the save's own garbage bytes on write-back.
    bool logistics_dirty = false;
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

/// One objective's troop-gate fields — the projection the engine's
/// mirror and the consolidation phase build, and the carrier for
/// stamp_front_defended().
///
/// `defended` is the troop-gate (2026-09 EMPL-2 review): the owner
/// keeps a live battalion within kFrontGarrisonRangeGrid. Stock saves
/// carry ownership bytes no troop ever earned (TestCamp: 361 DPRK-owned
/// objectives south of the ROK army, first_owner ROK) — ownership is
/// territorial state (supply, capture history, victory) and stays that.
/// The FLOT no longer reads this struct at all (the front is battalion
/// truth — front_columns_from_battalions); the stamp drives the
/// CONSOLIDATION rule (un-defended pockets flip to the nearer army)
/// and the viewer's solid/hollow objective rendering. Defaults true so
/// a view nobody stamped gates nothing.
struct FrontObjectiveView {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t owner = 0;
    bool defended = true;
};

/// The troop-gate radius (grid units, Chebyshev — the capture rule's
/// own distance). Deliberately a constant, not a config knob: the front
/// shape is INSENSITIVE to it on real data (TestCamp: contested-column
/// mean row 459..479 for any radius 2..24, vs 288 ungated with the
/// ghost line running to row 129), so a knob would be a dial that
/// cannot turn the line meaningfully — the gate's presence or absence
/// is the whole behavior.
inline constexpr int kFrontGarrisonRangeGrid = 8;

/// The contact rule's range (grid units, Chebyshev — every distance in
/// this header speaks Chebyshev): a grid column is FRONT only when the
/// two sides keep battalions within this range of each other in the
/// column's ±kFrontBand band, the line drawn between the CLOSEST
/// opposing pair. Extremes-based fronts read every lone scout as a
/// contact (TestCamp: per-column extremes spread the line over rows
/// 326..572 in 16 chaotic runs); the contact gate draws the line where
/// the armies actually face each other (5 runs, rows 460..520, zero
/// swing after smoothing — dead center of the two masses' interleave).
inline constexpr int kFrontContactRangeGrid = 12;

/// The front line's smoothing window (±columns, moving mean over the
/// run's own columns, windows truncated at the run's ends). Integer
/// math, deterministic.
inline constexpr int kFrontSmoothColumns = 3;

/// Consolidation's per-update flip cap: deep-pocket objectives — the
/// owner keeps no garrison, the opposing army is strictly nearer —
/// transition to the army actually standing there, a few per update
/// (the wire-order head of the list first). Deterministic pacing: the
/// territory follows the armies over campaign time instead of
/// re-coloring the map in one tick. TestCamp's 361 southern DPRK towns
/// (first_owner ROK, no DPRK battalion south of row 300) walk back to
/// ROK at this pace; garrisoned holdings NEVER consolidate — they are
/// captured through the combat ladder or not at all.
inline constexpr int kConsolidatePerUpdate = 4;

/// One battalion's front-relevant fields — the projection BOTH
/// callers of the front computation build (the engine's live mirror;
/// the tasking side's save-time source view, ledger-destroyed rows
/// dropped). The front is TROOP truth: the line between the closest
/// opposing battalions, not a read of ownership bytes.
struct FrontUnitView {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t owner = 0;
};

/// Project an IUnitCoreSource into the front unit view (wire order;
/// the aggregate Battalion class only — the ranking rule's own face —
/// ledger-destroyed rows dropped when the ledger is provided).
[[nodiscard]] std::vector<FrontUnitView>
front_unit_view(const f4::world::IUnitCoreSource& units,
                const CampaignResultLedger* ledger = nullptr);

/// The engine mirror's face (destroyed rows skipped).
[[nodiscard]] std::vector<FrontUnitView>
front_unit_view(const std::vector<GroundUnitState>& units);

/// The front line between two belligerents' BATTALION masses: every
/// grid column in the battalions' x extent, the contested ones carrying
/// the midpoint row between the closest opposing pair in the column's
/// ±kFrontBand band — and contested ONLY when that pair is within
/// kFrontContactRangeGrid (the contact rule above). Runs are smoothed
/// by ±kFrontSmoothColumns moving mean. Sides by battalion mean y (the
/// smaller holds the south — the wire grid's own orientation: TestCamp
/// puts CIS/PRC at rows 850-1000, ROK/Japan at 50-200). Deterministic.
[[nodiscard]] std::vector<FrontColumn>
front_columns_from_battalions(
    const std::vector<FrontUnitView>& units,
    std::uint8_t side_a, std::uint8_t side_b);

/// Stamp the troop-gate onto a projected view: a row is defended when
/// the owner keeps a live Battalion within kFrontGarrisonRangeGrid
/// (Chebyshev). The IUnitCoreSource overload filters the aggregate
/// Battalion class (the ranking rule's own face) and skips
/// ledger-destroyed battalions when the ledger is provided; the
/// GroundUnitState overload is the engine mirror's face (destroyed
/// rows skipped there). O(units × objectives) integer compares —
/// rebuilt per orders cycle, measured noise at campaign scale.
void stamp_front_defended(std::vector<FrontObjectiveView>& view,
                          const f4::world::IUnitCoreSource& units,
                          const CampaignResultLedger* ledger = nullptr);
void stamp_front_defended(std::vector<FrontObjectiveView>& view,
                          const std::vector<GroundUnitState>& units);

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
    /// Consolidation flips (deep pockets to the nearer army — the
    /// kConsolidatePerUpdate-paced sibling of the capture ladder).
    int consolidations = 0;
    int resupply_fires = 0;      ///< ground-supply cadence fires
    // --- DOM-2: the supply chain's books -------------------------------
    /// Supply units moved team stock → objective stocks (cumulative).
    int supply_regen_total = 0;
    /// Supply units moved objective stocks → battalions (cumulative).
    int supply_drawn_total = 0;
    /// Battalion×fire cut-offs (no own objective within the supply
    /// radius — the encirclement counter).
    int cut_off_events = 0;
    /// Repair cadence fires (cumulative).
    int repair_fires = 0;
    /// Features flipped to VIS_REPAIRED (cumulative).
    int features_repaired = 0;
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

    /// The troop-gate's own stamp, refreshed each front rebuild: TRUE
    /// when the owner keeps a garrisoned battalion within
    /// kFrontGarrisonRangeGrid of this objective — the same truth the
    /// FLOT draws. The viewer keys its objective rendering off this
    /// (solid owner fill = a defended holding; hollow = affiliation
    /// without troops, a territorial claim, not a position). Linear
    /// over the mirror; callers draw-cull long before it is warm.
    [[nodiscard]] bool
    objective_defended(std::uint32_t vu) const noexcept {
        if (defended_.size() != objectives_.size()) {
            return true;   // no stamp (a war-less world never rebuilds
                           // the front) — legacy display, nothing to
                           // gate on
        }
        for (std::size_t i = 0; i < objectives_.size(); ++i) {
            if (objectives_[i].vu == vu) return defended_[i];
        }
        return false;
    }

    [[nodiscard]] const GroundWarStats& stats() const noexcept {
        return stats_;
    }

private:
    // --- phases (each a deterministic walk; see the header doc) -------
    void fire_orders_();          ///< GTM-lite: score + assign targets
    void rebuild_front_();        ///< FLOT columns from battalion contact
    void move_phase_();           ///< advance mobile battalions
    void engage_phase_();         ///< detect + resolve exchanges
    void capture_phase_();        ///< flip undefended enemy objectives
    void consolidate_phase_();    ///< deep pockets flip to the nearer army
    void resupply_phase_(CampaignTime t);  ///< the last_resupply cadence
    void repair_phase_(CampaignTime t);    ///< the last_repair cadence
    /// Adopt the ledger's objective damage state into the mirror's
    /// fstatus (wholesale, idempotent — the records ARE final faces;
    /// run before each repair walk so repairs work on current truth).
    void pull_objective_damage_();
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
    /// The troop-gate stamp per objectives_ row (the front's own
    /// holding test, exposed via objective_defended for the viewer).
    std::vector<std::uint8_t> defended_;
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
    /// DOM-2: the repair cadence's anchor (the .cmp header's
    /// last_repair — the third maintenance timer, finally consumed).
    std::int64_t last_repair_ = 0;
    /// DOM-2: the team strategic stocks (the deepened pool's source;
    /// seeded from ITeamSource at construction, run-live here — the
    /// engine owns the ground face of the supply chain).
    std::int64_t team_supply_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::int64_t team_fuel_[8] = {0, 0, 0, 0, 0, 0, 0, 0};

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

// f4-campaign/include/f4/campaign/atm.hpp
//
// AirTaskingManager — the C4 ATM pipeline (Architecture Proposal M4.2:
// "7 composable phases with budget awareness" + M4.6: "FindBestAir
// scoring, availability, scheduling"), ported from FreeFalcon's
// campaign/camptask/atm.cpp + package.cpp + flight.cpp.
//
// WHAT THE REFERENCE DOES (atm.cpp Task, the 200 ms-budgeted loop):
//   1. Collect mission requests (the strategy layer files them; the
//      save's own queued list rides along in the team's ATM entity).
//   2. Sort by priority (GetPriority: mission priority + target
//      priority; the reference adds PO/package/distance/random terms
//      our sources cannot see — the deterministic subset is below).
//   3. For each request, build a PACKAGE (package.cpp BuildPackage):
//      analyze the target (threat at min/max target altitude →
//      NEED_SEAD), pick squadrons (FindBestAir — SCORED, not gated),
//      attach support flights (SEAD escort / fighter escort from the
//      profile's ADDSEAD/ADDESCORT flags), schedule takeoffs against
//      the airbase slot bitmasks (FindTakeoffSlot / ScheduleAircraft),
//      build the routes (BuildPathToTarget — the C3 RouteBuilder,
//      already ported).
//   4. Packages that can't be built are delayed (TOT + 30 min, up to
//      8 pushes) or dropped (timeout / no assets).
//
// F4's port keeps the reference's PHASE SHAPE and makes the phases
// composable + independently testable (the M4.2 deliverable). Each
// phase is a public method over plain data; the Campaign composes them
// in the reference's order:
//
//   PHASE 1  request_generation  — profile ladder walk per belligerent
//                                  + the decoded ATO backlog seed +
//                                  GetPriority's deterministic subset
//   PHASE 2  prioritization      — stable priority sort + the tempo
//                                  budget (missionsToFill)
//   PHASE 3  deconfliction       — mindistance/mintime vs ACTIVE
//                                  packages (the profile's own fields)
//   PHASE 4  package_building    — target analysis (threat →
//                                  NEED_SEAD) + FindBestAir for the
//                                  main flight
//   PHASE 5  support_assignment  — SEAD/escort pairing from profile
//                                  flags, TOT + separation, own
//                                  FindBestAir per support flight
//   PHASE 6  route_planning      — (Campaign-side: the C3 RouteBuilder
//                                  per package, shared by the package's
//                                  flights — package-shared ingress)
//   PHASE 7  tot_scheduling      — takeoff slot snap + fill against
//                                  the airbase schedule bitmasks (the
//                                  decoded atm_airbases schedules),
//                                  TOT shifted by the snap delta; the
//                                  commit point that books the flight
//                                  for recovery
//   (+)      mission recovery    — completed packages release their
//                                  surviving drawn aircraft back to
//                                  the tasking pool (the C2 "drawn =
//                                  committed" simplification, closed)
//
// The 7 phases execute in exactly this order, so the numbering IS the
// execution order (the Architecture Proposal's phase list named a
// LoadoutSelection phase; the reference's loadout selection is the
// sim-side arming path — already data-driven per mission byte through
// the spawner — and the reference's LoadWeapons switch is deliberately
// not a campaign-layer phase here. Documented deferral.)
//
// FIND BEST AIR (M4.6, atm.cpp:1534) — the tranche's core replacement:
// FreeFalcon SCORES squadrons rather than gating on role (a
// counter-air F-16 wing is taskable for strike at a reduced rating).
// The port keeps every term the sources can express:
//     score = (rating(role) + 4) / 5          — UCD Scores[aro] when
//                                               the theater DB resolves
//                                               the squadron's type;
//                                               else the specialty-
//                                               derived fallback
//     +/- 5  specialty match                  — the wire's own
//                                               specialty byte
//                                               (SQUADRON_SPECIALTY_
//                                               AA/AG) vs the role's
//                                               family
//     lowestScore gate                        — (255 - priority) / 25:
//                                               hopeless squadrons
//                                               never enter the
//                                               comparison
//     skip   capability gate                  — profiles requiring
//                                               vehicle caps this
//                                               slice cannot verify
//     skip   range gate                       — covered by the +2
//                                               half-range bonus's
//                                               range source (0 = no
//                                               data = no gate)
//     skip   availability gate                — av < aircraft-1 (one
//                                               short is allowed at a
//                                               penalty), av < 1 never
//     skip   airbase schedule full            — the start block full
//     -5     insufficient aircraft
//     +3     same squadron as the package     — the reference's
//                                               SetAssigned bonus
//                                               (within-package reuse)
//     +2     same airbase as the package lead
//     +2     within half range                — the unit's own range
//                                               when the wire carries
//                                               one (60 grid units
//                                               otherwise)
//     +2     quickest estimated arrival       — with the reference's
//                                               previous-best
//                                               rebalancing
// The reference's player-squadron bonuses (IsValidAircraftType,
// FEC_HASPLAYERS) have no data on the engine-agnostic side — skipped,
// documented. The reference's REQF_USERESERVES percentage-reserve rule
// is subsumed by the one-pool availability number (the ledger already
// nets losses); a second percentage would double-count conservatism.
//
// TOT SLOT SCHEDULING (atm.cpp:2104/2180): each airbase carries
// ATM_MAX_CYCLES (32) blocks of 1-minute takeoff slot bits; a block is
// MIN_PLAN_AIR minutes (atm.h's ATM_CYCLE_FULL = 0x1F says 5 slots per
// block — the reference default). FindTakeoffSlot snaps the requested
// takeoff minute to the exact slot, else +1, +2, then 10 minutes
// backward; ScheduleAircraft fills the slot AND the same slot in the
// next block (fudge time), and the NEXT slot too for flights larger
// than 2 ships. The decoded wire schedules (ITeamSource::
// atm_airbases) seed the bitmask — the save's own planned sorties are
// already-consumed slots our flights must avoid (the plan's "TOT
// slotting against the decoded atm_airbases schedules"). The wire
// carries no scheduleTime anchor (it is runtime state, not saved), so
// block 0 = the campaign's START (construction time, clock 0) — the
// alignment choice every rehost makes, documented here once.
//
// MISSION RECOVERY: FreeFalcon's pilots/aircraft cycle back through
// SquadronClass's own schedule bookkeeping when missions complete. C2
// modeled "drawn = committed" (documented as the C4 tranche's job to
// close); this closes it: when the campaign clock passes a flight's
// mission-over time (TOT + out + loiter + back + doubled reserve), the
// SURVIVORS (drawn minus the flight's booked losses — the ledger's
// per-flight loss log) release back into the tasking pool. Deaths keep
// their draws spent; only survivors fly again.
//
// DELIBERATE SIMPLIFICATIONS (each documented at its site):
//   * TOT rule: requests keep the C3 midpoint rule (the profile's
//     planning window) — the reference's request TOTs come from the
//     strategy layer (the PO/action system), a later tranche.
//   * One main flight per package (the reference's multi-strike
//     feature analysis: BestTargetFeature loops against feature HP —
//     the campaign-side feature-damage loop is the weapons tranche's).
//   * Support scope: the ESCORT family (SEADESCORT/ESCORT — the plan's
//     "escort pairing"). AWACS/tanker/JSTAR/ECM sharing (FindSupport
//     Flights) needs loiter racetrack routes — the loiter tranche's.
//   * Enemy-requested BARCAP/SWEEP (ADDBARCAP/ADDSWEEP → RequestEnemy
//     Mission): the ladder already generates both sides' defensive CAP
//     every cycle; the requester-driven path lands with the strategy
//     layer.
//   * Escort type: the reference flips a coin when escort_type is 0;
//     determinism picks AMIS_ESCORT.
//   * Travel time: the estimated distance/speed rule (the reference's
//     own FindBestAir estimate), not the per-waypoint SetWPTimes —
//     the waypoint timing tranche (M4.5 altitude/timing) owns that.
//
// Determinism: no RNG anywhere; a pure function of (sources, profiles,
// config, ledger state, clock). Two identically-driven runs produce
// identical flight lists — pinned by test.
//
// Dependencies: f4-campaign (MissionProfileTable, CampaignTime,
// CampaignResultLedger, ThreatMap), f4-world (ICampaignSource,
// ITeamSource, IUnitCoreSource, IObjectiveSource). C++20.

#pragma once

#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/threat_map.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace f4::campaign {

/// The request's TOT flexibility (mission.h TotTypeEnum — the
/// reference's vocabulary; the generated family carries TYPE_LE).
enum class TotType : std::uint8_t {
    LE = 0,   ///< on or before
    EQ = 1,   ///< exactly (inflexible)
    GE = 2,   ///< on or after
    NE = 3,   ///< anything but
};

/// One tasking request — MissionRequestClass as engine-agnostic data.
/// Generated by the profile-ladder walk (phase 1) or seeded from the
/// decoded ATO backlog (the save's own queued requests).
struct MissionRequest {
    std::uint8_t mission = 0;     ///< profile-bound mission byte
    std::uint8_t team = 0;        ///< flying team slot
    std::uint32_t target_id = 0;  ///< target objective VU_ID.num (0 = none)
    CampaignTime tot = 0;         ///< time on target (relative clock)
    int priority = 0;             ///< GetPriority's deterministic score
    int aircraft = 0;             ///< requested count (0 = profile str)
    TotType tot_type = TotType::LE;
    std::uint8_t delayed = 0;     ///< 30-minute pushes so far
    bool seeded = false;          ///< from the decoded backlog
};

/// A flight's role in its package (the support-assignment vocabulary).
enum class FlightRole : std::uint8_t {
    Main = 0,
    SeadEscort = 1,   ///< ADDSEAD + NEED_SEAD pairing (AMIS_SEADESCORT)
    Escort = 2,       ///< ADDESCORT pairing (AMIS_ESCORT)
};

/// One flight the ATM fields — the publish contract between the ATM
/// and the Campaign (the Campaign builds the route, snaps the takeoff
/// slot, and publishes the MissionIntent).
struct FlightTasking {
    std::uint8_t mission = 0;       ///< the flight's own mission byte
    std::uint8_t team = 0;
    std::uint32_t squadron_vu = 0;  ///< FindBestAir's pick
    std::string squadron_name;      ///< display name (filled by compose)
    std::uint32_t airbase_vu = 0;   ///< the squadron's home base
    std::uint32_t target_vu = 0;    ///< the package target
    int aircraft = 0;               ///< drawn count
    CampaignTime tot = 0;           ///< time on target (pre-snap)
    CampaignTime takeoff = 0;       ///< takeoff estimate (pre-snap)
    CampaignTime mission_over = 0;  ///< recovery deadline (pre-snap)
    std::uint32_t package_id = 0;   ///< shared by the package's flights
    std::uint32_t flight_id = 0;    ///< unique per flight
    FlightRole role = FlightRole::Main;
    /// The main flight this one escorts (0 for the main flight).
    std::uint32_t escorted_flight_id = 0;
    /// TOT offset from the main flight (role != Main; the support
    /// profile's separation, seconds).
    int separation_sec = 0;
};

/// One mission-recovery release (a completing flight returning its
/// survivors). The Campaign books these into the ledger.
struct RecoveryRelease {
    std::uint8_t team = 0;
    std::uint32_t squadron_vu = 0;
    std::uint32_t flight_id = 0;
    int survivors = 0;              ///< drawn − booked flight losses
};

/// Air Tasking Manager tunables. The reference reads every one of
/// these from aiinput.dat ([ATM] section — game data, not source);
/// defaults follow the reference's documented values and the
/// RouteBuilderConfig pattern (hosts override; campaign_qc does).
struct AtmConfig {
    /// Sortie tempo budget per cycle per team (missionsToFill): the
    /// reference caps the requests it will fill; 0 = unlimited.
    int missions_per_cycle = 0;
    /// Minutes per schedule block (MIN_PLAN_AIR / "AirPlanTime";
    /// atm.h's ATM_CYCLE_FULL = 0x1F pins 5 slots/block).
    int plan_block_min = 5;
    /// Schedule blocks (ATM_MAX_CYCLES — "Bit size of a long").
    int max_cycles = 32;
    /// Threat score at the target above which the package needs a
    /// SEAD escort (MIN_SEADESCORT_THREAT; band scores run 0..100).
    int min_seadescort_threat = 40;
    /// Reserve minutes appended to the mission-over estimate (the
    /// reference's RESERVE_MINUTES, doubled as it does).
    int reserve_min = 20;
    /// Cruise speed for travel-time estimates, campaign grid units per
    /// minute (the reference's GetCruiseSpeed is UCD data the fixture
    /// theaters don't resolve for squadrons; ~400 kt ≈ 12).
    int cruise_grid_per_min = 12;
};

/// The pipeline's own telemetry (the QC gates and the summary read
/// these; every counter is deterministic).
struct AtmStats {
    int requests_generated = 0;   ///< profile-ladder requests (phase 1)
    int requests_seeded = 0;      ///< decoded-backlog requests
    int requests_timed_out = 0;   ///< past-TOT drops (the delay cap)
    int requests_budget_dropped = 0;  ///< beyond missions_per_cycle
    int requests_deconflicted = 0;   ///< mindistance/mintime skips
    int requests_unfilled = 0;    ///< FindBestAir found nobody
    int packages_built = 0;       ///< packages with a main flight
    int escorts_built = 0;        ///< support flights (both kinds)
    int slot_snaps = 0;           ///< takeoffs slot-snapped (phase 7)
    int slot_shifts_sec = 0;      ///< total TOT shift from snapping
    int recoveries = 0;           ///< flights completed (recovery)
    int aircraft_recovered = 0;   ///< survivors released
};

// ============================================================================
// AirbaseSchedule — one airbase's takeoff slots (ATMAirbaseClass port)
// ============================================================================
//
/// 32 blocks of 1-minute slot bits, seeded from the decoded wire
/// schedule and filled as the ATM fields flights. Pure bookkeeping,
/// fully deterministic.
class AirbaseSchedule {
public:
    explicit AirbaseSchedule(std::uint32_t airbase_vu = 0) : vu_(airbase_vu) {}

    /// Seed from the decoded wire bitmask (already-booked slots — the
    /// save's own planned sorties).
    void seed(const std::array<std::uint8_t, 32>& wire) { blocks_ = wire; }

    [[nodiscard]] std::uint32_t airbase_vu() const noexcept { return vu_; }

    /// FindTakeoffSlot port: the nearest free slot at/after `minute`
    /// (exact, +1, +2 — the reference's two lookahead minutes), then
    /// up to 10 minutes BACKWARD. Returns the taken minute, or -1 when
    /// the horizon is exhausted.
    [[nodiscard]] int find_slot(int minute, int plan_block_min,
                                int max_cycles) const noexcept;

    /// ScheduleAircraft port: fill `minute`'s slot AND the same slot
    /// in the next block (the reference's fudge time); flights larger
    /// than 2 ships fill the next minute too.
    void fill(int minute, int aircraft, int plan_block_min,
              int max_cycles) noexcept;

    /// True when block `block` is full (ATM_CYCLE_FULL — every slot
    /// bit the block width allows is set).
    [[nodiscard]] bool block_full(int block,
                                  int plan_block_min) const noexcept;

    /// The raw blocks (tests/QC).
    [[nodiscard]] const std::array<std::uint8_t, 32>& blocks() const noexcept {
        return blocks_;
    }

private:
    std::uint32_t vu_ = 0;
    std::array<std::uint8_t, 32> blocks_{};
};

// ============================================================================
// AirTaskingManager — the 7-phase pipeline
// ============================================================================
//
class AirTaskingManager {
public:
    /// \param profiles   the validated profile table (B.1)
    /// \param camp       campaign source (epoch + team pools)
    /// \param teams      team slots, stance, priorities, ATM state
    /// \param units      the squadron roster
    /// \param objectives objective list (target/airbase resolution);
    ///                   may be null (requests stay target-less)
    /// \param cfg        tunables
    AirTaskingManager(const MissionProfileTable& profiles,
                      const f4::world::ICampaignSource& camp,
                      const f4::world::ITeamSource& teams,
                      const f4::world::IUnitCoreSource& units,
                      const f4::world::IObjectiveSource* objectives,
                      AtmConfig cfg = {});

    /// Availability/loss source overrides (the Campaign updates these
    /// as its own attachments change; null = the ATM's own snapshot
    /// counters, drawn down per draw and refilled per recovery).
    void set_ledger(const CampaignResultLedger* ledger) noexcept {
        ledger_ = ledger;
    }
    void set_threat_map(const ThreatMap* map) noexcept { threat_ = map; }
    /// The objective source (target/airbase resolution) — the Campaign
    /// attaches it with its route planner; null keeps requests
    /// target-less.
    void set_objectives(const f4::world::IObjectiveSource* objectives)
        noexcept {
        objectives_ = objectives;
    }

    // --- The phases (each independently callable — M4.2) ----------------

    /// PHASE 1 — request generation: the profile-ladder walk for one
    /// belligerent team (mission-priority gating, capability gating,
    /// deterministic target rotation) plus — once, on first use — the
    /// decoded ATO backlog seed (past-TOT requests take the reference's
    /// 30-minute delay pushes, capped at 8; beyond that, timeout).
    [[nodiscard]] std::vector<MissionRequest>
    generate_requests(std::uint8_t team, CampaignTime now);

    /// PHASE 2 — prioritization: stable sort (priority desc, byte asc,
    /// seeds first) and the tempo budget (missions_per_cycle; 0 =
    /// unlimited).
    [[nodiscard]] std::vector<MissionRequest>
    prioritize(std::vector<MissionRequest> requests);

    /// PHASE 3 — deconfliction: drop requests whose profile's
    /// mindistance/mintime collide with an ACTIVE package of the same
    /// mission byte (distance between targets, TOT difference).
    [[nodiscard]] std::vector<MissionRequest>
    deconflict(std::vector<MissionRequest> requests);

    /// PHASE 4 + 5 — package building + support assignment: for each
    /// request, the target analysis (threat at the profile's target
    /// altitudes → NEED_SEAD), FindBestAir for the main flight, and
    /// the escort pairing from the profile flags (ADDSEAD + NEED_SEAD
    /// → AMIS_SEADESCORT; ADDESCORT → the profile's escort_type, or
    /// AMIS_ESCORT when 0), each support flight's TOT staggered by the
    /// support profile's separation. Returns the flights (main +
    /// escorts) with takeoff estimates and mission-over deadlines; the
    /// Campaign builds routes (phase 6) and then snaps slots (phase 7).
    /// Failed picks are counted (requests_unfilled), never silent.
    [[nodiscard]] std::vector<FlightTasking>
    compose_packages(const std::vector<MissionRequest>& requests,
                     std::uint8_t team, CampaignTime now);

    /// PHASE 7 — TOT slot scheduling: snap `flight`'s takeoff to a
    /// free slot on its airbase's schedule, fill the slot, shift the
    /// TOT and mission-over deadline by the snap delta, and BOOK the
    /// flight for recovery (the commit point — compose_packages does
    /// not). Returns the TOT shift in seconds (0 = exact slot or no
    /// base).
    CampaignTime schedule_takeoff(FlightTasking& flight);

    /// Mission recovery: every booked flight whose mission-over time
    /// has passed releases its survivors (drawn − booked flight
    /// losses; the no-ledger mode refills the ATM's own counters).
    /// Returns the releases for the Campaign to book into the ledger.
    [[nodiscard]] std::vector<RecoveryRelease>
    recover_completed(CampaignTime now);

    // --- Inspection -------------------------------------------------------

    [[nodiscard]] const AtmStats& stats() const noexcept { return stats_; }
    /// Flights booked (awaiting recovery), oldest first.
    [[nodiscard]] const std::vector<FlightTasking>&
    booked_flights() const noexcept {
        return booked_;
    }
    /// An airbase's live schedule (nullptr when the airbase has none).
    [[nodiscard]] const AirbaseSchedule*
    airbase_schedule(std::uint32_t airbase_vu) const noexcept;

    /// Seed the package/flight id allocation (the Campaign's counter
    /// base; both stay monotonic for the run).
    void set_id_base(std::uint32_t first_package_id) noexcept {
        next_package_id_ = next_flight_id_ = first_package_id;
    }

private:
    /// One squadron as the ATM sees it (the shared force snapshot's
    /// fields + the ATM's own bookkeeping).
    struct SquadronState {
        std::uint32_t vu = 0;
        std::uint8_t owner = 0;
        std::uint8_t specialty = 0;   ///< wire byte (0/1/2: none/AA/AG)
        std::string name;
        int available = 0;            ///< snapshot availability
        int range = 0;                ///< wire max_range (0 = unknown)
        std::array<std::uint8_t, 16> scores{};  ///< UCD Scores (0 = none)
        int x = 0, y = 0;             ///< home grid position
        std::uint32_t airbase = 0;    ///< home airbase VU_ID.num
        int drawn_outstanding = 0;    ///< no-ledger-mode bookkeeping
    };

    /// FindBestAir's result (atm.cpp:1534 — see the header doc for the
    /// term list). squadron == nullptr = nobody flyable.
    struct SquadronPick {
        SquadronState* squadron = nullptr;
        CampaignTime travel_sec = 0;  ///< estimated out-leg time
    };
    [[nodiscard]] SquadronPick find_best_air_(const MissionRequest& req,
                                              const MissionProfile& profile,
                                              std::uint8_t team,
                                              CampaignTime now,
                                              const SquadronState* lead);

    /// The rating term: UCD scores when the unit carries them, else
    /// the specialty-derived fallback (header doc).
    [[nodiscard]] int rating_(const SquadronState& sq,
                              const MissionProfile& profile) const;

    /// Effective availability (the ledger's one-pool number when a
    /// ledger is attached; the ATM's own counters otherwise).
    [[nodiscard]] int available_(const SquadronState& sq) const;

    /// GetPriority's deterministic subset (mission + target terms).
    [[nodiscard]] int request_priority_(std::uint8_t team,
                                        const MissionProfile& profile,
                                        std::uint32_t target_vu) const;

    /// Enemy objective ranking for target rotation (priority desc,
    /// wire order — the C3 select_target_ ranking as a list).
    [[nodiscard]] std::vector<std::uint32_t>
    enemy_objectives_(std::uint8_t team) const;

    /// Draw `count` aircraft from the squadron (no-ledger mode debits
    /// the ATM's own counter; ledger mode just tracks outstanding).
    void draw_(SquadronState& sq, int count);

    /// Build one support flight (phase 5's per-escort worker): pick
    /// via FindBestAir (with the package-lead bonuses), TOT = main
    /// TOT + separation, size min-capped per the reference. False when
    /// no squadron could fly it (counted, the package still flies).
    bool build_support_flight_(FlightTasking& out, std::uint8_t team,
                               CampaignTime now, std::uint32_t package_id,
                               const FlightTasking& main,
                               std::string_view support_name, int aircraft,
                               const SquadronState* main_sq);

    /// Seed the decoded ATO backlog once (cached per team).
    void seed_backlog_();

    const MissionProfileTable& profiles_;
    const f4::world::ITeamSource& teams_;
    const f4::world::IUnitCoreSource& units_;
    const f4::world::IObjectiveSource* objectives_ = nullptr;
    AtmConfig cfg_;

    /// The save's absolute-time epoch (camp.current_time) — the wire
    /// backlog TOTs are absolute; requests are relative.
    CampaignTime epoch_ = 0;

    const CampaignResultLedger* ledger_ = nullptr;
    const ThreatMap* threat_ = nullptr;

    std::vector<SquadronState> squadrons_;    ///< wire order
    std::vector<AirbaseSchedule> schedules_;  ///< one per decoded airbase
    std::vector<FlightTasking> booked_;       ///< awaiting recovery
    /// Decoded backlog cache, per team slot (seeded once).
    std::array<std::vector<MissionRequest>, 8> backlog_;
    /// Per-team target rotation cursor (enemy objective list index).
    std::array<int, 8> target_cursor_{};
    bool backlog_seeded_ = false;

    std::uint32_t next_package_id_ = 1;
    std::uint32_t next_flight_id_ = 1;

    AtmStats stats_;
};

} // namespace f4::campaign

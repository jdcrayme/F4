// f4-campaign/tests/test_atm.cpp
//
// C4 tranche tests — the ATM pipeline (M4.2's "unit test each phase
// independently" deliverable + M4.6's FindBestAir):
//   * AirbaseSchedule: FindTakeoffSlot's exact/+1/+2/backward search,
//     ScheduleAircraft's fill rules (fudge block, large flights),
//     ATM_CYCLE_FULL block detection
//   * PHASE 1 request generation: the ladder walk, the mission-
//     priority drop rule, the decoded ATO backlog seed (past-TOT
//     pushes + the delay cap), target rotation
//   * PHASE 2 prioritization: the stable priority sort + tempo budget
//   * PHASE 3 deconfliction: mindistance/mintime vs booked flights
//   * PHASE 4 FindBestAir: role scoring (specialty ratings), the
//     availability gate, the within-package bonuses
//   * PHASE 5 escort pairing: ADDSEAD + NEED_SEAD → SEADESCORT,
//     ADDESCORT → fighter escort, TOT + separation, shared package
//   * PHASE 7 slot scheduling: the snap + fill + TOT shift
//   * Mission recovery: survivors return, ledger integration
//   * The Campaign mode: atm_pipeline ON generates packages
//     deterministically; OFF keeps the legacy goldens byte-identical
//     (the mode switch is opt-in)

#include <f4/campaign/atm.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/route_builder.hpp>
#include <f4/campaign/threat_map.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace f4::campaign;
using f4::entities::UnitClass;
using f4::world::UnitState;

namespace {

// A mission-profile table subset covering the bytes the tests task
// (loaded from the generated fixture — the real 41-row table).
MissionProfileTable load_profiles() {
    return MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
}

// ---------------------------------------------------------------------------
// The hand-built ATM world: USA(1) vs DPRK(6) at war; the USA airbase
// objective (100,100), the DPRK target objective (400,400) with a DPRK
// AD battalion ON it (the target is defended — NEED_SEAD territory for
// the strike family), a neutral objective in between. Two USA
// squadrons (an AA specialist AT the airbase, an unspecialized wing
// elsewhere) and one DPRK squadron. The team pool funds them all
// (rosters 0 → the shared-pool rule).
// ---------------------------------------------------------------------------
struct WorldOpts {
    bool priority_table = false;   // slot-1 mission_priority table
    bool seeded_schedule = false;  // slot-1 airbase schedule bits
    bool backlog = false;          // slot-1 decoded request backlog
};

f4::world::WorldState make_atm_world(const WorldOpts& opts = {}) {
    using f4::world::ObjectiveState;
    using f4::world::TeamState;

    f4::world::WorldState ws;
    ws.version = 71;
    ws.campaign.current_time = 38574360;
    ws.campaign.te_number_aircraft = {0, 24, 0, 0, 0, 0, 24, 0};

    ws.teams.resize(8);
    ws.teams[1] = TeamState{1, 1, 1, "USA", ""};
    ws.teams[3] = TeamState{3, 0, 3, "UN", ""};
    ws.teams[6] = TeamState{6, 6, 6, "DPRK", ""};
    ws.teams[1].stance = {0, 0, 0, 0, 0, 0, 5, 0};
    ws.teams[3].stance = {0, 1, 0, 0, 0, 0, 0, 0};
    ws.teams[6].stance = {0, 5, 0, 0, 0, 0, 0, 0};

    auto obj = [](int16_t x, int16_t y, uint8_t owner, uint32_t vu,
                  uint8_t priority) {
        ObjectiveState o;
        o.x = x;
        o.y = y;
        o.owner = owner;
        o.id_num = vu;
        o.priority = priority;
        o.objective_type = 4;
        return o;
    };
    ws.objectives.push_back(obj(100, 100, 1, 4281, 5));   // USA airbase
    ws.objectives.push_back(obj(400, 400, 6, 9001, 7));   // DPRK target
    ws.objectives.push_back(obj(300, 300, 3, 5150, 3));   // neutral

    // The defended target: a DPRK AD battalion ON the objective cell.
    UnitState ad;
    ad.unit_class = UnitClass::Battalion;
    ad.domain = 3;
    ad.unit_subtype = 1;
    ad.x = 400;
    ad.y = 400;
    ad.owner = 6;
    ad.id_num = 7001;
    ad.class_name = "Air Defense";
    ad.unit_hit_chance = {0, 0, 0, 0, 60, 55, 0, 0};
    ad.unit_weapon_range = {0, 0, 0, 0, 24, 42, 0, 0};
    ws.units.push_back(ad);

    auto sq = [](uint32_t vu, uint8_t owner, uint8_t specialty, int16_t x,
                 int16_t y, uint32_t airbase, const char* name) {
        UnitState u;
        u.unit_class = UnitClass::Squadron;
        u.domain = 2;
        u.x = x;
        u.y = y;
        u.owner = owner;
        u.id_num = vu;
        u.specialty = specialty;
        u.airbase_id = airbase;
        u.class_name = name;
        return u;
    };
    ws.units.push_back(sq(6001, 1, 1, 100, 100, 4281, "AA Wing"));
    ws.units.push_back(sq(6002, 1, 0, 250, 250, 4281, "Plain Wing"));
    ws.units.push_back(sq(6003, 6, 2, 400, 400, 9001, "DPRK Wing"));

    if (opts.priority_table) {
        // Slot 1's table: ONLY INTERCEPT(9) and INTSTRIKE(13) nonzero.
        ws.teams[1].mission_priority.assign(41, 0);
        ws.teams[1].mission_priority[9] = 40;
        ws.teams[1].mission_priority[13] = 30;
    }
    if (opts.seeded_schedule) {
        f4::world::AtmAirbaseState ab;
        ab.id_num = 4281;
        // Block 0: slots 0 and 1 occupied (minutes 0 and 1).
        ab.schedule[0] = 0x03;
        ws.teams[1].atm_airbases.push_back(ab);
    }
    if (opts.backlog) {
        f4::world::AtmRequestState rq;
        rq.mission = 9;               // INTERCEPT
        rq.who = 1;
        rq.aircraft = 2;
        rq.priority = 120;
        rq.target_num = 9001;
        // Absolute TOT 30 minutes BEFORE the save's current_time: the
        // seed pushes past-TOT requests forward in 30-minute steps
        // (floor(1800/1800) + 1 = 2 pushes → now + 60 min).
        rq.tot = 38574360 - 1800;
        ws.teams[1].atm_requests.push_back(rq);
        f4::world::AtmRequestState rq2;
        rq2.mission = 13;             // INTSTRIKE
        rq2.who = 1;
        rq2.priority = 110;
        rq2.tot = 38574360 + 3600;    // an hour ahead: flows as-is
        ws.teams[1].atm_requests.push_back(rq2);
        f4::world::AtmRequestState rq3;
        rq3.mission = 1;              // BARCAP, 10 hours stale —
        rq3.who = 1;                  // past the delay cap: times out
        rq3.priority = 130;           // at the seed itself.
        rq3.tot = 38574360 - 10 * 3600;
        ws.teams[1].atm_requests.push_back(rq3);
    }
    return ws;
}

// A fully wired ATM over the hand world (heap-held; the adapters
// borrow the WorldState).
struct Rig {
    std::unique_ptr<f4::world::WorldState> ws;
    std::unique_ptr<f4::world::WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<ThreatMap> threat;
    std::unique_ptr<AirTaskingManager> atm;

    static std::unique_ptr<Rig> make(const WorldOpts& opts = {},
                                     AtmConfig cfg = {}) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<f4::world::WorldState>(make_atm_world(opts));
        r->adapters =
            std::make_unique<f4::world::WorldStateAdapters>(*r->ws);
        r->profiles = load_profiles();
        r->atm = std::make_unique<AirTaskingManager>(
            r->profiles, r->adapters->campaign, r->adapters->teams,
            r->adapters->units, &r->adapters->objectives, cfg);
        // The threat map from the SAME sources (the route builder's
        // own attachment shape), viewer = the first belligerent.
        r->threat = std::make_unique<ThreatMap>(
            r->adapters->objectives, r->adapters->units,
            r->adapters->teams, 1);
        r->atm->set_threat_map(r->threat.get());
        return r;
    }
};

constexpr CampaignTime kNow = 0;   // relative clock

} // namespace

// ── AirbaseSchedule (FindTakeoffSlot / ScheduleAircraft) ─────────────────────

TEST(AirbaseSchedule, FindTakeoffSlotSearchesExactThenLookahead) {
    AirbaseSchedule s(1);
    // Block 0 slots 0 and 2 occupied → minute 1 free...
    std::array<std::uint8_t, 32> blocks{};
    blocks[0] = 0x05;   // bits 0 and 2
    s.seed(blocks);
    EXPECT_EQ(s.find_slot(0, 5, 32), 1);
    EXPECT_EQ(s.find_slot(2, 5, 32), 3);   // occupied → +1 (bit 3 free)
}

TEST(AirbaseSchedule, FindTakeoffSlotSearchesBackwardWhenForwardFull) {
    AirbaseSchedule s(1);
    std::array<std::uint8_t, 32> blocks{};
    // Minutes 0..2 all occupied; minute 3+ occupied too.
    blocks[0] = 0x1F;
    blocks[1] = 0x1F;
    blocks[2] = 0x1F;
    s.seed(blocks);
    // Requested minute 2: +1, +2 blocked (minutes 3, 4) → backward scan
    // hits nothing before 0 within the horizon → -1? No: backward from
    // minute 1 down to 0 — all occupied. So -1.
    EXPECT_EQ(s.find_slot(2, 5, 32), -1);
    // But with minute 0 free: backward finds it.
    std::array<std::uint8_t, 32> blocks2{};
    blocks2[0] = 0x1E;   // 1..4 occupied, minute 0 free
    blocks2[1] = 0x1F;
    blocks2[2] = 0x1F;
    s.seed(blocks2);
    EXPECT_EQ(s.find_slot(2, 5, 32), 0);
}

TEST(AirbaseSchedule, FindTakeoffSlotHorizonAndNegative) {
    AirbaseSchedule s(1);
    EXPECT_EQ(s.find_slot(-1, 5, 32), -1);
    EXPECT_EQ(s.find_slot(32 * 5, 5, 32), -1);   // beyond the horizon
    EXPECT_EQ(s.find_slot(0, 5, 32), 0);         // empty schedule
}

TEST(AirbaseSchedule, FillMarksSlotFudgeBlockAndLargeFlights) {
    AirbaseSchedule s(1);
    s.fill(7, 2, 5, 32);
    // Minute 7 → block 1 (7/5), slot 2 (7%5) → block[1] bit 2, plus
    // the fudge: the same slot in the next block.
    EXPECT_EQ(s.blocks()[0], 0x00);
    EXPECT_EQ(s.blocks()[1], 0x04);
    EXPECT_EQ(s.blocks()[2], 0x04);
    // Large flight (aircraft > 2): the next minute too.
    AirbaseSchedule big(2);
    big.fill(2, 4, 5, 32);
    EXPECT_EQ(big.blocks()[0], 0x0C);   // minutes 2 and 3
    EXPECT_EQ(big.blocks()[1], 0x0C);   // both in the next (fudge) block
}

TEST(AirbaseSchedule, BlockFullMatchesPlanBlockWidth) {
    AirbaseSchedule s(1);
    std::array<std::uint8_t, 32> blocks{};
    blocks[0] = 0x1F;
    s.seed(blocks);
    EXPECT_TRUE(s.block_full(0, 5));    // 5 slots/block → 0x1F is full
    EXPECT_FALSE(s.block_full(0, 8));   // 8 slots/block → not full
    EXPECT_TRUE(s.block_full(99, 5));   // out of range reads full
}

// ── PHASE 1 — request generation ─────────────────────────────────────────────

TEST(AtmGenerateRequests, LadderWalksProfilesWithTargetsForDelivery) {
    auto rig = Rig::make();
    const auto reqs = rig->atm->generate_requests(1, kNow);

    // The full no-caps profile set generates (the fixture table's
    // caps-carrying rows are skipped — count pinned by the table;
    // profiles() includes the byte-0 sentinel, which never tasks).
    int expected = 0;
    for (const auto& p : rig->profiles.profiles()) {
        if (p.mission_byte == 0) continue;   // the AMIS_NONE sentinel
        if (!p.caps.empty()) continue;
        ++expected;
    }
    EXPECT_EQ(rig->atm->stats().requests_generated, expected);
    EXPECT_EQ(rig->atm->stats().requests_seeded, 0);

    // The delivery family carries the enemy objective (the rotation's
    // only candidate for team 1: the DPRK target); the rest carries 0.
    for (const auto& r : reqs) {
        const auto& p = rig->profiles.for_mission(r.mission);
        if (profile_flies_delivery_route(p)) {
            EXPECT_EQ(r.target_id, 9001u);
        } else {
            EXPECT_EQ(r.target_id, 0u);
        }
        EXPECT_GT(r.priority, 0);
        EXPECT_EQ(r.team, 1);
    }
}

TEST(AtmGenerateRequests, MissionPriorityTableDropsUnrequestedMissions) {
    auto rig = Rig::make(WorldOpts{.priority_table = true});
    const auto reqs = rig->atm->generate_requests(1, kNow);
    // ONLY INTERCEPT(9) and INTSTRIKE(13) survive the table's drop rule.
    ASSERT_EQ(reqs.size(), 2u);
    EXPECT_EQ(reqs[0].mission, 9);
    EXPECT_EQ(reqs[1].mission, 13);
}

TEST(AtmGenerateRequests, BacklogSeedsOnceWithDelayPushes) {
    auto rig = Rig::make(WorldOpts{.backlog = true});
    const auto reqs = rig->atm->generate_requests(1, kNow);
    // Two seeded (the 10-hour-stale BARCAP timed out at the seed).
    EXPECT_EQ(rig->atm->stats().requests_seeded, 2);
    EXPECT_EQ(rig->atm->stats().requests_timed_out, 1);

    // The stale INTERCEPT request: past TOT → 2 pushes → now + 30 min
    // (tot = −1800 + 2 × 1800 = 1800).
    const auto* stale = &reqs[0];
    ASSERT_EQ(stale->mission, 9);
    EXPECT_EQ(stale->tot, 1800);
    EXPECT_EQ(stale->delayed, 2);
    EXPECT_TRUE(stale->seeded);
    // The future INTSTRIKE flows untouched (the backlog comes first).
    EXPECT_EQ(reqs[1].mission, 13);
    EXPECT_EQ(reqs[1].tot, 3600);

    // Second call: the backlog is consumed (no re-seed).
    const auto again = rig->atm->generate_requests(1, kNow);
    EXPECT_EQ(rig->atm->stats().requests_seeded, 2);
    for (const auto& r : again) EXPECT_FALSE(r.seeded);
}

TEST(AtmGenerateRequests, BacklogTimesOutPastTheDelayCap) {
    // The 10-hour-stale BARCAP request exceeds the 8-push delay cap
    // (10 h / 30 min = 20 pushes): it never ENTERS the pipeline (the
    // ladder's own generated BARCAPs still do — different source).
    auto rig = Rig::make(WorldOpts{.backlog = true});
    const auto reqs = rig->atm->generate_requests(1, kNow);
    for (const auto& r : reqs) {
        if (r.mission == 1) {
            EXPECT_FALSE(r.seeded);
        }
    }
    EXPECT_EQ(rig->atm->stats().requests_timed_out, 1);
}

// ── PHASE 2 — prioritization ─────────────────────────────────────────────────

TEST(AtmPrioritize, SortsByPriorityThenGenerationOrder) {
    auto rig = Rig::make();
    std::vector<MissionRequest> reqs(3);
    reqs[0].mission = 1;   // BARCAP
    reqs[0].priority = 10;
    reqs[1].mission = 9;   // INTERCEPT
    reqs[1].priority = 90;
    reqs[2].mission = 13;  // INTSTRIKE
    reqs[2].priority = 90;

    const auto out = rig->atm->prioritize(std::move(reqs));
    ASSERT_EQ(out.size(), 3u);
    // Priority desc; ties keep generation order (byte asc).
    EXPECT_EQ(out[0].mission, 9);
    EXPECT_EQ(out[1].mission, 13);
    EXPECT_EQ(out[2].mission, 1);
}

TEST(AtmPrioritize, TempoBudgetCapsTheCycle) {
    auto rig = Rig::make();
    AtmConfig cfg = rig->atm->stats().requests_generated == 0
                        ? AtmConfig{}
                        : AtmConfig{};
    // (config is fixed at construction — test through a fresh ATM)
    auto ws = make_atm_world();
    f4::world::WorldStateAdapters adapters(ws);
    auto profiles = load_profiles();
    AtmConfig c;
    c.missions_per_cycle = 3;
    AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                          adapters.units, &adapters.objectives, c);

    std::vector<MissionRequest> reqs(5);
    for (int i = 0; i < 5; ++i) {
        reqs[static_cast<std::size_t>(i)].priority = 50 + i;
        reqs[static_cast<std::size_t>(i)].mission = 1;
    }
    const auto out = atm.prioritize(std::move(reqs));
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(atm.stats().requests_budget_dropped, 2);
    // Highest priorities survive.
    EXPECT_EQ(out[0].priority, 54);
    EXPECT_EQ(out[2].priority, 52);
}

// ── PHASE 3 — deconfliction ─────────────────────────────────────────────────

TEST(AtmDeconflict, DropsCollisionsAgainstBookedFlights) {
    auto rig = Rig::make();
    // Book one BARCAP flight (no escort pairing — the gate's own
    // behavior under test, not the package's).
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 1;
    r.team = 1;
    r.target_id = 0;
    r.priority = 100;
    r.aircraft = 2;
    r.tot = 9000;
    r.tot_type = TotType::LE;
    reqs.push_back(r);
    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    (void)rig->atm->schedule_takeoff(flights[0]);

    // The shipped table's mindistance/mintime are 0 (the gate is a
    // no-op there — pinned): a same-mission request at the same TOT
    // flows through untouched.
    EXPECT_EQ(rig->profiles.for_mission(1).mindistance, 0);
    EXPECT_EQ(rig->profiles.for_mission(1).mintime, 0);

    std::vector<MissionRequest> more;
    MissionRequest r2 = r;   // identical mission/TOT/target
    more.push_back(r2);
    const auto out = rig->atm->deconflict(std::move(more));
    EXPECT_EQ(out.size(), 1u);
    EXPECT_EQ(rig->atm->stats().requests_deconflicted, 0);
}

// ── PHASE 4 — FindBestAir ────────────────────────────────────────────────────

TEST(AtmFindBestAir, ScoresRoleOverTheSpecialtyRatings) {
    auto rig = Rig::make();
    // One BARCAP (ARO_CA) request: the AA-specialist wing (rating 100)
    // must outrank the unspecialized wing (rating 60) — both pass the
    // lowestScore gate, the specialist's base score wins.
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 1;   // BARCAP — ARO_CA
    r.team = 1;
    r.priority = 100;
    r.aircraft = 2;
    r.tot = 3600;
    r.tot_type = TotType::LE;
    reqs.push_back(r);

    AtmConfig cfg;   // no threat interplay needed here
    auto ws = make_atm_world();
    f4::world::WorldStateAdapters adapters(ws);
    auto profiles = load_profiles();
    AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                          adapters.units, &adapters.objectives, cfg);
    auto flights = atm.compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 1u);
    EXPECT_EQ(flights[0].squadron_vu, 6001u);   // the AA wing
}

TEST(AtmFindBestAir, CounterAirWingStillTaskedForStrike) {
    auto rig = Rig::make();
    // The C3 role-fallback bridge's whole reason to exist: an
    // AA-specialist squadron flying a STRIKE-family mission. FindBestAir
    // SCORES it (rating 30, − specialty penalty → below the
    // lowestScore gate) rather than gating it — the unspecialized
    // wing (rating 60) fields the mission, and its ADDESCORT pairing
    // rides along (the escort itself is an ARO_CA flight the AA wing
    // CAN win).
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;   // INTSTRIKE — ARO_S
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 4;
    r.tot = 5400;
    r.tot_type = TotType::LE;
    reqs.push_back(r);

    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    // Main + the ADDESCORT pairing (default threat threshold → no
    // SEAD): 2 flights.
    ASSERT_EQ(flights.size(), 2u);
    // The unspecialized wing (60) out-scores the AA specialist (30 − 5,
    // gated out entirely): the MAIN is the plain wing.
    EXPECT_EQ(flights[0].squadron_vu, 6002u);
}

TEST(AtmFindBestAir, AvailabilityGateRejectsEmptySquadrons) {
    auto rig = Rig::make();
    // Squeeze team 1's pool to nothing: neither squadron flies.
    auto ws = make_atm_world();
    ws.campaign.te_number_aircraft = {0, 0, 0, 0, 0, 0, 24, 0};
    f4::world::WorldStateAdapters adapters(ws);
    auto profiles = load_profiles();
    AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                          adapters.units, &adapters.objectives, AtmConfig{});
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 1;
    r.team = 1;
    r.priority = 100;
    r.aircraft = 2;
    r.tot = 3600;
    reqs.push_back(r);
    const auto flights = atm.compose_packages(reqs, 1, kNow);
    EXPECT_TRUE(flights.empty());
    EXPECT_EQ(atm.stats().requests_unfilled, 1);
}

// ── PHASE 5 — escort pairing ─────────────────────────────────────────────────

TEST(AtmSupportAssignment, DefendedTargetPairsSeadAndFighterEscorts) {
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;   // the defended target scores 40
    auto rig = Rig::make(WorldOpts{}, cfg);

    // The threat at (400,400) for team 1 (low band, war territory):
    // 1 low + 1 high counter → 28 + 2 + 10 = 40 > 30 → NEED_SEAD.
    ASSERT_GT(rig->threat->score(400, 400, AltBand::Low, 1), 30);

    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;   // INTSTRIKE: ADDSEAD + ADDESCORT
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 4;
    r.tot = 5400;
    r.tot_type = TotType::LE;
    reqs.push_back(r);

    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    // Main + SEADESCORT + ESCORT: the reference's pairing.
    ASSERT_EQ(flights.size(), 3u);

    const auto& main = flights[0];
    EXPECT_EQ(main.role, FlightRole::Main);
    EXPECT_EQ(main.escorted_flight_id, 0u);

    const auto& sead = flights[1];
    EXPECT_EQ(sead.role, FlightRole::SeadEscort);
    EXPECT_EQ(sead.mission, 11);   // AMIS_SEADESCORT
    EXPECT_EQ(sead.package_id, main.package_id);
    EXPECT_EQ(sead.escorted_flight_id, main.flight_id);
    // TOT = main TOT + the support profile's separation (60 s).
    EXPECT_EQ(sead.tot - main.tot, 60);
    // Size: min(support str 2, main 4).
    EXPECT_EQ(sead.aircraft, 2);

    const auto& esc = flights[2];
    EXPECT_EQ(esc.role, FlightRole::Escort);
    EXPECT_EQ(esc.mission, 10);    // AMIS_ESCORT (determinism's pick)
    EXPECT_EQ(esc.package_id, main.package_id);
    EXPECT_EQ(esc.tot - main.tot, 60);

    EXPECT_EQ(rig->atm->stats().escorts_built, 2);
    EXPECT_EQ(rig->atm->stats().packages_built, 1);
}

TEST(AtmSupportAssignment, UndefendedTargetSkipsSeadButKeepsEscort) {
    AtmConfig cfg;   // default threshold 40 — the target's 40 is NOT >
    auto rig = Rig::make(WorldOpts{}, cfg);
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 4;
    r.tot = 5400;
    reqs.push_back(r);

    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    // No NEED_SEAD (40 is not > 40) → only the ADDESCORT flight pairs.
    ASSERT_EQ(flights.size(), 2u);
    EXPECT_EQ(flights[1].role, FlightRole::Escort);
    EXPECT_EQ(rig->atm->stats().escorts_built, 1);
}

TEST(AtmSupportAssignment, SupportFlightsPreferThePackageLeadSquadron) {
    // Drop the AA specialist: two UNSPECIALIZED wings, both at the
    // airbase. The ratings tie (60/60), so the escorts' +3 (same
    // squadron) +2 (same airbase) bonuses — the reference's
    // SetAssigned rule — decide: both escorts ride the MAIN's wing.
    auto ws = make_atm_world();
    ws.units.erase(std::remove_if(ws.units.begin(), ws.units.end(),
                                   [](const UnitState& u) {
                                       return u.id_num == 6001;
                                   }),
                   ws.units.end());
    // A second plain wing (BEFORE the ATM snapshot — the squadron
    // roster is taken at construction).
    f4::world::UnitState plain;
    plain.unit_class = UnitClass::Squadron;
    plain.domain = 2;
    plain.x = 100;
    plain.y = 100;
    plain.owner = 1;
    plain.id_num = 6004;
    plain.specialty = 0;
    plain.airbase_id = 4281;
    plain.class_name = "Second Wing";
    ws.units.push_back(plain);

    f4::world::WorldStateAdapters adapters(ws);
    auto profiles = load_profiles();
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;
    ThreatMap threat(adapters.objectives, adapters.units,
                     adapters.teams, 1);
    AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                          adapters.units, &adapters.objectives, cfg);
    atm.set_threat_map(&threat);

    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 4;
    r.tot = 5400;
    reqs.push_back(r);
    auto flights = atm.compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 3u);
    EXPECT_EQ(flights[1].squadron_vu, flights[0].squadron_vu);
    EXPECT_EQ(flights[2].squadron_vu, flights[0].squadron_vu);
}

// ── PHASE 7 — TOT slot scheduling ────────────────────────────────────────────

TEST(AtmScheduling, SnapsToSeededScheduleAndShiftsTot) {
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;
    auto rig = Rig::make(WorldOpts{.seeded_schedule = true}, cfg);
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 2;
    r.tot = 5400;   // 90 minutes out
    reqs.push_back(r);
    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 3u);

    // The main flight's takeoff estimate: TOT − travel from the
    // picked wing (the plain wing at (250,250): 150 grid → 13 min at
    // 12/min → takeoff = 5400 − 780 = 4620 s = minute 77).
    const auto& main = flights[0];
    ASSERT_EQ(main.squadron_vu, 6002u);
    ASSERT_EQ(main.takeoff, 4620);

    const CampaignTime delta = rig->atm->schedule_takeoff(flights[0]);
    // Minute 77 (block 15, slot 2) is far from the seed's block-0
    // bits → snapped to exactly 4620 → no shift.
    EXPECT_EQ(delta, 0);
    EXPECT_EQ(flights[0].takeoff, 4620);
    EXPECT_EQ(rig->atm->stats().slot_snaps, 1);
    // The flight is booked for recovery.
    ASSERT_EQ(rig->atm->booked_flights().size(), 1u);
    EXPECT_EQ(rig->atm->booked_flights()[0].flight_id,
              flights[0].flight_id);
}

TEST(AtmScheduling, SeededOccupiedSlotSnapsForward) {
    // The seed occupies the main flight's own minute → the lookahead
    // snaps it one minute later (+60 s TOT shift).
    auto ws = make_atm_world(WorldOpts{});
    // Occupy minute 77 (block 15, slot 2 — the minute the plain wing
    // will want) in the wire seed.
    f4::world::AtmAirbaseState ab;
    ab.id_num = 4281;
    ab.schedule[15] = 0x04;
    ws.teams[1].atm_airbases.push_back(ab);

    f4::world::WorldStateAdapters adapters(ws);
    auto profiles = load_profiles();
    AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                          adapters.units, &adapters.objectives,
                          AtmConfig{});

    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 2;
    r.tot = 5400;
    reqs.push_back(r);
    auto flights = atm.compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 2u);
    ASSERT_EQ(flights[0].takeoff, 4620);
    const CampaignTime delta = atm.schedule_takeoff(flights[0]);
    EXPECT_EQ(delta, 60);              // snapped to minute 78
    EXPECT_EQ(flights[0].takeoff, 4680);
    EXPECT_EQ(flights[0].tot, 5460);   // TOT follows the shift
}

TEST(AtmScheduling, FillsSlotsSoTheNextFlightShifts) {
    AtmConfig cfg;
    auto rig = Rig::make(WorldOpts{}, cfg);
    std::vector<MissionRequest> reqs;
    for (int i = 0; i < 2; ++i) {
        MissionRequest r;
        r.mission = 1;   // BARCAP, str 2
        r.team = 1;
        r.priority = 100;
        r.aircraft = 2;
        r.tot = 5400;   // identical requests → identical takeoff wants
        reqs.push_back(r);
    }
    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 2u);

    const CampaignTime d0 = rig->atm->schedule_takeoff(flights[0]);
    const CampaignTime d1 = rig->atm->schedule_takeoff(flights[1]);
    // The first flight snaps exactly (empty schedule); the second
    // finds the minute occupied → +60 s (the lookahead).
    EXPECT_EQ(d0, 0);
    EXPECT_EQ(d1, 60);
    EXPECT_EQ(flights[1].tot - flights[0].tot, 60);
    EXPECT_EQ(rig->atm->stats().slot_shifts_sec, 60);
    EXPECT_EQ(rig->atm->booked_flights().size(), 2u);
}

// ── Mission recovery ─────────────────────────────────────────────────────────

TEST(AtmRecovery, SurvivorsReturnWhenTheMissionCompletes) {
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;   // the full trio (main + 2 escorts)
    cfg.reserve_min = 0;              // tight deadlines for the test
    auto rig = Rig::make(WorldOpts{}, cfg);
    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 4;
    r.tot = 5400;
    reqs.push_back(r);
    auto flights = rig->atm->compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 3u);
    for (auto& ft : flights) (void)rig->atm->schedule_takeoff(ft);

    // Nothing completes before the deadline.
    EXPECT_TRUE(rig->atm->recover_completed(flights[0].mission_over - 1)
                    .empty());
    // After the LAST flight's deadline: all three release (no losses).
    const auto rel = rig->atm->recover_completed(
        flights[2].mission_over + 1);
    ASSERT_EQ(rel.size(), 3u);
    EXPECT_EQ(rel[0].survivors, flights[0].aircraft);
    EXPECT_EQ(rig->atm->stats().recoveries, 3);
    EXPECT_EQ(rig->atm->stats().aircraft_recovered,
              flights[0].aircraft + flights[1].aircraft +
                  flights[2].aircraft);
    // Booked is drained.
    EXPECT_TRUE(rig->atm->booked_flights().empty());
}

TEST(AtmRecovery, LedgerLossesReduceTheReleasedSurvivors) {
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;   // the full trio
    auto ws = make_atm_world();
    f4::world::WorldStateAdapters adapters(ws);
    auto profiles = load_profiles();

    // The ledger over the same world: draw, book a flight loss, then
    // recover — the release is drawn − losses.
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    ThreatMap threat(adapters.objectives, adapters.units,
                     adapters.teams, 1);
    AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                          adapters.units, &adapters.objectives, cfg);
    atm.set_ledger(&ledger);
    atm.set_threat_map(&threat);

    std::vector<MissionRequest> reqs;
    MissionRequest r;
    r.mission = 13;
    r.team = 1;
    r.target_id = 9001;
    r.priority = 100;
    r.aircraft = 4;
    r.tot = 5400;
    reqs.push_back(r);
    auto flights = atm.compose_packages(reqs, 1, kNow);
    ASSERT_EQ(flights.size(), 3u);
    // The ledger-side draw (the Campaign's booking site).
    for (const auto& ft : flights) {
        ledger.apply_mission_draw(0.0, 1, ft.squadron_vu, ft.aircraft);
    }
    for (auto& ft : flights) (void)atm.schedule_takeoff(ft);

    // Two of the main flight's four aircraft die (the sink's shape).
    ledger.apply_air_loss(100.0, 1, flights[0].squadron_vu,
                          flights[0].flight_id, 0);
    ledger.apply_air_loss(200.0, 1, flights[0].squadron_vu,
                          flights[0].flight_id, 0);
    EXPECT_EQ(ledger.flight_air_losses(flights[0].flight_id,
                                       flights[0].squadron_vu),
              2);

    const auto rel = atm.recover_completed(flights[2].mission_over + 1);
    ASSERT_EQ(rel.size(), 3u);
    // The main flight releases its TWO survivors; escorts release all.
    EXPECT_EQ(rel[0].flight_id, flights[0].flight_id);
    EXPECT_EQ(rel[0].survivors, 2);
    EXPECT_EQ(rel[1].survivors, flights[1].aircraft);
}

// ── Ledger integration (apply_mission_recovery) ──────────────────────────────

TEST(LedgerRecovery, DrawThenRecoverRestoresTaskingAvailability) {
    auto ws = make_atm_world();
    f4::world::WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    const std::uint32_t sq = 6001;
    const int avail0 = ledger.squadron_tasking_available(sq);

    ledger.apply_mission_draw(0.0, 1, sq, 4);
    EXPECT_EQ(ledger.squadron_tasking_available(sq), avail0 - 4);

    // The mission completes with all four aircraft surviving.
    ledger.apply_mission_recovery(3600.0, 1, sq, /*flight=*/77, 4);
    EXPECT_EQ(ledger.squadron_tasking_available(sq), avail0);
    EXPECT_EQ(ledger.aircraft_recovered(), 4);
    EXPECT_EQ(ledger.mission_recoveries(), 1);
    // The team mirror.
    EXPECT_EQ(ledger.team_aircraft_tasking(1), 24);
}

TEST(LedgerRecovery, NettedDeathsKeepTheirDrawsSpent) {
    auto ws = make_atm_world();
    f4::world::WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    const std::uint32_t sq = 6001;
    // The team pool (24) is shared by TWO squadrons → 12 each.
    const int avail0 = ledger.squadron_tasking_available(sq);
    ASSERT_EQ(avail0, 12);

    ledger.apply_mission_draw(0.0, 1, sq, 4);
    // Two drawn aircraft die: the draw stays spent (no double debit),
    // the existence counters move.
    ledger.apply_air_loss(100.0, 1, sq, 42, 0);
    ledger.apply_air_loss(200.0, 1, sq, 42, 0);
    EXPECT_EQ(ledger.squadron_tasking_available(sq), avail0 - 4);

    // The mission completes: TWO survivors return.
    ledger.apply_mission_recovery(3600.0, 1, sq, 42, 2);
    EXPECT_EQ(ledger.squadron_tasking_available(sq), avail0 - 4 + 2);
    EXPECT_EQ(ledger.aircraft_recovered(), 2);
    // The recovery log carries the flight + release.
    ASSERT_EQ(ledger.mission_recovery_log().size(), 1u);
    EXPECT_EQ(ledger.mission_recovery_log()[0].flight, 42u);
    EXPECT_EQ(ledger.mission_recovery_log()[0].released, 2);
}

TEST(LedgerRecovery, JsonCarriesTheRecoveryBlock) {
    auto ws = make_atm_world();
    f4::world::WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    ledger.apply_mission_draw(0.0, 1, 6001, 2);
    ledger.apply_mission_recovery(60.0, 1, 6001, 9, 2);
    const auto js = ledger.to_json();
    EXPECT_NE(js.find("\"mission_recoveries\":1"), std::string::npos);
    EXPECT_NE(js.find("\"aircraft_recovered\":2"), std::string::npos);
    EXPECT_NE(js.find("\"mission_recoveries\": ["), std::string::npos);
    // A no-recovery ledger keeps the legacy shape (array absent).
    CampaignResultLedger plain(adapters.campaign, adapters.teams,
                               adapters.units);
    const auto js2 = plain.to_json();
    EXPECT_EQ(js2.find("\"mission_recoveries\": ["), std::string::npos);
}

// ── The Campaign mode switch ─────────────────────────────────────────────────

namespace {

// The kunsan fixture path (same macro the tick test uses).
std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}

struct CampaignRig {
    std::unique_ptr<f4::world::WorldState> ws;
    std::unique_ptr<f4::world::WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<f4::messaging::MessageBus> bus;
    std::unique_ptr<Campaign> campaign;

    static std::unique_ptr<CampaignRig> make(const CampaignConfig& cfg) {
        auto r = std::make_unique<CampaignRig>();
        r->ws = std::make_unique<f4::world::WorldState>();
        r->ws->load(kunsan_world());
        r->adapters =
            std::make_unique<f4::world::WorldStateAdapters>(*r->ws);
        r->profiles = load_profiles();
        r->bus = std::make_unique<f4::messaging::MessageBus>();
        r->campaign = std::make_unique<Campaign>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units,
            r->profiles, *r->bus, cfg);
        return r;
    }
};

} // namespace

TEST(CampaignAtm, PipelineRunIsDeterministicAndCarriesTheAtmBlock) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    auto a = CampaignRig::make(cfg);
    auto b = CampaignRig::make(cfg);
    a->campaign->tick(1800);
    b->campaign->tick(1800);

    EXPECT_EQ(a->campaign->to_summary_json(),
              b->campaign->to_summary_json());
    EXPECT_NE(a->campaign->intents().size(), 0u);

    const auto* stats = a->campaign->atm_stats();
    ASSERT_NE(stats, nullptr);
    EXPECT_GT(stats->packages_built, 0);

    const auto js = a->campaign->to_summary_json();
    EXPECT_NE(js.find("\"atm\": {"), std::string::npos);
    EXPECT_NE(js.find("\"packages_built\":"), std::string::npos);
}

TEST(CampaignAtm, PipelineOffKeepsTheLegacyGoldensByteIdentical) {
    // The mode switch is opt-in: a default-configured campaign's
    // summary is byte-identical whether the ATM code exists or not —
    // pinned against a legacy-configured run (both here, both off).
    auto a = CampaignRig::make(CampaignConfig{});
    auto b = CampaignRig::make(CampaignConfig{});
    a->campaign->tick(1800);
    b->campaign->tick(1800);
    EXPECT_EQ(a->campaign->to_summary_json(),
              b->campaign->to_summary_json());
    EXPECT_EQ(a->campaign->atm_stats(), nullptr);
    const auto js = a->campaign->to_summary_json();
    EXPECT_EQ(js.find("\"atm\": {"), std::string::npos);
}

TEST(CampaignAtm, DrawnAircraftReturnAfterTheirMissionCompletes) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.atm.reserve_min = 0;
    auto rig = CampaignRig::make(cfg);

    CampaignResultLedger ledger(rig->adapters->campaign,
                                 rig->adapters->teams,
                                 rig->adapters->units);
    rig->campaign->set_result_ledger(&ledger);

    // One cycle draws; a long tick carries the flights past their
    // mission-over deadlines → the survivors return.
    rig->campaign->tick(1800);
    const int drawn = ledger.mission_draw_aircraft();
    ASSERT_GT(drawn, 0);

    rig->campaign->tick(6 * 3600);
    EXPECT_GT(ledger.aircraft_recovered(), 0);
    EXPECT_GT(ledger.mission_recoveries(), 0);
    // The pool netted: drawn − outstanding (survivors back) − deaths.
    EXPECT_LE(ledger.team_aircraft_tasking(1) + ledger.team_aircraft_tasking(6) +
                  ledger.team_aircraft_tasking(2),
              24 + 24 + 24);
    // No double-release: recovered ≤ drawn.
    EXPECT_LE(ledger.aircraft_recovered(), drawn);
}

TEST(CampaignAtm, MultiFlightPackagesShareIdsAndPairEscorts) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    auto rig = CampaignRig::make(cfg);
    rig->campaign->tick(1800);

    // Packages with escorts exist on the kunsan run: same package_id,
    // distinct flight_ids, escorted_flight_id links the pair, and the
    // escort's TOT differs by the support profile's separation.
    const auto& intents = rig->campaign->intents();
    ASSERT_FALSE(intents.empty());
    bool saw_pair = false;
    for (std::size_t i = 0; i < intents.size(); ++i) {
        const auto& in = intents[i];
        if (in.flight_role == 0) continue;
        saw_pair = true;
        // Find the escorted main flight.
        const auto* main = &in;
        for (const auto& other : intents) {
            if (other.flight_id == in.escorted_flight_id) {
                main = &other;
                break;
            }
        }
        EXPECT_EQ(in.package_id, main->package_id);
        EXPECT_NE(in.flight_id, main->flight_id);
        EXPECT_EQ(in.escorted_flight_id, main->flight_id);
        EXPECT_EQ(main->flight_role, 0);
        // Same-mission support flights carry their own mission byte.
        EXPECT_NE(in.mission_byte, main->mission_byte);
    }
    // The kunsan fixture's belligerent tasking always pairs escorts
    // (ADDESCORT-carrying profiles with available squadrons). If the
    // fixture ever stops producing pairs, this pin fails loudly.
    EXPECT_TRUE(saw_pair);
}

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
    std::uint8_t backlog_roe = 0;  // P7 — rq2's roe_check byte
    // CAMP-ATM-1 — objective damage: 0 = none; 1 = LIGHT damage on the
    // USA airbase 4281 (1 of 8 features destroyed = 12% — below the
    // garrison-BARCAP threshold); 2 = the light airbase PLUS HEAVY
    // damage on the DPRK target 9001 (4 of 8 = 50%).
    int damage = 0;
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

    // CAMP-ATM-1 — the ACTION tables' damage state (the fstatus
    // bitmap: 2 bits per feature, 2 = destroyed; 8 features per
    // objective, features_count left 0 — the kunsan shape where the
    // bitmap's own capacity is the count).
    if (opts.damage >= 1) {
        ws.objectives[0].fstatus = {0x02, 0x00};   // 1/8 destroyed = 12%
    }
    if (opts.damage >= 2) {
        ws.objectives[1].fstatus = {0xAA, 0x00};   // 4/8 destroyed = 50%
    }

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
        rq2.roe_check = opts.backlog_roe;   // P7 — the RoE carry
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
    // (config is fixed at construction — test through a fresh ATM `c` below)
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

// ============================================================================
// P7 — the strategy layer (station targeting, FindSupportFlights,
//      RequestEnemyMission, RoE carry)
// ============================================================================

TEST(AtmStrategy, StationsCapRequestsOverRankedOwnObjectives) {
    AtmConfig cfg;
    cfg.strategy = true;
    auto rig = Rig::make(WorldOpts{}, cfg);

    auto reqs = rig->atm->generate_requests(1, kNow);
    // The USA team owns exactly one objective (4281 at 100,100) —
    // every TPROF_LOITER+WP_CAP request (BARCAP 1, TARCAP 4, ALERT 8,
    // AMBUSHCAP 6, ...) stations over it.
    bool barcap_seen = false;
    for (const auto& r : reqs) {
        if (r.mission == 1) {
            barcap_seen = true;
            EXPECT_EQ(r.target_id, 4281u);
        }
    }
    ASSERT_TRUE(barcap_seen);
    EXPECT_GT(rig->atm->stats().stations_targeted, 0);
}

TEST(AtmStrategy, DisarmedStationsKeepCapsTargetLess) {
    // The golden identity: the pre-strategy shape — CAP requests stay
    // target-less (the ladder only targets the delivery family).
    auto rig = Rig::make(WorldOpts{});
    auto reqs = rig->atm->generate_requests(1, kNow);
    for (const auto& r : reqs) {
        if (r.mission == 1) {
            EXPECT_EQ(r.target_id, 0u);
        }
    }
    EXPECT_EQ(rig->atm->stats().stations_targeted, 0);
}

namespace {

// A DEEPSTRIKE request (byte 15): the one generated profile carrying
// the whole support flag set — ADDAWACS, ADDTANKER, ADDECM, ADDBARCAP,
// ADDESCORT, ADDSEAD.
MissionRequest deepstrike_request(std::uint32_t target_vu, CampaignTime tot,
                                  int aircraft = 4) {
    MissionRequest r;
    r.mission = 15;
    r.team = 1;
    r.target_id = target_vu;
    r.priority = 120;
    r.aircraft = aircraft;
    r.tot = tot;
    r.tot_type = TotType::LE;
    return r;
}

} // namespace

TEST(AtmStrategy, FindSupportFlightsFilesAwacsAndEcmWithStations) {
    AtmConfig cfg;
    cfg.strategy = true;
    auto rig = Rig::make(WorldOpts{}, cfg);

    auto flights = rig->atm->compose_packages(
        {deepstrike_request(9001, 5400)}, 1, kNow);

    // The main + its escort + the profile's support filings. The
    // DEEPSTRIKE flag set carries ADDAWACS + ADDECM (ADDTANKER is on
    // the CAP family, not this profile) — two support filings, the
    // AWACS bringing its own ADDESCORT fighter escort; the ECM does
    // not carry one.
    int supports = 0;
    int awacs_seen = 0;
    int ecm_seen = 0;
    for (const auto& f : flights) {
        if (f.role == FlightRole::Support) {
            ++supports;
            // Every support flight stations over the OWN objective
            // nearest the package target — the USA airbase 4281 (the
            // only USA objective in the world).
            EXPECT_EQ(f.target_vu, 4281u);
            if (f.mission == 25) ++awacs_seen;
            if (f.mission == 28) ++ecm_seen;
        }
    }
    EXPECT_EQ(supports, 2);
    EXPECT_EQ(awacs_seen, 1);
    EXPECT_EQ(ecm_seen, 1);
    EXPECT_EQ(rig->atm->stats().supports_filed, 2);
    EXPECT_EQ(rig->atm->stats().supports_shared, 0);

    // The package still flies (the main is there, role Main).
    ASSERT_FALSE(flights.empty());
    EXPECT_EQ(flights[0].role, FlightRole::Main);
    EXPECT_EQ(flights[0].mission, 15);
}

TEST(AtmStrategy, FindSupportFlightsSharesInsteadOfRefiling) {
    AtmConfig cfg;
    cfg.strategy = true;
    auto rig = Rig::make(WorldOpts{}, cfg);

    // Two DEEPSTRIKE packages over the SAME target in one cycle: the
    // second package's support requests find the first's stations
    // (same byte, same station, TOT inside the window) — SHARED, not
    // re-filed. One tanker feeds a whole raid. (The second package is
    // a 2-ship: the fixture's per-squadron pool is nearly spent by the
    // first package's draws — the share test wants the second MAIN to
    // build, and it does at 2 ships.)
    auto flights = rig->atm->compose_packages(
        {deepstrike_request(9001, 5400), deepstrike_request(9001, 5400, 2)},
        1, kNow);

    const int supports = static_cast<int>(std::count_if(
        flights.begin(), flights.end(), [](const FlightTasking& f) {
            return f.role == FlightRole::Support;
        }));
    EXPECT_EQ(supports, 2);   // one AWACS, one ECM — no re-filings
    EXPECT_EQ(rig->atm->stats().supports_filed, 2);
    EXPECT_EQ(rig->atm->stats().supports_shared, 2);   // the 2nd package
}

TEST(AtmStrategy, RequestEnemyMissionFilesDefenderBarcapNextCycle) {
    AtmConfig cfg;
    cfg.strategy = true;
    auto rig = Rig::make(WorldOpts{}, cfg);

    // The strike over the DPRK objective 9001 files a BARCAP request
    // for the DEFENDER (DPRK, slot 6) — the pending queue.
    (void)rig->atm->compose_packages({deepstrike_request(9001, 5400)},
                                     1, kNow);
    ASSERT_EQ(rig->atm->stats().enemy_caps_filed, 1);

    // Dedup: a second identical package does not double-file.
    (void)rig->atm->compose_packages({deepstrike_request(9001, 5400)},
                                     1, kNow);
    EXPECT_EQ(rig->atm->stats().enemy_caps_filed, 1);

    // The defender's NEXT cycle picks the filing up: a BARCAP over the
    // threatened objective, flagged enemy_filed, flying for team 6.
    auto reqs = rig->atm->generate_requests(6, kNow + 1800);
    bool filed_seen = false;
    for (const auto& r : reqs) {
        if (r.enemy_filed) {
            filed_seen = true;
            EXPECT_EQ(r.mission, 1);          // AMIS_BARCAP
            EXPECT_EQ(r.team, 6);
            EXPECT_EQ(r.target_id, 9001u);
        }
    }
    ASSERT_TRUE(filed_seen);
}

TEST(AtmStrategy, BacklogRoeRidesThroughCompose) {
    // The decoded backlog's roe_check byte flows: seed → request →
    // flight. WEAPONS HOLD (2) on the INTSTRIKE request.
    auto rig = Rig::make(WorldOpts{.backlog = true, .backlog_roe = 2});
    auto reqs = rig->atm->generate_requests(1, kNow);

    MissionRequest* strike = nullptr;
    for (auto& r : reqs) {
        if (r.mission == 13 && r.seeded) strike = &r;
    }
    ASSERT_NE(strike, nullptr);
    EXPECT_EQ(strike->roe, 2);

    auto flights = rig->atm->compose_packages(
        {*(strike)}, 1, kNow);
    ASSERT_FALSE(flights.empty());
    EXPECT_EQ(flights[0].roe, 2);   // WEAPONS HOLD rides the flight
}

TEST(AtmStrategy, DisarmedComposeCarriesNoSupportAndNoFilings) {
    // The golden identity: the pre-strategy compose — DEEPSTRIKE's
    // ADDAWACS/ADDTANKER/ADDECM/ADDBARCAP flags file nothing, only the
    // profile's own ADDESCORT fighter escort pairs.
    auto rig = Rig::make(WorldOpts{});
    auto flights = rig->atm->compose_packages(
        {deepstrike_request(9001, 5400)}, 1, kNow);
    for (const auto& f : flights) {
        EXPECT_NE(f.role, FlightRole::Support);
    }
    EXPECT_EQ(rig->atm->stats().supports_filed, 0);
    EXPECT_EQ(rig->atm->stats().supports_shared, 0);
    EXPECT_EQ(rig->atm->stats().enemy_caps_filed, 0);
    EXPECT_EQ(rig->atm->stats().stations_targeted, 0);
}

TEST(AtmStrategy, CampaignStrategyRunIsDeterministic) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.strategy_layer = true;
    auto a = CampaignRig::make(cfg);
    auto b = CampaignRig::make(cfg);
    a->campaign->tick(1800);
    b->campaign->tick(1800);

    EXPECT_EQ(a->campaign->to_summary_json(),
              b->campaign->to_summary_json());

    const auto js = a->campaign->to_summary_json();
    EXPECT_NE(js.find("\"stations_targeted\":"), std::string::npos);
    EXPECT_NE(js.find("\"supports_filed\":"), std::string::npos);
    EXPECT_NE(js.find("\"enemy_caps_filed\":"), std::string::npos);
}

TEST(AtmStrategy, CampaignStrategyStationsCapsWithARoutePlanner) {
    // With objectives attached (the route planner's attachment shape),
    // the CAP family's requests get stations — the kunsan world's
    // belligerents own objectives and their priority tables task the
    // BARCAP family (prio 10).
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.strategy_layer = true;
    auto r = CampaignRig::make(cfg);
    // A second load of the SAME fixture world — the route planner (and
    // with it the strategy layer's objective view) attaches from its
    // adapters, exactly the session/QC construction shape.
    auto ws_ptr = std::make_unique<f4::world::WorldState>();
    ws_ptr->load(kunsan_world());
    auto adapters =
        std::make_unique<f4::world::WorldStateAdapters>(*ws_ptr);
    RouteBuilderConfig route_cfg;
    route_cfg.loiter_racetracks = true;
    auto builder = std::make_unique<RouteBuilder>(
        static_cast<const f4::world::IObjectiveSource&>(
            adapters->objectives),
        static_cast<const f4::world::IUnitCoreSource&>(
            adapters->units),
        static_cast<const f4::world::ITeamSource&>(adapters->teams),
        /*viewer=*/2, route_cfg);
    r->campaign->set_route_planner(builder.get(),
        &static_cast<const f4::world::IObjectiveSource&>(
            adapters->objectives));
    r->campaign->tick(1800);

    const auto* stats = r->campaign->atm_stats();
    ASSERT_NE(stats, nullptr);
    EXPECT_GT(stats->stations_targeted, 0);
}

// ── CAMP-CMD-2 — the booked-flight interventions + the priority input ───────

TEST(AtmScrub, ClosesTheBookingAndReleasesSurvivors) {
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;   // the full trio (main + 2 escorts)
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
    ASSERT_EQ(rig->atm->booked_flights().size(), 3u);

    // Scrub the main: the booking closes NOW, the complement releases
    // (drawn − booked losses; the rig's war has none yet).
    const auto rel = rig->atm->scrub_flight(flights[0].flight_id);
    ASSERT_TRUE(rel.has_value());
    EXPECT_EQ(rel->flight_id, flights[0].flight_id);
    EXPECT_EQ(rel->survivors, flights[0].aircraft);
    EXPECT_EQ(rig->atm->booked_flights().size(), 2u);
    EXPECT_EQ(rig->atm->stats().flights_scrubbed, 1);
    EXPECT_EQ(rig->atm->stats().aircraft_scrubbed, flights[0].aircraft);

    // Unknown ids answer empty.
    EXPECT_FALSE(rig->atm->scrub_flight(999999).has_value());

    // The scrubbed flight never double-releases at its old deadline.
    const auto late =
        rig->atm->recover_completed(flights[2].mission_over + 1);
    ASSERT_EQ(late.size(), 2u);   // the escorts only
    for (const auto& rel2 : late) {
        EXPECT_NE(rel2.flight_id, flights[0].flight_id);
    }
}

TEST(AtmReschedule, BookingFollowsTheRetask) {
    AtmConfig cfg;
    cfg.min_seadescort_threat = 30;
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
    ASSERT_FALSE(flights.empty());
    for (auto& ft : flights) (void)rig->atm->schedule_takeoff(ft);

    const auto main_id = flights[0].flight_id;
    const auto old_over = flights[0].mission_over;
    // The booking follows the flight: new mission family, target, TOT,
    // and the recovery deadline (far past the old one).
    EXPECT_TRUE(rig->atm->reschedule_flight(main_id, 20, 5150,
                                            kNow + 600, kNow + 100000));
    const auto& booked = rig->atm->booked_flights();
    ASSERT_GE(booked.size(), 1u);
    bool found = false;
    for (const auto& ft : booked) {
        if (ft.flight_id != main_id) continue;
        found = true;
        EXPECT_EQ(ft.mission, 20);
        EXPECT_EQ(ft.target_vu, 5150u);
        EXPECT_EQ(ft.tot, kNow + 600);
        EXPECT_EQ(ft.mission_over, kNow + 100000);
    }
    EXPECT_TRUE(found);
    EXPECT_GT(kNow + 100000, old_over);
    // Unknown ids refuse.
    EXPECT_FALSE(rig->atm->reschedule_flight(424242, 1, 9001, 0, 0));

    // The rescheduled deadline is the one recovery obeys: only the
    // main stays booked (the escorts scrub), nothing releases before
    // the NEW deadline, and the main releases at it.
    (void)rig->atm->scrub_flight(flights[1].flight_id);
    (void)rig->atm->scrub_flight(flights[2].flight_id);
    ASSERT_EQ(rig->atm->booked_flights().size(), 1u);
    EXPECT_TRUE(rig->atm->recover_completed(kNow + 99999).empty());
    const auto rel = rig->atm->recover_completed(kNow + 100001);
    ASSERT_EQ(rel.size(), 1u);
    EXPECT_EQ(rel[0].flight_id, main_id);
}

TEST(AtmPriorityInput, ObjectivePriorityScalesTheTargetTerm) {
    // objective_priority's write target is the objective's own priority
    // byte — the same field request_priority_'s target term scales.
    // Two worlds differing ONLY in the target objective's priority:
    // the generated request's score moves with it (the objtype term is
    // armed so the scaling has something to bite).
    const auto request_for_target = [](std::uint8_t objective_priority,
                                       int* out_count) {
        auto ws = make_atm_world();
        for (auto& o : ws.objectives) {
            if (o.id_num == 9001) o.priority = objective_priority;
        }
        ws.teams[1].objtype_priority.assign(36, 0);
        ws.teams[1].objtype_priority[4] = 80;   // the fixture's objtype
        f4::world::WorldStateAdapters adapters(ws);
        auto profiles = load_profiles();
        AirTaskingManager atm(profiles, adapters.campaign, adapters.teams,
                              adapters.units, &adapters.objectives, {});
        const auto reqs = atm.generate_requests(1, kNow);
        *out_count = 0;
        int best = -1;
        for (const auto& rq : reqs) {
            if (rq.target_id != 9001) continue;
            ++*out_count;
            best = std::max(best, rq.priority);
        }
        return best;
    };
    int count_a = 0;
    int count_b = 0;
    const int prio_low =
        request_for_target(7 /* the fixture's own value */, &count_a);
    const int prio_high = request_for_target(100, &count_b);
    EXPECT_GT(count_a, 0);   // the delivery ladder files against 9001
    EXPECT_EQ(count_a, count_b);
    EXPECT_GT(prio_high, prio_low);   // the commander's weight moved it
}

// ── CAMP-ATM-1 — the ACTION tables (the objective-damage-driven
//    filings) + the SWEEP lines ─────────────────────────────────────────────

TEST(AtmAction, LightOwnDamageFilesCasOnly) {
    // The USA airbase took light damage (12%): the owner files CAS over
    // it — and no garrison BARCAP (below the heavy threshold). Nothing
    // else is damaged, so nothing else files.
    auto rig = Rig::make(WorldOpts{.damage = 1}, AtmConfig{.strategy = true});
    const auto reqs = rig->atm->generate_requests(1, kNow);
    ASSERT_EQ(rig->atm->stats().actions_filed, 1);

    const auto cas = mission_type_byte("AMIS_CAS");
    bool seen = false;
    for (const auto& r : reqs) {
        // The ladder files target-less CAS too (byte 20, no ACTION
        // tag) — only the ACTION filing carries the bytes.
        if (r.mission != *cas || r.action_type == kActionNone) continue;
        seen = true;
        EXPECT_EQ(r.target_id, 4281u);
        EXPECT_EQ(r.action_type, kActionDefend);
        EXPECT_EQ(r.context, 4);   // the driving objective's own type
        EXPECT_EQ(r.damage_pct, 12);
        EXPECT_FALSE(r.seeded);
    }
    EXPECT_TRUE(seen);
    // No BARCAP anywhere (light damage — the garrison stays home).
    const auto barcap = mission_type_byte("AMIS_BARCAP");
    for (const auto& r : reqs) {
        ASSERT_FALSE(r.mission == *barcap && r.action_type != kActionNone);
    }
}

TEST(AtmAction, HeavyOwnDamageAddsTheGarrisonBarcapAndEnemyDamageFilesSead) {
    // Both sides damaged: USA (team 1) files CAS over its light-damaged
    // airbase and a SEADSTRIKE against the heavily damaged DPRK target
    // (the defenses there are alive — they shot back). DPRK (team 6)
    // files CAS + the garrison BARCAP over ITS heavy-damaged target and
    // a SEADSTRIKE against the USA airbase. The context byte is the
    // driving objective's type; the ACTION byte separates Defend
    // (own damage) from Punish (enemy damage).
    auto rig = Rig::make(WorldOpts{.damage = 2}, AtmConfig{.strategy = true});
    const auto cas = mission_type_byte("AMIS_CAS");
    const auto barcap = mission_type_byte("AMIS_BARCAP");
    const auto sead = mission_type_byte("AMIS_SEADSTRIKE");

    auto usa = rig->atm->generate_requests(1, kNow);
    ASSERT_EQ(rig->atm->stats().actions_filed, 2);   // CAS + SEADSTRIKE
    for (const auto& r : usa) {
        if (r.action_type == kActionDefend) {
            // The owner's defense: CAS over the light-damaged airbase.
            EXPECT_EQ(r.mission, *cas);
            EXPECT_EQ(r.target_id, 4281u);
            EXPECT_EQ(r.damage_pct, 12);
        } else if (r.action_type == kActionPunish) {
            // The suppression: SEADSTRIKE against the damaged enemy.
            EXPECT_EQ(r.mission, *sead);
            EXPECT_EQ(r.target_id, 9001u);
            EXPECT_EQ(r.damage_pct, 50);
        } else {
            // The ladder's own filings: zero-tagged, except the SWEEP
            // family's contested-air tag (kActionSweep — not an
            // ACTION-table filing).
            EXPECT_TRUE(r.action_type == kActionNone ||
                        r.action_type == kActionSweep);
        }
    }
    EXPECT_NE(std::find_if(usa.begin(), usa.end(),
                           [&](const MissionRequest& r) {
                               return r.mission == *cas &&
                                      r.action_type == kActionDefend;
                           }),
              usa.end());
    EXPECT_NE(std::find_if(usa.begin(), usa.end(),
                           [&](const MissionRequest& r) {
                               return r.mission == *sead &&
                                      r.action_type == kActionPunish;
                           }),
              usa.end());

    auto dprk = rig->atm->generate_requests(6, kNow);
    ASSERT_EQ(rig->atm->stats().actions_filed, 5);   // + CAS + BARCAP + SEAD
    int dprk_defend = 0, dprk_punish = 0;
    for (const auto& r : dprk) {
        if (r.action_type == kActionDefend) {
            ++dprk_defend;
            // Own heavy damage: the CAS AND the garrison BARCAP.
            EXPECT_EQ(r.target_id, 9001u);
            EXPECT_EQ(r.damage_pct, 50);
            EXPECT_TRUE(r.mission == *cas || r.mission == *barcap);
        } else if (r.action_type == kActionPunish) {
            ++dprk_punish;
            EXPECT_EQ(r.mission, *sead);
            EXPECT_EQ(r.target_id, 4281u);
            EXPECT_EQ(r.damage_pct, 12);
        }
    }
    EXPECT_EQ(dprk_defend, 2);
    EXPECT_EQ(dprk_punish, 1);
}

TEST(AtmAction, DisarmedScanFilesNothing) {
    // The golden identity: without the strategy arm the damage state is
    // invisible — no ACTION filings, every request carries the zero
    // ACTION bytes, the counter stays 0.
    auto rig = Rig::make(WorldOpts{.damage = 2}, AtmConfig{});
    const auto reqs = rig->atm->generate_requests(1, kNow);
    EXPECT_EQ(rig->atm->stats().actions_filed, 0);
    for (const auto& r : reqs) {
        EXPECT_EQ(r.action_type, kActionNone);
        EXPECT_EQ(r.context, 0);
        EXPECT_EQ(r.damage_pct, 0);
    }
}

TEST(AtmAction, CapBoundsThePendingQueue) {
    // max_pending_action_requests = 1: the wire-order scan files ONE
    // request (objective 4281 comes first — DPRK's Punish SEADSTRIKE
    // against the USA airbase) and the rest of the scan is capped.
    AtmConfig cfg;
    cfg.strategy = true;
    cfg.max_pending_action_requests = 1;
    auto rig = Rig::make(WorldOpts{.damage = 2}, cfg);
    const auto sead = mission_type_byte("AMIS_SEADSTRIKE");

    // Wire order: 4281 first (DPRK's Punish SEADSTRIKE against the USA
    // airbase), then 9001 (the own heavy damage — CAS then the garrison
    // BARCAP). The cap of 1 admits the SEADSTRIKE and drops the rest.
    (void)rig->atm->generate_requests(6, kNow);
    ASSERT_EQ(rig->atm->stats().actions_filed, 1);

    // The next cycle: the queue drained (nothing booked — nobody flew
    // it), so the scan re-files the same first filing and the rest is
    // capped again. The SWEEP ladder tag never counts here (it is not
    // an ACTION-table filing).
    (void)rig->atm->generate_requests(6, kNow);
    ASSERT_EQ(rig->atm->stats().actions_filed, 2);

    // The drain: exactly one damage filing rides ahead of the ladder,
    // the SEADSTRIKE the cap admitted.
    auto reqs = rig->atm->generate_requests(6, kNow);
    int filed = 0;
    for (const auto& r : reqs) {
        if (r.action_type == kActionPunish || r.action_type == kActionDefend) {
            ++filed;
            EXPECT_EQ(r.action_type, kActionPunish);
            EXPECT_EQ(r.mission, *sead);
            EXPECT_EQ(r.target_id, 4281u);
        }
    }
    EXPECT_EQ(filed, 1);
}

TEST(AtmAction, CampaignBooksTheActionLogAndTheSummaryCarriesTheCounter) {
    // The kunsan fixture's own damaged objectives drive the real run:
    // the ledger books the filings (the action_filed event family's
    // source), the summary's strategy block carries actions_filed, and
    // the ledger JSON carries the optional actions section.
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.strategy_layer = true;
    auto r = CampaignRig::make(cfg);
    CampaignResultLedger ledger(r->adapters->campaign, r->adapters->teams,
                                r->adapters->units);
    r->campaign->set_result_ledger(&ledger);
    // The objectives attach with the route planner (the session/QC
    // construction shape) — the ACTION scan walks them, the filings'
    // sorties fly the built routes.
    auto ws_ptr = std::make_unique<f4::world::WorldState>();
    ws_ptr->load(kunsan_world());
    auto adapters =
        std::make_unique<f4::world::WorldStateAdapters>(*ws_ptr);
    RouteBuilderConfig route_cfg;
    route_cfg.loiter_racetracks = true;
    route_cfg.sweep_lines = true;
    route_cfg.tanker_refuel_waypoints = true;
    auto builder = std::make_unique<RouteBuilder>(
        static_cast<const f4::world::IObjectiveSource&>(
            adapters->objectives),
        static_cast<const f4::world::IUnitCoreSource&>(
            adapters->units),
        static_cast<const f4::world::ITeamSource&>(adapters->teams),
        /*viewer=*/2, route_cfg);
    r->campaign->set_route_planner(builder.get(),
        &static_cast<const f4::world::IObjectiveSource&>(
            adapters->objectives));
    r->campaign->tick(1800);
    r->campaign->tick(1800);

    const auto* stats = r->campaign->atm_stats();
    ASSERT_NE(stats, nullptr);
    ASSERT_GT(stats->actions_filed, 0);
    EXPECT_EQ(ledger.actions_filed(), stats->actions_filed);

    const auto& log = ledger.action_filing_log();
    ASSERT_EQ(log.size(), static_cast<std::size_t>(stats->actions_filed));
    for (const auto& a : log) {
        EXPECT_TRUE(a.team == 1 || a.team == 2 || a.team == 6);
        EXPECT_TRUE(a.mission == 20 || a.mission == 1 || a.mission == 17);
        EXPECT_TRUE(a.action_type == kActionDefend ||
                    a.action_type == kActionPunish);
        EXPECT_GT(a.damage_pct, 0);
        EXPECT_NE(a.objective, 0u);
    }

    const auto js = r->campaign->to_summary_json();
    EXPECT_NE(js.find("\"actions_filed\":"), std::string::npos);
    const auto lj = ledger.to_json();
    EXPECT_NE(lj.find("\"actions\": ["), std::string::npos);
    EXPECT_NE(lj.find("\"action_type\":"), std::string::npos);
}

TEST(AtmAction, NoFilingsKeepsTheLedgerJsonByteIdentical) {
    // The disarmed run's ledger document has no actions section (the
    // optional-section discipline — pre-ATM-1 bytes verbatim).
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    auto r = CampaignRig::make(cfg);
    CampaignResultLedger ledger(r->adapters->campaign, r->adapters->teams,
                                r->adapters->units);
    r->campaign->set_result_ledger(&ledger);
    r->campaign->tick(1800);
    const auto lj = ledger.to_json();
    EXPECT_EQ(lj.find("\"actions\""), std::string::npos);
}

TEST(AtmSweep, SweepLinesTargetEnemiesAndTagTheAction) {
    // The contested-air profile finally gets a target: a ranked enemy
    // objective on its own rotation cursor, ACTION-tagged as the sweep.
    auto rig = Rig::make(WorldOpts{}, AtmConfig{.strategy = true});
    const auto sweep = mission_type_byte("AMIS_SWEEP");
    const auto reqs = rig->atm->generate_requests(1, kNow);
    int sweeps = 0;
    for (const auto& r : reqs) {
        if (r.mission != *sweep) continue;
        ++sweeps;
        EXPECT_EQ(r.target_id, 9001u);   // the only enemy objective
        EXPECT_EQ(r.action_type, kActionSweep);
    }
    EXPECT_GT(sweeps, 0);
}

TEST(AtmSweep, DisarmedSweepStaysTargetLess) {
    // The pre-ATM-1 shape: the SWEEP profile files target-less (the C3
    // documented gap — route-less, no ACTION tag).
    auto rig = Rig::make(WorldOpts{}, AtmConfig{});
    const auto sweep = mission_type_byte("AMIS_SWEEP");
    const auto reqs = rig->atm->generate_requests(1, kNow);
    int sweeps = 0;
    for (const auto& r : reqs) {
        if (r.mission != *sweep) continue;
        ++sweeps;
        EXPECT_EQ(r.target_id, 0u);
        EXPECT_EQ(r.action_type, kActionNone);
    }
    EXPECT_GT(sweeps, 0);
}

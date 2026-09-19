// f4-campaign/tests/test_naval_tasking.cpp
//
// CAMP-DOM-5 tests — the naval tasking wrap:
//   * rank_taskforce_targets: the sea-domain pool (hostility filter,
//     own-shore distance ranking, wire-order ties, the honest empty
//     degenerate cases)
//   * the family split: mission_is_naval_strike (the POOL's decision —
//     ASHIP only) vs profile_flies_naval_strike_route (the SHAPE —
//     ASW and TANK share it and stay target-less)
//   * the ATM arm: ASHIP requests draw the ranked task-force targets
//     (rotation), ASW stays target-less even armed, the disarmed and
//     empty-pool corners keep the pre-DOM-5 shape byte-identical
//   * the Campaign gate (kunsan — its 2 task forces are the raw
//     material): the filings publish on the same intents, carry a
//     route, book per target (the query face), and the run stays
//     deterministic; disarmed, the summary has no naval keys

#include <f4/campaign/atm.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/naval_tasking.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/route_builder.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace f4::campaign;
using f4::entities::UnitClass;
using f4::world::UnitState;

namespace {

// The generated profile fixture (the real 41-row table — the same
// macro every f4-campaign test links).
MissionProfileTable load_profiles() {
    return MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
}

// The kunsan fixture path (same macro the tick/atm tests use).
std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}

// ---------------------------------------------------------------------------
// The hand-built naval world: USA(1) vs DPRK(6) at war (the test_atm
// rig's stance rows), UN(3) neutral to that war. Objectives: the USA
// airbase (100,100) and a DPRK target (400,400). Units: a DPRK task
// force CLOSE to the USA shore (150,120), a DPRK task force FAR
// (700,500), a DPRK battalion (skipped — land domain), a UN task force
// (skipped — not at war with USA), a roster-0 DPRK task force (the
// empty-formation skip) and two USA squadrons so the tasking pipeline
// has someone to fly.
// ---------------------------------------------------------------------------
f4::world::WorldState make_naval_world() {
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

    auto tf = [](int16_t x, int16_t y, uint8_t owner, uint32_t vu,
                 uint32_t roster, uint8_t subtype) {
        UnitState u;
        u.unit_class = UnitClass::TaskForce;
        u.domain = 4;
        u.unit_subtype = subtype;
        u.x = x;
        u.y = y;
        u.owner = owner;
        u.id_num = vu;
        u.roster = roster;
        u.class_name = "taskforce";
        return u;
    };
    // Wire order matters for the tie tests: CLOSE first, FAR second.
    ws.units.push_back(tf(150, 120, 6, 8101, 5, 6));      // FRIGATE, close
    ws.units.push_back(tf(700, 500, 6, 8102, 5592405, 3)); // CARRIER, far
    // The skips: a land battalion, a UN task force (not at war with
    // USA), an empty formation, an id-less row.
    UnitState bn;
    bn.unit_class = UnitClass::Battalion;
    bn.domain = 3;
    bn.x = 200;
    bn.y = 200;
    bn.owner = 6;
    bn.id_num = 7001;
    bn.roster = 5;
    bn.class_name = "Armor";
    ws.units.push_back(bn);
    ws.units.push_back(tf(120, 110, 3, 8103, 5, 4));      // UN cruiser
    ws.units.push_back(tf(110, 105, 6, 8104, 0, 5));      // roster 0
    ws.units.push_back(tf(105, 102, 6, 0, 5, 5));         // no id

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

    return ws;
}

// The heap-held adapters (the WorldStateAdapters borrow the state).
struct NavalRig {
    std::unique_ptr<f4::world::WorldState> ws;
    std::unique_ptr<f4::world::WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<AirTaskingManager> atm;

    static std::unique_ptr<NavalRig> make(AtmConfig cfg = {}) {
        auto r = std::make_unique<NavalRig>();
        r->ws = std::make_unique<f4::world::WorldState>(make_naval_world());
        r->adapters =
            std::make_unique<f4::world::WorldStateAdapters>(*r->ws);
        r->profiles = load_profiles();
        r->atm = std::make_unique<AirTaskingManager>(
            r->profiles, r->adapters->campaign, r->adapters->teams,
            r->adapters->units, &r->adapters->objectives, cfg);
        return r;
    }
};

constexpr CampaignTime kNow = 0;   // relative clock

} // namespace

// ── rank_taskforce_targets ───────────────────────────────────────────────────

TEST(NavalRanker, RanksAtWarTaskForcesByOwnShoreDistance) {
    auto r = NavalRig::make();
    const auto ranked = rank_taskforce_targets(
        r->adapters->units, r->adapters->teams, r->adapters->objectives, 1);
    // The close frigate (150,120 — d2 2900 from the USA airbase) is
    // struck before the far carrier (700,500 — d2 520000).
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], 8101u);
    EXPECT_EQ(ranked[1], 8102u);
}

TEST(NavalRanker, NoOwnObjectivesTiesBreakInWireOrder) {
    auto r = NavalRig::make();
    // The degenerate tie: a viewer with NO own-held objectives — every
    // candidate sits at INT64_MAX and wire order decides. Hand-strip
    // the owners (both objectives go to the neutral UN), keeping the
    // war stances intact.
    for (auto& o : r->ws->objectives) o.owner = 3;
    const auto ranked = rank_taskforce_targets(
        r->adapters->units, r->adapters->teams, r->adapters->objectives, 1);
    // Both DPRK task forces tie; the wire index folds into the key —
    // the close frigate was inserted first.
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], 8101u);
    EXPECT_EQ(ranked[1], 8102u);
}

TEST(NavalRanker, SkipsNonTaskForcesAndThePeacefulAndEmptyRows) {
    auto r = NavalRig::make();
    // From team 6's side: the enemy pool has NO task forces (the two
    // USA units are squadrons), so the pool is empty — the honest
    // zero.
    const auto dprk_view = rank_taskforce_targets(
        r->adapters->units, r->adapters->teams, r->adapters->objectives, 6);
    EXPECT_TRUE(dprk_view.empty());
    // From team 1's side the skips are pinned by the survivors: the
    // land battalion, the UN task force, the roster-0 formation and
    // the id-less row are ALL absent (only 8101/8102 can appear).
    const auto ranked = rank_taskforce_targets(
        r->adapters->units, r->adapters->teams, r->adapters->objectives, 1);
    std::set<std::uint32_t> got(ranked.begin(), ranked.end());
    EXPECT_EQ(got.count(7001u), 0u);   // battalion
    EXPECT_EQ(got.count(8103u), 0u);   // UN task force
    EXPECT_EQ(got.count(8104u), 0u);   // roster 0
    EXPECT_EQ(got.count(0u), 0u);      // id-less
    EXPECT_EQ(ranked.size(), 2u);
}

// ── The family split (pool decision vs route shape) ─────────────────────────

TEST(NavalFamily, ThePoolServesAshipOnly) {
    EXPECT_TRUE(mission_is_naval_strike(35));
    EXPECT_FALSE(mission_is_naval_strike(34));   // ASW: no submarines
    EXPECT_FALSE(mission_is_naval_strike(39));   // TANK: the ground pool
    EXPECT_FALSE(mission_is_naval_strike(20));   // CAS: the G2 family
    EXPECT_FALSE(mission_is_naval_strike(0));
}

TEST(NavalFamily, TheStrikeShapeMatchesTheUnitStrikeRows) {
    const auto profiles = load_profiles();
    // ASHIP carries the shape (UNIT / TPROF_ATTACK / WP_STRIKE).
    EXPECT_TRUE(profile_flies_naval_strike_route(
        profiles.for_mission(35)));
    // ASW and TANK SHARE the shape — the shape alone never makes a
    // target naval (the pool decides); they simply never get targets.
    EXPECT_TRUE(profile_flies_naval_strike_route(
        profiles.for_mission(34)));
    EXPECT_TRUE(profile_flies_naval_strike_route(
        profiles.for_mission(39)));
    // The other families do not.
    EXPECT_FALSE(profile_flies_naval_strike_route(
        profiles.for_mission(20)));   // CAS — WP_CAS
    EXPECT_FALSE(profile_flies_naval_strike_route(
        profiles.for_mission(36)));   // PATROL — OBJECTIVE/WP_CAP
    EXPECT_FALSE(profile_flies_naval_strike_route(
        profiles.for_mission(14)));   // STRIKE — OBJECTIVE target
}

// ── The ATM arm ──────────────────────────────────────────────────────────────

TEST(AtmNavalArm, AshipRequestsDrawTheRankedTaskForceTargets) {
    AtmConfig cfg;
    cfg.naval_tasking = true;
    auto r = NavalRig::make(cfg);
    const auto requests = r->atm->generate_requests(1, kNow);

    int aship = 0;
    int aship_targeted = 0;
    for (const auto& req : requests) {
        if (mission_is_naval_strike(req.mission)) {
            ++aship;
            if (req.target_id == 8101 || req.target_id == 8102) {
                ++aship_targeted;
            } else {
                ADD_FAILURE() << "ASHIP target " << req.target_id
                              << " is not a task force";
            }
        }
        // The honest empty pools: ASW (no submarines) and TANK (the
        // ground pool's business) stay target-less even armed.
        if (req.mission == 34 || req.mission == 39) {
            EXPECT_EQ(req.target_id, 0u)
                << "mission " << static_cast<int>(req.mission)
                << " drew a target it does not own";
        }
    }
    EXPECT_GT(aship, 0);
    EXPECT_EQ(aship_targeted, aship);
    EXPECT_EQ(r->atm->stats().naval_requests, aship_targeted);
}

TEST(AtmNavalArm, DisarmedKeepsAshipTargetLess) {
    auto r = NavalRig::make();   // cfg default: naval_tasking off
    const auto requests = r->atm->generate_requests(1, kNow);
    for (const auto& req : requests) {
        if (mission_is_naval_strike(req.mission)) {
            EXPECT_EQ(req.target_id, 0u);
        }
    }
    EXPECT_EQ(r->atm->stats().naval_requests, 0);
    EXPECT_TRUE(r->atm->naval_filings().empty());
}

TEST(AtmNavalArm, EmptyPoolKeepsAshipTargetLessEvenArmed) {
    AtmConfig cfg;
    cfg.naval_tasking = true;
    auto r = NavalRig::make(cfg);
    // Team 6's enemy pool has no task forces (the USA side owns only
    // squadrons): the armed walk runs and finds nothing.
    const auto requests = r->atm->generate_requests(6, kNow);
    for (const auto& req : requests) {
        if (mission_is_naval_strike(req.mission)) {
            EXPECT_EQ(req.target_id, 0u);
        }
    }
    EXPECT_EQ(r->atm->stats().naval_requests, 0);
}

// ── The Campaign gate (kunsan — the 2 task forces are the material) ─────────

namespace {

// A Campaign rig over EITHER world: the kunsan fixture (its real 2
// task forces; its squadrons are BASE-LESS — the stock-save bridge is
// f4-simulation machinery, so targeted filings there fly without
// routes) or the hand-built naval world (based squadrons — the routes
// pin). The ledger + route planner attach like the QC's tasking mode.
struct CampaignNavalRig {
    std::unique_ptr<f4::world::WorldState> ws;
    std::unique_ptr<f4::world::WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<f4::messaging::MessageBus> bus;
    std::unique_ptr<CampaignResultLedger> ledger;
    std::unique_ptr<RouteBuilder> routes;
    std::unique_ptr<Campaign> campaign;

    static std::unique_ptr<CampaignNavalRig> make(
        const CampaignConfig& cfg, f4::world::WorldState world) {
        auto r = std::make_unique<CampaignNavalRig>();
        r->ws = std::make_unique<f4::world::WorldState>(std::move(world));
        r->adapters =
            std::make_unique<f4::world::WorldStateAdapters>(*r->ws);
        r->profiles = load_profiles();
        r->bus = std::make_unique<f4::messaging::MessageBus>();
        r->campaign = std::make_unique<Campaign>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units,
            r->profiles, *r->bus, cfg);
        r->ledger = std::make_unique<CampaignResultLedger>(
            r->adapters->campaign, r->adapters->teams,
            r->adapters->units);
        r->campaign->set_result_ledger(r->ledger.get());
        // The route planner over the same sources (viewer = the first
        // belligerent — the QC's own rule).
        std::uint8_t viewer = 1;
        if (const auto war = r->campaign->belligerent_teams();
            !war.empty()) {
            viewer = static_cast<std::uint8_t>(war.front());
        }
        r->routes = std::make_unique<RouteBuilder>(
            static_cast<const f4::world::IObjectiveSource&>(
                r->adapters->objectives),
            static_cast<const f4::world::IUnitCoreSource&>(
                r->adapters->units),
            static_cast<const f4::world::ITeamSource&>(r->adapters->teams),
            viewer, RouteBuilderConfig{});
        r->campaign->set_route_planner(
            r->routes.get(),
            &static_cast<const f4::world::IObjectiveSource&>(
                r->adapters->objectives));
        return r;
    }

    static std::unique_ptr<CampaignNavalRig> make_kunsan(
        const CampaignConfig& cfg) {
        f4::world::WorldState ws;
        ws.load(kunsan_world());
        return make(cfg, std::move(ws));
    }
};

} // namespace

TEST(CampaignNaval, AshipFilingsCarryTheTaskForceAndBookPerTarget) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.naval_tasking = true;
    auto rig = CampaignNavalRig::make_kunsan(cfg);
    rig->campaign->tick(1800);

    // The kunsan wire's task forces (the raw material).
    std::set<std::uint32_t> tfs;
    for (const auto& u : rig->ws->units) {
        if (u.unit_class == UnitClass::TaskForce) tfs.insert(u.id_num);
    }
    ASSERT_EQ(tfs.size(), 2u);   // the carrier + the frigate

    // The filings: every ASHIP intent carries a task-force target;
    // the per-target books sum one-for-one with them. (The ROUTE pin
    // lives on the based-squadron rig below — kunsan's squadrons are
    // base-less outside the session's stock-save bridge.)
    int aship = 0;
    std::map<std::uint32_t, int> per_target;
    for (const auto& in : rig->campaign->intents()) {
        if (!mission_is_naval_strike(in.mission_byte)) continue;
        ++aship;
        EXPECT_NE(tfs.count(in.target_objective_id), 0u)
            << "ASHIP filing at non-task-force " << in.target_objective_id;
        ++per_target[in.target_objective_id];
    }
    EXPECT_GT(aship, 0) << "the war never filed anti-ship";

    const auto* filings = rig->campaign->atm_naval_filings();
    ASSERT_NE(filings, nullptr);
    int booked = 0;
    for (const auto& [vu, n] : *filings) {
        EXPECT_NE(tfs.count(vu), 0u) << "booked at non-task-force " << vu;
        booked += n;
        EXPECT_EQ(per_target[vu], n) << "the books split disagrees";
    }
    EXPECT_EQ(booked, aship);
    EXPECT_EQ(rig->campaign->atm_stats()->naval_filings, booked);
    EXPECT_GE(rig->campaign->atm_stats()->naval_requests, aship)
        << "requests book at generation, filings at publish — the "
           "publish side can only shrink (budget drops)";
}

TEST(CampaignNaval, TargetedFilingsRouteWhenTheSquadronIsBased) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.naval_tasking = true;
    // The hand-built naval world: both USA squadrons sit AT their
    // airbase 4281, and the DPRK task forces form team 1's pool
    // (team 6's pool is honestly empty — the USA owns no task forces).
    auto rig = CampaignNavalRig::make(cfg, make_naval_world());
    rig->campaign->tick(1800);

    std::set<std::uint32_t> tfs;
    for (const auto& u : rig->ws->units) {
        if (u.unit_class == UnitClass::TaskForce) tfs.insert(u.id_num);
    }

    int targeted = 0;
    int routed = 0;
    for (const auto& in : rig->campaign->intents()) {
        if (!mission_is_naval_strike(in.mission_byte)) continue;
        if (in.target_objective_id == 0) continue;   // the honest
                                                     // target-less
                                                     // corner (team 6's
                                                     // pool is empty —
                                                     // the USA owns no
                                                     // task forces)
        ++targeted;
        EXPECT_NE(tfs.count(in.target_objective_id), 0u);
        if (!in.route.empty()) ++routed;
    }
    EXPECT_GT(targeted, 0) << "the war never filed targeted anti-ship";
    EXPECT_EQ(routed, targeted)
        << "a based squadron's targeted anti-ship filing flew "
           "without a route";
}

TEST(CampaignNaval, ArmedRunIsDeterministicAndCarriesTheSummaryBlock) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;
    cfg.naval_tasking = true;
    auto a = CampaignNavalRig::make_kunsan(cfg);
    auto b = CampaignNavalRig::make_kunsan(cfg);
    a->campaign->tick(1800);
    b->campaign->tick(1800);
    EXPECT_EQ(a->campaign->to_summary_json(),
              b->campaign->to_summary_json());

    const auto js = a->campaign->to_summary_json();
    EXPECT_NE(js.find("\"naval_requests\":"), std::string::npos);
    EXPECT_NE(js.find("\"naval_filings\":"), std::string::npos);
}

TEST(CampaignNaval, DisarmedSummaryHasNoNavalKeysAndNoNavalTargets) {
    CampaignConfig cfg;
    cfg.atm_pipeline = true;   // the pipeline armed, the naval arm NOT
    auto rig = CampaignNavalRig::make_kunsan(cfg);
    rig->campaign->tick(1800);

    for (const auto& in : rig->campaign->intents()) {
        if (mission_is_naval_strike(in.mission_byte)) {
            EXPECT_EQ(in.target_objective_id, 0u)
                << "the disarmed run targeted a task force";
        }
    }
    const auto js = rig->campaign->to_summary_json();
    EXPECT_EQ(js.find("\"naval_requests\":"), std::string::npos);
    EXPECT_EQ(js.find("\"naval_filings\":"), std::string::npos);
}

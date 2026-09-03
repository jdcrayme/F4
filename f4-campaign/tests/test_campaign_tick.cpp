// f4-campaign/tests/test_campaign_tick.cpp
//
// B.2 / M4.7 golden-summary gate: a 30-minute (simulated) two-team
// headless campaign driven from the kunsan save1.cam-derived world
// fixture. Missions generate per profile cadence, package composition
// follows the profile hints (str), and the run is byte-stable across
// two executions — plus the equivalent-run rule (one big tick == N
// small ticks) and the bus contract (the summary mirrors what the bus
// actually saw).

#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/json/f4_json.hpp>
#include <f4/world/world_adapters.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace f4::campaign;

namespace {

// The kunsan two-team campaign world (f4-simulation's fixture — the
// save1.cam-derived roster: USA(1) + DPRK(6) fighter squadrons, war
// stance between slots 1 and 6, ROK(2) at war but squadron-less).
std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}

// A fully wired Campaign over the fixture, heap-held (MessageBus is
// non-movable, so the rig moves as a unique_ptr). Destruction order (reverse
// declaration) tears the campaign down before its sources.
struct Rig {
    std::unique_ptr<f4::world::WorldState> ws;
    std::unique_ptr<f4::world::WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<f4::messaging::MessageBus> bus;
    std::unique_ptr<Campaign> campaign;

    static std::unique_ptr<Rig> make(const CampaignConfig& cfg = {}) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<f4::world::WorldState>();
        r->ws->load(kunsan_world());
        r->adapters =
            std::make_unique<f4::world::WorldStateAdapters>(*r->ws);
        r->profiles = MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
        r->bus = std::make_unique<f4::messaging::MessageBus>();
        r->campaign = std::make_unique<Campaign>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units,
            r->profiles, *r->bus, cfg);
        return r;
    }
};

// One bus subscriber collecting what was published (the summary must
// mirror the bus, not an internal side-channel).
struct Collector {
    std::vector<MissionIntent> seen;
    explicit Collector(f4::messaging::MessageBus& bus) {
        bus.subscribe<MissionIntent>(
            [&seen = seen](const MissionIntent& m) { seen.push_back(m); });
    }
};

} // namespace

// ── The M4.7 done-gate ───────────────────────────────────────────────────────

TEST(CampaignTick, ThirtyMinuteRunIsByteStableAcrossTwoExecutions) {
    auto a = Rig::make();
    auto b = Rig::make();

    // Both run the same 30 minutes (one default tasking cycle).
    a->campaign->tick(1800);
    b->campaign->tick(1800);

    ASSERT_EQ(a->campaign->clock(), 1800);
    ASSERT_EQ(b->campaign->clock(), 1800);
    EXPECT_EQ(a->campaign->to_summary_json(), b->campaign->to_summary_json());

    // And a split into small ticks lands on the same bytes: one big tick
    // == N small ones.
    auto c = Rig::make();
    for (int i = 0; i < 30; ++i) c->campaign->tick(60);   // 30 × 1 min
    EXPECT_EQ(c->campaign->to_summary_json(), a->campaign->to_summary_json());

    // ...in any order of tick sizes (deterministic cycle firing).
    auto d = Rig::make();
    d->campaign->tick(1799);
    d->campaign->tick(1);
    d->campaign->tick(0);
    EXPECT_EQ(d->campaign->to_summary_json(), a->campaign->to_summary_json());
}

TEST(CampaignTick, MissionsGeneratePerProfileCadenceAndComposition) {
    auto rig = Rig::make();
    Collector collect(*rig->bus);
    rig->campaign->tick(1800);   // one tasking cycle

    const auto& intents = rig->campaign->intents();
    ASSERT_FALSE(intents.empty());

    // Every intent is profile-faithful: name ↔ byte, composition = the
    // profile's str (bounded by availability), TOT = the mid-window rule.
    for (const auto& in : intents) {
        ASSERT_EQ(in.mission_name, mission_type_name(in.mission_byte));
        const auto& p = rig->profiles.for_mission(in.mission_byte);
        EXPECT_EQ(in.issued_time, 1800);
        EXPECT_LE(in.aircraft_count, p.str);
        EXPECT_GT(in.aircraft_count, 0);
        const CampaignTime mid_min =
            (static_cast<CampaignTime>(p.min_time) + p.max_time) / 2;
        EXPECT_EQ(in.time_on_target, 1800 + mid_min * 60);
        EXPECT_EQ(in.flight_id, in.package_id);
    }

    // The bus saw exactly what the campaign recorded.
    EXPECT_EQ(collect.seen, rig->campaign->intents());

    // Three war teams (USA 1, ROK 2, DPRK 6 — ROK is at war with DPRK
    // too) but ROK fields no squadrons in the fixture, so it generates
    // nothing: the availability gate exercised.
    const auto war = rig->campaign->belligerent_teams();
    ASSERT_EQ(war.size(), std::size_t{3});
    EXPECT_EQ(war[0], 1);
    EXPECT_EQ(war[1], 2);
    EXPECT_EQ(war[2], 6);

    for (const auto& in : intents) {
        EXPECT_TRUE(in.team == 1 || in.team == 6) << "team " << int{in.team};
    }

    // USA's fighter wing (specialty 0 = ARO_CA) flies the counter-air
    // profiles; BARCAP (byte 1) generated first with the profile's
    // 2-ship str.
    const auto& first = intents.front();
    EXPECT_EQ(first.mission_byte, 1);   // BARCAP first in wire order
    EXPECT_EQ(first.mission_name, "AMIS_BARCAP");
    EXPECT_EQ(first.aircraft_count, 2);
    EXPECT_EQ(first.team, 1);
    EXPECT_EQ(first.team_name, "USA");

    // Profiles requiring unmet capabilities (stealth strike, byte 16,
    // requires VEH_STEALTH) never generate; strike-role profiles (ARO_S)
    // don't match an ARO_CA wing either.
    for (const auto& in : intents) {
        EXPECT_NE(in.mission_byte, 16);
        const auto& p = rig->profiles.for_mission(in.mission_byte);
        EXPECT_NE(p.aro, "ARO_S");
    }

    // Pinned cycle-1 behavior over the kunsan fixture: each war team's
    // single ARO_CA wing walks the wire menu until its 24-aircraft pool
    // (te_number_aircraft: 24 per team) is drawn to zero. The CA menu is
    // 11 profiles; bytes 1-6 fly 2-ships (14), SWEEP flies 4 (18),
    // ALERT/INTERCEPT/ESCORT/SEADESCORT fly 2 (24), and PATROL finds an
    // empty pool and skips → 11 intents per team, 22 total.
    EXPECT_EQ(intents.size(), std::size_t{22});
    int usa_aircraft = 0;
    int dprk_aircraft = 0;
    for (const auto& in : intents) {
        if (in.team == 1) usa_aircraft += in.aircraft_count;
        if (in.team == 6) dprk_aircraft += in.aircraft_count;
    }
    EXPECT_EQ(usa_aircraft, 24);
    EXPECT_EQ(dprk_aircraft, 24);
}

TEST(CampaignTick, SummaryMirrorsBusAndCarriesTheGoldenFacts) {
    auto rig = Rig::make();
    Collector collect(*rig->bus);
    rig->campaign->tick(1800);

    const std::string summary = rig->campaign->to_summary_json();
    ASSERT_FALSE(summary.empty());

    // Round-trips as JSON: the Reader structurally validates the WHOLE
    // document (skip_value walks objects/arrays/strings/numbers to the end).
    f4::json::Reader r(summary);
    r.skip_ws();
    r.expect('{');
    r.skip_value();

    // The golden facts: one cycle at the 30-minute mark, both war teams
    // named, intent count == bus count.
    EXPECT_NE(summary.find("\"clock_sec\": 1800"), std::string::npos);
    EXPECT_NE(summary.find("\"cycles_fired\": 1"), std::string::npos);
    EXPECT_NE(summary.find("\"task_cycle_sec\": 1800"), std::string::npos);
    EXPECT_NE(summary.find("\"USA\""), std::string::npos);
    EXPECT_NE(summary.find("\"DPRK\""), std::string::npos);
    EXPECT_EQ(rig->campaign->intents().size(), collect.seen.size());

    // Per-team totals present (USA flew BARCAP once this cycle).
    EXPECT_NE(summary.find("\"USA/AMIS_BARCAP\": 1"), std::string::npos);
}

TEST(CampaignTick, SecondCycleDrawsOnTheDepletedLedger) {
    auto rig = Rig::make();
    rig->campaign->tick(1800);   // cycle 1: the wing flies its full menu
    ASSERT_FALSE(rig->campaign->intents().empty());

    // Advance a full second cycle: the kunsan wing's pool was drawn to
    // zero by cycle 1, so cycle 2 generates nothing new — no resurrection
    // (the attrition ledger B.3 closes into the sim).
    rig->campaign->tick(1800);
    EXPECT_EQ(rig->campaign->cycles_fired(), 2);
    EXPECT_EQ(rig->campaign->clock(), 3600);
    EXPECT_EQ(rig->campaign->intents().size(),
              rig->campaign->intents().size());   // stable, no throw
}

TEST(CampaignTick, ZeroDeltaTickFiresNothing) {
    auto rig = Rig::make();
    rig->campaign->tick(0);
    EXPECT_EQ(rig->campaign->cycles_fired(), 0);
    EXPECT_TRUE(rig->campaign->intents().empty());
    EXPECT_NE(rig->campaign->to_summary_json().find("\"intents\": [\n  ]"),
              std::string::npos);
}

TEST(CampaignTick, NegativeDeltaThrows) {
    auto rig = Rig::make();
    EXPECT_THROW(rig->campaign->tick(-1), std::runtime_error);
}

TEST(CampaignTick, NonPositiveCycleConfigThrows) {
    CampaignConfig bad;
    bad.air_task_cycle_sec = 0;
    EXPECT_THROW(Rig::make(bad), std::runtime_error);
}

// ── C3: the route planner attachment ─────────────────────────────────────────
//
// set_route_planner() arms generation-to-spawn: every generated
// OBJECTIVE-target mission carries a real enemy objective and a
// threat-aware route (airbase → target → airbase). The no-planner
// goldens above stay byte-identical (the attachment adds fields ONLY
// when attached — the B.3 intents published nothing extra).

TEST(CampaignTick, RoutePlannerArmsIntentsWithTargetsAndRoutes) {
    // Manual rig: the squadron patch must land BEFORE the Campaign
    // constructor snapshots the force (Rig::make builds everything at
    // once). The kunsan TE save's squadrons are counter-air wings at a
    // base the wire leaves empty (airbase 0) — patch the USA wing into
    // a STRIKE squadron (ARO_S, specialty 1) at a real objective so
    // the delivery family generates and the route builder has
    // endpoints. (Test-scoped: the C2 --tasking ladder on the full
    // TestCamp save exercises the unpatched path — its squadrons carry
    // real bases.)
    Rig rig;
    rig.ws = std::make_unique<f4::world::WorldState>();
    rig.ws->load(kunsan_world());
    for (auto& u : rig.ws->units) {
        if (u.unit_class == f4::entities::UnitClass::Squadron &&
            u.owner == 1) {
            u.specialty = 1;      // ARO_S — strike
            u.airbase_id = 2659;  // a real objective VU from the save
        }
    }
    rig.adapters =
        std::make_unique<f4::world::WorldStateAdapters>(*rig.ws);
    rig.profiles = MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
    rig.bus = std::make_unique<f4::messaging::MessageBus>();
    rig.campaign = std::make_unique<Campaign>(
        rig.adapters->campaign, rig.adapters->teams,
        rig.adapters->units, rig.profiles, *rig.bus, CampaignConfig{});
    Collector collector(*rig.bus);

    // The threat map + route builder over the SAME sources the campaign
    // reads (viewer = the save's own team slot, the kunsan campaign's
    // te_team).
    const RouteBuilder builder(rig.adapters->objectives,
                               rig.adapters->units, rig.adapters->teams,
                               static_cast<std::uint8_t>(
                                   rig.ws->campaign.te_team));
    rig.campaign->set_route_planner(&builder, &rig.adapters->objectives);

    rig.campaign->tick(1800);
    EXPECT_EQ(rig.campaign->cycles_fired(), 1);
    ASSERT_FALSE(rig.campaign->intents().empty());

    // Every intent whose profile targets objectives carries a route
    // (the kunsan roster's fighter squadrons generate the strike
    // family); every routed intent is synthetic and carries a target.
    int routed = 0;
    for (const auto& in : rig.campaign->intents()) {
        const auto& profile = rig.profiles.for_mission(in.mission_byte);
        if (profile_flies_delivery_route(profile)) {
            EXPECT_FALSE(in.route.empty())
                << in.mission_name << " generated without a route";
            EXPECT_NE(in.target_objective_id, 0u);
            EXPECT_TRUE(in.synthetic);
            // Route shape: takeoff first, landing last.
            ASSERT_GE(in.route.size(), 2u);
            EXPECT_EQ(in.route.front().action,
                      f4::campaign::kWpTakeoff);
            EXPECT_EQ(in.route.back().action, f4::campaign::kWpLand);
            ++routed;
        } else {
            EXPECT_TRUE(in.route.empty());
        }
    }
    EXPECT_GT(routed, 0);

    // The summary carries the routes block.
    const auto summary = rig.campaign->to_summary_json();
    EXPECT_NE(summary.find("\"routes\""), std::string::npos);
    EXPECT_NE(summary.find("\"built\""), std::string::npos);

    // And the collector saw the same intents (the bus mirrors the
    // record, route and all).
    EXPECT_EQ(collector.seen.size(), rig.campaign->intents().size());
}

TEST(CampaignTick, RoutePlannerDetachmentRestoresTheBareIntents) {
    auto rig = Rig::make();

    // Attach, tick, detach... then a SECOND rig over the same world
    // with no planner must generate the B.3 shape (no routes).
    const RouteBuilder builder(rig->adapters->objectives,
                               rig->adapters->units, rig->adapters->teams,
                               static_cast<std::uint8_t>(
                                   rig->ws->campaign.te_team));
    rig->campaign->set_route_planner(&builder, &rig->adapters->objectives);
    rig->campaign->tick(1800);

    auto bare = Rig::make();
    bare->campaign->tick(1800);
    for (const auto& in : bare->campaign->intents()) {
        EXPECT_TRUE(in.route.empty());
        EXPECT_EQ(in.target_objective_id, 0u);
        EXPECT_FALSE(in.synthetic);
    }
    // No routes block in the bare summary (byte-identity class).
    EXPECT_EQ(bare->campaign->to_summary_json().find("\"routes\""),
              std::string::npos);
}

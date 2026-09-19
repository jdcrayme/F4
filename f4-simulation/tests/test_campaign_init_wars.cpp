// test_campaign_init_wars.cpp — the CAMP-INIT-1 war gates.
//
// The acceptance contract (CAMP_HOST_PLAN.md §8): "a generated save
// decodes in the existing reader; the C5 24-hour harness passes on a
// generated save."
//
// The worlds under test are the BUILD-TIME campinit fixtures — each is
// the decode of the generated .cam (campinit → CamWriter →
// CamArchive::load_from_memory → to_world_json), so the harness runs on
// exactly the bytes the Task-70 encoder stack produced. The 24-hour run
// on the small war is the gate; medium/large/twinwars pin determinism
// and the two-war-pair bed at compressed horizons.

#include <f4/simulation/campaign_war_harness.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path generated_world(const char* name) {
    return std::filesystem::path(F4_CAMPINIT_FIXTURES_DIR) /
           (std::string("campinit_") + name + ".world.json");
}
std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}
std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

bool fixtures_ready() {
    return std::filesystem::exists(f16_config()) &&
           std::filesystem::exists(generated_world("small"));
}

// The generated-war rig: the REAL tasking pipeline (the generated
// teams carry the stock profile and their ATM airbase rows), the
// campaign's own default tasking cadence, ground war armed (the packs
// carry battalions), tiered fidelity for the long horizons.
WarHarnessOptions make_opts(const char* world, std::int64_t horizon_sec,
                            double sample_sec) {
    WarHarnessOptions o;
    o.session.world_json = generated_world(world);
    o.session.class_table = class_table();
    o.session.aircraft_config = f16_config();
    o.session.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.session.tasking_cycle_sec = 1800;
    o.session.atm_pipeline = true;
    o.session.ground_war = true;
    o.session.fidelity_policy = FidelityPolicy::Tiered;
    o.session.max_flights = 24;
    o.horizon_sec = horizon_sec;
    o.sample_sec = sample_sec;
    o.runs = 2;
    return o;
}

bool is_hex_32(const std::string& s) {
    if (s.size() != 32) return false;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c)) &&
            (c < 'a' || c > 'f')) {
            return false;
        }
    }
    return true;
}

void expect_green(const WarReport& r) {
    const WarVerdict& v = r.verdict;
    EXPECT_TRUE(v.drew_aircraft) << "a generated war must draw aircraft";
    EXPECT_TRUE(v.routes_built);
    EXPECT_TRUE(v.materialized);
    EXPECT_TRUE(v.packages_built);
    EXPECT_TRUE(v.deterministic);
    EXPECT_TRUE(v.ledger_consistent) << v.ledger_drift;
    EXPECT_TRUE(v.entities_bounded) << v.entity_leak;
    EXPECT_TRUE(v.war_alive) << v.war_stall;
}

} // namespace

// ── THE gate: the C5 24-hour harness passes on a generated save ─────────

TEST(CampaignInitWars, GeneratedSmallWarPassesThe24HourHarness) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness = CampaignWarHarness::create(
        make_opts("small", 86400, 3600.0), &err);
    ASSERT_NE(harness, nullptr) << err;

    const WarReport r = harness->execute();
    EXPECT_EQ(r.diary.size(), 24u) << "one row per sample hour";
    expect_green(r);
    // The determinism certificate: two 24-hour passes over the
    // generated save, byte-identical books.
    EXPECT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}

// ── the fleet scales: medium and large wars certify too ──────────────────

TEST(CampaignInitWars, GeneratedMediumWarIsDeterministicAndAlive) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness =
        CampaignWarHarness::create(make_opts("medium", 7200, 1800.0), &err);
    ASSERT_NE(harness, nullptr) << err;
    const WarReport r = harness->execute();
    expect_green(r);
    EXPECT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}

TEST(CampaignInitWars, GeneratedLargeWarIsDeterministicAndAlive) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness =
        CampaignWarHarness::create(make_opts("large", 7200, 1800.0), &err);
    ASSERT_NE(harness, nullptr) << err;
    const WarReport r = harness->execute();
    expect_green(r);
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}

// ── the G1 two-war-pair bed at harness level ─────────────────────────────
// The twin-wars pack carries THREE at-war relationships (1↔6, 2↔6,
// 3↔7). The engine's own rule fights the FIRST pair in slot order —
// (1,6) — and the other armed sides' BATTALIONS stand down (the G1
// limitation's engine-level pin lives in f4-campaign's
// test_ground_war.cpp). The per-team ATM is deliberately NOT pair-
// gated — each team tasking against its own war rows is the C4
// design — so the harness-level pins are: the war certifies green
// with the third sides in the world, and the war pair drew.

TEST(CampaignInitWars, TwinWarsHarnessCertifiesWithTheThirdSidesStandingDown) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness =
        CampaignWarHarness::create(make_opts("twinwars", 7200, 1800.0), &err);
    ASSERT_NE(harness, nullptr) << err;
    const WarReport r = harness->execute();
    expect_green(r);
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);

    // The war pair (USA 1 / DPRK 6) drew aircraft; the ground picture
    // carried every pack battalion into the war alive.
    ASSERT_FALSE(r.diary.empty());
    const auto& teams = r.diary.back().teams;
    const auto find_team = [&](int slot) -> const WarHourSample::TeamPool* {
        for (const auto& t : teams)
            if (t.slot == slot) return &t;
        return nullptr;
    };
    const auto* usa = find_team(1);
    const auto* dprk = find_team(6);
    ASSERT_NE(usa, nullptr);
    ASSERT_NE(dprk, nullptr);
    EXPECT_GT(usa->drawn_total, 0) << "the first war pair draws";
    EXPECT_GT(dprk->drawn_total, 0);
    EXPECT_GT(r.diary.back().ground_battalions, 0)
        << "all five sides' battalions entered the world";
}

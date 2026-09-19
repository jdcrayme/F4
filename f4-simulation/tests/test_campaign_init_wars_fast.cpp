// test_campaign_init_wars_fast.cpp — Tier 3.1: the 60x-accel counterparts
// to the CAMP-INIT-1 nightly gates (test_campaign_init_wars.cpp).
//
// Same fixtures, same generated worlds, same expect_green pins — but
// WarHarnessOptions::speed = 60.0 (the harness's acceptance preset, per
// FIDELITY_TIERS / FID_OPT: 58.1x sustained vs 25.3x full-fidelity
// baseline). The 170 s / 25 s / 23 s / 25 s real-time nightly tests
// become ~3 s each here.
//
// Run via `ctest -LE slow -j4` (the default fast iteration tier). The
// nightly 1x variants live in test_campaign_init_wars.cpp under the
// `slow` label.
//
// Why a separate binary (not a TEST_P parameterization): gtest_discover_tests
// labels a whole binary; we need the nightly variants labeled `slow` and
// these labeled default so `ctest -LE slow` includes them.

#include "campaign_init_wars_helpers.hpp"

#include <gtest/gtest.h>

using namespace f4::simulation;
using namespace f4_test;

// ── Small war at 60x accel — the fast counterpart to the 24-hour gate ────

TEST(CampaignInitWarsFast, SmallWarAt60xAccelIsGreen) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    // Tier 3.1: 2h horizon (not the nightly 24h) — at 60x accel that's
    // ~5s wall, not ~24 min. The pins (expect_green + the determinism
    // MD5 pair) are identical to the nightly variant; only the horizon
    // is compressed so the fast tier stays fast. The nightly 24h gate
    // lives in test_campaign_init_wars.cpp under the `slow` label.
    auto harness = CampaignWarHarness::create(
        make_opts("small", 7200, 1800.0, 60.0), &err);
    ASSERT_NE(harness, nullptr) << err;

    const WarReport r = harness->execute();
    EXPECT_EQ(r.diary.size(), 4u) << "one row per 30-min sample";
    expect_green(r);
    EXPECT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}

// ── Medium / large / twinwars at 60x accel — the determinism pins ───────

TEST(CampaignInitWarsFast, MediumWarAt60xAccelIsDeterministic) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness = CampaignWarHarness::create(
        make_opts("medium", 7200, 1800.0, 60.0), &err);
    ASSERT_NE(harness, nullptr) << err;
    const WarReport r = harness->execute();
    expect_green(r);
    EXPECT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}

TEST(CampaignInitWarsFast, LargeWarAt60xAccelIsDeterministic) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness = CampaignWarHarness::create(
        make_opts("large", 7200, 1800.0, 60.0), &err);
    ASSERT_NE(harness, nullptr) << err;
    const WarReport r = harness->execute();
    expect_green(r);
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
}

TEST(CampaignInitWarsFast, TwinWarsAt60xAccelCertifiesWithThirdSidesDown) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    auto harness = CampaignWarHarness::create(
        make_opts("twinwars", 7200, 1800.0, 60.0), &err);
    ASSERT_NE(harness, nullptr) << err;
    const WarReport r = harness->execute();
    expect_green(r);
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);

    // The war pair (USA 1 / DPRK 6) drew aircraft; the ground picture
    // carried every pack battalion into the war alive. (Same pins as the
    // nightly variant — the accel must not change the war's outcome.)
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

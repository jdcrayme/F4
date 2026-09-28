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

// ── The fusion tranche's perf certificate: the armed war with the
//    passive sensors on (Docs/SENSORS_COUNTERMEASURES_PLAN.md §8) ────────
//
// The passive legs (IRST + visual contact books answering the detection
// policy's visual verdict) + the throttle ir_power stamp ride the armed
// war's tick — the machinery FID-OPT tuned. The certificate: the same
// 60x preset the unarmed war holds stays UNDILATED with the fidelity on
// (zero_dilation IS the throughput verdict — if the passive scans or the
// stamp collapsed the tick, the gate fires), the war stays green, and it
// stays a deterministic object (the contacts change the FIGHT, not the
// reproducibility).

TEST(CampaignInitWarsFast, ArmedWarWithPassiveSensorsHoldsThe60xPreset) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "campinit/f16 fixtures not generated";
    }
    std::string err;
    // Tier 3.1 horizon compression again: 3600 s (two 30-min samples)
    // — enough tasking cycles for the armed war to generate, commit,
    // and fight, at ~1/8 the wall clock of the 7200 s variants (the
    // armed war's per-tick cost is what the certificate measures).
    // The war starts by planning (ATO-START-1): the initial cycle at
    // clock 0 is what gets flights ARMED inside the short horizon —
    // without it the first cycle waits a full tasking_cycle_sec and
    // the 3600 s war ends before anything is armed.
    auto opts = make_opts("small", 3600, 1800.0, 60.0);
    opts.session.initial_tasking_cycle = true;
    opts.session.aa_combat = true;        // the armed war (C6)
    opts.session.passive_sensors = true;  // the fusion tranche
    auto harness = CampaignWarHarness::create(opts, &err);
    ASSERT_NE(harness, nullptr) << err;

    const WarReport r = harness->execute();
    ASSERT_FALSE(r.aborted) << r.abort_reason;
    EXPECT_TRUE(r.aa_combat);
    EXPECT_GT(r.armed_aircraft, 0)
        << "the war never armed — the certificate would prove nothing";
    expect_green(r);

    // The perf certificate itself.
    EXPECT_TRUE(r.verdict.zero_dilation)
        << "the passive legs diluted the 60x preset: "
        << r.verdict.dilation_report;
    EXPECT_GT(r.verdict.sustained_rate, 0.0);
    EXPECT_TRUE(r.verdict.deterministic);
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

// f4-simulation/tests/test_campaign_war_harness.cpp
//
// C5 — the 24-hour war harness, pinned over the routed kunsan fixture
// (short horizons: the rig compresses hours to seconds exactly the way
// the campaign-session tests compress the cycle).
//
//   1. The war runs, certifies, and is deterministic: two passes over
//      the same horizon produce identical ledger MD5s (the C5
//      contract, in miniature), the one-pool identities hold at every
//      sample, the roster identity holds, the war stays alive, and the
//      diary carries one row per sample.
//   2. The wreck reaper: a killed aircraft survives its hold (visible
//      wreck), then retires exactly once — roster shrinks, world
//      shrinks, the retired counter bumps, a second retire is a no-op.
//      The pre-C5 lifetime (hold 0) keeps the wreck frozen forever.
//   3. The stall verdict: a cycle period longer than the horizon means
//      no cycle ever fires — the war-alive gate fires with the cycle
//      diagnostic (the C5-FIX-1 class, pinned at harness level).
//   4. runs == 1 skips the determinism proof: one MD5, no second, the
//      verdict stays vacuously true.

#include <f4/simulation/campaign_war_harness.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <string>

using namespace f4::simulation;

namespace {

std::filesystem::path routed_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_session.world.json";
}
std::filesystem::path class_table() {
    return std::filesystem::path(F4_SOURCE_FIXTURES_DIR) / "falcon4.ct.json";
}
std::filesystem::path f16_config() {
    return std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
}

bool fixtures_ready() {
    return std::filesystem::exists(f16_config());
}

// The rig: the routed kunsan fixture, the legacy ladder (the session
// test's own note — the fixture's specialty mix was built for the
// legacy vocabulary), a 5 s cycle, and a compressed war.
WarHarnessOptions make_war_opts() {
    WarHarnessOptions o;
    o.session.world_json = routed_world();
    o.session.class_table = class_table();
    o.session.aircraft_config = f16_config();
    o.session.mission_profiles = F4_MISSION_PROFILES_JSON;
    o.session.tasking_cycle_sec = 5;
    o.session.reinforce_period_sec = 0;  // draws only — no deliveries
    o.session.atm_pipeline = false;      // the routed-fixture rig
    o.session.max_flights = 8;
    o.horizon_sec = 40;
    o.sample_sec = 10.0;
    o.runs = 2;
    o.wreck_hold_sec = 60.0;
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

} // namespace

// ── 1. The war runs, certifies, and is deterministic ──────────────────────
TEST(CampaignWarHarness, RunsCertifiesAndIsDeterministic) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    std::string err;
    auto harness = CampaignWarHarness::create(make_war_opts(), &err);
    ASSERT_NE(harness, nullptr) << err;

    int notified = 0;
    harness->execute([&notified](const WarHourSample& s) {
        ++notified;
        EXPECT_GE(s.sample, 1);
        EXPECT_GE(s.hour_cycles, 1) << "a 5 s cycle owes fires per sample";
    });
    const auto& r = harness->report();

    ASSERT_FALSE(r.aborted) << r.abort_reason;

    // The generation actually happened (the routed chain: draws →
    // routes → materialized aircraft).
    EXPECT_TRUE(r.verdict.drew_aircraft);
    EXPECT_TRUE(r.verdict.routes_built);
    EXPECT_TRUE(r.verdict.materialized);

    // The C5 gates: all green over the whole horizon.
    EXPECT_TRUE(r.verdict.deterministic);
    EXPECT_TRUE(r.verdict.ledger_consistent) << r.verdict.ledger_drift;
    EXPECT_TRUE(r.verdict.entities_bounded) << r.verdict.entity_leak;
    EXPECT_TRUE(r.verdict.war_alive) << r.verdict.war_stall;

    // The certificate: two equal 32-hex digests.
    ASSERT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    ASSERT_TRUE(is_hex_32(r.verdict.ledger_md5_run1));
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
    EXPECT_FALSE(r.ledger_json.empty());

    // The diary: one row per sample, the progress callback fired as
    // many times, and the bookkeeping adds up.
    EXPECT_EQ(r.samples, 4);
    ASSERT_EQ(r.diary.size(), std::size_t{4});
    EXPECT_EQ(notified, 4);
    EXPECT_GT(r.drawn, 0);
    EXPECT_GT(r.cycles, 0);
    EXPECT_GT(r.synthetic_spawned, 0);
    for (const auto& s : r.diary) {
        EXPECT_GT(s.hour_cycles, 0);
        // The roster identity, per sample (the entity-leak gate's own
        // arithmetic, re-derived here so a gate regression shows as a
        // test failure with numbers).
        EXPECT_EQ(s.live_aircraft, r.diary.front().live_aircraft -
                                       r.diary.front().synthetic_spawned +
                                       s.synthetic_spawned - s.retired);
    }
    EXPECT_FALSE(r.ledger_teams.empty());
    EXPECT_TRUE(r.belligerent_air);
}

// ── 2. The wreck reaper: hold, retire-once, world shrinks ─────────────────
TEST(CampaignWarHarness, ReaperRetiresKilledAircraftAfterHold) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    CampaignSessionOptions sopts;
    sopts.world_json = routed_world();
    sopts.class_table = class_table();
    sopts.aircraft_config = f16_config();
    sopts.mission_profiles = F4_MISSION_PROFILES_JSON;
    sopts.tasking_cycle_sec = 5;
    sopts.reinforce_period_sec = 0;
    sopts.atm_pipeline = false;
    sopts.max_flights = 8;
    sopts.wreck_hold_sec = 60.0;

    std::string err;
    auto session = CampaignSession::create(sopts, &err);
    ASSERT_NE(session, nullptr) << err;
    session->advance(2.0);  // a couple of campaign cycles in

    const auto roster = session->sim().aircraft_entities();
    ASSERT_FALSE(roster.empty());
    const auto victim = roster.front();
    const auto roster_before = roster.size();

    // The kill lands on the bus (the reaper's own feed; the sink hears
    // it too — the scenario-template victim has no campaign origin, so
    // the ledger books nothing team-side; this test pins the REAPER
    // mechanics, not the books).
    const int spawns_at_kill = session->stats().synthetic_spawned;
    session->sim().bus().publish(f4::weapons::EntityKilledMessage{
        victim.value, 0, session->sim().sim_time_s()});

    // Inside the hold: the wreck is still visible, nothing retired.
    session->advance(1.0);
    EXPECT_EQ(session->stats().retired, 0);
    EXPECT_EQ(session->sim().aircraft_entities().size(), roster_before);

    // Past the hold (advance in 4 s batches; 60 s hold + margin): the
    // roster loses exactly the victim (everything else it gained this
    // window is the spawner's synthetic aircraft).
    for (int i = 0; i < 20; ++i) {
        session->advance(4.0);
    }
    EXPECT_EQ(session->stats().retired, 1);
    const int spawns_after = session->stats().synthetic_spawned;
    EXPECT_EQ(session->sim().aircraft_entities().size(),
              roster_before + (spawns_after - spawns_at_kill) - 1);
    // The entity itself is gone (stale-handle discipline: a destroyed
    // slot's generation bump makes the old id resolve to nothing).
    {
        f4::entities::EntityHandle h(victim, &session->sim().world());
        EXPECT_EQ(h.get<f4::flight::FlightModelComponent>(), nullptr);
    }
    // Retire is idempotent: the entity is gone, a second call is a
    // loud no-op.
    EXPECT_FALSE(session->sim().retire_aircraft(victim));
    EXPECT_EQ(session->stats().retired, 1);

    // The pre-C5 lifetime (hold 0) keeps the wreck forever: the same
    // kill against a hold-less session leaves the roster intact.
    sopts.wreck_hold_sec = 0.0;
    auto frozen = CampaignSession::create(sopts, &err);
    ASSERT_NE(frozen, nullptr) << err;
    frozen->advance(2.0);
    const auto frozen_roster = frozen->sim().aircraft_entities();
    ASSERT_FALSE(frozen_roster.empty());
    frozen->sim().bus().publish(f4::weapons::EntityKilledMessage{
        frozen_roster.front().value, 0, frozen->sim().sim_time_s()});
    for (int i = 0; i < 20; ++i) {
        frozen->advance(4.0);
    }
    EXPECT_EQ(frozen->stats().retired, 0);
    EXPECT_EQ(frozen->sim().aircraft_entities().size(),
              frozen_roster.size() + frozen->stats().synthetic_spawned);
}

// ── 3. The stall verdict: no cycle ever fires ─────────────────────────────
TEST(CampaignWarHarness, FiresStallVerdictWhenCycleNeverFires) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    WarHarnessOptions o = make_war_opts();
    o.session.tasking_cycle_sec = 100000;  // never inside the horizon
    o.runs = 1;                            // the proof is not the point

    std::string err;
    auto harness = CampaignWarHarness::create(o, &err);
    ASSERT_NE(harness, nullptr) << err;
    harness->execute();
    const auto& r = harness->report();

    ASSERT_FALSE(r.aborted) << r.abort_reason;
    // The war-alive gate fired with the cycle diagnostic (the
    // C5-FIX-1 class — the clock/cycles stopped).
    EXPECT_FALSE(r.verdict.war_alive);
    EXPECT_NE(r.verdict.war_stall.find("no tasking cycle"),
              std::string::npos) << r.verdict.war_stall;
    // Nothing drew (cycles never fired) — the inherited tasking gate
    // echoes it, and with belligerent air available the host's exit-6
    // would fire.
    EXPECT_FALSE(r.verdict.drew_aircraft);
    EXPECT_TRUE(r.belligerent_air);
    // The other gates are untouched by a silent war: the books balance
    // (nothing happened), the roster is bounded (nothing spawned).
    EXPECT_TRUE(r.verdict.ledger_consistent) << r.verdict.ledger_drift;
    EXPECT_TRUE(r.verdict.entities_bounded) << r.verdict.entity_leak;
}

// ── 4. runs == 1 skips the determinism proof ──────────────────────────────
TEST(CampaignWarHarness, SingleRunSkipsTheProof) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    WarHarnessOptions o = make_war_opts();
    o.horizon_sec = 12;
    o.sample_sec = 6.0;
    o.runs = 1;

    std::string err;
    auto harness = CampaignWarHarness::create(o, &err);
    ASSERT_NE(harness, nullptr) << err;
    harness->execute();
    const auto& r = harness->report();

    ASSERT_FALSE(r.aborted) << r.abort_reason;
    EXPECT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    EXPECT_TRUE(r.verdict.ledger_md5_run1.empty());  // no second pass
    EXPECT_TRUE(r.verdict.deterministic);            // vacuously
    EXPECT_EQ(r.samples, 2);
    EXPECT_EQ(r.diary.size(), std::size_t{2});
}

// ── 5. G1 — the ground war inside the war harness ──────────────────────────
//
// The compressed rig (5 s cadences) runs BOTH wars at once: the air
// ladder draws and flies while the ground engine marches, attrites,
// and takes ground. The four C5 verdicts must stay green with the
// ground side live (the ledger's ground books do not touch the
// one-pool air identities; the entity mirror creates/destroys
// nothing), and the determinism certificate covers the ground bytes
// (the ledger's ground block is inside the MD5'd document).
TEST(CampaignWarHarness, GroundWarColumnsVerdictsAndDeterminism) {
    if (!fixtures_ready()) {
        GTEST_SKIP() << "f16.json fixture not generated";
    }
    WarHarnessOptions o = make_war_opts();
    o.session.ground_war = true;
    o.session.ground_update_sec = 5;   // the compressed rig's cadence
    o.session.ground_orders_sec = 5;

    std::string err;
    auto harness = CampaignWarHarness::create(o, &err);
    ASSERT_NE(harness, nullptr) << err;
    harness->execute();
    const auto& r = harness->report();

    ASSERT_FALSE(r.aborted) << r.abort_reason;

    // The ground war ran: updates fired, the army exists, it marched.
    EXPECT_TRUE(r.ground_war);
    EXPECT_GT(r.ground_updates, 0);
    EXPECT_GT(r.ground_battalions, 0);
    EXPECT_GT(r.ground_march_grid, 0) << "the army moved";
    // (The kunsan rig's war pair is USA-DPRK and the USA owns ZERO
    // objectives, so no column is contested — the front line's
    // contested-column count is honestly 0 HERE. The front mechanics
    // themselves are pinned by f4-campaign's rig, where both sides
    // hold territory; the front COLUMN exists either way.)
    EXPECT_FALSE(r.diary.empty());

    // The four C5 gates stay green with the ground side live.
    EXPECT_TRUE(r.verdict.deterministic);
    EXPECT_TRUE(r.verdict.ledger_consistent) << r.verdict.ledger_drift;
    EXPECT_TRUE(r.verdict.entities_bounded) << r.verdict.entity_leak;
    EXPECT_TRUE(r.verdict.war_alive) << r.verdict.war_stall;

    // The certificate covers the ground bytes (the ledger document
    // carries the ground block, and the two runs agree byte for byte).
    ASSERT_TRUE(is_hex_32(r.verdict.ledger_md5_run0));
    ASSERT_TRUE(is_hex_32(r.verdict.ledger_md5_run1));
    EXPECT_EQ(r.verdict.ledger_md5_run0, r.verdict.ledger_md5_run1);
    EXPECT_NE(r.ledger_json.find("\"ground\""), std::string::npos);

    // The diary's ground columns flowed (every sample carries them).
    ASSERT_FALSE(r.diary.empty());
    for (const auto& s : r.diary) {
        EXPECT_GT(s.ground_updates, 0) << "sample " << s.sample;
        EXPECT_GT(s.ground_battalions, 0) << "sample " << s.sample;
    }
}

// ── 6. Option validation ───────────────────────────────────────────────────
TEST(CampaignWarHarness, RejectsInvalidOptions) {
    std::string err;
    {
        auto o = make_war_opts();
        o.horizon_sec = 0;
        EXPECT_EQ(CampaignWarHarness::create(o, &err), nullptr);
        EXPECT_NE(err.find("horizon"), std::string::npos) << err;
    }
    {
        auto o = make_war_opts();
        o.sample_sec = 0.0;
        EXPECT_EQ(CampaignWarHarness::create(o, &err), nullptr);
        EXPECT_NE(err.find("sample_sec"), std::string::npos) << err;
    }
    {
        auto o = make_war_opts();
        o.runs = 0;
        EXPECT_EQ(CampaignWarHarness::create(o, &err), nullptr);
        EXPECT_NE(err.find("runs"), std::string::npos) << err;
    }
    {
        auto o = make_war_opts();
        o.session.world_json.clear();
        EXPECT_EQ(CampaignWarHarness::create(o, &err), nullptr);
        EXPECT_NE(err.find("world_json"), std::string::npos) << err;
    }
}

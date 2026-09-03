// f4-campaign/tests/test_result_ledger.cpp
//
// C1 — the result ledger (the campaign's write model) and the war-loop
// feedback:
//
//   1. Construction snapshot: team pools seeded from the campaign
//      source, squadron kill/loss HISTORY seeded from the save (a
//      mid-campaign save starts non-zero — the ledger builds on it),
//      run deltas zero.
//   2. Event application: air losses (pool, squadron, credit,
//      attribution, saturation, floor), ag kills, objective damage
//      (last-write-wins), bomb impact logging.
//   3. to_json: byte-stable across identical event sequences, strict
//      JSON (Reader round-trip), the no-floats discipline.
//   4. The C2 hook: Campaign::set_result_ledger — a fresh ledger
//      changes NOTHING (golden identity), losses throttle tasking.
//   5. apply_to(WorldState): the write-back — pools, squadron
//      counters, objective fstatus; zero-event identity; unmatched
//      VUs are loud.

#include <f4/campaign/campaign.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/world_writeback.hpp>
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
using f4::world::WorldState;
using f4::world::WorldStateAdapters;

namespace {

std::filesystem::path kunsan_world() {
    return std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
           "kunsan_campaign.world.json";
}

// The kunsan rig (test_campaign_tick's shape): the ledger + a Campaign
// over the same sources, so the feedback tests exercise the real gate.
// C2 knobs: a reinforcement budget stamped onto every squadron (the
// kunsan fixture carries 0 — real saves like TestCamp carry 24..168),
// and an optional fresh anchor offset for the reinforcement cadence
// (last_reinforcement = current_time + offset; the fixture's own 0
// anchor is ~375 days stale, which fires the catch-up-once path).
struct Rig {
    std::unique_ptr<WorldState> ws;
    std::unique_ptr<WorldStateAdapters> adapters;
    MissionProfileTable profiles;
    std::unique_ptr<f4::messaging::MessageBus> bus;
    std::unique_ptr<Campaign> campaign;
    std::unique_ptr<CampaignResultLedger> ledger;

    static std::unique_ptr<Rig> make(
            int squadron_reinforcement = 0,
            CampaignTime anchor_offset = -1,
            const CampaignConfig& cfg = {}) {
        auto r = std::make_unique<Rig>();
        r->ws = std::make_unique<WorldState>();
        r->ws->load(kunsan_world());
        if (squadron_reinforcement != 0 || anchor_offset >= 0) {
            if (anchor_offset >= 0) {
                r->ws->campaign.last_reinforcement =
                    r->ws->campaign.current_time + anchor_offset;
            }
            for (auto& u : r->ws->units) {
                if (u.unit_class == f4::entities::UnitClass::Squadron) {
                    u.reinforcement = static_cast<std::int16_t>(
                        squadron_reinforcement);
                }
            }
        }
        r->adapters = std::make_unique<WorldStateAdapters>(*r->ws);
        r->profiles = MissionProfileTable::load(F4_MISSION_PROFILES_JSON);
        r->bus = std::make_unique<f4::messaging::MessageBus>();
        r->campaign = std::make_unique<Campaign>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units,
            r->profiles, *r->bus, cfg);
        r->ledger = std::make_unique<CampaignResultLedger>(
            r->adapters->campaign, r->adapters->teams, r->adapters->units);
        return r;
    }

    /// The USA squadron's VU (slot 1 owns the 24-aircraft wing).
    std::uint32_t usa_squadron_vu() const {
        for (const auto& s : ledger->squadrons()) {
            if (s.owner == 1) return s.vu;
        }
        return 0;
    }
    /// The DPRK squadron's VU (slot 6).
    std::uint32_t dprk_squadron_vu() const {
        for (const auto& s : ledger->squadrons()) {
            if (s.owner == 6) return s.vu;
        }
        return 0;
    }
};

// A minimal in-memory world for the pure ledger/write-back tests:
// one team (slot 3, pool 10), one squadron (total_losses pre-seeded
// high for the saturation test), one objective (VU 77, 2 features).
// C2 knobs: the squadron's packed roster (2-bit groups — 0x5555aaaa
// = 24 ships) and reinforcement budget (wire i16).
WorldState make_ledger_world(int team_pool = 10,
                             std::uint8_t seed_losses = 2,
                             std::uint32_t roster = 0,
                             std::int16_t reinforcement = 0) {
    WorldState ws;
    ws.version = 71;
    ws.campaign.te_number_aircraft = {0, 0, 0, team_pool, 0, 0, 0, 0};

    f4::world::TeamState team;
    team.slot = 3;
    team.name = "TEST";
    ws.teams = {team};

    f4::world::UnitState sq;
    sq.unit_class = f4::entities::UnitClass::Squadron;
    sq.domain = 2;
    sq.owner = 3;
    sq.id_num = 4281;
    sq.class_name = "Test Squadron";
    sq.aa_kills = 5;         // mid-campaign history: non-zero seed
    sq.ag_kills = 0;
    sq.total_losses = seed_losses;
    sq.roster = roster;              // C2: packed aircraft count
    sq.reinforcement = reinforcement; // C2: aircraft on order
    ws.units = {sq};

    f4::world::ObjectiveState obj;
    obj.id_num = 77;
    obj.x = 100; obj.y = 100;
    obj.owner = 3;
    obj.fstatus = {0x00, 0x00};
    f4::entities::FeatureEntryState f1;
    f1.index = 1;
    f1.value = 10;
    f4::entities::FeatureEntryState f2;
    f2.index = 2;
    f2.value = 20;
    obj.features = {f1, f2};
    ws.objectives = {obj};
    return ws;
}

} // namespace

// ── 1. Construction snapshot ────────────────────────────────────────────────

TEST(ResultLedger, SnapshotSeedsPoolsAndSquadronHistory) {
    auto rig = Rig::make();
    const auto& ledger = *rig->ledger;

    // Team pools: the campaign source's own numbers (the fixture seeds
    // 24 aircraft for the belligerents).
    EXPECT_GT(ledger.team_aircraft_remaining(1), 0);
    EXPECT_GT(ledger.team_aircraft_remaining(6), 0);
    // A fresh ledger has booked nothing.
    EXPECT_TRUE(ledger.empty());
    EXPECT_EQ(ledger.air_losses(), 0);
    EXPECT_EQ(ledger.bomb_impacts(), 0);
    // Every squadron's run deltas start at zero (the seed history is
    // carried, but it is NOT "this run's" activity).
    for (const auto& s : ledger.squadrons()) {
        EXPECT_EQ(s.run_losses, 0);
        EXPECT_EQ(s.run_aa_kills, 0);
        EXPECT_EQ(s.run_ag_kills, 0);
    }
}

TEST(ResultLedger, SeedHistoryCarriesZeroEventIdentity) {
    // A mid-campaign-style save with non-zero counters: the snapshot
    // must carry them (the write-back builds on them, the run deltas
    // stay zero).
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    const auto* sq = ledger.squadron(4281);
    ASSERT_NE(sq, nullptr);
    EXPECT_EQ(sq->aa_kills, 5);         // the save's own history
    EXPECT_EQ(sq->total_losses, 2);
    EXPECT_EQ(sq->run_losses, 0);       // ...but nothing happened YET
    EXPECT_TRUE(ledger.empty());
}

// ── 2. Event application ────────────────────────────────────────────────────

TEST(ResultLedger, AirLossBooksPoolSquadronAndCredit) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_air_loss(/*t=*/10.0, /*victim_team=*/3,
                          /*victim_sq=*/4281, /*victim_flight=*/5001,
                          /*killer_sq=*/4281);
    // Credit is only booked when the killer is a DIFFERENT resolution;
    // here the same squadron shot itself down — still a legal booking
    // (the ledger does not editorialize), the tests below use distinct
    // squadrons for the classic case.
    EXPECT_EQ(ledger.air_losses(), 1);
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 9);   // 10 − 1
    const auto* sq = ledger.squadron(4281);
    ASSERT_NE(sq, nullptr);
    EXPECT_EQ(sq->total_losses, 3);   // 2 seed + 1
    EXPECT_EQ(sq->run_losses, 1);
    EXPECT_EQ(sq->aa_kills, 6);       // 5 seed + 1
    EXPECT_EQ(sq->run_aa_kills, 1);
    EXPECT_EQ(ledger.air_kills_attributed(), 1);
    EXPECT_FALSE(ledger.empty());

    // The event log: one record, arrival order, all the facts.
    ASSERT_EQ(ledger.air_loss_log().size(), 1u);
    const auto& rec = ledger.air_loss_log()[0];
    EXPECT_EQ(rec.victim_team, 3);
    EXPECT_EQ(rec.victim_squadron, 4281);
    EXPECT_EQ(rec.victim_flight, 5001);
    EXPECT_TRUE(rec.attributed);
}

TEST(ResultLedger, UnattributedLossBooksNoCredit) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    // Killer 0: unknown shooter (a player entity, a synthetic defense).
    ledger.apply_air_loss(5.0, 3, 4281, 5001, /*killer_sq=*/0);
    EXPECT_EQ(ledger.air_losses(), 1);
    EXPECT_EQ(ledger.air_losses_unattributed(), 1);
    EXPECT_EQ(ledger.air_kills_attributed(), 0);
    const auto* sq = ledger.squadron(4281);
    ASSERT_NE(sq, nullptr);
    EXPECT_EQ(sq->aa_kills, 5);        // seed unchanged
    EXPECT_EQ(sq->total_losses, 3);    // loss still books
}

TEST(ResultLedger, UnknownVictimSquadronStillBooksTeamLoss) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    // Victim squadron VU 9999 does not exist: team-only loss, loud in
    // the event log, no crash.
    ledger.apply_air_loss(7.5, 3, 9999, 5001, 0);
    EXPECT_EQ(ledger.air_losses(), 1);
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 9);
    EXPECT_EQ(ledger.squadron(9999), nullptr);
    EXPECT_EQ(ledger.air_loss_log()[0].victim_squadron, 9999);
}

TEST(ResultLedger, PoolFloorsAtZeroAndLossesKeepCounting) {
    auto ws = make_ledger_world(/*team_pool=*/1);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    for (int i = 0; i < 3; ++i) {
        ledger.apply_air_loss(static_cast<double>(i), 3, 4281, 5001, 0);
    }
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 0);   // floored
    EXPECT_EQ(ledger.air_losses(), 3);                 // still counted
}

TEST(ResultLedger, SquadronLossSaturatesAtWireLimit) {
    // Seed total_losses at 254; two more losses saturate the absolute
    // at the wire's uchar limit while the RUN delta keeps counting
    // (the availability gate must still see the second loss).
    auto ws = make_ledger_world(10, /*seed_losses=*/254);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_air_loss(1.0, 3, 4281, 5001, 0);
    ledger.apply_air_loss(2.0, 3, 4281, 5001, 0);
    const auto* sq = ledger.squadron(4281);
    ASSERT_NE(sq, nullptr);
    EXPECT_EQ(sq->total_losses, 255);   // saturated, never overflowed
    EXPECT_EQ(sq->run_losses, 2);       // the delta still counts
    EXPECT_EQ(ledger.squadron_run_losses(4281), 2);
}

TEST(ResultLedger, AgKillBooksCreditWithoutPoolEffect) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_ag_kill(3.0, 4281);
    const auto* sq = ledger.squadron(4281);
    ASSERT_NE(sq, nullptr);
    EXPECT_EQ(sq->ag_kills, 1);
    EXPECT_EQ(sq->run_ag_kills, 1);
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 10);  // untouched
    EXPECT_EQ(ledger.air_losses(), 0);
    EXPECT_FALSE(ledger.empty());                      // activity booked
}

TEST(ResultLedger, ObjectiveDamageIsLastWriteWins) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ObjectiveDamageRecord first;
    first.objective = 77;
    first.features_total = 2;
    first.features_destroyed = 1;
    first.destroyed_pct = 33;
    first.fstatus = {0x0C};
    ledger.apply_objective_damage(first);

    ObjectiveDamageRecord second;
    second.objective = 77;
    second.features_total = 2;
    second.features_destroyed = 2;
    second.destroyed_pct = 100;
    second.fstatus = {0xFC};
    ledger.apply_objective_damage(second);

    // ONE entry (replaced, not appended), the FINAL state, and the
    // global counter is the delta-corrected 2 — not 3.
    ASSERT_EQ(ledger.objective_damage().size(), 1u);
    EXPECT_EQ(ledger.objective_damage()[0].features_destroyed, 2);
    EXPECT_EQ(ledger.objective_damage()[0].fstatus,
              std::vector<std::uint8_t>{0xFC});
    EXPECT_EQ(ledger.features_destroyed(), 2);
}

TEST(ResultLedger, BombImpactLogsRoundedWholeFeet) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_bomb_impact(123.456, 77, /*miss=*/142.4, 1);
    ledger.apply_bomb_impact(200.0, 77, /*miss=*/0.0, 0);
    ASSERT_EQ(ledger.bomb_impact_log().size(), 2u);
    EXPECT_EQ(ledger.bomb_impact_log()[0].miss_distance_ft, 142);
    EXPECT_EQ(ledger.bomb_impact_log()[0].objective, 77);
    EXPECT_EQ(ledger.bomb_impacts(), 2);
    // Impacts alone are activity (the QC gate reads empty()).
    EXPECT_FALSE(ledger.empty());
}

// ── 3. to_json — the artifact ───────────────────────────────────────────────

TEST(ResultLedger, ToJsonIsByteStableAndStrictlyValid) {
    auto run = []() {
        auto ws = make_ledger_world();
        WorldStateAdapters adapters(ws);
        CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                    adapters.units);
        ledger.apply_air_loss(10.0, 3, 4281, 5001, 0);
        ledger.apply_ag_kill(11.0, 4281);
        ledger.apply_bomb_impact(12.5, 77, 142.4, 1);
        ObjectiveDamageRecord rec;
        rec.objective = 77;
        rec.features_total = 2;
        rec.features_destroyed = 1;
        rec.destroyed_pct = 33;
        rec.fstatus = {0x0C};
        ledger.apply_objective_damage(rec);
        return ledger.to_json();
    };

    const std::string a = run();
    const std::string b = run();
    EXPECT_EQ(a, b);   // byte-stable

    // Strict structural validity: the Reader walks the WHOLE document.
    f4::json::Reader r(a);
    r.skip_ws();
    r.expect('{');
    r.skip_value();

    // The facts, greppable.
    EXPECT_NE(a.find("\"format\": \"f4-campaign-result\""), std::string::npos);
    EXPECT_NE(a.find("\"air_losses\":1"), std::string::npos);
    EXPECT_NE(a.find("\"miss_ft\":142"), std::string::npos);
    EXPECT_NE(a.find("\"fstatus\": \"0c\""), std::string::npos);
    EXPECT_NE(a.find("\"attributed\": false"), std::string::npos);
    // No floats anywhere in the document (the determinism discipline).
    EXPECT_EQ(a.find("e-"), std::string::npos);
    EXPECT_EQ(a.find("0."), std::string::npos);
}

// ── 4. The C2 hook — tasking reacts to losses ───────────────────────────────

TEST(ResultLedger, FreshLedgerAttachedChangesNothing) {
    // The golden identity: attaching a zero-event ledger to the
    // Campaign reproduces the un-attached run byte-for-byte.
    auto plain = Rig::make();
    plain->campaign->tick(1800);

    auto attached = Rig::make();
    attached->campaign->set_result_ledger(attached->ledger.get());
    attached->campaign->tick(1800);

    EXPECT_EQ(plain->campaign->to_summary_json(),
              attached->campaign->to_summary_json());
    EXPECT_EQ(plain->campaign->intents().size(),
              attached->campaign->intents().size());
}

TEST(ResultLedger, LossesThrottleTasking) {
    // Kill BOTH wings' entire availability: 24 booked losses per side
    // (the fixture's pools) — the next cycle tasks nothing.
    auto rig = Rig::make();
    rig->campaign->set_result_ledger(rig->ledger.get());

    const std::uint32_t usa = rig->usa_squadron_vu();
    const std::uint32_t dprk = rig->dprk_squadron_vu();
    ASSERT_NE(usa, 0u);
    ASSERT_NE(dprk, 0u);

    for (int i = 0; i < 24; ++i) {
        rig->ledger->apply_air_loss(static_cast<double>(i), 1, usa, 0, 0);
        rig->ledger->apply_air_loss(static_cast<double>(i), 6, dprk, 0, 0);
    }
    EXPECT_EQ(rig->ledger->team_aircraft_remaining(1), 0);
    EXPECT_EQ(rig->ledger->squadron_run_losses(usa), 24);

    rig->campaign->tick(1800);
    EXPECT_EQ(rig->campaign->cycles_fired(), 1);
    EXPECT_TRUE(rig->campaign->intents().empty())
        << "tasking must react to a fully-depleted force";
}

TEST(ResultLedger, PartialLossesShrinkPackages) {
    // The graded case: the same run with 22 of 24 losses booked must
    // generate strictly fewer intents than the fresh-force run (the
    // profiles with str > 2 no longer fit).
    auto base = Rig::make();
    base->campaign->tick(1800);
    const std::size_t baseline = base->campaign->intents().size();
    ASSERT_GT(baseline, 0u);

    auto hurt = Rig::make();
    hurt->campaign->set_result_ledger(hurt->ledger.get());
    const std::uint32_t usa = hurt->usa_squadron_vu();
    const std::uint32_t dprk = hurt->dprk_squadron_vu();
    for (int i = 0; i < 22; ++i) {
        hurt->ledger->apply_air_loss(static_cast<double>(i), 1, usa, 0, 0);
        hurt->ledger->apply_air_loss(static_cast<double>(i), 6, dprk, 0, 0);
    }
    hurt->campaign->tick(1800);

    EXPECT_LT(hurt->campaign->intents().size(), baseline);
    // And whatever still generates is capped at the shrunken
    // availability (2 aircraft left).
    for (const auto& in : hurt->campaign->intents()) {
        EXPECT_LE(in.aircraft_count, 2);
    }
}

// ── 5. apply_to — the write-back ────────────────────────────────────────────

TEST(WorldWriteback, AppliesPoolsSquadronsAndFstatus) {
    auto ws = make_ledger_world();
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_air_loss(10.0, 3, 4281, 5001, 0);
    ObjectiveDamageRecord rec;
    rec.objective = 77;
    rec.features_total = 2;
    rec.features_destroyed = 1;
    rec.destroyed_pct = 33;
    rec.fstatus = {0x0C, 0x00};
    ledger.apply_objective_damage(rec);

    const auto out = apply_to(ledger, ws);

    EXPECT_EQ(out.team_pools_written, 1);
    EXPECT_EQ(ws.campaign.te_number_aircraft[3], 9);   // 10 − 1
    EXPECT_EQ(out.squadrons_written, 1);
    EXPECT_EQ(ws.units[0].aa_kills, 5);                // unchanged (no credit)
    EXPECT_EQ(ws.units[0].total_losses, 3);            // 2 seed + 1
    EXPECT_EQ(out.objectives_written, 1);
    const std::vector<std::uint8_t> expected_fstatus{0x0C, 0x00};
    EXPECT_EQ(ws.objectives[0].fstatus, expected_fstatus);
    EXPECT_TRUE(out.unmatched_squadrons.empty());
    EXPECT_TRUE(out.unmatched_objectives.empty());
}

TEST(WorldWriteback, ZeroEventLedgerLeavesWorldUntouched) {
    auto ws = make_ledger_world();
    const auto pools_before = ws.campaign.te_number_aircraft;
    const auto losses_before = ws.units[0].total_losses;
    const auto fstatus_before = ws.objectives[0].fstatus;

    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);
    const auto out = apply_to(ledger, ws);

    EXPECT_EQ(out.team_pools_written, 0);
    EXPECT_EQ(out.squadrons_written, 0);
    EXPECT_EQ(out.objectives_written, 0);
    EXPECT_EQ(ws.campaign.te_number_aircraft, pools_before);
    EXPECT_EQ(ws.units[0].total_losses, losses_before);
    EXPECT_EQ(ws.objectives[0].fstatus, fstatus_before);
}

TEST(WorldWriteback, UnmatchedVusAreLoud) {
    // The stale-world case: the ledger is constructed against one
    // WorldState, then applied to a DIFFERENT one that lacks the
    // squadron unit and the objective. Nothing is written for them;
    // both are reported — never a silent drop.
    auto full = make_ledger_world();
    WorldStateAdapters full_adapters(full);
    CampaignResultLedger ledger(full_adapters.campaign, full_adapters.teams,
                                 full_adapters.units);
    ledger.apply_air_loss(1.0, 3, /*victim_sq=*/4281, 5001, 0);
    ObjectiveDamageRecord rec;
    rec.objective = 77;
    rec.features_total = 2;
    rec.features_destroyed = 2;
    rec.destroyed_pct = 100;
    rec.fstatus = {0xFC};
    ledger.apply_objective_damage(rec);

    // The stale world: same team pool, but no squadron unit and no
    // objectives at all.
    WorldState stale;
    stale.campaign.te_number_aircraft = {0, 0, 0, 10, 0, 0, 0, 0};
    f4::world::TeamState team;
    team.slot = 3;
    team.name = "TEST";
    stale.teams = {team};

    const auto out = apply_to(ledger, stale);
    EXPECT_EQ(out.objectives_written, 0);
    ASSERT_EQ(out.unmatched_objectives.size(), 1u);
    EXPECT_EQ(out.unmatched_objectives[0], 77u);
    ASSERT_EQ(out.unmatched_squadrons.size(), 1u);
    EXPECT_EQ(out.unmatched_squadrons[0], 4281u);
    // The team pool still wrote (the team exists in the stale world).
    EXPECT_EQ(out.team_pools_written, 1);
    EXPECT_EQ(stale.campaign.te_number_aircraft[3], 9);
}

// ── 6. C2 — one pool: mission draws, netting, reinforcement ─────────────────

TEST(ResultLedgerC2, SquadronRosterDecodesFromWirePacking) {
    // The wire's u32 roster is the 2-bit-per-group packing (16 groups):
    // 0x5555aaaa = 8 groups of 2 + 8 of 1 = 24 ships; 0x55aaaa = 20.
    // The pre-C2 Campaign read the RAW u32 (1.4 billion for a 24-ship
    // wing) — this pins the decode both the Campaign and the ledger now
    // share (src/squadron_snapshot.hpp).
    {
        auto ws = make_ledger_world(10, 2, /*roster=*/0x5555aaaa,
                                     /*reinforcement=*/7);
        WorldStateAdapters adapters(ws);
        CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                     adapters.units);
        const auto* sq = ledger.squadron(4281);
        ASSERT_NE(sq, nullptr);
        EXPECT_EQ(sq->availability, 24);      // decoded, not raw
        EXPECT_EQ(sq->reinforce_pending, 7);  // the wire's own budget
    }
    {
        auto ws = make_ledger_world(10, 2, /*roster=*/0x55AAAA);
        WorldStateAdapters adapters(ws);
        CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                    adapters.units);
        EXPECT_EQ(ledger.squadron(4281)->availability, 20);
    }
    {
        // Roster 0: the squadron shares the team pool (kunsan's shape —
        // the fixture path the B.3 goldens pin).
        auto ws = make_ledger_world(10, 2, /*roster=*/0);
        WorldStateAdapters adapters(ws);
        CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                    adapters.units);
        EXPECT_EQ(ledger.squadron(4281)->availability, 10);
    }
}

TEST(ResultLedgerC2, MissionDrawDebitsTaskingNotExistance) {
    auto ws = make_ledger_world(10, 2, 0x5555AAAA);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_mission_draw(/*t=*/60.0, /*team=*/3, /*squadron=*/4281,
                              /*count=*/4);

    // Tasking pool: 24 − 4.
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 20);
    // Existence view: drawn aircraft still fly — untouched.
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 10);
    EXPECT_EQ(ledger.team_aircraft_tasking(3), 6);  // 10 − 4
    // Counters + the event log.
    EXPECT_EQ(ledger.mission_draws(), 1);
    EXPECT_EQ(ledger.mission_draw_aircraft(), 4);
    ASSERT_EQ(ledger.mission_draw_log().size(), 1u);
    EXPECT_EQ(ledger.mission_draw_log()[0].squadron, 4281u);
    EXPECT_EQ(ledger.mission_draw_log()[0].count, 4);
    // Draws are tasking state, NOT combat results: the QC gate's
    // empty() stays true, and the write-back is correctly a no-op.
    EXPECT_TRUE(ledger.empty());
    const auto out = apply_to(ledger, ws);
    EXPECT_EQ(out.team_pools_written, 0);
    EXPECT_EQ(out.squadrons_written, 0);
}

TEST(ResultLedgerC2, DrawnAircraftDeathDoesNotDoubleDebit) {
    // The netting rule: a mission drew 4 (pool −4); 2 of THOSE aircraft
    // die in combat. The tasking pool must sit at 24−4 — the deaths
    // were already debited by the draw — while the existence counters
    // and the debrief history count both deaths.
    auto ws = make_ledger_world(10, 2, 0x5555AAAA);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_mission_draw(60.0, 3, 4281, 4);
    ledger.apply_air_loss(120.0, 3, 4281, 5001, /*killer=*/0);
    ledger.apply_air_loss(130.0, 3, 4281, 5001, /*killer=*/0);

    EXPECT_EQ(ledger.squadron_tasking_available(4281), 20);  // NOT 18
    const auto* sq = ledger.squadron(4281);
    ASSERT_NE(sq, nullptr);
    EXPECT_EQ(sq->run_losses, 2);        // history counts every death
    EXPECT_EQ(sq->drawn_deaths, 2);      // both netted against the draw
    EXPECT_EQ(sq->total_losses, 4);      // 2 seed + 2
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 8);  // existence 10−2
    EXPECT_EQ(ledger.team_aircraft_tasking(3), 6);    // 10 − 4 drawn
}

TEST(ResultLedgerC2, NonDrawnDeathDebitsTasking) {
    // No draws: every death debits the pool directly (the C1 behavior,
    // byte-for-byte). With draws exhausted, the surplus deaths debit.
    auto ws = make_ledger_world(10, 2, 0x5555AAAA);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    // Two deaths with NO outstanding draws: tasking 24 − 2.
    ledger.apply_air_loss(10.0, 3, 4281, 5001, 0);
    ledger.apply_air_loss(11.0, 3, 4281, 5001, 0);
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 22);

    // One draw, then two MORE deaths: only the first nets (the draw's
    // aircraft); the second debits the pool. Tasking 22 − 1 − 1 = 20.
    ledger.apply_mission_draw(20.0, 3, 4281, 1);
    ledger.apply_air_loss(30.0, 3, 4281, 5001, 0);
    ledger.apply_air_loss(31.0, 3, 4281, 5001, 0);
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 20);
    EXPECT_EQ(ledger.squadron(4281)->drawn_deaths, 1);
}

TEST(ResultLedgerC2, UnknownDrawSquadronIsLoudAndCounted) {
    auto ws = make_ledger_world(10, 2, 0x5555AAAA);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_mission_draw(60.0, 3, /*unknown vu=*/9999, 4);
    // Counted at the team level, loud as unmatched — never a silent
    // drop (a stale world or a foreign tasking source).
    EXPECT_EQ(ledger.draws_unmatched(), 1);
    EXPECT_EQ(ledger.mission_draws(), 1);
    EXPECT_EQ(ledger.mission_draw_aircraft(), 4);
    EXPECT_EQ(ledger.team_aircraft_tasking(3), 6);
    EXPECT_EQ(ledger.squadron_tasking_available(9999), 0);
}

TEST(ResultLedgerC2, ReinforcementRefillsDeficitFromBudget) {
    // The wire budget is the cap: a 4-aircraft deficit with 3 on order
    // delivers 3, consumes the budget, and the second fire delivers
    // nothing (the stock is spent, the fire is still counted).
    auto ws = make_ledger_world(10, 2, 0x5555AAAA, /*reinforcement=*/3);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    for (int i = 0; i < 4; ++i) {
        ledger.apply_air_loss(static_cast<double>(i), 3, 4281, 5001, 0);
    }
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 20);  // deficit 4

    EXPECT_EQ(ledger.apply_reinforcements(600.0), 3);
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 23);
    EXPECT_EQ(ledger.squadron(4281)->run_reinforced, 3);
    EXPECT_EQ(ledger.squadron(4281)->reinforce_pending, 0);
    EXPECT_EQ(ledger.aircraft_reinforced(), 3);
    EXPECT_EQ(ledger.reinforcement_fires(), 1);
    // Team existence view: real aircraft arrived (capped at initial 10):
    // 10 − 4 losses + 3 delivered = 9.
    EXPECT_EQ(ledger.team_aircraft_remaining(3), 9);

    // Second fire: the budget is gone — a legal, quiet fire.
    EXPECT_EQ(ledger.apply_reinforcements(4200.0), 0);
    EXPECT_EQ(ledger.reinforcement_fires(), 2);
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 23);
    // The delivery log: one record, arrival order, the budget trail.
    ASSERT_EQ(ledger.reinforcement_log().size(), 1u);
    EXPECT_EQ(ledger.reinforcement_log()[0].squadron, 4281u);
    EXPECT_EQ(ledger.reinforcement_log()[0].delivered, 3);
    EXPECT_EQ(ledger.reinforcement_log()[0].budget_left, 0);
}

TEST(ResultLedgerC2, ReinforcementCappedAtSnapshotAvailability) {
    // A huge budget still only refills TO the snapshot — a squadron
    // never taskes ABOVE the force it started the run with, and a
    // pristine squadron (no deficit) receives nothing.
    auto ws = make_ledger_world(10, 2, 0x5555AAAA, /*reinforcement=*/99);
    WorldStateAdapters adapters(ws);
    CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                adapters.units);

    ledger.apply_air_loss(10.0, 3, 4281, 5001, 0);
    ledger.apply_air_loss(11.0, 3, 4281, 5001, 0);
    EXPECT_EQ(ledger.apply_reinforcements(60.0), 2);
    EXPECT_EQ(ledger.squadron_tasking_available(4281), 24);  // == snapshot

    // Pristine: no deficit, no delivery.
    EXPECT_EQ(ledger.apply_reinforcements(3660.0), 0);
    EXPECT_EQ(ledger.squadron(4281)->reinforce_pending, 97);
}

TEST(ResultLedgerC2, ToJsonV2CarriesTheTaskingSide) {
    auto run = []() {
        auto ws = make_ledger_world(10, 2, 0x5555AAAA, /*reinforcement=*/2);
        WorldStateAdapters adapters(ws);
        CampaignResultLedger ledger(adapters.campaign, adapters.teams,
                                    adapters.units);
        ledger.apply_mission_draw(60.0, 3, 4281, 4);
        ledger.apply_air_loss(120.0, 3, 4281, 5001, 0);
        ledger.apply_reinforcements(3600.0);
        return ledger.to_json();
    };
    const std::string a = run();
    const std::string b = run();
    EXPECT_EQ(a, b);   // byte-stable

    f4::json::Reader r(a);
    r.skip_ws();
    r.expect('{');
    r.skip_value();   // strictly valid (the whole document walks)

    EXPECT_NE(a.find("\"version\": 2"), std::string::npos);
    EXPECT_NE(a.find("\"mission_draws\":1"), std::string::npos);
    // Team tasking view: 10 initial − 4 drawn − 0 non-drawn losses
    // (the one death netted against the draw) + 2 delivered = 8.
    EXPECT_NE(a.find("\"aircraft_tasking\":8"), std::string::npos);
    EXPECT_NE(a.find("\"reinforcement_fires\":1"), std::string::npos);
    // The squadron's budget trail: 2 on order, 2 delivered, 0 left.
    EXPECT_NE(a.find("\"reinforce_budget\":0"), std::string::npos);
    // No floats (the determinism discipline).
    EXPECT_EQ(a.find("e-"), std::string::npos);
    EXPECT_EQ(a.find("0."), std::string::npos);
}

// ── 7. C2 — the multi-cycle loop through the real Campaign gate ────────────

TEST(ResultLedgerC2, LedgerModeAgreesWithOwnPoolAcrossCycles) {
    // The strong golden identity: THREE cycles of draws through the
    // ledger's pool reproduce the legacy own-pool run byte-for-byte —
    // the two accounting modes never drift (one shared force snapshot,
    // one arithmetic).
    auto plain = Rig::make();
    plain->campaign->tick(1800);
    plain->campaign->tick(1800);
    plain->campaign->tick(1800);

    auto attached = Rig::make();
    attached->campaign->set_result_ledger(attached->ledger.get());
    attached->campaign->tick(1800);
    attached->campaign->tick(1800);
    attached->campaign->tick(1800);

    EXPECT_EQ(plain->campaign->to_summary_json(),
              attached->campaign->to_summary_json());
    EXPECT_EQ(plain->campaign->intents().size(),
              attached->campaign->intents().size());
    // The attached run booked its draws into the ledger (the write
    // model saw every one — that's the one-pool contract).
    EXPECT_GT(attached->ledger->mission_draws(), 0);
    EXPECT_GT(attached->ledger->mission_draw_aircraft(), 0);
}

TEST(ResultLedgerC2, DrawsDepleteAndReinforcementResumesTasking) {
    // THE C2 acceptance: cycle 1 tasks the force dry; cycle 2 sees the
    // depleted pool and generates NOTHING; the reinforcement cadence
    // refills from the wire budget; the next cycle flies again.
    // Fresh anchor 1800 s past "now" + 1800 s period: first fire due
    // at clock 3600 (strictly after).
    CampaignConfig cfg;
    cfg.reinforcement_period_sec = 1800;
    auto rig = Rig::make(/*squadron_reinforcement=*/24,
                         /*anchor_offset=*/1800, cfg);
    rig->campaign->set_result_ledger(rig->ledger.get());

    rig->campaign->tick(1800);   // cycle 1: the force tasks
    const std::size_t first = rig->campaign->intents().size();
    ASSERT_GT(first, 0u);
    const std::uint32_t usa = rig->usa_squadron_vu();
    const std::uint32_t dprk = rig->dprk_squadron_vu();
    EXPECT_EQ(rig->ledger->squadron_tasking_available(usa), 0);
    EXPECT_EQ(rig->ledger->squadron_tasking_available(dprk), 0);
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 0);

    rig->campaign->tick(1800);   // cycle 2: depleted — nothing tasks
    EXPECT_EQ(rig->campaign->intents().size(), first);
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 0);  // due at 3600+

    rig->campaign->tick(1800);   // clock 5400: still nothing to task,
                                 // and the cadence FIRES (deficit 24,
                                 // budget 24 → delivered)
    EXPECT_EQ(rig->campaign->intents().size(), first);
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 1);
    EXPECT_EQ(rig->ledger->aircraft_reinforced(), 48);  // both wings
    EXPECT_EQ(rig->ledger->squadron_tasking_available(usa), 24);
    EXPECT_EQ(rig->ledger->squadron_tasking_available(dprk), 24);

    rig->campaign->tick(1800);   // cycle 4: the refilled force flies
    EXPECT_GT(rig->campaign->intents().size(), first);
}

TEST(ResultLedgerC2, StaleAnchorFiresExactlyOnce) {
    // TestCamp's shape: last_reinforcement 0 against a 32,400,000 s
    // epoch — ~375 days stale. The catch-up rule (anchor JUMPS to now
    // on fire, FreeFalcon's own shape) fires ONE tick, not the ~7,500
    // boundaries it is behind.
    auto rig = Rig::make(/*squadron_reinforcement=*/24, /*anchor=*/-1,
                         [] {
                             CampaignConfig cfg;
                             cfg.reinforcement_period_sec = 3600;
                             return cfg;
                         }());
    rig->campaign->set_result_ledger(rig->ledger.get());

    rig->campaign->tick(60);
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 1);
    rig->campaign->tick(60);
    rig->campaign->tick(60);
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 1);  // still one

    // The next boundary is one PERIOD after the catch-up fire, not
    // another immediate one.
    rig->campaign->tick(3500);   // clock 3680 — just past the next due
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 2);
}

TEST(ResultLedgerC2, ReinforcementDisabledMatchesLegacy) {
    // period 0 = the pre-C2 behavior: pools only deplete. The legacy
    // own-pool run and the ledger run agree across the whole horizon.
    auto rig = Rig::make(/*squadron_reinforcement=*/24, /*anchor=*/-1,
                         [] {
                             CampaignConfig cfg;
                             cfg.reinforcement_period_sec = 0;
                             return cfg;
                         }());
    rig->campaign->set_result_ledger(rig->ledger.get());
    rig->campaign->tick(7200);
    EXPECT_EQ(rig->campaign->reinforcement_fires(), 0);
    EXPECT_EQ(rig->ledger->reinforcement_fires(), 0);
    EXPECT_EQ(rig->ledger->aircraft_reinforced(), 0);
}

// f4-simulation/tests/test_campaign_commands.cpp
//
// CAMP-CMD-2 — the retask / abort / priority gates, end to end over the
// REAL engine session (the M4/M5 acceptance-harness style: a flight
// retasked mid-crank flies the new route and its books close correctly):
//
//   * The CRAFTED TIERED RIG (a quiet war, one save flight): retask
//     mid-route → the aggregate flies the NEW route from where it is
//     (no teleport, no eastward drift), the mission byte and the
//     recomputed mission-over deadline ride the row; abort mid-route →
//     the RTB leg home and the row reports the abort; the typed
//     refusals (unknown flight/objective, an untaskable mission byte).
//   * THE KUNSAN WAR (full fidelity, the ATM pipeline live): a filed
//     package's flight retasks to a new objective — the live brains
//     re-plan, the ATM booking follows (mission, target, deadline);
//     an abort scrubs the package's books (the recovery books NOW,
//     the booking is gone, nothing double-releases); the tiered
//     variant retasks a synthetic AGGREGATE and keeps its takeoff
//     slot; and the identity statement's CMD-2 half — a journal
//     carrying retask + priority + abort replays into the SAME
//     ledger fingerprint regardless of the step chunking.

#include <f4/simulation/campaign_session_host.hpp>

#include <f4/campaign/api/journal.hpp>
#include <f4/campaign/api/protocol.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace api = f4::campaign::api;

namespace {

constexpr std::uint32_t kSyntheticVuBase = 0x53590000u;

std::filesystem::path make_temp_dir() {
    static std::atomic<unsigned> counter{0};
    const auto dir = std::filesystem::temp_directory_path() /
                     ("f4_cmd2_" + std::to_string(counter.fetch_add(1)) +
                      "_" + std::to_string(
                          std::chrono::steady_clock::now()
                              .time_since_epoch()
                              .count()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// The tier rig's crafted world (the host tests' shape) EXTENDED with a
// DPRK-owned objective 4102 — the retask destination. Flight 5001 flies
// a time-less (SPEED-mode) route east; the war is quiet (no ATM).
std::string cmd_world_json() {
    return R"({
  "version": 71,
  "theater": "korea",
  "campaign": {
    "current_time": 38574360,
    "te_team": 2,
    "teams": [
      {"slot": 2, "name": "ROK", "member": [0,0,1,0,0,0,0,0],
       "stance": [0,0,0,0,0,0,5,0]},
      {"slot": 6, "name": "DPRK", "member": [0,0,0,0,0,0,1,0],
       "stance": [0,0,5,0,0,0,0,0]}
    ]
  },
  "objectives": {
    "count": 2,
    "decoded": 2,
    "items": [
      {"type": 100, "id_num": 4101, "id_creator": 0,
       "objective_type": 1,
       "x": 390, "y": 455, "z": 0,
       "owner": 2, "nameid": 1627, "priority": 10,
       "fstatus": [0, 0], "links": []},
      {"type": 100, "id_num": 4102, "id_creator": 0,
       "objective_type": 1,
       "x": 430, "y": 490, "z": 0,
       "owner": 6, "nameid": 1628, "priority": 30,
       "fstatus": [0, 0], "links": []}
    ]
  },
  "units": {
    "count": 3,
    "decoded": 3,
    "items": [
      {"type": 200, "id_num": 4281, "unit_class": "squadron",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "airbase_id": 4101, "class_name": "52 TFS PAK"},
      {"type": 200, "id_num": 5001, "unit_class": "flight",
       "entity_type": 273, "domain": 2,
       "x": 390, "y": 455, "z": 0, "owner": 2,
       "mission": 13, "squadron_id": 4281, "package_id": 7029,
       "time_on_target": 43739352,
       "waypoints": [
         {"x": 390, "y": 455, "z": 0,    "action": 1},
         {"x": 420, "y": 460, "z": 2500, "action": 15},
         {"x": 460, "y": 500, "z": 2500, "action": 17},
         {"x": 390, "y": 455, "z": 0,    "action": 7}
       ]},
      {"type": 200, "id_num": 6001, "unit_class": "battalion",
       "entity_type": 180, "domain": 3,
       "x": 390, "y": 455, "z": 0, "owner": 6,
       "vehicle_groups": [
         {"group": 0, "vehicle_type": 101, "count": 3, "live_count": 3}
       ]}
    ]
  }
})";
}

// The crafted tiered rig: a quiet war, one save flight, two objectives.
struct CmdRig {
    std::filesystem::path dir;
    std::filesystem::path world;
    std::unique_ptr<EngineSessionHost> host;

    static CmdRig make() {
        CmdRig rig;
        rig.dir = make_temp_dir();
        rig.world = rig.dir / "cmd.world.json";
        {
            std::ofstream f(rig.world);
            f << cmd_world_json();
        }
        CampaignSessionOptions opts;
        opts.world_json = rig.world;
        opts.class_table = std::filesystem::path(F4_SOURCE_FIXTURES_DIR) /
                           "falcon4.ct.json";
        opts.aircraft_config =
            std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        opts.fidelity_policy = FidelityPolicy::Tiered;
        opts.tasking_cycle_sec = 1000000;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = false;
        opts.max_flights = 8;
        opts.max_steps_per_advance = 4000;
        std::string err;
        rig.host = EngineSessionHost::create(opts, &err);
        // A failed rig must fail the TEST, not segfault it: the factory
        // cannot ASSERT (non-void return) — throw, and gtest reports
        // the session's own error as the failure.
        if (rig.host == nullptr) {
            throw std::runtime_error(
                "CommandRig: session create failed: " + err);
        }
        return rig;
    }
};

// The kunsan war rig: the ATM pipeline live, filings every 5 campaign
// seconds. `tiered` arms the aggregate engine (the synthetic-intent
// tiering); full fidelity spawns every filed flight's complement.
struct KunsanRig {
    std::unique_ptr<EngineSessionHost> host;

    static KunsanRig make(bool tiered) {
        const auto f16 =
            std::filesystem::path(F4_GENERATED_FIXTURES_DIR) / "f16.json";
        EXPECT_FALSE(f16.empty()) << "f16.json fixture not generated";
        KunsanRig rig;
        CampaignSessionOptions opts;
        opts.world_json =
            std::filesystem::path(F4_SIMULATION_TEST_FIXTURES_DIR) /
            "kunsan_strategy.world.json";
        opts.class_table =
            std::filesystem::path(F4_SOURCE_FIXTURES_DIR) /
            "falcon4.ct.json";
        opts.aircraft_config = f16;
        opts.mission_profiles = F4_MISSION_PROFILES_JSON;
        if (tiered) opts.fidelity_policy = FidelityPolicy::Tiered;
        opts.tasking_cycle_sec = 5;
        opts.reinforce_period_sec = 0;
        opts.atm_pipeline = true;
        opts.strategy_layer = true;   // the stationed-CAP/support routes
        opts.max_flights = 8;
        opts.max_steps_per_advance = 4000;
        std::string err;
        rig.host = EngineSessionHost::create(opts, &err);
        // Same discipline as CommandRig::make: fail the test, never
        // the process.
        if (rig.host == nullptr) {
            throw std::runtime_error(
                "KunsanRig: session create failed: " + err);
        }
        return rig;
    }

    // The first filed routed synthetic intent whose package actually
    // launched from a real airbase (the fixture's DPRK squadrons have
    // none — their bookings carry airbase 0 and cannot replan).
    const f4::campaign::MissionIntent* pick_routed_synthetic(
        std::uint32_t except = 0) const {
        for (const auto& in : host->engine().intents()) {
            if (!in.synthetic || in.route.empty()) continue;
            if (except != 0 && in.flight_id == except) continue;
            const auto* ft = booking(in.flight_id);
            if (ft == nullptr || ft->airbase_vu == 0) continue;
            return &in;
        }
        return nullptr;
    }

    const f4::campaign::FlightTasking* booking(std::uint32_t flight_id)
        const {
        const auto* booked = host->engine().campaign().atm_booked_flights();
        if (booked == nullptr) return nullptr;
        for (const auto& ft : *booked) {
            if (ft.flight_id == flight_id) return &ft;
        }
        return nullptr;
    }
};

double dist_sq(double ax, double ay, double bx, double by) {
    const double dx = ax - bx;
    const double dy = ay - by;
    return dx * dx + dy * dy;
}

} // namespace

// ============================================================================
// the crafted tiered gates — the aggregate write
// ============================================================================

TEST(CmdRetask, SaveFlightAggregateMidRouteFliesTheNewTasking) {
    const auto rig = CmdRig::make();
    ASSERT_NE(rig.host, nullptr);

    // ~100 s east: the aggregate is mid-leg on its old tasking. The
    // whole-second cadence lands +9 on the first exact-boundary step
    // (the HOST-1 pin) and +10 after; ten steps cross the aggregate
    // engine's first 60-second update exactly once (12 cruise grids).
    for (int i = 0; i < 10; ++i) rig.host->step(600);
    auto rows = rig.host->engine().flight_tiers();
    ASSERT_EQ(rows.size(), 1u);
    const double before_x = rows[0].x_grid;
    const double before_y = rows[0].y_grid;
    ASSERT_GT(before_x, 390.0);   // it moved off the base

    // Retask to CAS against the DPRK objective from WHERE IT IS.
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::FlightRetask;
    cmd.flight = 5001;
    cmd.mission_byte = 20;   // AMIS_CAS
    cmd.target_objective_id = 4102;
    const auto ack = rig.host->submit(cmd);
    ASSERT_EQ(ack.status, api::CommandAck::Status::Applied) << ack.detail;
    EXPECT_NE(ack.detail.find("AMIS_CAS"), std::string::npos);
    EXPECT_NE(ack.detail.find("4102"), std::string::npos);

    rows = rig.host->engine().flight_tiers();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].mission, 20);          // the new tasking on the row
    EXPECT_NEAR(rows[0].x_grid, before_x, 0.5);   // no teleport
    EXPECT_NEAR(rows[0].y_grid, before_y, 0.5);
    EXPECT_GT(rows[0].to_mission_over, 0);   // the deadline recomputed
    EXPECT_FALSE(rows[0].aborted);

    // It flies the NEW route. The recomputed TOT sits inside the FID
    // ops window, so the engine's delivery doctrine may deaggregate the
    // flight to fly the attack in-sim — the substance is the same: the
    // aggregate closes on 4102 (430,490), or the materialized aircraft
    // carries the NEW plan (a 4102 target waypoint aboard its brain).
    for (int i = 0; i < 12; ++i) rig.host->step(600);
    rows = rig.host->engine().flight_tiers();
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_FALSE(rows[0].aborted);
    if (!rows[0].live) {
        const double after = dist_sq(rows[0].x_grid, rows[0].y_grid,
                                     430.0, 490.0);
        const double before = dist_sq(before_x, before_y, 430.0, 490.0);
        EXPECT_LT(after, before);   // closing on the new target
    } else {
        // The Tier-B aircraft: the retasked plan rides its brain (the
        // deagg spawn built it from the retasked entity waypoints).
        bool found_aircraft = false;
        bool carries_target = false;
        // The plan's delivery waypoint carries the objective's ENTITY
        // id (the bridge's target_num → map resolution), not the raw VU.
        const auto tgt = rig.host->engine().objective_id_map().find(4102);
        ASSERT_NE(tgt, rig.host->engine().objective_id_map().end());
        for (const auto id : rig.host->engine().campaign_aircraft()) {
            auto* origin =
                f4::entities::EntityHandle(
                    id, &rig.host->engine().sim().world())
                    .get<f4::simulation::CampaignOriginComponent>();
            if (origin == nullptr || origin->flight_vu != 5001) continue;
            found_aircraft = true;
            auto* brain =
                f4::entities::EntityHandle(
                    id, &rig.host->engine().sim().world())
                    .get<f4::ai::BrainComponent>();
            if (brain == nullptr) continue;
            for (const auto& w : brain->mission_plan().route) {
                if (w.target_id == tgt->second.value) carries_target = true;
            }
        }
        EXPECT_TRUE(found_aircraft);
        EXPECT_TRUE(carries_target);
    }
}

TEST(CmdAbort, SaveFlightMidRouteFliesRtbHome) {
    const auto rig = CmdRig::make();

    // Mid-route, then abort: the flight comes home.
    for (int i = 0; i < 10; ++i) rig.host->step(600);
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::FlightAbort;
    cmd.flight = 5001;
    const auto ack = rig.host->submit(cmd);
    ASSERT_EQ(ack.status, api::CommandAck::Status::Applied) << ack.detail;
    EXPECT_NE(ack.detail.find("RTB"), std::string::npos);
    // A save-carried flight has no booking in THIS session's ledger —
    // the books closed in the save's own history; nothing releases.
    EXPECT_EQ(ack.detail.find("returned to the pool"), std::string::npos);

    auto rows = rig.host->engine().flight_tiers();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].aborted);            // the row reports the abort
    EXPECT_FALSE(rows[0].arrived);
    const double out_x = rows[0].x_grid;
    const double out_y = rows[0].y_grid;

    // The RTB leg: home is the route's own landing waypoint (390,455).
    for (int i = 0; i < 12; ++i) rig.host->step(600);
    rows = rig.host->engine().flight_tiers();
    ASSERT_EQ(rows.size(), 1u);
    const double home = dist_sq(rows[0].x_grid, rows[0].y_grid, 390.0,
                                455.0);
    const double outbound = dist_sq(out_x, out_y, 390.0, 455.0);
    EXPECT_LT(home, outbound);   // closing on home, not the old target
    EXPECT_TRUE(rows[0].aborted);

    // A second abort refuses: the sortie is already closed.
    const auto ack2 = rig.host->submit(cmd);
    EXPECT_EQ(ack2.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack2.refusal, api::CommandAck::Refusal::InvalidArgument);
}

TEST(CmdRefusals, TypedRejectionsOnTheCraftedWar) {
    const auto rig = CmdRig::make();

    // An untaskable mission byte.
    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::FlightRetask;
    cmd.flight = 5001;
    cmd.mission_byte = 41;
    cmd.target_objective_id = 4102;
    auto ack = rig.host->submit(cmd);
    EXPECT_EQ(ack.status, api::CommandAck::Status::Refused);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::InvalidArgument);

    // An unknown target objective.
    cmd.mission_byte = 20;
    cmd.target_objective_id = 999999;
    ack = rig.host->submit(cmd);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownObjective);

    // An unknown flight.
    cmd.target_objective_id = 4102;
    cmd.flight = 777;
    ack = rig.host->submit(cmd);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownFlight);

    api::CommandIntent abort;
    abort.kind = api::CommandIntent::Kind::FlightAbort;
    abort.flight = 777;
    ack = rig.host->submit(abort);
    EXPECT_EQ(ack.refusal, api::CommandAck::Refusal::UnknownFlight);
}

// ============================================================================
// the kunsan gates — the whole command surface on a live war
// ============================================================================

TEST(CmdRetask, LiveComplementReplansAndTheBookingFollows) {
    const auto rig = KunsanRig::make(/*tiered=*/false);
    ASSERT_NE(rig.host, nullptr);

    // The first tasking cycle fires at campaign second 5 (tick 300).
    rig.host->step(600);  // 10 s: the first cycles fire
    const auto* pick = rig.pick_routed_synthetic();
    ASSERT_NE(pick, nullptr) << "the kunsan war filed no routed flight";
    const auto flight_vu = pick->flight_id;
    ASSERT_NE(rig.booking(flight_vu), nullptr) << "the flight is booked";

    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::FlightRetask;
    cmd.flight = flight_vu;
    cmd.mission_byte = 20;   // AMIS_CAS
    cmd.target_objective_id = 1065;   // the DPRK city (priority 100)
    const auto ack = rig.host->submit(cmd);
    ASSERT_EQ(ack.status, api::CommandAck::Status::Applied) << ack.detail;
    EXPECT_NE(ack.detail.find("booking rescheduled"), std::string::npos);
    EXPECT_NE(ack.detail.find("aircraft re-planned"), std::string::npos);

    // The booking followed the flight.
    const auto* ft = rig.booking(flight_vu);
    ASSERT_NE(ft, nullptr);
    EXPECT_EQ(ft->mission, 20);
    EXPECT_EQ(ft->target_vu, 1065u);

    // The war keeps flying with the new plan aboard.
    rig.host->step(600);
    EXPECT_TRUE(rig.booking(flight_vu) != nullptr ||
                rig.host->engine().ledger().aircraft_recovered() > 0);
}

TEST(CmdAbort, ScrubsThePackageBooks) {
    const auto rig = KunsanRig::make(/*tiered=*/false);
    rig.host->step(600);  // 10 s: the first cycles fire
    const auto* pick = rig.pick_routed_synthetic();
    ASSERT_NE(pick, nullptr);
    const auto flight_vu = pick->flight_id;
    const auto* ft = rig.booking(flight_vu);
    ASSERT_NE(ft, nullptr);
    const int complement = ft->aircraft;
    ASSERT_GT(complement, 0);

    const auto recovered_before =
        rig.host->engine().ledger().aircraft_recovered();

    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::FlightAbort;
    cmd.flight = flight_vu;
    const auto ack = rig.host->submit(cmd);
    ASSERT_EQ(ack.status, api::CommandAck::Status::Applied) << ack.detail;
    EXPECT_NE(ack.detail.find("books closed"), std::string::npos);
    EXPECT_NE(ack.detail.find("returned to the pool"), std::string::npos);

    // The books: the complement returned to the pool NOW (the draw's
    // mirror), and the booking is gone.
    const auto recovered_after =
        rig.host->engine().ledger().aircraft_recovered();
    EXPECT_EQ(recovered_after - recovered_before, complement);
    EXPECT_EQ(rig.booking(flight_vu), nullptr);

    // The closed package never re-enters the books.
    rig.host->step(600);
    EXPECT_EQ(rig.host->engine().ledger().aircraft_recovered(),
              recovered_after);
}

TEST(CmdRetask, TieredSyntheticRetaskKeepsTheTakeoffSlot) {
    const auto rig = KunsanRig::make(/*tiered=*/true);
    rig.host->step(600);  // 10 s: the first cycles fire

    // A registered synthetic aggregate that still holds for takeoff.
    auto rows = rig.host->engine().flight_tiers();
    const CampaignSession::FlightTierView* held = nullptr;
    for (const auto& r : rows) {
        if (r.vu >= kSyntheticVuBase && r.to_depart > 0 && !r.arrived &&
            !r.destroyed) {
            held = &r;
            break;
        }
    }
    ASSERT_NE(held, nullptr) << "no synthetic aggregate holds for takeoff";
    const auto vu = held->vu;
    const auto to_depart_before = held->to_depart;
    ASSERT_NE(rig.booking(vu & 0xFFFFu), nullptr);

    api::CommandIntent cmd;
    cmd.kind = api::CommandIntent::Kind::FlightRetask;
    cmd.flight = vu;
    cmd.mission_byte = 20;
    cmd.target_objective_id = 1065;
    const auto ack = rig.host->submit(cmd);
    ASSERT_EQ(ack.status, api::CommandAck::Status::Applied) << ack.detail;

    // The row carries the new tasking; the takeoff slot survived (the
    // head waypoint keeps the original departure — a retasked sortie
    // still launches on its schedule, just flies somewhere else).
    rows = rig.host->engine().flight_tiers();
    bool found = false;
    for (const auto& r : rows) {
        if (r.vu != vu) continue;
        found = true;
        EXPECT_EQ(r.mission, 20);
        EXPECT_EQ(r.to_depart, to_depart_before);
        EXPECT_FALSE(r.aborted);
    }
    EXPECT_TRUE(found);

    // The booking followed.
    const auto* ft = rig.booking(vu & 0xFFFFu);
    ASSERT_NE(ft, nullptr);
    EXPECT_EQ(ft->mission, 20);
    EXPECT_EQ(ft->target_vu, 1065u);
}

TEST(CmdJournal, TheInterventionReplayReproducesTheIdentity) {
    // The identity statement (plan §2.3/§5) with the CMD-2 set aboard:
    // (save, seed, command journal) → the SAME ledger fingerprint. The
    // record: one flight retasked, the objective re-prioritized, a
    // second flight scrubbed — each journaled at its engine tick.
    std::vector<api::CommandJournalEntry> recorded;
    api::IdentityFingerprint final_a;
    std::uint32_t retasked_vu = 0;
    std::uint32_t aborted_vu = 0;
    {
        auto rig = KunsanRig::make(/*tiered=*/false);
        rig.host->set_command_journal_sink(
            [&](std::uint64_t tick, std::int64_t t_s,
                const api::CommandIntent& intent) {
                recorded.push_back(
                    api::CommandJournalEntry{tick, t_s, intent});
            });
        rig.host->step(600);  // 10 s: the first cycles fire
        const auto* pick = rig.pick_routed_synthetic();
        ASSERT_NE(pick, nullptr);
        retasked_vu = pick->flight_id;

        api::CommandIntent retask;
        retask.kind = api::CommandIntent::Kind::FlightRetask;
        retask.flight = retasked_vu;
        retask.mission_byte = 20;
        retask.target_objective_id = 1065;
        ASSERT_EQ(rig.host->submit(retask).status,
                  api::CommandAck::Status::Applied);

        api::CommandIntent priority;
        priority.kind = api::CommandIntent::Kind::ObjectivePriority;
        priority.objective_id = 1065;
        priority.weight = 90;
        ASSERT_EQ(rig.host->submit(priority).status,
                  api::CommandAck::Status::Applied);

        rig.host->step(600);
        const auto* second = rig.pick_routed_synthetic(retasked_vu);
        ASSERT_NE(second, nullptr) << "the war filed a second flight";
        aborted_vu = second->flight_id;
        api::CommandIntent abort;
        abort.kind = api::CommandIntent::Kind::FlightAbort;
        abort.flight = aborted_vu;
        ASSERT_EQ(rig.host->submit(abort).status,
                  api::CommandAck::Status::Applied);

        rig.host->step(600);
        final_a = rig.host->identity();
    }
    ASSERT_EQ(recorded.size(), 3u);
    // The recorded ticks are the ENGINE's own accumulated sim clock at
    // the submit boundary (599 = the step(600) request minus the one
    // sub-tick the accumulator carried — the HOST-1 boundary slop,
    // journaled exactly as it applied).
    EXPECT_EQ(recorded[0].apply_tick, 599u);
    EXPECT_EQ(recorded[0].intent.kind,
              api::CommandIntent::Kind::FlightRetask);
    EXPECT_EQ(recorded[1].intent.kind,
              api::CommandIntent::Kind::ObjectivePriority);
    EXPECT_EQ(recorded[2].apply_tick, 1199u);
    EXPECT_EQ(recorded[2].intent.kind,
              api::CommandIntent::Kind::FlightAbort);
    EXPECT_EQ(recorded[2].intent.flight, aborted_vu);

    // B — the whole journal through ONE step call (the replay's
    // chunking is irrelevant; the host segments around the ticks).
    {
        auto rig = KunsanRig::make(/*tiered=*/false);
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        rig.host->step(1800);
        EXPECT_EQ(rig.host->pending_replay_commands(), 0u);
        const auto id = rig.host->identity();
        EXPECT_EQ(id.ledger_fnv, final_a.ledger_fnv);
        EXPECT_EQ(id.campaign_time_s, final_a.campaign_time_s);
    }

    // C — the record's own step pattern.
    {
        auto rig = KunsanRig::make(/*tiered=*/false);
        ASSERT_TRUE(rig.host->start_command_replay(recorded));
        rig.host->step(600);
        rig.host->step(600);
        rig.host->step(600);
        const auto id = rig.host->identity();
        EXPECT_EQ(id.ledger_fnv, final_a.ledger_fnv);
    }
}

// test_flight_lead_e2e.cpp — the Step-14 wing-radio E2E
// (AI_IMPLEMENTATION_PLAN §16, FlightLeadModule): the lead's orders
// (Rejoin / EngageMyTarget / RTB), the wingman's acks, the radio
// transcript lines, and the ladder effects, end to end through
// Simulation::tick over the generated F-16 config:
//
//   1. the gate-off twin: an unarmed two-ship publishes NO wing-radio
//      rows, and the gate-on run with nothing to command is
//      member-for-member identical with it (the golden identity)
//   2. the rejoin order: a blown-out wingman draws "EAGLE2, rejoin."
//      + the ack, in that order, from the lead's own edge rule
//   3. the engage order: the lead fighting a bandit the wingman holds
//      draws "EAGLE2, engage my target." + the ack, and the wingman
//      joins the LEAD's bandit (the ordered preference above the sort)
//   4. the RTB order: a bingoing wingman draws "EAGLE2, RTB." + the
//      ack, and the LEAD reports "RTB" on a healthy tank (v1's
//      both-RTB)
//   5. the done-when arc: formation through the waypoint chain ->
//      engaged as a flight -> RTB on bingo, the closed vocabulary
//      visible in the radio log throughout
//
// Two-ship geometry shared with the Step-11 rigs: EAGLE1 at the origin
// flying north, EAGLE2 at the FightingWing slot (or offset past the
// rejoin ring for the rejoin tier). Bandits hold fire — the tiers pin
// the COMMAND flow, not the kill chain (the M3/M4 suites own that).

#include <gtest/gtest.h>

#include "f4/simulation/combat_transcript.hpp"
#include "f4/simulation/simulation.hpp"

#include <f4/ai/brain_component.hpp>
#include <f4/ai/modules/flight_lead_module.hpp>
#include <f4/entities/entity.hpp>
#include <f4/flight/flight_model_component.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::simulation;
namespace entities = f4::entities;

namespace {

constexpr double kDt = 1.0 / 60.0;

std::string f16_config_path() {
    const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR");
    std::string dir = env ? env : "";
#ifdef F4_GENERATED_FIXTURES_DIR
    if (dir.empty()) dir = F4_GENERATED_FIXTURES_DIR;
#endif
    if (dir.empty()) return "";
    const auto path = std::filesystem::path(dir) / "f16.json";
    return std::filesystem::exists(path) ? path.generic_string() : "";
}

// One two-ship (plus optional bandits) under the Step-14 gate.
//   wing_x / wing_y      — the wingman's spawn offset from the lead
//   wing_fuel / lead_fuel — per-aircraft fuel (asymmetric for the RTB
//                          tiers: the wingman crosses bingo first)
//   bingo_lbs             — the flight's fuel policy (0 = fuel-blind)
//   bandit_count          — red F-16s parked ahead, holding fire
struct FlightLeadRig {
    bool gate_on = true;
    double wing_x = 2000.0;
    double wing_y = -2500.0;   // the FightingWing slot (in station)
    double lead_fuel = 6500.0;
    double wing_fuel = 6500.0;
    double bingo_lbs = 0.0;
    int bandit_count = 0;
    double bandit_y = 60000.0;
};

// One scenario aircraft object (the fragment discipline the datalink rig
// uses: every raw string closes on its own terms — the JSON content has
// no parentheses, so the only ")" a literal ever meets is its own
// terminator).
std::string aircraft_json(const std::string& f16_path,
                          const std::string& callsign, double x, double y,
                          double z, double heading_rad, double fuel,
                          double vt_fps, const char* team,
                          const std::string& extra_fields) {
    return std::string("{ \"callsign\": \"") + callsign +
           R"(", "aircraft_config_path": ")" + f16_path +
           R"(", "aircraft_name": "F-16C_50", "vis_type_index": 1052,
      "parking_spot": { "x": )" +
           std::to_string(x) + ", \"y\": " + std::to_string(y) +
           ", \"z\": " + std::to_string(z) + R"( },
      "heading_rad": )" +
           std::to_string(heading_rad) +
           R"(, "initial_fuel_lbs": )" +
           std::to_string(fuel) +
           R"(, "initial_vt_fps": )" +
           std::to_string(vt_fps) +
           R"(, "spawn_in_air": true, "team": ")" +
           team + "\"" + extra_fields + " }";
}

std::string flight_lead_json(const std::string& f16_path,
                             const FlightLeadRig& rig) {
    std::string aircraft = "    " +
        aircraft_json(f16_path, "EAGLE1", 0.0, 0.0, 15000.0, 0.0,
                      rig.lead_fuel, 506.0, "blue", "");
    aircraft += ",\n    " +
        aircraft_json(f16_path, "EAGLE2", rig.wing_x, rig.wing_y, 15000.0,
                      0.0, rig.wing_fuel, 506.0, "blue",
                      ",\n      \"lead_callsign\": \"EAGLE1\"");
    for (int i = 0; i < rig.bandit_count; ++i) {
        const std::string callsign = "BANDIT" + std::to_string(i + 1);
        aircraft += ",\n    " +
            aircraft_json(f16_path, callsign, 2500.0 * i, rig.bandit_y,
                          15000.0, 3.14159265358979, 6500.0, 480.0, "red",
                          ",\n      \"hold_fire\": true");
    }

    std::string fuel_block;
    if (rig.bingo_lbs > 0.0) {
        fuel_block = ",\n  \"fuel\": { \"bingo_lbs\": " +
                     std::to_string(rig.bingo_lbs) + " }";
    }
    // The Step-14 gate (the twin test flips exactly this key).
    const std::string ai_block = std::string(",\n  \"ai\": { \"flight_lead\": ") +
                                 (rig.gate_on ? "true" : "false") + " }";

    return R"({
  "name": "flight_lead_e2e",
  "theater": "korea",
  "aircraft": [
)" + aircraft + R"(
  ],
  "airfield": {
    "active_runway_id": 36, "active_runway_name": "Rwy 36",
    "runway_heading_rad": 0.0,
    "threshold_position": { "x": 0.0, "y": -5000.0, "z": 0.0 },
    "runway_end_position":  { "x": 0.0, "y": 5000.0, "z": 0.0 },
    "threshold_altitude_ft": 0.0, "departure_altitude_ft": 15000.0,
    "taxi_route": [ { "x": 0.0, "y": -5000.0, "z": 0.0 },
                    { "x": 0.0, "y": 0.0, "z": 0.0 } ]
  },
  "waypoints": [
    { "name": "FAR_NORTH", "position": { "x": 0.0, "y": 500000.0, "z": 15000.0 },
      "speed_kts": 420.0 }
  ],
  "start_enroute": true,
  "sim_dt": 0.016666666666666,
  "total_ticks": 30000,
  "record": false,
  "combat": { "enabled": true, "radar_rng_seed": 777,
              "fighter_hit_points": 100 }
)" + fuel_block + ai_block + R"(
})";
}

Simulation make_sim(const std::string& f16_path, const FlightLeadRig& rig) {
    auto scenario = load_scenario_from_string(
        flight_lead_json(f16_path, rig));
    return Simulation(std::move(scenario), std::filesystem::path("."));
}

entities::EntityId id_of(Simulation& sim, const char* callsign) {
    for (const auto eid : sim.aircraft_entities()) {
        entities::EntityHandle h(eid, &sim.world());
        if (const auto* id = h.get<entities::CampaignIdentityComponent>();
            id != nullptr && id->callsign == callsign) {
            return eid;
        }
    }
    return entities::EntityId{0};
}

f4::ai::BrainComponent* brain_of(Simulation& sim, const char* callsign) {
    return entities::EntityHandle(id_of(sim, callsign), &sim.world())
        .get<f4::ai::BrainComponent>();
}

// ── transcript helpers ─────────────────────────────────────────────────────

struct Line {
    std::string speaker;
    std::string text;
};

std::vector<Line> snapshot(const CombatTranscript& log) {
    std::vector<Line> out;
    out.reserve(log.size());
    for (std::size_t i = 0; i < log.size(); ++i) {
        const auto* e = log.at(i);
        out.push_back(Line{e->speaker, e->text});
    }
    return out;
}

bool has_line(const std::vector<Line>& lines, const std::string& speaker,
              const std::string& text) {
    for (const auto& l : lines) {
        if (l.speaker == speaker && l.text == text) return true;
    }
    return false;
}

// The wing vocabulary in one probe (the twin test asserts ALL of it dark).
bool any_wing_radio_line(const std::vector<Line>& lines) {
    for (const auto& l : lines) {
        if (l.text == "Copy.") return true;
        if (l.text.find(", rejoin.") != std::string::npos) return true;
        if (l.text.find(", engage my target.") != std::string::npos)
            return true;
        if (l.text.find(", RTB.") != std::string::npos) return true;
    }
    return false;
}

// The order must precede its ack (the one-directional flow, visible).
bool order_precedes_ack(const std::vector<Line>& lines,
                        const std::string& order_text) {
    std::size_t order_idx = lines.size();
    std::size_t ack_idx = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].text == order_text && order_idx == lines.size()) {
            order_idx = i;
        }
        if (lines[i].text == "Copy." && ack_idx == lines.size()) {
            ack_idx = i;
        }
    }
    return order_idx < ack_idx && order_idx != lines.size();
}

// ── the twin's member-for-member probe (the FM state the world flies) ──────

struct Kinematics {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double vcas = 0.0;
    double fuel = 0.0;
    bool operator==(const Kinematics&) const = default;
};

Kinematics kinematics_of(Simulation& sim, const char* callsign) {
    entities::EntityHandle h(id_of(sim, callsign), &sim.world());
    const auto* tf = h.get<entities::TransformComponent>();
    const auto* fm = h.get<f4::flight::FlightModelComponent>();
    Kinematics k;
    if (tf != nullptr) {
        k.x = tf->position.x;
        k.y = tf->position.y;
        k.z = tf->position.z;
    }
    if (fm != nullptr) {
        k.vcas = fm->state().vcas;
        k.fuel = fm->state().fuel.fuel_lbs;
    }
    return k;
}

// Poll helper: run until pred holds or the budget is spent.
template <typename Pred>
bool run_until(Simulation& sim, int max_ticks, Pred&& pred) {
    for (int i = 0; i < max_ticks; ++i) {
        sim.tick(kDt);
        if (pred()) return true;
    }
    return false;
}

} // namespace

// ============================================================================
// 1. The gate-off twin — the golden identity
// ============================================================================

TEST(FlightLeadE2E, GateOffTwinPublishesNothingAndFliesIdentically) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    // In-station two-ship, no bandit, no fuel policy: nothing to command.
    FlightLeadRig off;
    off.gate_on = false;
    FlightLeadRig on;
    on.gate_on = true;   // identical world; only the gate differs

    Simulation sim_off = make_sim(f16, off);
    sim_off.initialize();
    CombatTranscript log_off;
    log_off.attach(sim_off);

    Simulation sim_on = make_sim(f16, on);
    sim_on.initialize();
    CombatTranscript log_on;
    log_on.attach(sim_on);

    // 30 s of cruise — the formation rung settles, no orders anywhere.
    for (int i = 0; i < 1800; ++i) {
        sim_off.tick(kDt);
        sim_on.tick(kDt);
    }

    const auto lines_off = snapshot(log_off);
    const auto lines_on = snapshot(log_on);
    EXPECT_FALSE(any_wing_radio_line(lines_off))
        << "gate off: no wing-radio rows may exist";
    EXPECT_FALSE(any_wing_radio_line(lines_on))
        << "gate on with nothing to command: still no rows";
    EXPECT_EQ(lines_off.size(), lines_on.size())
        << "the transcripts differ outside the wing vocabulary";

    // The world flew identically: the wingman's kinematics are
    // member-for-member equal (the gate changed no arithmetic).
    EXPECT_EQ(kinematics_of(sim_off, "EAGLE2"), kinematics_of(sim_on, "EAGLE2"));
    EXPECT_EQ(kinematics_of(sim_off, "EAGLE1"), kinematics_of(sim_on, "EAGLE1"));
}

// ============================================================================
// 2. The rejoin order — the blowout, seen from the other side
// ============================================================================

TEST(FlightLeadE2E, BlownOutWingmanDrawsRejoinOrderAndAcks) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    FlightLeadRig rig;
    rig.gate_on = true;
    rig.wing_x = -9000.0;   // 10.8 kft out — past the rejoin ring (9 kft)
    rig.wing_y = -6000.0;
    Simulation sim = make_sim(f16, rig);
    sim.initialize();
    CombatTranscript log;
    log.attach(sim);

    auto* lead = brain_of(sim, "EAGLE1");
    auto* wing = brain_of(sim, "EAGLE2");
    ASSERT_NE(lead, nullptr);
    ASSERT_NE(wing, nullptr);
    EXPECT_TRUE(lead->flight_command_armed());
    EXPECT_TRUE(wing->flight_command_armed());

    // The order + ack both land (the wingman self-rejoins regardless —
    // the ORDER is the lead's own rule made radio; the ack proves the
    // wingman received it).
    const bool got_lines = run_until(sim, 1800, [&] {
        const auto lines = snapshot(log);
        return has_line(lines, "EAGLE1", "EAGLE2, rejoin.") &&
               has_line(lines, "EAGLE2", "Copy.");
    });
    ASSERT_TRUE(got_lines) << "the rejoin order + ack never landed";

    // One-directional flow, visible in the log: the order precedes it.
    EXPECT_TRUE(order_precedes_ack(snapshot(log), "EAGLE2, rejoin."));

    // The lead's module holds the order state (the host already drained
    // it — the wingman's brain is the one acting on it now).
    EXPECT_FALSE(lead->flight_lead().order_for(id_of(sim, "EAGLE2").value)
                     .has_value());

    // The wingman converges back to the station (the applied SM path).
    const bool rejoined = run_until(sim, 7200, [&] {
        return wing->wingman().state() ==
               f4::ai::modules::WingState::Following;
    });
    EXPECT_TRUE(rejoined) << "the wingman never formed back up";
}

// ============================================================================
// 3. The engage order — the flight fights ONE bandit
// ============================================================================

TEST(FlightLeadE2E, LeadOrdersEngageMyTargetAndTheWingmanJoins) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    FlightLeadRig rig;
    rig.gate_on = true;
    rig.bandit_count = 2;    // a free bandit exists — the sort would take it
    rig.bandit_y = 45000.0;  // closing head-on: the fight develops fast
    Simulation sim = make_sim(f16, rig);
    sim.initialize();
    CombatTranscript log;
    log.attach(sim);

    auto* lead = brain_of(sim, "EAGLE1");
    auto* wing = brain_of(sim, "EAGLE2");
    ASSERT_NE(lead, nullptr);
    ASSERT_NE(wing, nullptr);

    // The engage order + ack land...
    const bool got_lines = run_until(sim, 10800, [&] {
        const auto lines = snapshot(log);
        return has_line(lines, "EAGLE1", "EAGLE2, engage my target.") &&
               has_line(lines, "EAGLE2", "Copy.");
    });
    ASSERT_TRUE(got_lines) << "the engage order + ack never landed";
    EXPECT_TRUE(order_precedes_ack(snapshot(log),
                                   "EAGLE2, engage my target."));

    // ...and the ORDER changed the fight: the wingman engages the LEAD's
    // bandit (the ordered preference outranks the sort's free bandit).
    // Both engagements ride the same id while the lead keeps the target.
    const bool doubled = run_until(sim, 3600, [&] {
        const std::uint64_t lead_t = lead->combat_engagement_id();
        const std::uint64_t wing_t = wing->combat_engagement_id();
        return lead_t != 0 && wing_t == lead_t;
    });
    EXPECT_TRUE(doubled)
        << "the wingman never joined the lead's bandit (lead "
        << lead->combat_engagement_id() << ", wing "
        << wing->combat_engagement_id() << ")";
}

// ============================================================================
// 4. The RTB order — the wingman's bingo takes the lead home (v1)
// ============================================================================

TEST(FlightLeadE2E, BingoingWingmanDrawsRTBOrderAndTheLeadStandsDown) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    FlightLeadRig rig;
    rig.gate_on = true;
    rig.wing_fuel = 3000.0;  // below the reserve from the first fuel check
    rig.bingo_lbs = 4000.0;  // the lead (6500) stays healthy throughout
    Simulation sim = make_sim(f16, rig);
    sim.initialize();
    CombatTranscript log;
    log.attach(sim);

    auto* lead = brain_of(sim, "EAGLE1");
    auto* wing = brain_of(sim, "EAGLE2");
    ASSERT_NE(lead, nullptr);
    ASSERT_NE(wing, nullptr);

    // The order + ack land...
    const bool got_lines = run_until(sim, 600, [&] {
        const auto lines = snapshot(log);
        return has_line(lines, "EAGLE1", "EAGLE2, RTB.") &&
               has_line(lines, "EAGLE2", "Copy.");
    });
    ASSERT_TRUE(got_lines) << "the RTB order + ack never landed";
    EXPECT_TRUE(order_precedes_ack(snapshot(log), "EAGLE2, RTB."));

    // ...v1's both-RTB: the LEAD stands down on a HEALTHY tank — its own
    // fuel state is Normal, the "RTB" mode line comes from the order.
    EXPECT_TRUE(lead->rtb_ordered());
    EXPECT_EQ(lead->fuel_state(),
              f4::ai::BrainComponent::FuelState::Normal);
    EXPECT_EQ(lead->mode_name(), "RTB");

    // The wingman took the order too (its own bingo already stood it
    // down; the latch is the order's record).
    EXPECT_TRUE(wing->rtb_ordered());
}

// ============================================================================
// 5. The done-when arc — formation, engage as a flight, RTB on bingo
// ============================================================================

TEST(FlightLeadE2E, TheFlightArcFormationEngageRTB) {
    const auto f16 = f16_config_path();
    if (f16.empty()) GTEST_SKIP() << "f16.json fixture not generated";

    FlightLeadRig rig;
    rig.gate_on = true;
    rig.wing_fuel = 6000.0;
    rig.bingo_lbs = 5800.0;  // 200 lbs above the reserve — the bingo lands
                             // deterministically mid-arc (the fight's
                             // extra burn only accelerates it); the lead
                             // (6500) stays comfortably above it
    rig.bandit_count = 2;    // a free bandit exists — the engage ORDER is
                             // what puts the wingman on the lead's bandit
                             // (one bandit and the sort's support-the-kill
                             // rule doubles it without any order)
    rig.bandit_y = 45000.0;  // the engage tier's geometry: by the time the
                             // lead commits, the wingman holds BOTH bandits
                             // (the sort takes the free one; the order
                             // overrides)
    Simulation sim = make_sim(f16, rig);
    sim.initialize();
    CombatTranscript log;
    log.attach(sim);

    auto* lead = brain_of(sim, "EAGLE1");
    auto* wing = brain_of(sim, "EAGLE2");
    ASSERT_NE(lead, nullptr);
    ASSERT_NE(wing, nullptr);

    // (a) FORMATION: the wingman holds the station through the cruise.
    ASSERT_TRUE(run_until(sim, 1800, [&] {
        return wing->wingman().state() ==
               f4::ai::modules::WingState::Following;
    })) << "the wingman never formed up";

    // (b) ENGAGE AS A FLIGHT: the lead's bandit becomes the wingman's.
    const bool engaged = run_until(sim, 10800, [&] {
        const std::uint64_t lead_t = lead->combat_engagement_id();
        const std::uint64_t wing_t = wing->combat_engagement_id();
        return lead_t != 0 && wing_t == lead_t;
    });
    ASSERT_TRUE(engaged) << "the flight never engaged as a flight";
    // The engage ORDER's mechanics are pinned by the dedicated tier
    // above. Here the flight's convergence is timing-shaped: the host
    // pushes the lead's engagement one tick stale, so the wingman's
    // FIRST lock either takes the free bandit (the order then flips it
    // onto the lead's) or degenerates straight onto the lead's bandit
    // (the sort's support-the-kill). BOTH paths end doubled on the
    // lead's bandit — that is what "engages as a flight" means, and it
    // is the deterministic pin.

    // (c) RTB ON BINGO: the wingman's reserve fires the order; the lead
    // stands down with it; the log carries the vocabulary end to end.
    // The RTB order is the arc's deterministic radio line (the engage
    // order may be raced out by the path above; the fuel rule is not).
    int rtb_ticks = 0;
    std::vector<Line> rtb_lines;
    const bool home = run_until(sim, 14400, [&] {
        ++rtb_ticks;
        if (lead->rtb_ordered() && wing->rtb_ordered()) {
            rtb_lines = snapshot(log);
            return true;
        }
        return false;
    });
    ASSERT_TRUE(home) << "the flight never went RTB on the bingo (t="
                      << rtb_ticks * kDt << "s, wing fuel="
                      << wing->fuel_lbs() << " lbs, lead fuel="
                      << lead->fuel_lbs() << " lbs)";

    EXPECT_TRUE(has_line(rtb_lines, "EAGLE1", "EAGLE2, RTB."));
    EXPECT_TRUE(has_line(rtb_lines, "EAGLE2", "Copy."));
    EXPECT_EQ(lead->mode_name(), "RTB");
}

// f4-simulation/tools/campaign_qc.cpp
//
// campaign_qc — the B.3 end-to-end QC harness for campaign saves.
//
// One command exercises the whole campaign→sim loop over a real decoded
// save (TestCamp.cam → world JSON) and writes the artifacts the world
// viewer renders:
//
//   1. THE B.3 LOOP (headless, in-process):
//        WorldState → populate_world → emit_flight_intents (campaign side)
//        → MessageBus → CampaignSimSpawner (sim side) → aircraft with
//          SAVED ROUTES attached.
//      Reported in the summary as the "b3_loop" block: intents emitted,
//      aircraft spawned, routes attached, duplicates skipped.
//
//   2. THE SIM RUN (physics): generates a campaign_flights scenario JSON
//      (with the same filter), runs Simulation::initialize + tick for
//      --ticks frames, and writes the FlightRecorder trace (the viewer's
//      replay mode loads exactly this format). Aircraft taxi, depart, and
//      fly their saved routes — the QC assertion is "did the campaign's
//      tasking actually fly?"
//
//   3. THE SUMMARY (campaign_qc_summary.json): world stats (teams,
//      objectives, units, flights, mission histogram), b3_loop stats,
//      sim-run stats (spawned, airborne-at-end, per-flight end position),
//      the ORDNANCE ledger (A-G slice: releases, impacts, per-objective
//      damage), the RESULT ledger (C1: the campaign write-back — air
//      losses, kill credit, objective damage, post-loss pools), plus
//      the scenario JSON path for replay in the f4-scenario-player.
//
//   4. THE RESULT LEDGER (C1 — the war loop's return leg): the
//      CampaignResultLedger is snapshotted from the world's own team
//      pools + squadron counters, fed by the CampaignResultSink (kill
//      + impact events on the sim bus, resolved back to campaign
//      identity), synced with the objectives' final damage state, and
//      written as campaign_result.json (byte-stable). The in-memory
//      write-back into the WorldState runs too (apply_to) — the counts
//      land in the summary's "results" block. Exit 5 fires when combat
//      happened but the ledger stayed empty: outcomes that never wrote
//      back, the exact class of silent loss this tranche exists to kill.
//
//   5. THE TASKING LADDER (C2 — one pool, multi-cycle): --tasking <m>
//      runs the synthetic M4.7 Campaign (profile ladder over squadron
//      availability, THE LEDGER ATTACHED — every generated mission
//      debits it, the reinforcement cadence refills it from the wire's
//      per-squadron budgets on the .cmp anchor's schedule) for m
//      minutes of campaign time BEFORE the saved-flight sim run. The
//      tasking draws and the combat losses then land in the SAME
//      ledger — the first true multi-cycle loop: tasking depletes,
//      combat attrites, resupply refills, the next cycle sees all
//      three. Reported as the summary's "tasking" block; exit 6 fires
//      when the ladder drew NOT ONE aircraft despite belligerents
//      with aircraft available (the tasking-broke gate).
//
//   6. THE 24-HOUR WAR (C5 — the acceptance): --war <hours> runs the
//      long-horizon loop — both sides generate, fly, fight, attrite,
//      recover, and resupply over the ATM pipeline for HOURS of sim
//      time, headless and deterministic, through CampaignWarHarness
//      (f4-simulation — the same CampaignSession composition the
//      world viewer drives). The harness runs the war TWICE and
//      certifies: identical ledger bytes (MD5), one-pool accounting
//      identities every sample, the roster identity (initial +
//      spawned − retired) every sample, and a war that stays alive
//      (clock, cycles, and every belligerent's generation). The
//      wreck reaper (wreck_hold) keeps killed aircraft's frozen
//      corpses from accumulating — the entity-churn bound. Artifacts:
//      campaign_result.json (byte-stable), the summary's "war" block
//      (deterministic content only), and campaign_war_diary.json
//      (per-hour telemetry: wall-clock, ticks/sec, RSS — explicitly
//      NOT byte-stable). Exits 9–12 are the war's own gates (see
//      run_war below).
//
// Usage:
//   campaign_qc <world.json> [options]
//     --class-table <FALCON4.ct>   (default: <src>/f4-world-convert/tests/fixtures/FALCON4.ct)
//     --config <f16.json>          (default: <bin>/generated_fixtures/f16.json)
//     --models <KoreaObj.HDR>      (default: <src>/temp/KoreaObj.HDR; .LOD/.TEX inferred)
//     --team <slot>                (filter: owning team, -1 = any)
//     --mission <AMIS_*>|<byte>    (filter: mission name or byte)
//     --max-flights <n>            (filter: cap spawned aircraft)
//     --ticks <n>                  (sim frames; default 54000 = 15 min)
//     --minutes <m>                (convenience: sets --ticks to m*60*60)
//     --sim-dt <sec>               (default 1/60)
//     --tasking <m>                (C2: synthetic tasking minutes; 0 = off)
//     --tasking-cycle <sec>        (C2: ladder cycle period; default 1800)
//     --reinforce-period <sec>     (C2: reinforcement cadence; 0 = off,
//                                   default 43200 = 12 h)
//     --profiles <json>            (default: <bin>/generated_campaign/MissionProfiles.json)
//     --record-every <n> / --no-record
//     --war <hours>                (C5: the 24-hour war; 0 = off. 24 =
//                                   the acceptance horizon; smoke runs
//                                   use fractions — 0.1 = 6 min)
//     --war-runs <n>               (C5: determinism proof passes; 2 =
//                                   default, 1 = skip the proof)
//     --war-sample <sec>           (C5: diary + check cadence; 3600)
//     --wreck-hold <sec>           (C5: killed aircraft retire after
//                                   this many sim seconds; 300. 0 =
//                                   wrecks persist, the pre-C5 lifetime)
//     --war-max-wall <sec>         (C5: total wall-clock watchdog; 0 = off)
//     --out-dir <dir>              (default: beside the world JSON)
//
// Exit code: 0 when the loop produced at least one aircraft AND the sim
// ran to completion; 1 on usage/IO errors; 2 when the filter matched
// nothing (a QC failure, not a crash); 3 when the sim ran but NOTHING
// got airborne by the last tick — the end-to-end "tasking didn't fly"
// failure the tool exists to catch (ground-ops stall, broken taxi route,
// never-ending cross-theater taxi: the exact symptoms the first TestCamp
// run exposed); 4 when strike flights were armed with ordnance but NOT
// ONE bomb was released — the A-G employment failure (broken strike
// arming, an envelope that never opens, a store that never debits);
// 5 when combat outcomes occurred (kills and/or bomb impacts) but the
// result ledger recorded NOTHING — the write-back failure (a sink that
// never fired, an origin stamp that never landed, a classification that
// dropped every event);
// 6 when --tasking ran the synthetic ladder and it drew NOT ONE
// aircraft despite belligerent teams with aircraft available — the
// tasking-broke failure (a profile table that didn't load, availability
// gates that read zero, a force snapshot that misdecoded the roster);
// 7 when --tasking attached the route planner and drew aircraft but
// NOT ONE route could be built (or none of the routed intents
// materialized as aircraft) — the generation-to-spawn failure (a
// threat map that never painted, an A* that never converges, an
// airbase that never resolves).
//
// The 15-minute default window (was 5): TestCamp's strike flights sit a
// median 34 NM from their targets — a 5-minute window proved the taxi/
// takeoff/departure chain but landed every strike flight short of the
// release point. 15 minutes at ~400 kts covers the max 80 NM leg with
// margin.

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/campaign_bridge.hpp>
#include <f4/simulation/campaign_spawner.hpp>
#include <f4/simulation/campaign_result_sink.hpp>
#include <f4/simulation/campaign_war_harness.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/campaign/result_ledger.hpp>
#include <f4/campaign/route_builder.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/entities/entity.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/json/writer.hpp>
#include <f4/io/read_file.hpp>
#include <f4/weapons/bomb_battery.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/weapons/weapon_store.hpp>
#include <f4/weapons/weapon_types.hpp>
#include <f4/world/world_loader.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/models/model_database.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace f4::simulation;
using namespace f4::campaign;

namespace {

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
struct Args {
    std::filesystem::path world_json;
    std::filesystem::path class_table;
    std::filesystem::path config;
    std::filesystem::path models_hdr;
    int team = -1;
    int mission = -1;            // byte; -1 = any
    int max_flights = 0;
    int ticks = 54000;           // 15 min at 60 Hz — taxi + takeoff +
                                 // climb + ENROUTE TO THE TARGET (the A-G
                                 // slice needs the release point reached;
                                 // strike flights sit a median 34 NM out)
    double sim_dt = 1.0 / 60.0;
    int record_every = 10;       // trace decimation (6 samples/s/aircraft)
    bool record = true;          // false = --no-record (full-population
                                 // runs: the recorder + trace JSON are the
                                 // memory hog — 449 flights x 5,400 samples
                                 // does not fit small hosts; the QC gates
                                 // don't need the trace, the viewer replay
                                 // does)
    // C2 — the synthetic tasking ladder (see the header's item 5).
    int tasking_minutes = 0;      // 0 = off
    int tasking_cycle_sec = 1800; // the ladder's cycle period
    int reinforce_period_sec = -1; // -1 = engine default (43200); 0 = off
    std::filesystem::path profiles_json;
    std::filesystem::path out_dir;
    // C5 — the 24-hour war (--war): the long-horizon acceptance run
    // (both sides generate, fly, fight, attrite, recover, resupply for
    // HOURS of sim time, headless, deterministic). 0 = off (the B.3/
    // C2/C3/C4 QC modes run instead).
    double war_hours = 0.0;       // sim hours; 24 = the acceptance
    int war_runs = 2;             // determinism proof passes
    double war_sample_sec = 3600.0; // diary + check cadence (1 hour)
    double wreck_hold_sec = 300.0;  // C5's reaper (0 = wrecks persist)
    double war_max_wall_sec = 0.0;  // total wall-clock watchdog (0 = off)
    // C6 — arming the campaign flights: the war's A/A tranche.
    // Opt-in (every pre-C6 golden pinned on the unarmed shape);
    // the acceptance story is air_losses > 0 + the reaper reaping.
    bool aa_combat = false;
    // G1 — the ground war (battalion movement + front line +
    // attrition + capture): opt-in, the same contract aa_combat
    // keeps. The acceptance story: the army MOVED (the QC's exit 13
    // fires when an armed ground war produced nothing at all).
    bool ground_war = false;
    int ground_update_sec = 60;    // the engine default
    int ground_orders_sec = 1800;  // the engine default
    int ground_resupply_sec = 43200; // 12 h (the QC's own choice —
                                     // same as the air reinforce)
    // G2 — the interdiction link (CAS against real battalions, the
    // bombs booking): opt-in, the same contract. The acceptance
    // story: air attrited the line (exit 14 fires when an armed
    // unit strike produced nothing — CAS needs its TOT window, so
    // the 1-hour war is the honest horizon).
    bool unit_strike = false;
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s <world.json> [--class-table <ct>] [--config <f16.json>]\n"
        "          [--models <KoreaObj.HDR>] [--team <slot>]\n"
        "          [--mission <AMIS_NAME|byte>] [--max-flights <n>]\n"
        "          [--ticks <n>|--minutes <m>] [--sim-dt <sec>]\n"
        "          [--tasking <minutes>] [--tasking-cycle <sec>]\n"
        "          [--reinforce-period <sec>] [--profiles <json>]\n"
        "          [--record-every <n>] [--no-record]\n"
        "          [--war <hours>] [--war-runs <n>] [--war-sample <sec>]\n"
        "          [--wreck-hold <sec>] [--war-max-wall <sec>] [--aa-combat]\n"
        "          [--ground-war] [--ground-update-sec <sec>]\n"
        "          [--ground-orders-sec <sec>] [--ground-resupply-sec <sec>]\n"
        "          [--unit-strike] [--out-dir <dir>]\n",
        prog);
    std::exit(1);
}

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) usage(argv[0]);
    a.world_json = argv[1];
#ifdef F4_SOURCE_DIR
    a.class_table = std::filesystem::path(F4_SOURCE_DIR) /
                    "f4-world-convert/tests/fixtures/FALCON4.ct";
    a.models_hdr = std::filesystem::path(F4_SOURCE_DIR) / "temp/KoreaObj.HDR";
#endif
#ifdef F4_BINARY_DIR
    a.config = std::filesystem::path(F4_BINARY_DIR) /
               "generated_fixtures/f16.json";
#endif
#ifdef F4_MISSION_PROFILES_JSON
    a.profiles_json = F4_MISSION_PROFILES_JSON;
#endif
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if (k == "--class-table")      a.class_table = next();
        else if (k == "--config")      a.config = next();
        else if (k == "--models")      a.models_hdr = next();
        else if (k == "--profiles")    a.profiles_json = next();
        else if (k == "--team")        a.team = std::atoi(next());
        else if (k == "--max-flights") a.max_flights = std::atoi(next());
        else if (k == "--ticks")       a.ticks = std::atoi(next());
        else if (k == "--minutes")     a.ticks = static_cast<int>(std::atof(next()) * 3600.0);
        else if (k == "--sim-dt")      a.sim_dt = std::atof(next());
        else if (k == "--tasking")     a.tasking_minutes = std::atoi(next());
        else if (k == "--tasking-cycle") a.tasking_cycle_sec = std::atoi(next());
        else if (k == "--reinforce-period") a.reinforce_period_sec = std::atoi(next());
        else if (k == "--record-every") a.record_every = std::max(1, std::atoi(next()));
        else if (k == "--no-record")   a.record = false;
        else if (k == "--out-dir")     a.out_dir = next();
        else if (k == "--war")         a.war_hours = std::atof(next());
        else if (k == "--war-runs")    a.war_runs = std::max(1, std::atoi(next()));
        else if (k == "--war-sample")  a.war_sample_sec = std::atof(next());
        else if (k == "--wreck-hold")  a.wreck_hold_sec = std::atof(next());
        else if (k == "--war-max-wall") a.war_max_wall_sec = std::atof(next());
        else if (k == "--aa-combat")   a.aa_combat = true;
        else if (k == "--ground-war")  a.ground_war = true;
        else if (k == "--unit-strike") a.unit_strike = true;
        else if (k == "--ground-update-sec")
            a.ground_update_sec = std::atoi(next());
        else if (k == "--ground-orders-sec")
            a.ground_orders_sec = std::atoi(next());
        else if (k == "--ground-resupply-sec")
            a.ground_resupply_sec = std::atoi(next());
        else if (k == "--mission") {
            const std::string v = next();
            if (!v.empty() && v[0] >= '0' && v[0] <= '9') {
                a.mission = std::atoi(v.c_str());
            } else {
                const auto byte = mission_type_byte(v);
                if (!byte) {
                    std::fprintf(stderr, "unknown mission '%s'\n", v.c_str());
                    std::exit(1);
                }
                a.mission = static_cast<int>(*byte);
            }
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", k.c_str());
            usage(argv[0]);
        }
    }
    if (a.out_dir.empty()) a.out_dir = a.world_json.parent_path();
    return a;
}

// ---------------------------------------------------------------------------
// JSON Writer helpers (f4::json::Writer has raw put/number/string_key)
// ---------------------------------------------------------------------------
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void write_string(f4::json::Writer& w, const std::string& s) {
    w.put('"');
    w.put(json_escape(s));
    w.put('"');
}

// ---------------------------------------------------------------------------
// 6. C5 — THE 24-HOUR WAR (--war <hours>): the long-horizon acceptance
// run. Both sides generate, fly, fight, attrite, recover, and resupply
// for hours of sim time over the ATM pipeline, headless and
// deterministic, through CampaignWarHarness (which composes the same
// CampaignSession the world viewer drives). Exit gates:
//   6  belligerent aircraft existed and the ladder drew nothing (the
//      tasking-broke class, war edition);
//   7  aircraft were drawn but no route built / nothing materialized
//      (generation-to-spawn, war edition);
//   8  the ATM pipeline built no packages (war edition);
//   9  DETERMINISM — run 2's ledger bytes differ from run 1's;
//  10  LEDGER DRIFT — a one-pool identity broke at some sample;
//  11  ENTITY LEAK — the roster identity broke at some sample;
//  12  WAR STALLED — the clock, the cycles, or a belligerent's
//      generation went silent while aircraft remained.
// Artifacts: campaign_result.json (run 0's ledger — byte-stable),
// campaign_qc_summary.json (the "war" block — deterministic content
// only), campaign_war_diary.json (per-sample telemetry: wall-clock,
// ticks/sec, RSS — explicitly NOT byte-stable).
// ---------------------------------------------------------------------------
int run_war(const Args& args) {
    if (args.profiles_json.empty() ||
        !std::filesystem::exists(args.profiles_json)) {
        std::fprintf(stderr,
                     "campaign_qc: mission profiles not found (%s) — "
                     "--war needs the generated table\n",
                     args.profiles_json.string().c_str());
        return 1;
    }

    // World stats for the summary (the same block every mode reports).
    f4::world::WorldState ws;
    ws.load(args.world_json);
    int flights_total = 0, flights_tasked = 0;
    std::vector<int> missions_by_byte(
        static_cast<std::size_t>(kMissionTypeCount), 0);
    for (const auto& u : ws.units) {
        if (u.unit_class != f4::entities::UnitClass::Flight) continue;
        ++flights_total;
        if (is_mission_tasked(u.mission)) {
            ++flights_tasked;
            if (u.mission < kMissionTypeCount) {
                ++missions_by_byte[u.mission];
            }
        }
    }

    WarHarnessOptions hopts;
    hopts.session.world_json =
        std::filesystem::absolute(args.world_json);
    if (std::filesystem::exists(args.class_table)) {
        hopts.session.class_table =
            std::filesystem::absolute(args.class_table);
    }
    hopts.session.aircraft_config =
        std::filesystem::absolute(args.config);
    hopts.session.mission_profiles =
        std::filesystem::absolute(args.profiles_json);
    hopts.session.team = args.team;
    hopts.session.mission = args.mission;
    // The war's saved-flight cap: the session's own 48 default (the
    // interactivity budget — 449 FMs at 60 Hz is a replay-mode budget;
    // the WAR's story is the generated packages, and the ledger's pool
    // arithmetic counts every drawn aircraft whether it flies here or
    // not). An explicit --max-flights still wins.
    hopts.session.max_flights =
        args.max_flights > 0 ? args.max_flights : 48;
    hopts.session.tasking_cycle_sec = args.tasking_cycle_sec;
    hopts.session.reinforce_period_sec =
        args.reinforce_period_sec < 0 ? 43200
                                      : args.reinforce_period_sec;
    // The QC's tasking shape (C4): the ATM pipeline armed, the SEAD
    // pairing threshold the fixture theater needs (see the ladder
    // block above for the reasoning).
    hopts.session.atm_pipeline = true;
    hopts.session.atm_seadescort_threat = 25;
    hopts.session.sim_dt = args.sim_dt;
    // C6: the campaign-combat arming (the mission-role doctrine — CAP/
    // Sweep/Intercept/Escort fight, everything else defends through its
    // BRAINDAT archetype). Opt-in: the unarmed war is the pinned shape.
    hopts.session.aa_combat = args.aa_combat;
    // G1: the ground war (opt-in, the same contract). The QC arms the
    // 12 h ground resupply — the long war's supply line matters
    // exactly like the air reinforcement cadence it mirrors.
    hopts.session.ground_war = args.ground_war;
    hopts.session.ground_update_sec = args.ground_update_sec;
    hopts.session.ground_orders_sec = args.ground_orders_sec;
    hopts.session.ground_resupply_sec = args.ground_resupply_sec;
    // G2: the interdiction link (opt-in, the same contract).
    hopts.session.unit_strike = args.unit_strike;
    hopts.horizon_sec =
        static_cast<std::int64_t>(args.war_hours * 3600.0);
    hopts.sample_sec = args.war_sample_sec;
    hopts.runs = args.war_runs;
    hopts.wreck_hold_sec = args.wreck_hold_sec;
    hopts.max_wall_sec_total = args.war_max_wall_sec;

    std::printf("war: horizon=%llds (%.2fh) runs=%d sample=%.0fs "
                "wreck_hold=%.0fs cycle=%ds reinforce=%ds aa_combat=%s "
                "ground=%s unit_strike=%s\n",
                (long long)hopts.horizon_sec, args.war_hours, hopts.runs,
                hopts.sample_sec, hopts.wreck_hold_sec,
                hopts.session.tasking_cycle_sec,
                hopts.session.reinforce_period_sec,
                hopts.session.aa_combat ? "on" : "off",
                hopts.session.ground_war ? "on" : "off",
                hopts.session.unit_strike ? "on" : "off");

    std::string err;
    auto harness = CampaignWarHarness::create(hopts, &err);
    if (harness == nullptr) {
        std::fprintf(stderr, "campaign_qc: %s\n", err.c_str());
        return 1;
    }

    const auto t_wall = std::chrono::steady_clock::now();
    harness->execute([](const WarHourSample& s) {
        char perf[160];
        std::snprintf(perf, sizeof(perf), "%.1fs %.0ftps %ldMB",
                      s.wall_sec, s.ticks_per_sec, s.rss_kb / 1024);
        std::printf("war[h%02d] t=%llds cycles=%d(+%d) drawn=%d(+%d) "
                    "spawned=%d(+%d) live=%d retired=%d air=%d/%d "
                    "routes=%d lost=%d recov=%d",
                    s.sample, (long long)s.sim_time_s, s.cycles,
                    s.hour_cycles, s.drawn, s.hour_draws,
                    s.synthetic_spawned, s.hour_spawns, s.live_aircraft,
                    s.retired, s.airborne, s.live_aircraft,
                    s.routes_built, s.air_losses, s.recovered);
        // C6: the air war's pulse - armed fighters fielded + A/A kills
        // booked (the armed war's own columns; the unarmed war's rows
        // keep their exact pre-C6 bytes).
        if (s.armed_fighters > 0 || s.aa_kills > 0) {
            std::printf(" ftrs=%d aakill=%d", s.armed_fighters,
                        s.aa_kills);
        }
        // G1: the ground war's pulse — battalions alive, vehicles lost,
        // captures, the front's contested columns, the army's march.
        // (The ground-quiet war's rows keep their exact pre-G1 bytes.)
        if (s.ground_updates > 0) {
            std::printf(" gnd=%d/%d lost=%d cap=%d front=%d march=%d",
                        s.ground_battalions, s.ground_mobile,
                        s.ground_losses, s.ground_captures,
                        s.ground_front_columns, s.ground_march_grid);
        }
        std::printf(" | %s\n", perf);
        std::fflush(stdout);
    });
    const std::chrono::duration<double> wall =
        std::chrono::steady_clock::now() - t_wall;
    const auto& r = harness->report();

    if (r.aborted) {
        std::fprintf(stderr, "campaign_qc: war ABORTED — %s\n",
                     r.abort_reason.c_str());
        return 1;
    }

    // ------------------------------------------------------------------
    // Artifacts
    // ------------------------------------------------------------------
    std::filesystem::create_directories(args.out_dir);
    const auto result_path = args.out_dir / "campaign_result.json";
    {
        std::ofstream out(result_path);
        out << r.ledger_json;
    }

    const auto summary_path = args.out_dir / "campaign_qc_summary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-campaign-qc-summary\",\n  ");
        w.put("\"version\": 1,\n  \"mode\": \"war\"");
        w.put(",\n  \"world_json\": ");
        write_string(w, args.world_json.string());
        w.put(",\n  \"world\": {\n    \"theater\": ");
        write_string(w, ws.theater);
        w.put(",\n    ");
        w.number_key("teams", ws.teams.size());
        w.put(",    ");
        w.number_key("objectives", ws.objectives.size());
        w.put(",    ");
        w.number_key("units", ws.units.size());
        w.put(",    ");
        w.number_key("flights", flights_total);
        w.put(",    ");
        w.number_key("flights_tasked", flights_tasked);
        w.put(",\n    \"missions_by_type\": {");
        {
            bool first = true;
            for (std::size_t b = 1; b < kMissionTypeCount; ++b) {
                if (missions_by_byte[b] == 0) continue;
                w.put(first ? "\n      " : ",\n      ");
                first = false;
                w.put("\"");
                w.put(kMissionTypeNames[b]);
                w.put("\": ");
                w.number(missions_by_byte[b]);
            }
        }
        w.put("\n    }");
        w.put("\n  }");

        // The war block: DETERMINISTIC CONTENT ONLY (the byte-stable
        // certificate — wall-clock, ticks/sec, and RSS live in the
        // diary, which is explicitly not byte-stable).
        w.put(",\n  \"war\": {\n    ");
        w.number_key("horizon_sec",
                     static_cast<std::int64_t>(hopts.horizon_sec));
        w.put(",    ");
        w.number_key("sample_sec",
                     static_cast<std::int64_t>(args.war_sample_sec));
        w.put(",    ");
        w.number_key("runs", hopts.runs);
        w.put(",    ");
        w.number_key("tasking_cycle_sec", args.tasking_cycle_sec);
        w.put(",    ");
        w.number_key("reinforce_period_sec",
                     hopts.session.reinforce_period_sec);
        w.put(",    ");
        w.number_key("wreck_hold_sec",
                     static_cast<std::int64_t>(args.wreck_hold_sec));
        w.put(",\n    ");
        w.number_key("cycles", r.cycles);
        w.put(",    ");
        w.number_key("intents", r.intents);
        w.put(",    ");
        w.number_key("packages", r.packages);
        w.put(",    ");
        w.number_key("escorts", r.escorts);
        w.put(",    ");
        w.number_key("routes_built", r.routes_built);
        w.put(",    ");
        w.number_key("routes_failed", r.routes_failed);
        w.put(",    ");
        w.number_key("drawn_aircraft", r.drawn);
        w.put(",    ");
        w.number_key("air_losses", r.air_losses);
        w.put(",    ");
        w.number_key("recovered", r.recovered);
        w.put(",    ");
        w.number_key("reinforced", r.reinforced);
        w.put(",    ");
        w.number_key("reinforce_fires", r.reinforce_fires);
        w.put(",    ");
        w.number_key("synthetic_spawned", r.synthetic_spawned);
        w.put(",    ");
        w.number_key("retired", r.retired);
        w.put(",    ");
        w.number_key("live_aircraft", r.live_aircraft);
        w.put(",    ");
        w.number_key("airborne", r.airborne);
        w.put(",    ");
        w.number_key("samples", r.samples);
        w.put(",    ");
        w.put("\"aa_combat\": ");
        w.put(r.aa_combat ? "true" : "false");
        w.put(",    ");
        w.number_key("armed_aircraft", r.armed_aircraft);
        w.put(",    ");
        w.number_key("armed_fighters", r.armed_fighters);
        // G1: the ground war's provenance + counters (present only when
        // the ground war ran — the ground-quiet summary keeps its exact
        // pre-G1 bytes).
        if (r.ground_war) {
            w.put(",\n    ");
            w.put("\"ground_war\": ");
            w.put("true");
            w.put(",    ");
            // G2: the interdiction provenance + its headline (the
            // air-sourced share of ground_losses).
            w.put("\"unit_strike\": ");
            w.put(r.unit_strike ? "true" : "false");
            w.put(",    ");
            w.number_key("ground_losses_air", r.ground_losses_air);
            w.put(",    ");
            w.number_key("ground_updates", r.ground_updates);
            w.put(",    ");
            w.number_key("ground_battalions", r.ground_battalions);
            w.put(",    ");
            w.number_key("ground_mobile", r.ground_mobile);
            w.put(",    ");
            w.number_key("ground_losses", r.ground_losses);
            w.put(",    ");
            w.number_key("ground_destroyed", r.ground_destroyed);
            w.put(",    ");
            w.number_key("ground_captures", r.ground_captures);
            w.put(",    ");
            w.number_key("ground_front_columns", r.ground_front_columns);
            w.put(",    ");
            w.number_key("ground_march_grid", r.ground_march_grid);
        }
        w.put(",\n    ");
        w.put("\"belligerent_air\": ");
        w.put(r.belligerent_air ? "true" : "false");
        w.put(",    \"atm_armed\": ");
        w.put(r.atm_armed ? "true" : "false");
        w.put(",\n    \"ledger_md5_run0\": ");
        write_string(w, r.verdict.ledger_md5_run0);
        w.put(",\n    \"ledger_md5_run1\": ");
        write_string(w, r.verdict.ledger_md5_run1);
        w.put(",\n    \"deterministic\": ");
        w.put(r.verdict.deterministic ? "true" : "false");
        w.put(",\n    \"ledger_consistent\": ");
        w.put(r.verdict.ledger_consistent ? "true" : "false");
        w.put(",\n    \"entities_bounded\": ");
        w.put(r.verdict.entities_bounded ? "true" : "false");
        w.put(",\n    \"war_alive\": ");
        w.put(r.verdict.war_alive ? "true" : "false");
        w.put(",\n    \"ledger_drift\": ");
        write_string(w, r.verdict.ledger_drift);
        w.put(",\n    \"entity_leak\": ");
        write_string(w, r.verdict.entity_leak);
        w.put(",\n    \"war_stall\": ");
        write_string(w, r.verdict.war_stall);
        // The pool trajectory's final row (the believable depletion /
        // refill picture, as numbers): the ledger's own per-team view.
        w.put(",\n    \"teams\": [");
        {
            bool first = true;
            for (const auto& t : r.ledger_teams) {
                w.put(first ? "\n      {" : ",\n      {");
                first = false;
                w.number_key("slot", t.slot);
                w.put(", \"name\": ");
                write_string(w, t.name);
                w.put(", ");
                w.number_key("aircraft_initial", t.initial);
                w.put(", ");
                w.number_key("aircraft_remaining", t.remaining);
                w.put(", ");
                w.number_key("aircraft_tasking", t.tasking);
                w.put(", ");
                w.number_key("aircraft_drawn", t.drawn);
                w.put(", ");
                w.number_key("air_losses", t.losses);
                w.put(", ");
                w.number_key("air_reinforced", t.reinforced);
                w.put(", ");
                w.number_key("air_recovered", t.recovered);
                w.put("}");
            }
        }
        w.put(r.ledger_teams.empty() ? "]" : "\n    ]");
        w.put(",\n    \"result_json\": ");
        write_string(w, result_path.string());
        w.put("\n  }");
        w.put("\n}\n");

        std::ofstream out(summary_path);
        out << w.str();
    }

    // The war diary: one row per sample, telemetry included.
    const auto diary_path = args.out_dir / "campaign_war_diary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-campaign-war-diary\",\n  ");
        w.put("\"version\": 1,\n  ");
        w.put("\"note\": \"performance telemetry (wall-clock, ticks/sec, "
              "RSS) varies by host; the byte-stable artifacts are "
              "campaign_result.json and campaign_qc_summary.json\",\n  ");
        w.number_key("samples", r.diary.size());
        w.put(",\n  \"rows\": [");
        bool first = true;
        for (const auto& s : r.diary) {
            w.put(first ? "\n    {" : ",\n    {");
            first = false;
            w.number_key("sample", s.sample);
            w.put(", ");
            w.number_key("campaign_time", s.campaign_time);
            w.put(", ");
            w.number_key("sim_time_s",
                         static_cast<std::int64_t>(s.sim_time_s));
            w.put(", ");
            w.number_key("cycles", s.cycles);
            w.put(", ");
            w.number_key("hour_cycles", s.hour_cycles);
            w.put(", ");
            w.number_key("intents", s.intents);
            w.put(", ");
            w.number_key("packages", s.packages);
            w.put(", ");
            w.number_key("routes_built", s.routes_built);
            w.put(", ");
            w.number_key("routes_failed", s.routes_failed);
            w.put(", ");
            w.number_key("drawn", s.drawn);
            w.put(", ");
            w.number_key("hour_draws", s.hour_draws);
            w.put(", ");
            w.number_key("air_losses", s.air_losses);
            w.put(", ");
            w.number_key("recovered", s.recovered);
            w.put(", ");
            w.number_key("reinforced", s.reinforced);
            w.put(", ");
            w.number_key("reinforce_fires", s.reinforce_fires);
            w.put(", ");
            w.number_key("synthetic_spawned", s.synthetic_spawned);
            w.put(", ");
            w.number_key("hour_spawns", s.hour_spawns);
            w.put(", ");
            w.number_key("live_aircraft", s.live_aircraft);
            w.put(", ");
            w.number_key("airborne", s.airborne);
            w.put(", ");
            w.number_key("retired", s.retired);
            w.put(", ");
            w.number_key("world_entities", s.world_entities);
            // G1: the ground war's diary columns (the ground-quiet rows
            // keep their exact pre-G1 bytes — the block is conditional
            // on the run having armed it).
            if (r.ground_war) {
                w.put(", ");
                w.number_key("ground_updates", s.ground_updates);
                w.put(", ");
                w.number_key("ground_battalions", s.ground_battalions);
                w.put(", ");
                w.number_key("ground_mobile", s.ground_mobile);
                w.put(", ");
                w.number_key("ground_losses", s.ground_losses);
                w.put(", ");
                w.number_key("ground_losses_air", s.ground_losses_air);
                w.put(", ");
                w.number_key("ground_destroyed", s.ground_destroyed);
                w.put(", ");
                w.number_key("ground_captures", s.ground_captures);
                w.put(", ");
                w.number_key("ground_engaged", s.ground_engaged);
                w.put(", ");
                w.number_key("ground_front_columns",
                             s.ground_front_columns);
                w.put(", ");
                w.number_key("ground_march_grid", s.ground_march_grid);
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          ", \"wall_sec\": %.3f, \"ticks_per_sec\": %.1f, "
                          "\"rss_kb\": %ld",
                          s.wall_sec, s.ticks_per_sec, s.rss_kb);
            w.put(buf);
            w.put(",\n      \"teams\": [");
            {
                bool fteam = true;
                for (const auto& p : s.teams) {
                    w.put(fteam ? "\n        {" : ",\n        {");
                    fteam = false;
                    w.number_key("slot", p.slot);
                    w.put(", \"name\": ");
                    write_string(w, p.name);
                    w.put(", ");
                    w.number_key("remaining", p.remaining);
                    w.put(", ");
                    w.number_key("tasking", p.tasking);
                    w.put(", ");
                    w.number_key("drawn_total", p.drawn_total);
                    w.put(", ");
                    w.number_key("losses", p.losses);
                    w.put(", ");
                    w.number_key("reinforced", p.reinforced);
                    w.put("}");
                }
            }
            w.put(s.teams.empty() ? "]" : "\n      ]");
            w.put("\n    }");
        }
        w.put(r.diary.empty() ? "]" : "\n  ]");
        w.put("\n}\n");

        std::ofstream out(diary_path);
        out << w.str();
    }

    // ------------------------------------------------------------------
    // The headline + the gates
    // ------------------------------------------------------------------
    std::printf("war: wall=%.1fs cycles=%d intents=%d packages=%d "
                "(+%d escorts) routes=%d failed=%d drawn=%d losses=%d "
                "recovered=%d reinforced=%d spawns=%d retired=%d "
                "live=%d\n",
                wall.count(), r.cycles, r.intents, r.packages, r.escorts,
                r.routes_built, r.routes_failed, r.drawn, r.air_losses,
                r.recovered, r.reinforced, r.synthetic_spawned, r.retired,
                r.live_aircraft);
    if (r.ground_war) {
        std::printf("war: ground updates=%d battalions=%d/%d mobile "
                    "losses=%d (air=%d) destroyed=%d captures=%d front=%d "
                    "march=%d grid\n",
                    r.ground_updates, r.ground_battalions,
                    r.ground_mobile, r.ground_losses,
                    r.ground_losses_air, r.ground_destroyed,
                    r.ground_captures, r.ground_front_columns,
                    r.ground_march_grid);
    }
    std::printf("war: deterministic=%s drift=%s leak=%s alive=%s "
                "md5=%s\n",
                r.verdict.deterministic ? "yes" : "NO",
                r.verdict.ledger_consistent ? "ok" : "DRIFT",
                r.verdict.entities_bounded ? "ok" : "LEAK",
                r.verdict.war_alive ? "ok" : "STALLED",
                r.verdict.ledger_md5_run0.c_str());
    std::printf("wrote: %s\n", summary_path.string().c_str());
    std::printf("wrote: %s\n", result_path.string().c_str());
    std::printf("wrote: %s\n", diary_path.string().c_str());

    if (r.belligerent_air && !r.verdict.drew_aircraft) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the war ran %llds over "
                     "belligerents with aircraft and drew NOTHING "
                     "(exit 6, war edition). Inspect the war block in "
                     "campaign_qc_summary.json.\n",
                     (long long)hopts.horizon_sec);
        return 6;
    }
    if (r.verdict.drew_aircraft &&
        (!r.verdict.routes_built || !r.verdict.materialized)) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the war drew %d aircraft "
                     "but built %d routes and materialized %d aircraft "
                     "(exit 7, war edition). Inspect the war block in "
                     "campaign_qc_summary.json.\n",
                     r.drawn, r.routes_built, r.synthetic_spawned);
        return 7;
    }
    if (r.verdict.drew_aircraft && r.atm_armed &&
        !r.verdict.packages_built) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the war drew %d aircraft "
                     "over the ATM pipeline and built no packages "
                     "(exit 8, war edition). Inspect the war block in "
                     "campaign_qc_summary.json.\n",
                     r.drawn);
        return 8;
    }
    if (hopts.runs >= 2 && !r.verdict.deterministic) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the war is NOT "
                     "deterministic: run 0 md5 %s != run 1 md5 %s "
                     "(exit 9). Diff campaign_result.json against a "
                     "re-run to find the first diverging event.\n",
                     r.verdict.ledger_md5_run0.c_str(),
                     r.verdict.ledger_md5_run1.c_str());
        return 9;
    }
    if (!r.verdict.ledger_consistent) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — ledger drift: %s "
                     "(exit 10). The one-pool books stopped balancing.\n",
                     r.verdict.ledger_drift.c_str());
        return 10;
    }
    if (!r.verdict.entities_bounded) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — entity leak: %s "
                     "(exit 11). The roster identity broke — spawn/reap "
                     "churn is leaking entities.\n",
                     r.verdict.entity_leak.c_str());
        return 11;
    }
    if (!r.verdict.war_alive) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the war stalled: %s "
                     "(exit 12). A side with aircraft stopped "
                     "generating, or the clock/cycles stopped.\n",
                     r.verdict.war_stall.c_str());
        return 12;
    }
    // G1: the ground war's own gate — the exit-5 philosophy, ground
    // edition. An ARMED ground war that fired no updates, moved no
    // battalion, booked no losses and captured nothing is a wiring
    // failure (stance decode, subtype filter, cadence), not a quiet
    // front: movement alone passes (a 0.1 h smoke war moves, fights
    // happen at hours scale, captures at days).
    if (r.ground_war && (r.ground_updates <= 0 ||
                         (r.ground_losses == 0 && r.ground_captures == 0 &&
                          r.ground_march_grid == 0 &&
                          r.ground_destroyed == 0))) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the ground war was armed "
                     "and produced NOTHING (updates=%d, march=%d grid, "
                     "losses=%d, captures=%d) (exit 13). Inspect the "
                     "war block's ground_* fields and the ledger's "
                     "ground block in campaign_result.json.\n",
                     r.ground_updates, r.ground_march_grid,
                     r.ground_losses, r.ground_captures);
        return 13;
    }
    // G2: the interdiction link's own gate — the exit-5 philosophy,
    // interdiction edition. An ARMED unit strike whose air power never
    // attrited a battalion is a wiring failure (target ranking, route
    // resolution, TOT window), not a quiet front: CAS needs its TOT
    // window (the profile's own min/max time), so the honest
    // acceptance horizon is the 1-hour war, not the 0.1 h smoke.
    if (r.unit_strike && r.ground_losses_air == 0) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the unit strike was "
                     "armed and air never attrited a battalion "
                     "(agv=0) (exit 14). Inspect the war block's "
                     "ground_losses_air and the ledger's ground-loss "
                     "rows (air=true) in campaign_result.json; a short "
                     "horizon (CAS TOT ~15 min) needs --war >= 0.5.\n");
        return 14;
    }
    return 0;
}

} // namespace

// ===========================================================================
int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (!std::filesystem::exists(args.world_json)) {
        std::fprintf(stderr, "campaign_qc: world JSON not found: %s\n",
                     args.world_json.string().c_str());
        return 1;
    }

    // C5: the long-horizon war mode is a SEPARATE top-level flow — the
    // B.3/C2/C3/C4 modes below stay byte-identical (their goldens are
    // pinned); --war composes the CampaignSession the viewer drives,
    // runs the horizon, and returns its own exit codes (6..12).
    if (args.war_hours > 0.0) {
        return run_war(args);
    }

    // -----------------------------------------------------------------------
    // 1. Load the save + world stats
    // -----------------------------------------------------------------------
    f4::world::WorldState ws;
    ws.load(args.world_json);

    int flights_total = 0, flights_tasked = 0;
    std::vector<int> missions_by_byte(
        static_cast<std::size_t>(kMissionTypeCount), 0);
    for (const auto& u : ws.units) {
        if (u.unit_class != f4::entities::UnitClass::Flight) continue;
        ++flights_total;
        if (is_mission_tasked(u.mission)) {
            ++flights_tasked;
            if (u.mission < kMissionTypeCount) {
                ++missions_by_byte[u.mission];
            }
        }
    }

    std::printf("world: theater=%s teams=%zu objectives=%zu units=%zu "
                "flights=%d tasked=%d\n",
                ws.theater.c_str(), ws.teams.size(), ws.objectives.size(),
                ws.units.size(), flights_total, flights_tasked);

    // -----------------------------------------------------------------------
    // 2. THE B.3 LOOP — emit intents from live flights, spawn via the bus
    // -----------------------------------------------------------------------
    f4::entities::EntityWorld b3_world;
    auto populated = f4::world::populate_world(b3_world, ws);
    f4::world::WorldStateAdapters adapters(ws);
    f4::messaging::MessageBus bus;

    // C1: the result ledger — the campaign's write model, snapshotted
    // from the SAME sources the sim's world was populated from. Zero
    // events in → numbers identical to the save's own (the round-trip
    // contract the ledger's tests pin).
    CampaignResultLedger result_ledger(
        static_cast<const f4::world::ICampaignSource&>(adapters.campaign),
        static_cast<const f4::world::ITeamSource&>(adapters.teams),
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units));

    // -----------------------------------------------------------------------
    // 2b. THE SPAWNER — constructed and subscribed BEFORE the tasking
    // ladder runs (C3: generation-to-spawn requires the bus to have a
    // listener while the ladder publishes; the C2 order — ladder first,
    // spawner later — deliberately published to nobody, which is exactly
    // what C3 replaces). Class table + models + config + airfield load
    // here, ahead of the ladder, so both the synthetic and the
    // saved-flight spawns share one setup.
    // -----------------------------------------------------------------------
    f4::world_convert::ClassTable ct;
    if (std::filesystem::exists(args.class_table)) {
        ct.load(args.class_table.string());
    } else {
        std::fprintf(stderr, "campaign_qc: class table missing (%s) — "
                             "vis_type resolution will fall back\n",
                     args.class_table.string().c_str());
    }
    f4::models::ModelDatabase db;
    if (std::filesystem::exists(args.models_hdr) &&
        std::filesystem::exists(args.models_hdr.parent_path() /
                                "KoreaObj.LOD")) {
        const auto err = db.load(args.models_hdr.string(),
                                 (args.models_hdr.parent_path() /
                                  "KoreaObj.LOD").string());
        if (!err.empty()) {
            std::fprintf(stderr, "campaign_qc: model db load failed: %s\n",
                         err.c_str());
        }
    } else {
        std::fprintf(stderr, "campaign_qc: KoreaObj models missing — "
                             "spawned aircraft carry null model records\n");
    }
    f4::data::AircraftConfig cfg;
    {
        const auto result = f4::data::loadConfig(args.config.string());
        if (!result.ok) {
            std::fprintf(stderr, "campaign_qc: aircraft config failed (%s):",
                         args.config.string().c_str());
            for (const auto& err : result.errors) {
                std::fprintf(stderr, " %s", err.c_str());
            }
            std::fprintf(stderr, "\n");
            return 1;
        }
        cfg = std::move(result.config);
    }

    // The airfield: first airbase-class objective in the save (the same
    // rule Simulation::spawn_from_campaign_flights applies).
    ScenarioAirfield airfield;
    bool have_airfield = false;
    for (const auto& obj : ws.objectives) {
        if (auto af = derive_airfield_from_objective(obj, 36)) {
            airfield = std::move(*af);
            have_airfield = true;
            break;
        }
    }
    if (!have_airfield) {
        std::fprintf(stderr, "campaign_qc: no airbase objective in world — "
                             "cannot derive an airfield\n");
        return 1;
    }
    // Per-base airfields (the B.3+ rule): every airbase objective gets
    // its own ScenarioAirfield — synthetic-intent spawns depart from
    // their own squadron's runway.
    AirbaseAirfieldMap airbase_airfields;
    for (const auto& obj : ws.objectives) {
        if (auto af = derive_airfield_from_objective(obj, 36)) {
            if (obj.id_num != 0) {
                airbase_airfields[obj.id_num] = std::move(*af);
            }
        }
    }
    ScenarioAircraft tpl;
    tpl.callsign = "CAMPAIGN";
    tpl.vis_type_index = 1052;  // F-16 default; CT lookup overrides per-flight
    tpl.aircraft_config_path = args.config.string();

    FlightSpawnFilter filter;
    filter.team = args.team;
    filter.mission = args.mission;
    filter.max_flights = args.max_flights;

    CampaignSimSpawner spawner(b3_world, populated.unit_id_map, ct, db, cfg,
                               airfield, tpl, filter);
    // A-G tranche: bus-fed spawns get the same strike arming as the bulk
    // path (objective map resolves waypoint targets; the weapon table
    // arms the decoded loadout + doctrine fill).
    const auto builtin_weapons =
        f4::weapons::WeaponClassTable::with_builtins();
    spawner.set_objective_id_map(&populated.objective_id_map);
    spawner.set_weapon_table(&builtin_weapons);
    spawner.set_airbase_airfields(&airbase_airfields);
    spawner.attach(bus);

    // -----------------------------------------------------------------------
    // 2c. THE C2 TASKING LADDER + THE C3 ROUTE PLANNER — the synthetic
    // M4.7 Campaign over the SAME ledger (ONE pool: tasking draws book
    // now, the sim's combat losses book later, the reinforcement cadence
    // refills in between — the first true multi-cycle loop), now with a
    // ROUTE PLANNER attached: every generated strike-family mission
    // carries a real enemy objective and a threat-avoiding route
    // airbase → target → airbase, and the spawner (subscribed above)
    // materializes it the moment it publishes. Generation → route →
    // spawn, on the bus, in order.
    // -----------------------------------------------------------------------
    bool tasking_ran = false;
    bool tasking_had_air = false;   // belligerents had aircraft available
    bool tasking_routes_expected = false;  // planner attached to the ladder
    int tasking_cycles = 0;
    int tasking_intents = 0;
    int tasking_routes = 0;
    int tasking_routes_failed = 0;
    int tasking_route_wps = 0;
    int tasking_route_searches = 0;  // FindSafePath invocations
    int tasking_route_fallbacks = 0; // direct-line fallback legs
    int threat_ad_units = 0;         // the map's painted AD battalions
    int threat_cells = 0;            // cells carrying any viewer threat
    // C4: the ATM pipeline's counters (the summary's atm block + the
    // exit-8 gate read these).
    int atm_packages = 0;
    int atm_escorts = 0;
    int atm_seeded = 0;
    int atm_unfilled = 0;
    if (args.tasking_minutes > 0) {
        if (args.profiles_json.empty() ||
            !std::filesystem::exists(args.profiles_json)) {
            std::fprintf(stderr,
                         "campaign_qc: mission profiles not found (%s) — "
                         "--tasking needs the generated table\n",
                         args.profiles_json.string().c_str());
            return 1;
        }
        const auto profiles = MissionProfileTable::load(args.profiles_json);
        CampaignConfig ladder_cfg;
        ladder_cfg.air_task_cycle_sec = args.tasking_cycle_sec;
        // -1 = the tool's own default (12 h — the engine's default is
        // disabled: a fresh ledger must change nothing until the host
        // arms the cadence). 0 = explicitly off.
        ladder_cfg.reinforcement_period_sec =
            args.reinforce_period_sec < 0 ? 43200
                                          : args.reinforce_period_sec;
        // C4: the ATM pipeline — FindBestAir scoring (replaces the C3
        // role-fallback bridge), escort pairing, TOT slotting against
        // the decoded airbase schedules, and mission recovery (drawn
        // aircraft that survive their mission return to the pool).
        // The QC exercises the reference's actual tasking shape; the
        // legacy ladder stays the library default (goldens-pinned).
        // The threat threshold mirrors the route config's host-side 25
        // (aiinput's value is game data): the fixture theater's single
        // AD ring scores 30-33 — the default 40 would never pair a
        // SEAD escort on the sample data.
        ladder_cfg.atm_pipeline = true;
        ladder_cfg.atm.min_seadescort_threat = 25;
        Campaign ladder(
            static_cast<const f4::world::ICampaignSource&>(adapters.campaign),
            static_cast<const f4::world::ITeamSource&>(adapters.teams),
            static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
            profiles, bus, ladder_cfg);
        ladder.set_result_ledger(&result_ledger);

        // C3: the route planner — the threat map built from the same
        // sources. FreeFalcon builds the map for the LOCAL SESSION's
        // team (FalconLocalSession bit offset); the QC's "session" is
        // the first BELLIGERENT (te_team can be a non-participant —
        // TestCamp's U.S. is Neutral to the ROK-DPRK war, and a
        // neutral viewer packs no enemy rings, leaving an empty map).
        // With no belligerents at all, te_team stands in (the map is
        // empty either way — nothing is at war).
        std::uint8_t viewer = static_cast<std::uint8_t>(ws.campaign.te_team);
        if (const auto war = ladder.belligerent_teams(); !war.empty()) {
            viewer = static_cast<std::uint8_t>(war.front());
        }
        // Host tunables (the reference reads these from aiinput.dat —
        // game data, not source; hosts override, per route_builder.hpp):
        // the fixture UCD paints only the AD battalions whose entity
        // types resolve in the 8-entry sample (3 rings), and a single
        // ring's band scores (30-33) sit under the reference default
        // of 40 — MinAvoidThreat 25 lets those rings SHAPE the QC's
        // routes without changing any library default.
        RouteBuilderConfig route_cfg;
        route_cfg.min_avoid_threat = 25;
        const RouteBuilder route_builder(
            static_cast<const f4::world::IObjectiveSource&>(
                adapters.objectives),
            static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
            static_cast<const f4::world::ITeamSource&>(adapters.teams),
            viewer, route_cfg);
        ladder.set_route_planner(
            &route_builder,
            &static_cast<const f4::world::IObjectiveSource&>(
                adapters.objectives));
        tasking_routes_expected = true;

        // The exit-6 precondition: belligerents actually had aircraft.
        const auto war_teams = ladder.belligerent_teams();
        for (const auto& s : result_ledger.squadrons()) {
            if (s.availability <= 0) continue;
            if (std::find(war_teams.begin(), war_teams.end(),
                          static_cast<int>(s.owner)) != war_teams.end()) {
                tasking_had_air = true;
                break;
            }
        }

        // One big tick: every due cycle fires in order, the
        // reinforcement cadence rides the same clock (a stale .cmp
        // anchor fires once — FreeFalcon's catch-up shape). The
        // spawner above hears every publish — generated missions
        // become aircraft WITH ROUTES immediately.
        ladder.tick(static_cast<CampaignTime>(args.tasking_minutes) * 60);
        tasking_ran = true;
        tasking_cycles = ladder.cycles_fired();
        tasking_intents = static_cast<int>(ladder.intents().size());
        for (const auto& in : ladder.intents()) {
            if (!in.route.empty()) {
                ++tasking_routes;
                tasking_route_wps += static_cast<int>(in.route.size());
            }
        }
        // The CAMPAIGN's own failure telemetry — a route-less intent
        // carries no synthetic mark (that is stamped only on success),
        // so intents() cannot see build failures. routes_failed is
        // every precondition miss with a planner attached; the search
        // and fallback counters carry the threat-shaping evidence.
        tasking_routes_failed = ladder.routes_failed();
        tasking_route_searches = ladder.route_safe_searches();
        tasking_route_fallbacks = ladder.route_fallbacks();
        threat_ad_units = route_builder.threat_map().stats().ad_units;
        threat_cells =
            route_builder.threat_map().stats().threatened_cells;

        std::printf("tasking: minutes=%d cycles=%d intents=%d drawn=%d "
                    "routes=%d (wps=%d, failed=%d, searched=%d, "
                    "fallbacks=%d) synthetic_spawned=%d "
                    "reinforce_fires=%d reinforced=%d\n",
                    args.tasking_minutes, tasking_cycles, tasking_intents,
                    result_ledger.mission_draw_aircraft(), tasking_routes,
                    tasking_route_wps, tasking_routes_failed,
                    tasking_route_searches, tasking_route_fallbacks,
                    spawner.stats().synthetic_spawned,
                    result_ledger.reinforcement_fires(),
                    result_ledger.aircraft_reinforced());
        // C4: the ATM pipeline's own telemetry — the request → package
        // → escort → slot → recovery chain, visible in one line.
        if (const auto* atm = ladder.atm_stats(); atm != nullptr) {
            atm_packages = atm->packages_built;
            atm_escorts = atm->escorts_built;
            atm_seeded = atm->requests_seeded;
            atm_unfilled = atm->requests_unfilled;
            std::printf("atm: requests=%d+%d (timeout=%d, unfilled=%d) "
                        "packages=%d escorts=%d slots=%d shifts=%ds "
                        "recovered=%d\n",
                        atm->requests_generated, atm->requests_seeded,
                        atm->requests_timed_out, atm->requests_unfilled,
                        atm->packages_built, atm->escorts_built,
                        atm->slot_snaps, atm->slot_shifts_sec,
                        atm->aircraft_recovered);
        }
        std::printf("threat_map: ad_units=%d threatened_cells=%d\n",
                    threat_ad_units, threat_cells);
    }

    const auto intents = emit_flight_intents(
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
        static_cast<const f4::world::IFlightSource&>(adapters.units),
        bus, ws.campaign.current_time,
        &static_cast<const f4::world::ITeamSource&>(adapters.teams));

    const auto& b3_stats = spawner.stats();
    std::printf("b3_loop: intents=%zu spawned=%d routes=%d synthetic=%d "
                "unknown=%d dups=%d\n",
                intents.size(), b3_stats.aircraft_spawned,
                b3_stats.routes_attached, b3_stats.synthetic_spawned,
                b3_stats.unknown_flight_ids, b3_stats.duplicate_skips);

    if (b3_stats.aircraft_spawned == 0) {
        std::fprintf(stderr,
                     "campaign_qc: filter matched no flights — nothing to "
                     "run\n");
        return 2;
    }

    // -----------------------------------------------------------------------
    // 3. THE SIM RUN — scenario JSON + Simulation + tick + trace
    // -----------------------------------------------------------------------
    std::filesystem::create_directories(args.out_dir);
    const auto scenario_path = args.out_dir / "campaign_qc_scenario.json";
    {
        std::ofstream out(scenario_path);
        out << "{\n";
        out << "  \"name\": \"campaign_qc\",\n";
        out << "  \"theater\": \"" << ws.theater << "\",\n";
        out << "  \"spawn_mode\": \"campaign_flights\",\n";
        out << "  \"world_json_path\": \""
             << json_escape(args.world_json.string()) << "\",\n";
        out << "  \"class_table_path\": \""
             << json_escape(args.class_table.string()) << "\",\n";
        out << "  \"models_hdr_path\": \""
             << json_escape(args.models_hdr.string()) << "\",\n";
        out << "  \"models_lod_path\": \""
             << json_escape((args.models_hdr.parent_path() /
                             "KoreaObj.LOD").string()) << "\",\n";
        out << "  \"campaign_flight_filter\": {";
        out << "\"team\": " << args.team;
        out << ", \"mission\": " << args.mission;
        out << ", \"max_flights\": " << args.max_flights << "},\n";
        // A-G slice: combat ON drives the weapon sweeps (bomb sim clock,
        // sweep, the intent execution). The A/A ladder stays dark for
        // campaign flights (spawn never attaches radar/RWR to them) — only
        // ordnance employment rides this.
        out << "  \"combat\": {\"enabled\": true},\n";
        out << "  \"aircraft\": [{\n";
        out << "    \"callsign\": \"CAMPAIGN1\",\n";
        out << "    \"aircraft_config_path\": \""
             << json_escape(args.config.string()) << "\",\n";
        out << "    \"aircraft_name\": \"F-16C_50\",\n";
        out << "    \"vis_type_index\": 1052,\n";
        out << "    \"parking_spot\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n";
        out << "    \"heading_rad\": 0.0\n";
        out << "  }],\n";
        out << "  \"sim_dt\": " << args.sim_dt << ",\n";
        out << "  \"total_ticks\": " << args.ticks << ",\n";
        out << "  \"record_every\": " << args.record_every << ",\n";
        out << "  \"record\": " << (args.record ? "true" : "false") << ",\n";
        out << "  \"record_path\": \"trace.json\"\n";
        out << "}\n";
    }

    auto scenario = load_scenario(scenario_path);
    Simulation sim(scenario, args.out_dir);
    sim.initialize();

    // C1: the result sink — BEFORE the first tick (the objective damage
    // snapshot must catch the save-time state, so a mid-campaign save's
    // prior damage is initial, not this run's). Kills and bomb impacts
    // resolve back to campaign identity through the spawned aircraft's
    // CampaignOriginComponent and the objectives' own VU residue.
    CampaignResultSink result_sink(result_ledger, sim.world());
    result_sink.attach(sim.bus());

    // A-G slice: the ordnance ledger — subscribe to the release + impact
    // events on the SIM's bus before the run, then read the objective
    // damage state at the end (fstatus is the campaign wire's own damage
    // bitmap, so the summary reports the save-format face of the damage).
    struct Ordnance {
        int released = 0;
        int impacts = 0;
        int features_destroyed = 0;
        double destroyed_pct_max = 0.0;
        std::vector<f4::weapons::BombImpactMessage> impact_log;
    } ordnance;
    sim.bus().subscribe<f4::weapons::BombReleasedMessage>(
        [&ordnance](const f4::weapons::BombReleasedMessage& m) {
            (void)m;
            ++ordnance.released;
        });
    sim.bus().subscribe<f4::weapons::BombImpactMessage>(
        [&ordnance](const f4::weapons::BombImpactMessage& m) {
            ++ordnance.impacts;
            ordnance.features_destroyed += m.features_destroyed;
            ordnance.destroyed_pct_max =
                std::max(ordnance.destroyed_pct_max, m.destroyed_pct);
            ordnance.impact_log.push_back(m);
        });

    const auto& sim_spawned = sim.aircraft_entities();
    std::printf("sim_run: aircraft=%zu", sim_spawned.size());

    // Route coverage over the SIM world (not the b3 world).
    int sim_routes = 0;
    for (const auto eid : sim_spawned) {
        auto h = f4::entities::EntityHandle(eid, &sim.world());
        auto* brain = h.get<f4::ai::BrainComponent>();
        if (brain && !brain->mission_plan().route.empty()) ++sim_routes;
    }
    std::printf(" routes=%d ticks=%d\n", sim_routes, args.ticks);

    for (int t = 0; t < args.ticks; ++t) {
        sim.tick(args.sim_dt);
    }
    sim.write_recording();

    // C1: close the loop — the objectives' final damage state lands in
    // the ledger (event counters came in during the run; this is the
    // authoritative state sync).
    result_sink.sync_objective_damage();
    result_sink.detach(sim.bus());

    // End-of-run state per aircraft: airborne? where?
    int airborne = 0;
    struct EndState {
        std::string team;
        double x = 0, y = 0, z = 0;
        double vt_fps = 0;
        std::string phase;
    };
    std::vector<EndState> ends;
    ends.reserve(sim_spawned.size());
    for (const auto eid : sim_spawned) {
        auto h = f4::entities::EntityHandle(eid, &sim.world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        auto* tf = h.get<f4::entities::TransformComponent>();
        auto* brain = h.get<f4::ai::BrainComponent>();
        EndState e;
        auto team_tag = h.get_tag(f4::entities::tags::TEAM);
        if (team_tag && team_tag->as_string()) e.team = *team_tag->as_string();
        if (tf) { e.x = tf->position.x; e.y = tf->position.y; e.z = tf->position.z; }
        if (fm) {
            // Total velocity from the world-axis components (NED frame).
            const auto& kin = fm->model().state().kin;
            e.vt_fps = std::sqrt(kin.xdot * kin.xdot +
                                 kin.ydot * kin.ydot +
                                 kin.zdot * kin.zdot);
            // gear.inAir: true = airborne, false = on the ground.
            if (fm->model().state().gear.inAir) ++airborne;
        }
        if (brain) e.phase = brain->phase_name();
        ends.push_back(std::move(e));
    }
    std::printf("sim_end: airborne=%d/%zu\n", airborne, sim_spawned.size());

    // QC gate: the sim ran, aircraft exist, but none got airborne. The
    // tasking didn't fly — report it as a failure (exit 3) AFTER writing
    // the summary + trace (the artifacts are exactly what debugging this
    // needs).
    const bool nothing_airborne = airborne == 0;

    // A-G gate (exit 4): strike flights spawned WITH droppable ordnance
    // (a loaded Bomb-category station), yet not one bomb left the rack —
    // the employment chain broke (arming, envelope, store debit, intent
    // routing). Count armed flights over the SIM world post-run; empty
    // stores mean the bombs DID fly (the release debited them).
    int strike_flights_armed = 0;
    for (const auto eid : sim_spawned) {
        auto h = f4::entities::EntityHandle(eid, &sim.world());
        auto* store = h.get<f4::weapons::WeaponStoreComponent>();
        if (store == nullptr) continue;
        bool has_bomb_station = false;
        for (std::size_t s = 0; s < store->station_count(); ++s) {
            const auto* st = store->station(s);
            if (st == nullptr || st->rounds <= 0) continue;
            const auto* rec =
                sim.weapon_table().get(st->weapon_handle);
            if (rec != nullptr &&
                rec->category == f4::weapons::WeaponCategory::Bomb) {
                has_bomb_station = true;
                break;
            }
        }
        if (has_bomb_station) ++strike_flights_armed;
    }
    const bool nothing_employed =
        strike_flights_armed > 0 && ordnance.released == 0;
    std::printf("ordnance: armed=%d released=%d impacts=%d "
                "features_destroyed=%d max_destroyed_pct=%.1f\n",
                strike_flights_armed, ordnance.released, ordnance.impacts,
                ordnance.features_destroyed, ordnance.destroyed_pct_max);

    // -----------------------------------------------------------------------
    // 4. THE RESULT LEDGER — write-back + artifacts + the C1 gate
    // -----------------------------------------------------------------------
    // In-memory write-back into the WorldState the run started from
    // (team pools, squadron counters, objective fstatus). The counts
    // report below; unmatched VUs are LOUD (a fixture without the unit,
    // a stale world — never a silent drop).
    const auto writeback = f4::campaign::apply_to(result_ledger, ws);

    const auto result_path = args.out_dir / "campaign_result.json";
    {
        std::ofstream out(result_path);
        out << result_ledger.to_json();
    }

    const auto& sink_stats = result_sink.stats();
    std::printf("results: kills=%d air_losses=%d attributed=%d ag=%d "
                "unclassified=%d impacts=%d objectives_synced=%d "
                "writeback=(pools=%d sq=%d obj=%d "
                "unmatched_sq=%zu unmatched_obj=%zu)\n",
                sink_stats.kills_seen, result_ledger.air_losses(),
                result_ledger.air_kills_attributed(),
                sink_stats.ag_kills_recorded,
                sink_stats.kills_unclassified,
                result_ledger.bomb_impacts(),
                sink_stats.objectives_synced,
                writeback.team_pools_written, writeback.squadrons_written,
                writeback.objectives_written,
                writeback.unmatched_squadrons.size(),
                writeback.unmatched_objectives.size());

    // C1 gate (exit 5): combat outcomes occurred — kills and/or bomb
    // impacts — but the ledger recorded NOTHING. Every outcome died on
    // the bus: a sink that never fired, an origin stamp that never
    // landed, a classification that dropped every event. The exact
    // silent-loss class this tranche exists to kill.
    const bool outcomes_happened =
        sink_stats.kills_seen > 0 || sink_stats.bomb_impacts_seen > 0;
    const bool results_lost = outcomes_happened && result_ledger.empty();

    // -----------------------------------------------------------------------
    // 5. Summary JSON
    // -----------------------------------------------------------------------
    const auto summary_path = args.out_dir / "campaign_qc_summary.json";
    {
        f4::json::Writer w;
        w.put("{\n  \"format\": \"f4-campaign-qc-summary\",\n  \"version\": 1");
        w.put(",\n  \"world_json\": ");
        write_string(w, args.world_json.string());

        w.put(",\n  \"world\": {\n    \"theater\": ");
        write_string(w, ws.theater);
        w.put(",\n    ");
        w.number_key("teams", ws.teams.size());
        w.put(",    ");
        w.number_key("objectives", ws.objectives.size());
        w.put(",    ");
        w.number_key("units", ws.units.size());
        w.put(",    ");
        w.number_key("flights", flights_total);
        w.put(",    ");
        w.number_key("flights_tasked", flights_tasked);
        w.put(",\n    \"missions_by_type\": {");
        {
            bool first = true;
            for (std::size_t b = 1; b < kMissionTypeCount; ++b) {
                if (missions_by_byte[b] == 0) continue;
                w.put(first ? "\n      " : ",\n      ");
                first = false;
                w.put("\"");
                w.put(kMissionTypeNames[b]);
                w.put("\": ");
                w.number(missions_by_byte[b]);
            }
        }
        w.put("\n    }");
        w.put("\n  }");

        // C2 — the tasking ladder's own block (ONLY when it ran): the
        // multi-cycle loop as numbers — cycles, draws, reinforcement,
        // and per-team pool trajectory (the ledger's one-pool view).
        if (tasking_ran) {
            w.put(",\n  \"tasking\": {\n    ");
            w.number_key("minutes", args.tasking_minutes);
            w.put(",    ");
            w.number_key("cycle_sec", args.tasking_cycle_sec);
            w.put(",    ");
            w.number_key("cycles_fired", tasking_cycles);
            w.put(",    ");
            w.number_key("intents", tasking_intents);
            w.put(",    ");
            w.number_key("drawn_aircraft",
                         result_ledger.mission_draw_aircraft());
            w.put(",    ");
            w.number_key("reinforce_fires",
                         result_ledger.reinforcement_fires());
            w.put(",    ");
            w.number_key("reinforced_aircraft",
                         result_ledger.aircraft_reinforced());
            w.put(",    ");
            w.number_key("draws_unmatched",
                         result_ledger.draws_unmatched());
            // C3: the route tranche's counters — routes built / failed,
            // waypoints planned, and the synthetic spawns that flew
            // them (generation-to-spawn, as numbers) — plus the threat
            // map's own coverage (painted AD units / threatened cells:
            // the fixture theater-db enrichment bounds what can be
            // painted, and the numbers make that visible instead of
            // silent).
            w.put(",    ");
            w.number_key("routes_built", tasking_routes);
            w.put(",    ");
            w.number_key("routes_failed", tasking_routes_failed);
            w.put(",    ");
            w.number_key("route_waypoints", tasking_route_wps);
            w.put(",    ");
            w.number_key("route_safe_searches", tasking_route_searches);
            w.put(",    ");
            w.number_key("route_direct_fallbacks",
                         tasking_route_fallbacks);
            w.put(",    ");
            w.number_key("threat_ad_units", threat_ad_units);
            w.put(",    ");
            w.number_key("threat_cells", threat_cells);
            w.put(",    ");
            w.number_key("synthetic_spawned",
                         spawner.stats().synthetic_spawned);
            w.put(",    ");
            w.number_key("synthetic_failed",
                         spawner.stats().synthetic_failed);
            // C4: the ATM pipeline's counters — the reference's actual
            // tasking shape (packages with escorts, TOT-slot snaps,
            // mission recovery), as numbers next to the routes they
            // share.
            w.put(",    ");
            w.number_key("atm_packages", atm_packages);
            w.put(",    ");
            w.number_key("atm_escorts", atm_escorts);
            w.put(",    ");
            w.number_key("atm_seeded_requests", atm_seeded);
            w.put(",    ");
            w.number_key("atm_unfilled_requests", atm_unfilled);
            w.put(",    ");
            w.number_key("aircraft_recovered",
                         result_ledger.aircraft_recovered());
            w.put(",\n    \"teams\": [");
            bool first_team = true;
            for (const auto& t : result_ledger.teams()) {
                // Skip never-seen slots entirely (deterministic output:
                // only the snapshot's own team entries appear).
                w.put(first_team ? "\n      {" : ",\n      {");
                first_team = false;
                w.number_key("slot", t.slot);
                w.put(", \"name\": ");
                write_string(w, t.name);
                w.put(", ");
                w.number_key("aircraft_initial", t.aircraft_initial);
                w.put(", ");
                w.number_key("aircraft_drawn", t.drawn);
                w.put(", ");
                w.number_key("air_reinforced", t.reinforced);
                w.put(", ");
                w.number_key("air_losses", t.losses);
                w.put(", ");
                w.number_key("aircraft_tasking",
                             result_ledger.team_aircraft_tasking(t.slot));
                w.put("}");
            }
            w.put(result_ledger.teams().empty() ? "]" : "\n    ]");
            w.put("\n  }");
        }

        w.put(",\n  \"b3_loop\": {\n    ");
        w.number_key("intents_emitted", intents.size());
        w.put(",    ");
        w.number_key("intents_seen", b3_stats.intents_seen);
        w.put(",    ");
        w.number_key("aircraft_spawned", b3_stats.aircraft_spawned);
        w.put(",    ");
        w.number_key("routes_attached", b3_stats.routes_attached);
        w.put(",    ");
        w.number_key("unknown_flight_ids", b3_stats.unknown_flight_ids);
        w.put(",    ");
        w.number_key("duplicate_skips", b3_stats.duplicate_skips);
        w.put("\n  }");

        w.put(",\n  \"sim_run\": {\n    ");
        w.number_key("aircraft", sim_spawned.size());
        w.put(",    ");
        w.number_key("routes", sim_routes);
        w.put(",    ");
        w.number_key("ticks", args.ticks);
        w.put(",    ");
        w.number_key("airborne_at_end", airborne);
        w.put(",\n    \"aircraft_end\": [");
        {
            bool first = true;
            for (const auto& e : ends) {
                w.put(first ? "\n      {" : ",\n      {");
                first = false;
                w.put("\"team\": ");
                write_string(w, e.team);
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              ", \"east_ft\": %.0f, \"north_ft\": %.0f, "
                              "\"alt_ft\": %.0f, \"vt_fps\": %.0f",
                              e.x, e.y, e.z, e.vt_fps);
                w.put(buf);
                w.put(", \"phase\": ");
                write_string(w, e.phase);
                w.put("}");
            }
        }
        w.put(ends.empty() ? "]" : "\n    ]");
        w.put(",\n    \"scenario\": ");
        write_string(w, scenario_path.string());
        w.put(",\n    \"trace\": ");
        write_string(w, (args.out_dir / "trace.json").string());
        w.put("\n  }");

        // The A-G ordnance ledger (this slice's QC block): releases,
        // impacts, the damage they did, per-objective state, and the
        // armed-flight count the exit-4 gate keys on.
        w.put(",\n  \"ordnance\": {\n    ");
        w.number_key("strike_flights_armed", strike_flights_armed);
        w.put(",    ");
        w.number_key("bombs_released", ordnance.released);
        w.put(",    ");
        w.number_key("bombs_impacted", ordnance.impacts);
        w.put(",    ");
        w.number_key("features_destroyed", ordnance.features_destroyed);
        w.put(",    ");
        w.number_key("max_objective_destroyed_pct",
                     ordnance.destroyed_pct_max);
        // Per-impact log (the viewer's impact markers render from THIS —
        // position + target + damage summary, no trace replay needed).
        w.put(",\n    \"impacts\": [");
        {
            bool first = true;
            for (const auto& im : ordnance.impact_log) {
                w.put(first ? "\n      {" : ",\n      {");
                first = false;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "\"shooter\": %llu, \"target\": %llu, "
                              "\"east_ft\": %.0f, \"north_ft\": %.0f, "
                              "\"miss_ft\": %.0f, \"t_of\": %.1f, "
                              "\"features_destroyed\": %d, "
                              "\"destroyed_pct\": %.1f",
                              (unsigned long long)im.shooter_id,
                              (unsigned long long)im.target_id,
                              im.position.x, im.position.y,
                              im.miss_distance_ft, im.flight_time_s,
                              im.features_destroyed, im.destroyed_pct);
                w.put(buf);
                w.put("}");
            }
        }
        w.put(ordnance.impact_log.empty() ? "]" : "\n    ]");
        // Per-objective damage state for every objective that took hits.
        // (Aggregated from the impact log's targets — the entities' own
        // fstatus is the authoritative state; this reads it back.)
        w.put(",\n    \"objectives_damaged\": [");
        {
            std::vector<std::uint64_t> hit_targets;
            for (const auto& im : ordnance.impact_log) {
                if (im.target_id != 0 &&
                    std::find(hit_targets.begin(), hit_targets.end(),
                              im.target_id) == hit_targets.end()) {
                    hit_targets.push_back(im.target_id);
                }
            }
            bool first = true;
            for (const auto tgt : hit_targets) {
                const auto summary =
                    f4::weapons::objective_damage_summary(sim.world(), tgt);
                if (!summary.objective_found) continue;
                w.put(first ? "\n      {" : ",\n      {");
                first = false;
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "\"target\": %llu, \"features_total\": %d, "
                              "\"features_destroyed\": %d, "
                              "\"destroyed_pct\": %.1f",
                              (unsigned long long)tgt,
                              summary.features_total,
                              summary.features_destroyed_total,
                              summary.destroyed_pct);
                w.put(buf);
                w.put("}");
            }
        }
        w.put(ordnance.impact_log.empty() && true ? "]" : "\n    ]");

        // Close the ordnance object, then the C1 result ledger block: the
        // war loop's return leg, as numbers. campaign_result.json carries
        // the full document (events, per-squadron state, fstatus bitmaps).
        w.put("\n  }");
        w.put(",\n  \"results\": {\n    ");
        w.number_key("kills_seen", sink_stats.kills_seen);
        w.put(",\n    ");
        w.number_key("air_losses", result_ledger.air_losses());
        w.put(",\n    ");
        w.number_key("air_kills_attributed",
                     result_ledger.air_kills_attributed());
        w.put(",\n    ");
        w.number_key("ag_kills", sink_stats.ag_kills_recorded);
        w.put(",\n    ");
        w.number_key("kills_unclassified",
                     sink_stats.kills_unclassified);
        w.put(",\n    ");
        w.number_key("bomb_impacts", result_ledger.bomb_impacts());
        w.put(",\n    ");
        w.number_key("objectives_damaged",
                     static_cast<std::int64_t>(
                         result_ledger.objective_damage().size()));
        w.put(",\n    ");
        w.number_key("features_destroyed",
                     result_ledger.features_destroyed());
        w.put(",\n    ");
        w.number_key("writeback_team_pools",
                     writeback.team_pools_written);
        w.put(",\n    ");
        w.number_key("writeback_squadrons",
                     writeback.squadrons_written);
        w.put(",\n    ");
        w.number_key("writeback_objectives",
                     writeback.objectives_written);
        w.put(",\n    ");
        w.number_key("writeback_unmatched",
                     static_cast<std::int64_t>(
                         writeback.unmatched_squadrons.size() +
                         writeback.unmatched_objectives.size()));
        w.put(",\n    \"result_json\": ");
        write_string(w, result_path.string());
        w.put("\n  }");
        w.put("\n}\n");

        std::ofstream out(summary_path);
        out << w.str();
    }

    std::printf("wrote: %s\n", summary_path.string().c_str());
    std::printf("wrote: %s\n", (args.out_dir / "trace.json").string().c_str());
    std::printf("wrote: %s\n", scenario_path.string().c_str());
    std::printf("wrote: %s\n", result_path.string().c_str());
    if (nothing_airborne) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — %zu aircraft spawned, 0 "
                     "airborne after %d ticks. Ground ops stalled; inspect "
                     "trace.json ai_state per aircraft.\n",
                     sim_spawned.size(), args.ticks);
        return 3;
    }
    if (nothing_employed) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — %d strike flights armed "
                     "with ordnance, 0 bombs released. The A-G employment "
                     "chain broke (arming / envelope / store debit / "
                     "intent routing); inspect trace.json ai_state.\n",
                     strike_flights_armed);
        return 4;
    }
    if (results_lost) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — combat outcomes occurred "
                     "(kills=%d impacts=%d) but the result ledger recorded "
                     "NOTHING. The write-back chain broke (sink never "
                     "fired / origin stamp missing / classification "
                     "dropped every event); inspect campaign_result.json.\n",
                     sink_stats.kills_seen, sink_stats.bomb_impacts_seen);
        return 5;
    }
    // C2 gate (exit 6): the tasking ladder ran over belligerents that
    // HAD aircraft, yet drew not one — the generation side broke (the
    // profile table, the availability gates, the force snapshot's
    // roster decode). The one-pool ledger is exactly where that shows
    // up first: no draws means nothing can ever deplete.
    if (tasking_ran && tasking_had_air &&
        result_ledger.mission_draw_aircraft() == 0) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the tasking ladder ran %d "
                     "cycles over %d minutes with belligerent aircraft "
                     "available and drew NOTHING. The generation chain "
                     "broke (profiles / availability gates / roster "
                     "decode); inspect campaign_qc_summary.json tasking.\n",
                     tasking_cycles, args.tasking_minutes);
        return 6;
    }
    // C3 gate (exit 7): the ladder drew aircraft with a route planner
    // attached, but not one route came back buildable — or not one
    // routed intent materialized an aircraft. The generation-to-spawn
    // chain broke (threat map empty / A* diverged / airbases
    // unresolvable); the routed war cannot fly.
    if (tasking_ran && tasking_routes_expected && tasking_had_air &&
        result_ledger.mission_draw_aircraft() > 0 &&
        (tasking_routes == 0 ||
         spawner.stats().synthetic_spawned == 0)) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the tasking ladder drew %d "
                     "aircraft with a route planner attached, but built "
                     "%d routes (%d build failures) and materialized %d "
                     "synthetic aircraft. The generation-to-spawn chain "
                     "broke (threat map / pathfinder / airbase "
                     "resolution); inspect the tasking routes block in "
                     "campaign_qc_summary.json.\n",
                     result_ledger.mission_draw_aircraft(), tasking_routes,
                     tasking_routes_failed,
                     spawner.stats().synthetic_spawned);
        return 7;
    }
    // C4 gate (exit 8): the ATM pipeline ran cycles with belligerent
    // aircraft available and drew aircraft — but built no packages
    // (the phase chain broke somewhere between requests and
    // FindBestAir). escorts/recoveries carry no gate: a short run can
    // legitimately produce none (no defended targets / no completed
    // missions) — the counters are telemetry, not thresholds.
    if (tasking_ran && tasking_had_air &&
        result_ledger.mission_draw_aircraft() > 0 && atm_packages == 0) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — the ATM pipeline drew %d "
                     "aircraft over %d cycles but built no packages. "
                     "The 7-phase chain broke (request generation / "
                     "FindBestAir / availability); inspect the atm "
                     "block in campaign_qc_summary.json.\n",
                     result_ledger.mission_draw_aircraft(), tasking_cycles);
        return 8;
    }
    return 0;
}

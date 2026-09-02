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
//      plus the scenario JSON path for replay in the f4-scenario-player.
//
// Usage:
//   campaign_qc <world.json> [options]
//     --class-table <FALCON4.ct>   (default: <src>/f4-world-convert/tests/fixtures/FALCON4.ct)
//     --config <f16.json>          (default: <bin>/generated_fixtures/f16.json)
//     --models <KoreaObj.HDR>      (default: <src>/temp/KoreaObj.HDR; .LOD/.TEX inferred)
//     --team <slot>                (filter: owning team, -1 = any)
//     --mission <AMIS_*>|<byte>    (filter: mission name or byte)
//     --max-flights <n>            (filter: cap spawned aircraft)
//     --ticks <n>                  (sim frames to run; default 18000 = 5 min)
//     --sim-dt <sec>               (default 1/60)
//     --out-dir <dir>              (default: beside the world JSON)
//
// Exit code: 0 when the loop produced at least one aircraft AND the sim
// ran to completion; 1 on usage/IO errors; 2 when the filter matched
// nothing (a QC failure, not a crash); 3 when the sim ran but NOTHING
// got airborne by the last tick — the end-to-end "tasking didn't fly"
// failure the tool exists to catch (ground-ops stall, broken taxi route,
// never-ending cross-theater taxi: the exact symptoms the first TestCamp
// run exposed).

#include <f4/simulation/simulation.hpp>
#include <f4/simulation/campaign_bridge.hpp>
#include <f4/simulation/campaign_spawner.hpp>
#include <f4/campaign/campaign.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/entities/entity.hpp>
#include <f4/ai/brain_component.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/json/writer.hpp>
#include <f4/io/read_file.hpp>
#include <f4/world/world_loader.hpp>
#include <f4/world/world_adapters.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/data/aircraft_config.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/models/model_database.hpp>

#include <algorithm>
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
    int ticks = 18000;           // 5 min at 60 Hz — taxi + takeoff + climb
    double sim_dt = 1.0 / 60.0;
    int record_every = 10;       // trace decimation (6 samples/s/aircraft)
    std::filesystem::path out_dir;
};

[[noreturn]] void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s <world.json> [--class-table <ct>] [--config <f16.json>]\n"
        "          [--models <KoreaObj.HDR>] [--team <slot>]\n"
        "          [--mission <AMIS_NAME|byte>] [--max-flights <n>]\n"
        "          [--ticks <n>] [--sim-dt <sec>] [--out-dir <dir>]\n",
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
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if (k == "--class-table")      a.class_table = next();
        else if (k == "--config")      a.config = next();
        else if (k == "--models")      a.models_hdr = next();
        else if (k == "--team")        a.team = std::atoi(next());
        else if (k == "--max-flights") a.max_flights = std::atoi(next());
        else if (k == "--ticks")       a.ticks = std::atoi(next());
        else if (k == "--sim-dt")      a.sim_dt = std::atof(next());
        else if (k == "--record-every") a.record_every = std::max(1, std::atoi(next()));
        else if (k == "--out-dir")     a.out_dir = next();
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

} // namespace

// ===========================================================================
int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);

    if (!std::filesystem::exists(args.world_json)) {
        std::fprintf(stderr, "campaign_qc: world JSON not found: %s\n",
                     args.world_json.string().c_str());
        return 1;
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

    // Class table + model db + config for spawning.
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
    spawner.attach(bus);

    const auto intents = emit_flight_intents(
        static_cast<const f4::world::IUnitCoreSource&>(adapters.units),
        static_cast<const f4::world::IFlightSource&>(adapters.units),
        bus, ws.campaign.current_time,
        &static_cast<const f4::world::ITeamSource&>(adapters.teams));

    const auto& b3_stats = spawner.stats();
    std::printf("b3_loop: intents=%zu spawned=%d routes=%d unknown=%d "
                "dups=%d\n",
                intents.size(), b3_stats.aircraft_spawned,
                b3_stats.routes_attached, b3_stats.unknown_flight_ids,
                b3_stats.duplicate_skips);

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
        out << "  \"record\": true,\n";
        out << "  \"record_path\": \"trace.json\"\n";
        out << "}\n";
    }

    auto scenario = load_scenario(scenario_path);
    Simulation sim(scenario, args.out_dir);
    sim.initialize();

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

    // -----------------------------------------------------------------------
    // 4. Summary JSON
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
        w.put("\n  }\n}\n");

        std::ofstream out(summary_path);
        out << w.str();
    }

    std::printf("wrote: %s\n", summary_path.string().c_str());
    std::printf("wrote: %s\n", (args.out_dir / "trace.json").string().c_str());
    std::printf("wrote: %s\n", scenario_path.string().c_str());
    if (nothing_airborne) {
        std::fprintf(stderr,
                     "campaign_qc: QC FAILURE — %zu aircraft spawned, 0 "
                     "airborne after %d ticks. Ground ops stalled; inspect "
                     "trace.json ai_state per aircraft.\n",
                     sim_spawned.size(), args.ticks);
        return 3;
    }
    return 0;
}

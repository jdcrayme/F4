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
        "          [--out-dir <dir>]\n",
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
        // C3: arm the role fallback — the QC exercises the generation-
        // to-spawn chain, and TestCamp's belligerents (the ROK-DPRK
        // war the corrected RelType decode reveals) field all-counter-
        // air squadrons: the strict role gate would never generate a
        // delivery mission, so the routed war could not be exercised
        // at all. The reference's own selection SCORES role vs
        // capability (FindBestAir, the C4 tranche); the fallback is
        // the honest bridge to that. Default-off in the library —
        // B.3/C2 goldens stay byte-identical.
        ladder_cfg.tasking_role_fallback = true;
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
    return 0;
}

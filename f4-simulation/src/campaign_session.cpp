// f4-simulation/src/campaign_session.cpp
//
// CampaignSession — see campaign_session.hpp for the design. Headless
// orchestration: no rendering, no clocks of its own, deterministic.

#include <f4/simulation/campaign_session.hpp>

#include <f4/campaign/api/events.hpp>   // CAMP-HOST-2: the event pump
#include <f4/ai/brain_component.hpp>   // FID-5: combat_engagement_id
#include <f4/campaign/ground_writeback.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/simulation/campaign_bridge.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/simulation/combat_bridge.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/io/read_file.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/world/airbase_synthesis.hpp>   // the stock-save bridge
#include <f4/world/detail/world_state.hpp>  // the bridge mutates the WS
#include <f4/world/world_loader.hpp>   // populate_world (G1 mirror)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <optional>
#include <filesystem>
#include <cstdint>
#include <sstream>
#include <system_error>
#include <vector>
#include <unordered_map>
#include <string>

namespace f4::simulation {

namespace {

// ---------------------------------------------------------------------------
// Scenario authoring — the same JSON campaign_qc writes, in a temp dir.
// ---------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Count Flight-class units in the save (decides the scenario's spawn
// mode: campaign_flights when the save flies its own tasking, the
// scenario-list template when it doesn't — the ladder's generated
// missions spawn either way).
int count_flights(const f4::world::WorldState& ws) {
    int n = 0;
    for (const auto& u : ws.units) {
        if (u.unit_class == f4::entities::UnitClass::Flight) ++n;
    }
    return n;
}

std::string session_scenario_json(
        const std::filesystem::path& world_json,
        const std::filesystem::path& class_table,
        const std::filesystem::path& aircraft_config,
        bool campaign_flights,
        const f4::simulation::FlightSpawnFilter& filter,
        double sim_dt,
        bool aa_combat,
        bool tiered,
        const std::filesystem::path& brain_data,
        const std::filesystem::path& theater_tables,
        bool pilot_skill_flow) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"name\": \"f4_viewer_campaign_session\",\n";
    out << "  \"theater\": \"korea\",\n";
    out << "  \"spawn_mode\": \""
        << (campaign_flights ? "campaign_flights" : "scenario_list")
        << "\",\n";
    out << "  \"world_json_path\": \"" << json_escape(world_json.string())
        << "\",\n";
    out << "  \"class_table_path\": \"" << json_escape(class_table.string())
        << "\",\n";
    if (!campaign_flights) {
        // The flight-less fallback: a synthetic airfield anchored at the
        // theater origin. campaign_flights mode derives real airfields
        // from the world's airbase objectives; a world with none (the
        // kunsan fixture) still needs the 2-point taxi route the scenario
        // loader validates — the spawner's parking fallback ladder then
        // places generated missions from their routes' takeoff points.
        out << "  \"airfield\": {\n";
        out << "    \"active_runway_id\": 36, \"active_runway_name\": \"Rwy 36\",\n";
        out << "    \"runway_heading_rad\": 0.0,\n";
        out << "    \"threshold_position\": "
               "{\"x\": 0.0, \"y\": -5000.0, \"z\": 0.0},\n";
        out << "    \"runway_end_position\": "
               "{\"x\": 0.0, \"y\": 5000.0, \"z\": 0.0},\n";
        out << "    \"threshold_altitude_ft\": 0.0, "
               "\"departure_altitude_ft\": 10000.0,\n";
        out << "    \"taxi_route\": ["
               "{\"x\": 0.0, \"y\": -5000.0, \"z\": 0.0}, "
               "{\"x\": 0.0, \"y\": 0.0, \"z\": 0.0}]\n";
        out << "  },\n";
    }
    // Combat: the sweeps are always on (the brain-intent driver + RWR
    // rebuild + missile sweeps — no-ops when no combat components
    // exist). C6's campaign arming is OPT-IN: aa_combat writes
    // campaign_armed + the full ROE (bvr/missiles/guns free — the war's
    // fights resolve to kills, the acceptance the ledger books) + the
    // doctrine's brain-data path when the caller pinned one. With
    // aa_combat off the block is EXACTLY the pre-C6 bytes — every
    // golden pins that shape.
    if (aa_combat) {
        out << "  \"combat\": {\"enabled\": true, \"campaign_armed\": true,"
               " \"bvr_hold\": false, \"missiles_hold\": false,"
               " \"guns_hold\": false";
        if (!theater_tables.empty() || pilot_skill_flow) {
            // CAMP-SCALE-1: the converted tables + the pilot-skill gate
            // ride the scenario so the Simulation's bulk spawn path runs
            // the same flows the session's spawn paths run.
            if (!theater_tables.empty()) {
                out << ", \"theater_tables_path\": \""
                    << json_escape(theater_tables.string()) << "\"";
            }
            if (pilot_skill_flow) {
                out << ", \"pilot_skill_flow\": true";
            }
        }
        out << "},\n";
        if (!brain_data.empty()) {
            out << "  \"brain_data_path\": \""
                << json_escape(brain_data.string()) << "\",\n";
        }
    } else {
        out << "  \"combat\": {\"enabled\": true},\n";
    }
    // The template every spawn path shares (callsign prefix, config,
    // vis fallback). In scenario-list mode it is also the one parked
    // aircraft that anchors the FM workload while the ladder generates.
    out << "  \"aircraft\": [{\n";
    out << "    \"callsign\": \"CAMPAIGN\",\n";
    out << "    \"aircraft_config_path\": \""
        << json_escape(aircraft_config.string()) << "\",\n";
    out << "    \"aircraft_name\": \"F-16C_50\",\n";
    out << "    \"vis_type_index\": 1052,\n";
    out << "    \"parking_spot\": {\"x\": 0.0, \"y\": 0.0, \"z\": 0.0},\n";
    out << "    \"heading_rad\": 0.0\n";
    out << "  }],\n";
    if (campaign_flights) {
        out << "  \"campaign_flight_filter\": {";
        out << "\"team\": " << filter.team;
        // The mission set: one byte stays a bare int (the long-standing
        // shape every saved scenario carries), a MIX writes the array —
        // the scenario parser takes both.
        if (filter.missions.size() == 1) {
            out << ", \"mission\": " << filter.missions.front();
        } else if (filter.missions.size() > 1) {
            out << ", \"mission\": [";
            for (std::size_t i = 0; i < filter.missions.size(); ++i) {
                if (i != 0) out << ", ";
                out << filter.missions[i];
            }
            out << "]";
        } else {
            out << ", \"mission\": -1";
        }
        out << ", \"max_flights\": " << filter.max_flights << "},\n";
        if (tiered) {
            // FID-1: the tiered session's deferred spawn — the world
            // populates, the flights stay aggregates (see
            // Simulation::spawn_from_campaign_flights' deferred gate).
            out << "  \"campaign_flights_deferred\": true,\n";
        }
    }
    out << "  \"sim_dt\": " << sim_dt << ",\n";
    out << "  \"total_ticks\": 1000000000,\n";
    out << "  \"record\": false\n";
    out << "}\n";
    return out.str();
}

// Rebuild the VU_ID.num → EntityId maps over a POPULATED world (the
// sim populated its own world during initialize(); populate_world's
// return value stayed inside that call). Same resolution rule the
// bridge and the C1 sink use: PropertyBag "vu_id_num".
void build_id_maps(
        const f4::entities::EntityWorld& world,
        std::unordered_map<std::uint32_t, f4::entities::EntityId>& units,
        std::unordered_map<std::uint32_t, f4::entities::EntityId>&
            objectives) {
    const auto scan = [&](const std::vector<f4::entities::EntityId>& ids,
                          std::unordered_map<std::uint32_t,
                              f4::entities::EntityId>& out) {
        for (const auto& eid : ids) {
            auto h = f4::entities::EntityHandle(
                eid, const_cast<f4::entities::EntityWorld*>(&world));
            auto* pb = h.get<f4::entities::PropertyBag>();
            if (pb == nullptr) continue;
            const auto it = pb->ints.find("vu_id_num");
            if (it == pb->ints.end() || it->second <= 0) continue;
            out.emplace(static_cast<std::uint32_t>(it->second), eid);
        }
    };
    scan(world.with_component<f4::entities::UnitCoreComponent>(), units);
    scan(world.with_component<f4::entities::ObjectiveTypeComponent>(),
         objectives);
}

} // namespace

// ---------------------------------------------------------------------------
// create()
// ---------------------------------------------------------------------------

std::unique_ptr<CampaignSession>
CampaignSession::create(const CampaignSessionOptions& opts,
                        std::string* error) {
    const auto fail = [error](const std::string& msg) {
        if (error != nullptr) *error = msg;
        return nullptr;
    };

    if (opts.world_json.empty() || !std::filesystem::exists(opts.world_json)) {
        return fail("world JSON not found: " + opts.world_json.string());
    }
    if (opts.aircraft_config.empty() ||
        !std::filesystem::exists(opts.aircraft_config)) {
        return fail("aircraft config not found: " +
                    opts.aircraft_config.string());
    }
    if (opts.mission_profiles.empty() ||
        !std::filesystem::exists(opts.mission_profiles)) {
        return fail("mission profiles not found: " +
                    opts.mission_profiles.string());
    }

    auto session = std::unique_ptr<CampaignSession>(new CampaignSession());
    session->sim_dt_ = opts.sim_dt;
    session->max_steps_per_advance_ = opts.max_steps_per_advance;

    // 1. The WorldState (the session's own copy: the ledger snapshot,
    //    the adapter sources, and the write-back target).
    try {
        session->ws_.load(opts.world_json);
    } catch (const std::exception& e) {
        return fail(std::string("world load failed: ") + e.what());
    }
    session->epoch_ = session->ws_.campaign.current_time;

    // 1b. The stock-save bridge: BEFORE the adapters (step 4) and the
    //     ledger snapshot (step 5) — every campaign-side consumer must
    //     see the same bases the sim's world will carry. When the pass
    //     changed anything, the mutated state is also written out (step
    //     6): the Simulation re-loads the world from the SCENARIO's
    //     world_json_path into its own WorldState copy, so an in-memory
    //     mutation alone would leave the sim's world unbased while the
    //     ladder's was based.
    bool airbases_synthesized = false;
    if (opts.synthesize_airbases) {
        const auto synth = f4::world::synthesize_squadron_airbases(
            session->ws_);
        airbases_synthesized = synth.assigned > 0;
    }

    // 2. The aircraft config + profiles (throwing loaders wrapped).
    try {
        auto result = f4::data::loadConfig(opts.aircraft_config.string());
        if (!result.ok) {
            std::string msg = "aircraft config rejected:";
            for (const auto& e : result.errors) msg += " " + e;
            return fail(msg);
        }
        session->cfg_ = std::move(result.config);
    } catch (const std::exception& e) {
        return fail(std::string("aircraft config load failed: ") + e.what());
    }
    try {
        session->profiles_ = f4::campaign::MissionProfileTable::load(
            opts.mission_profiles);
    } catch (const std::exception& e) {
        return fail(std::string("mission profiles load failed: ") + e.what());
    }

    // 3. The class table (optional — spawn falls back without it, but
    //    the session wants it; the QC tolerates absence, so we do too).
    if (!opts.class_table.empty() && std::filesystem::exists(
            opts.class_table)) {
        try {
            session->ct_.load_auto(opts.class_table.string());
        } catch (const std::exception& e) {
            return fail(std::string("class table load failed: ") + e.what());
        }
    }

    // 4. Adapters (read-side over the session's WorldState).
    session->adapters_ =
        std::make_unique<f4::world::WorldStateAdapters>(session->ws_);

    // 5. The ledger (C1's write model — snapshotted BEFORE anything
    //    moves, so a zero-event session changes nothing).
    session->ledger_ =
        std::make_unique<f4::campaign::CampaignResultLedger>(
            session->adapters_->campaign, session->adapters_->teams,
            session->adapters_->units);

    // 5b. CAMP-DOM-1: the verdict's opening baseline — the LIVE owner
    //     of every objective at session start (wire order), snapshotted
    //     alongside the ledger (the same run scope: the verdict's
    //     swings measure this run, exactly like the books do).
    session->verdict_opening_owners_.reserve(
        session->ws_.objectives.size());
    for (const auto& o : session->ws_.objectives) {
        session->verdict_opening_owners_.push_back(o.owner);
    }

    // 6. The scenario (temp dir; world_json_path ABSOLUTE — the QC's
    //    relative-path lesson: the sim resolves it against the scenario
    //    file's directory).
    std::error_code ec;
    // Unique per session INSTANCE (and per process): a fixed
    // "f4_viewer_session" dir is shared by every session in every
    // process, so two concurrent sessions (parallel ctest -jN, two
    // viewer instances) race on the SAME scenario.json — one session's
    // create() can read the other's world path, a torn write, or a
    // freshly-removed file and fail outright. Same rule the test rigs
    // apply (unique per call AND per process): instance counter within
    // the process, steady-clock nanos across processes.
    static std::atomic<unsigned> instance_counter{0};
    const auto instance_tag =
        std::to_string(instance_counter.fetch_add(1)) + "_" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
    session->scenario_temp_dir_ =
        std::filesystem::temp_directory_path(ec) /
        ("f4_viewer_session_" + instance_tag);
    if (!ec) {
        std::filesystem::create_directories(session->scenario_temp_dir_, ec);
    }
    if (ec) {
        return fail("cannot create temp dir " +
                    session->scenario_temp_dir_.string() + ": " +
                    ec.message());
    }
    // The sim re-loads the world from the SCENARIO's world_json_path
    // into its own WorldState copy — when the stock-save bridge changed
    // anything, materialize the mutated state (the tested §6.1 emitter,
    // a semantic round-trip) and point the scenario at it. Unchanged
    // (bridge off, or nothing to do) → the original path verbatim.
    std::filesystem::path world_abs =
        std::filesystem::absolute(opts.world_json);
    if (airbases_synthesized) {
        const auto patched =
            session->scenario_temp_dir_ / "world.world.json";
        {
            std::ofstream out(patched);
            out << session->ws_.to_json_string();
            if (!out.good()) {
                return fail("cannot write " + patched.string());
            }
        }
        world_abs = std::filesystem::absolute(patched);
    }
    const auto ct_abs =
        opts.class_table.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(opts.class_table);
    const auto brain_abs =
        opts.brain_data.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(opts.brain_data);
    f4::simulation::FlightSpawnFilter filter;
    filter.team = opts.team;
    filter.set_mission(opts.mission);
    filter.max_flights = opts.max_flights;
    const bool have_flights = count_flights(session->ws_) > 0;
    const auto scenario_path =
        session->scenario_temp_dir_ / "scenario.json";
    {
        std::ofstream out(scenario_path);
        out << session_scenario_json(world_abs, ct_abs,
                                     opts.aircraft_config, have_flights,
                                     filter, opts.sim_dt, opts.aa_combat,
                                     opts.fidelity_policy ==
                                         FidelityPolicy::Tiered,
                                     brain_abs,
                                     opts.theater_tables,
                                     opts.pilot_skill_flow);
        if (!out.good()) {
            return fail("cannot write " + scenario_path.string());
        }
    }

    // 7. The Simulation — initialize() populates ITS world from the
    //    world JSON (teams, objectives, units), spawns the save's own
    //    flights, wires the ATC per airbase, and starts the bubble
    //    manager. This world is the ONE world: everything after this
    //    point spawns into it and ticks inside it.
    try {
        auto scenario = f4::simulation::load_scenario(scenario_path);
        session->sim_ = std::make_unique<f4::simulation::Simulation>(
            std::move(scenario), session->scenario_temp_dir_);
        session->sim_->initialize();
    } catch (const std::exception& e) {
        return fail(std::string("simulation init failed: ") + e.what());
    }

    // 7b. G1 — the ground mirror needs the world POPULATED. The
    //     campaign_flights spawn mode populates it inside initialize();
    //     the flight-less scenario-list mode does not (a world with no
    //     flights never needed battalion entities before the ground
    //     war). When the ground war is armed on a flight-less world,
    //     populate here — the SAME call the campaign_flights path
    //     makes, the same entity shape (teams + objectives + units
    //     with transforms + tactical components). Ground-quiet
    //     sessions keep the lean world (the opt-in contract).
    //     G2: unit_strike needs the battalion entities too (the CAS
    //     bombs' targets — transforms + UnitCore the blast endpoint
    //     and the sink resolve), so the populate gate takes either arm.
    if ((opts.ground_war || opts.unit_strike) && !have_flights) {
        (void)f4::world::populate_world(session->sim_->world(),
                                        session->ws_);
    }

    // 8. Cross-reference maps over the SIM's world (the spawner's
    //    squadron/parking resolution + the objective map for strike
    //    arming — the same maps the QC gets from populate_world).
    build_id_maps(session->sim_->world(), session->unit_id_map_,
                  session->objective_id_map_);

    // 9. The airfield + per-airbase map (spawn parking): derived from
    //    the session's WorldState, the same rule the QC applies. Both
    //    live in MEMBERS (airfield_ / airbase_airfields_) — the spawner
    //    references them for the session's lifetime; locals would
    //    dangle at the end of create() (the Start Session crash's
    //    second leg).
    bool have_airfield = false;
    for (const auto& obj : session->ws_.objectives) {
        if (auto af = f4::simulation::derive_airfield_from_objective(
                obj, 36)) {
            if (!have_airfield) {
                session->airfield_ = *af;
                have_airfield = true;
            }
            if (obj.id_num != 0) {
                session->airbase_airfields_[obj.id_num] = std::move(*af);
            }
        }
    }
    if (!have_airfield) {
        // The QC hard-fails here; a SESSION degrades instead — the
        // spawner's own fallback ladder (route takeoff waypoint →
        // template threshold) still parks every generated mission, so
        // the loop runs on worlds the QC would reject.
        session->airfield_ = f4::simulation::ScenarioAirfield{};
    }

    // 10. The spawner — feeding THE SIM'S WORLD + BUS. Intents the
    //     ladder publishes materialize as aircraft in the same world
    //     the physics ticks. (The template is the member spawn_tpl_;
    //     the airfield/map lenders are members too — see step 9.)
    session->spawn_tpl_.callsign = "CAMPAIGN";
    session->spawn_tpl_.vis_type_index = 1052;
    session->spawn_tpl_.aircraft_config_path = opts.aircraft_config.string();
    session->spawner_ =
        std::make_unique<f4::simulation::CampaignSimSpawner>(
            session->sim_->world(), session->unit_id_map_,
            session->ct_, session->cfg_, session->airfield_,
            session->spawn_tpl_, filter);
    // Arming + parking: the builtin weapon table is a MEMBER (the
    // spawner borrows it; a temporary would dangle) — the QC keeps a
    // named local alive for the whole run, the session stores one.
    session->weapon_table_ =
        f4::simulation::resolve_weapon_table(opts.weapon_data_path,
                                             &session->weapon_import_warnings_);
    session->spawner_->set_objective_id_map(&session->objective_id_map_);
    session->spawner_->set_weapon_table(&session->weapon_table_);
    session->spawner_->set_airbase_airfields(
        session->airbase_airfields_.empty()
            ? nullptr
            : &session->airbase_airfields_);
    // FID-5 pacing + arms (copied from Options — they outlive the
    // options object, the sim_dt_ pattern). The spawner's deferral flag
    // is read at handle() time; inert unless the Tiered policy armed
    // the engine below (the intent handler checks flights_ itself).
    session->synthetic_as_aggregates_ = opts.synthetic_as_aggregates;
    session->combat_deagg_ = opts.combat_deagg;
    session->combat_envelope_ft_ = opts.combat_envelope_ft;
    session->pilot_skill_flow_ = opts.pilot_skill_flow;
    session->combat_lookahead_sec_ = opts.combat_lookahead_sec;
    session->combat_window_sec_ = opts.combat_window_sec;
    session->spawner_->set_synthetic_deferred(
        opts.fidelity_policy == FidelityPolicy::Tiered &&
        session->synthetic_as_aggregates_);
    // FID-5: the session's OWN MissionIntent subscription, registered
    // BEFORE the spawner's (bus order is subscription order) so a
    // tiered session's synthetic-deferral handler hears the intent
    // first and registers the aggregate; the spawner (flagged deferred)
    // then skips the spawn. Full-fidelity sessions: the handler no-ops
    // (flights_ == nullptr) and the spawner spawns exactly as before.
    session->intent_subscription_ = session->sim_->bus()
        .subscribe<f4::campaign::MissionIntent>(
            [raw = session.get()](const f4::campaign::MissionIntent& in) {
                raw->handle_mission_intent_(in);
            });
    session->spawner_->attach(session->sim_->bus());

    // 11. The ladder (C2's one-pool tasking + C4's ATM pipeline) over
    //     the SAME bus.
    f4::campaign::CampaignConfig ladder_cfg;
    ladder_cfg.air_task_cycle_sec = opts.tasking_cycle_sec;
    ladder_cfg.reinforcement_period_sec = opts.reinforce_period_sec;
    // CAMP-DOM-2: the strategic reserve flow (default off — the C2
    // golden identity; the QC's --replacement-stock arms it).
    ladder_cfg.replacement_stock_flow = opts.replacement_stock_flow;
    ladder_cfg.atm_pipeline = opts.atm_pipeline;
    ladder_cfg.atm.min_seadescort_threat = opts.atm_seadescort_threat;
    // CAMP-CMD-2: the retask arithmetic reuses the ATM's own reserve
    // and cruise constants (copied — the options object dies).
    session->atm_reserve_min_ = ladder_cfg.atm.reserve_min;
    session->atm_cruise_grid_per_min_ =
        static_cast<double>(ladder_cfg.atm.cruise_grid_per_min);
    // G2: the interdiction arm — BOTH ladders (legacy + ATM) and the
    // sink's unit-loss booking ride this one flag (the aa_combat /
    // ground_war opt-in contract).
    ladder_cfg.unit_strike = opts.unit_strike;
    // P7: the strategy layer — station targeting, FindSupportFlights,
    // RequestEnemyMission, racetrack routes (one flag, the same
    // opt-in contract). The ATM inherits it at construction.
    ladder_cfg.strategy_layer = opts.strategy_layer;
    // CAMP-DOM-3: the personnel arms — the AssignPilots crew pick and
    // the per-role rating decay (one flag each, the same opt-in
    // contract; the ATM inherits both at construction — the
    // Campaign's own pass-through, campaign.cpp's constructor).
    ladder_cfg.pilot_assignment = opts.pilot_assignment;
    ladder_cfg.rating_decay = opts.rating_decay;
    // CAMP-DOM-4: the airbase-scheduling depth arm (one flag, the same
    // opt-in contract; the ATM inherits it at construction — the
    // Campaign's own pass-through).
    ladder_cfg.airbase_scheduling = opts.airbase_scheduling;
    session->airbase_scheduling_ = opts.airbase_scheduling;
    // CAMP-DOM-5: the naval tasking wrap (one flag, the same opt-in
    // contract; the ATM inherits it at construction — the Campaign's
    // own pass-through).
    ladder_cfg.naval_tasking = opts.naval_tasking;
    session->ladder_ = std::make_unique<f4::campaign::Campaign>(
        static_cast<const f4::world::ICampaignSource&>(
            session->adapters_->campaign),
        static_cast<const f4::world::ITeamSource&>(
            session->adapters_->teams),
        static_cast<const f4::world::IUnitCoreSource&>(
            session->adapters_->units),
        session->profiles_, session->sim_->bus(), ladder_cfg);
    session->ladder_->set_result_ledger(session->ledger_.get());

    // 11b. G1 — the ground war engine, over the same sources and the
    //      same ledger (one writer, one clock, one certificate). It
    //      borrows the ledger MUTABLY (the C2 discipline, bound at
    //      construction because the engine has no ledger-less
    //      campaign mode). The ADAPTERS outlive it (member order:
    //      adapters die after ground_).
    if (opts.ground_war) {
        f4::campaign::GroundWarConfig gcfg;
        gcfg.update_sec = opts.ground_update_sec > 0
            ? opts.ground_update_sec : 60;
        gcfg.orders_sec = opts.ground_orders_sec > 0
            ? opts.ground_orders_sec : 1800;
        gcfg.resupply_period_sec = opts.ground_resupply_sec;
        // CAMP-DOM-2: the deepened pool + the repair cadence (both
        // default-off — the G1 golden identity).
        gcfg.objective_supply = opts.ground_objective_supply;
        gcfg.repair_period_sec = opts.ground_repair_sec;
        session->ground_ = std::make_unique<f4::campaign::GroundWar>(
            static_cast<const f4::world::ICampaignSource&>(
                session->adapters_->campaign),
            static_cast<const f4::world::ITeamSource&>(
                session->adapters_->teams),
            static_cast<const f4::world::IObjectiveSource&>(
                session->adapters_->objectives),
            static_cast<const f4::world::IUnitCoreSource&>(
                session->adapters_->units),
            session->ledger_.get(), gcfg);
    }

    // 11b-2. CAMP-DOM-6 — the task-force movement engine (the naval
    //      GroundWar sibling): snapshot the wire's domain-4 TaskForce
    //      rows, walk each belligerent force toward its own dest at
    //      the update cadence, and sync the moved rows into the
    //      session's WorldState per update (the `taskforces` query's
    //      serving face). NO ledger — the engine books nothing (see
    //      f4/campaign/naval_war.hpp). Default off: never constructed,
    //      never touches a byte (the golden identity).
    if (opts.naval_movement) {
        f4::campaign::NavalWarConfig ncfg;
        ncfg.update_sec = opts.naval_update_sec > 0
            ? opts.naval_update_sec : 60;
        session->naval_ = std::make_unique<f4::campaign::NavalWar>(
            static_cast<const f4::world::ICampaignSource&>(
                session->adapters_->campaign),
            static_cast<const f4::world::ITeamSource&>(
                session->adapters_->teams),
            static_cast<const f4::world::IUnitCoreSource&>(
                session->adapters_->units),
            ncfg);
    }

    // 11c. FID — the aggregate flight engine (Tier-A truth; see
    //      Docs/FIDELITY_TIERS_PLAN.md). Tiered sessions only: a
    //      FullFidelity session never constructs it and never defers
    //      the saved flights' spawn (the byte-identical contract). The
    //      scenario JSON above carries campaign_flights_deferred so
    //      initialize() populated the world WITHOUT the per-flight
    //      aircraft — this engine is where those flights live now.
    if (opts.fidelity_policy == FidelityPolicy::Tiered) {
        f4::campaign::FlightAggregateConfig fcfg;
        fcfg.update_sec = opts.air_agg_update_sec > 0
            ? opts.air_agg_update_sec : 60;
        f4::campaign::FlightAggregateFilter ffilter;
        ffilter.team = opts.team;
        ffilter.mission = opts.mission;
        ffilter.max_flights =
            opts.max_flights > 0 ? opts.max_flights : -1;
        session->flights_ =
            std::make_unique<f4::campaign::FlightAggregateEngine>(
                static_cast<const f4::world::ICampaignSource&>(
                    session->adapters_->campaign),
                static_cast<const f4::world::IUnitCoreSource&>(
                    session->adapters_->units),
                static_cast<const f4::world::IFlightSource&>(
                    session->adapters_->units),
                fcfg, ffilter);
        // FID pacing (the options die after create(); the members
        // outlive them — the sim_dt_/max_steps_ pattern).
        session->air_agg_update_sec_ = opts.air_agg_update_sec;
        session->ops_window_sec_ = opts.ops_window_sec;
        session->air_reagg_factor_ = opts.air_reagg_factor;
        session->deagg_cooldown_sec_ = opts.deagg_cooldown_sec;
        // FID-1: the AII-parsed SIM_BUBBLE_SIZE is the air-bubble
        // floor (the camera bubble never shrinks below it).
        session->default_air_radius_ft_ =
            session->sim_->air_bubble_radius_ft();

        // FID-5: the aggregate air picture + the commit-window veto.
        // Armed ONLY with combat_deagg — the feed makes the aggregates
        // HOSTILE contacts (the picture's team interning), and without
        // the triggers a hostile aggregate could never be fought (the
        // veto would stand forever). Disarmed sessions keep the
        // FID-1..4 shape exactly. The exclusion set covers every save
        // flight's world entity (synthetic flights have none) — the
        // feed is the aggregate truth's single publisher.
        if (session->combat_deagg_) {
            for (const auto& f : session->flights_->flights()) {
                const auto it = session->unit_id_map_.find(f.vu);
                if (it != session->unit_id_map_.end() &&
                    it->second.valid()) {
                    session->flight_entity_ids_.insert(it->second.value);
                }
            }
            session->sim_->set_air_picture_excluded(
                &session->flight_entity_ids_);
            session->sim_->set_air_picture_aggregates(
                &session->aggregate_contacts_);
            session->sim_->set_deferred_launch_ids(
                &session->aggregate_vu_set_);
        }
    }

    // 12. The route planner (C3) — threat map from the same sources,
    //     viewed from the FIRST BELLIGERENT (te_team can be neutral;
    //     the QC's own correction). Host tunable: MinAvoidThreat 25
    //     (aiinput's default 40 sits above the fixture UCD's single-
    //     ring band scores — the same override campaign_qc arms).
    std::uint8_t viewer = static_cast<std::uint8_t>(
        session->ws_.campaign.te_team);
    if (const auto war = session->ladder_->belligerent_teams();
        !war.empty()) {
        viewer = static_cast<std::uint8_t>(war.front());
    }
    session->threat_viewer_ = viewer;
    f4::campaign::RouteBuilderConfig route_cfg;
    route_cfg.min_avoid_threat = 25;
    // P7: the loiter racetracks ride the strategy arm (one source of
    // truth — the routes only change shape for strategy-armed
    // sessions). CAMP-ATM-1: the sweep lines and the tanker refuel
    // waypoints ride the same arm.
    route_cfg.loiter_racetracks = opts.strategy_layer;
    route_cfg.sweep_lines = opts.strategy_layer;
    route_cfg.tanker_refuel_waypoints = opts.strategy_layer;
    session->route_builder_ = std::make_unique<f4::campaign::RouteBuilder>(
        static_cast<const f4::world::IObjectiveSource&>(
            session->adapters_->objectives),
        static_cast<const f4::world::IUnitCoreSource&>(
            session->adapters_->units),
        static_cast<const f4::world::ITeamSource&>(
            session->adapters_->teams),
        viewer, route_cfg);
    session->ladder_->set_route_planner(
        session->route_builder_.get(),
        &static_cast<const f4::world::IObjectiveSource&>(
            session->adapters_->objectives));

    // 13. The result sink (C1's return leg) — BEFORE the first tick so
    //     the objective-damage snapshot catches the pristine state.
    session->sink_ =
        std::make_unique<f4::simulation::CampaignResultSink>(
            *session->ledger_, session->sim_->world());
    // G2: arm the unit-loss booking BEFORE attach (the first bomb
    // can land within the first advance).
    session->sink_->set_book_unit_losses(opts.unit_strike);
    session->sink_->attach(session->sim_->bus());

    // 14. C5's wreck policy — subscribe the kill feed when armed. The
    //     ledger's loss booking happens in the SINK's own subscription
    //     (registered above, so it hears the message FIRST — bus order
    //     is subscription order); this one only schedules the corpse's
    //     removal. A kill published between advance() calls (a host
    //     driving combat by hand) lands here too — the retire walk
    //     reads the sim clock on the next cadence tick.
    session->wreck_hold_sec_ = opts.wreck_hold_sec;
    if (session->wreck_hold_sec_ > 0.0) {
        auto* pending = &session->pending_wrecks_;
        session->kill_subscription_ =
            session->sim_->bus()
                .subscribe<f4::weapons::EntityKilledMessage>(
                    [pending](const f4::weapons::EntityKilledMessage& m) {
                        f4::entities::EntityId victim{};
                        victim.value = m.target_id;
                        if (!victim.valid()) return;
                        pending->push_back(
                            PendingWreck{victim, m.sim_time_s});
                    });
    }

    session->refresh_stats_();
    return session;
}

CampaignSession::~CampaignSession() {
    // Detach every bus subscriber BEFORE the sim (the bus owner) dies.
    // Destruction order already guarantees sim_ outlives these, but
    // detaching explicitly is the documented contract of both classes
    // (the QC does the same at end-of-run).
    if (sim_) {
        if (sink_) sink_->detach(sim_->bus());
        if (spawner_) spawner_->detach(sim_->bus());
        if (intent_subscription_ != 0) {
            sim_->bus().unsubscribe<f4::campaign::MissionIntent>(
                intent_subscription_);
        }
        if (kill_subscription_ != 0) {
            sim_->bus().unsubscribe<f4::weapons::EntityKilledMessage>(
                kill_subscription_);
        }
    }
}

// ---------------------------------------------------------------------------
// advance()
// ---------------------------------------------------------------------------

bool CampaignSession::advance(double real_seconds, int max_steps_override) {
    if (paused_ || real_seconds <= 0.0) {
        refresh_stats_();
        return false;
    }
    accumulator_ += real_seconds;

    // V-THREAD: the runner's per-call budget (never above the option).
    const int step_cap = max_steps_override > 0
        ? std::min(max_steps_override, max_steps_per_advance_)
        : max_steps_per_advance_;

    int steps = 0;
    bool capped = false;
    while (accumulator_ >= sim_dt_) {
        if (steps >= step_cap) {
            capped = true;
            break;
        }
        sim_->tick(sim_dt_);

        // The campaign clock advances in whole seconds accumulated from
        // the same ticks (CampaignTime is integral). One big tick ==
        // N small ones (pinned by the C2 tests), so per-second ladder
        // ticks are exactly the QC's single advance, split.
        campaign_sec_accum_ += sim_dt_;
        if (campaign_sec_accum_ >= 1.0) {
            const auto whole = static_cast<int>(campaign_sec_accum_);
            campaign_sec_accum_ -= static_cast<double>(whole);
            ladder_->tick(whole);
            // CAMP-HOST-2: the cadence events — the cycle fires and the
            // reinforcement deliveries the tick just booked, published
            // immediately so the stream's order is the engine's order.
            emit_cadence_events_();
            // CAMP-ATM-1: the ACTION tables' filings the tick just
            // booked (the war's reactions, right after the cycle that
            // generated them).
            emit_action_filed_events_();
            // G1: the ground war rides the same whole-second cadence
            // (its own accumulator gates on the update granularity).
            if (ground_ != nullptr) {
                ground_sec_accum_ += static_cast<double>(whole);
                advance_ground_();
                // CAMP-HOST-2: the flips the ground pass just booked.
                emit_capture_events_();
            }
            // CAMP-DOM-6: the naval movement rides the same cadence
            // (its own accumulator; the moved rows sync inside —
            // the query face reads the WorldState, not the engine).
            if (naval_ != nullptr) {
                naval_sec_accum_ += static_cast<double>(whole);
                advance_naval_();
            }
            // FID: the aggregate flights ride the same whole-second
            // cadence (the engine accumulates to its own update gate;
            // the tier pass runs per second — O(flights)).
            if (flights_ != nullptr) {
                flight_sec_accum_ += static_cast<double>(whole);
                advance_flights_();
                // FID-5: the combat pass — the aggregate feed + the
                // commit/convergence triggers (§4.5), after the tier
                // pass so the feed reflects the final tier state.
                evaluate_combat_();
            }
            // The damage sync rides the same cadence: final-state diff
            // of every damaged objective (cheap — the diff walks only
            // objectives with damage components).
            sink_->sync_objective_damage();
            // CAMP-HOST-2: the changed objectives publish here (the
            // sink collects; the session fills owner + time).
            emit_damage_events_();
            // CAMP-DOM-2: the repair cadence's books (the ledger log's
            // tail) + the sim-side bitmap mirror (a repaired feature's
            // entity face joins the engine's truth so the NEXT damage
            // sync diff sees the repair, not the stale rubble).
            emit_repair_events_();
            // CAMP-DOM-3: the personnel logs' tails — the crews the
            // cycle's filings just drew (assigned), the slots the sink's
            // losses consumed (lost), and the sorties the recoveries
            // credited (recovered). Three independent cursors: the logs
            // append from three different engines (the tasking cadence,
            // the result sink, the recovery pass).
            emit_pilot_events_();
            // CAMP-DOM-4: the scheduling books' denials — the refusals
            // the cycle's tasking walk just produced (the gate's skips
            // and the horizon's refusals the ledger logged).
            emit_slot_denied_events_();
            // CAMP-DOM-1: the verdict's coarse state — the event fires
            // only when the band or the leader changed (a capture is
            // the only mover today; the diff keeps the stream sparse).
            emit_verdict_events_();
            adopt_new_spawns_();
            retire_due_wrecks_();
        }

        accumulator_ -= sim_dt_;
        ++steps;
    }
    if (capped) {
        // Drop the debt, stay live (the scenario player's rule — never
        // queue unbounded catch-up behind a stall).
        accumulator_ = 0.0;
    }
    refresh_stats_();
    return capped;
}

void CampaignSession::adopt_new_spawns_() {
    // The one-world closure: everything the spawner materialized since
    // the last look joins the sim's roster, so the FM → transform sync
    // covers it. Registered idempotently — a spawn already in the
    // roster is a no-op. C6: registration and ARMING ride the same
    // cadence — every late spawn becomes a FIGHTING (or defending)
    // aircraft the same campaign second it joins the roster (the arm
    // is itself idempotent, so the pairing can never double-attach).
    const auto& spawned = spawner_->spawned();
    while (registered_spawns_ < spawned.size()) {
        sim_->register_aircraft(spawned[registered_spawns_]);
        sim_->arm_campaign_aircraft(spawned[registered_spawns_]);
        ++registered_spawns_;
    }
    // P7 — the flights' RoE rides on top of the armed doctrine (the
    // arm's configure_brain_combat owns the scenario's holds; the
    // flight's own roe_check byte gates from here on). CAMP-CMD-1: the
    // walk serves the COMMAND store too — the effective level is the
    // tightest of the carried byte and every matching roe_set scope,
    // recomputed from the doctrine baseline so a command can LOWER as
    // well as tighten (Simulation::set_flight_roe). Applied every
    // cadence — the writes are idempotent and the roster walk is
    // arrival-ordered, so re-applying is a no-op for an un-commanded
    // session (byte-identity holds: the recomputed values equal the
    // armed baseline).
    reapply_roe_();
}

void CampaignSession::retire_due_wrecks_() {
    // C5's roster bound: wrecks past their hold leave the world. The
    // walk is arrival-ordered and stable (compaction keeps relative
    // order), so two identically-driven sessions retire the same
    // entities on the same ticks — the ledger books stayed identical
    // anyway; this keeps the WORLDS identical too.
    if (wreck_hold_sec_ <= 0.0 || pending_wrecks_.empty()) return;
    const double now = sim_->sim_time_s();
    const double horizon = now - wreck_hold_sec_;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_wrecks_.size(); ++i) {
        if (pending_wrecks_[i].death_s <= horizon) {
            sim_->retire_aircraft(pending_wrecks_[i].id);
        } else {
            if (kept != i) {
                pending_wrecks_[kept] = pending_wrecks_[i];
            }
            ++kept;
        }
    }
    pending_wrecks_.resize(kept);
}

// ---------------------------------------------------------------------------
// G1 — the ground war's cadence + the entity-side mirror
// ---------------------------------------------------------------------------

namespace {

// One grid unit = 1024 ft (the campaign bridge's own constant; not
// exported — re-declared here the same way campaign_bridge.cpp does).
constexpr double kFtPerGrid = 1024.0;

// FID-5: the synthetic flights' reserved VU namespace. The save's
// VU_ID.nums share the 32-bit space, so the base is chosen in a range
// packed ids never take ("SY"); a collision is still handled loudly
// (register_synthetic refuses duplicates — the intent is dropped, the
// ladder's ledger books the draw, nothing crashes).
constexpr std::uint32_t kSyntheticVuBase = 0x53590000u;

} // namespace

void CampaignSession::advance_ground_() {
    if (ground_ == nullptr) return;

    // The engine's tick() accumulates on its own clock; feed it the
    // whole campaign seconds owed. One big tick == N small ones (the
    // C2 pin — the engine fires updates at fixed update_sec
    // boundaries).
    if (ground_sec_accum_ >= 1.0) {
        const auto whole = static_cast<f4::campaign::CampaignTime>(
            ground_sec_accum_);
        ground_sec_accum_ -= static_cast<double>(whole);
        ground_->tick(whole);
    }

    // Mirror whenever the engine actually advanced.
    if (ground_->stats().updates != ground_synced_updates_) {
        ground_synced_updates_ = ground_->stats().updates;
        sync_ground_entities_();
    }
}

void CampaignSession::sync_ground_entities_() {
    // The engine's state is campaign truth; the sim's entities are its
    // mirror (the one-world rule). One full pass per engine update —
    // only CHANGED values write (read first), so a stalled front
    // costs nothing. Destroyed battalions flip their ALIVE tag (the
    // entity stays in the world, the same lifetime a wreck keeps
    // before the reaper — a ground reaper is a later tranche's).
    auto& world = sim_->world();
    for (const auto& g : ground_->units()) {
        const auto it = unit_id_map_.find(g.vu);
        if (it == unit_id_map_.end()) continue;
        f4::entities::EntityHandle h(it->second, &world);

        auto* tf = h.get<f4::entities::TransformComponent>();
        if (tf != nullptr) {
            const f4::geo::WorldPosition want{
                static_cast<double>(g.x) * kFtPerGrid,
                static_cast<double>(g.y) * kFtPerGrid,
                tf->position.z   // terrain-following is not modeled; z
                                 // keeps the populate-time value
            };
            if (want.x != tf->position.x || want.y != tf->position.y) {
                tf->position = want;
            }
        }

        auto* gt = h.get<f4::entities::GroundTacticalComponent>();
        if (gt != nullptr) {
            gt->supply = g.supply;
            gt->morale = g.morale;
            gt->fatigue = g.fatigue;
            gt->heading = g.heading;
            gt->last_move = static_cast<std::int32_t>(std::min(
                static_cast<std::int64_t>(g.last_move),
                static_cast<std::int64_t>(2147483647)));
            gt->last_combat = static_cast<std::int32_t>(std::min(
                static_cast<std::int64_t>(g.last_combat),
                static_cast<std::int64_t>(2147483647)));
        }

        auto* uc = h.get<f4::entities::UnitCoreComponent>();
        if (uc != nullptr && uc->roster != g.roster) {
            uc->roster = g.roster;
        }

        if (g.destroyed) {
            const auto alive = h.get_tag(f4::entities::tags::ALIVE);
            if (!alive.has_value() || alive->as_bool()) {
                h.set_tag(f4::entities::tags::ALIVE,
                          f4::entities::TagValue::from(false));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// CAMP-DOM-6 — the task-force movement (the naval GroundWar sibling)
// ---------------------------------------------------------------------------

void CampaignSession::advance_naval_() {
    if (naval_ == nullptr) return;

    // The engine's tick() accumulates on its own clock; feed it the
    // whole campaign seconds owed (the ground cadence's twin — one
    // big tick == N small ones, the C2 pin).
    if (naval_sec_accum_ >= 1.0) {
        const auto whole = static_cast<f4::campaign::CampaignTime>(
            naval_sec_accum_);
        naval_sec_accum_ -= static_cast<double>(whole);
        naval_->tick(whole);
    }

    // Sync whenever the engine actually advanced: the moved rows land
    // in the session's WorldState (the `taskforces` query reads the
    // WorldState — the wire-state rule — so the sync IS the serving
    // face), then the 3D task-force entities mirror.
    if (naval_->stats().updates != naval_synced_updates_) {
        naval_synced_updates_ = naval_->stats().updates;
        (void)apply_naval_writeback();
        sync_naval_entities_();
    }
}

void CampaignSession::sync_naval_entities_() {
    // One full pass per engine update over the engine's task forces —
    // only CHANGED transforms write (read first, the ground mirror's
    // own rule). Sea hulls that never got a sim entity (the spawner's
    // own population rule) simply miss the id map and are skipped:
    // the WorldState row is the serving face either way.
    auto& world = sim_->world();
    for (const auto& n : naval_->units()) {
        const auto it = unit_id_map_.find(n.vu);
        if (it == unit_id_map_.end()) continue;
        f4::entities::EntityHandle h(it->second, &world);

        auto* tf = h.get<f4::entities::TransformComponent>();
        if (tf != nullptr) {
            const f4::geo::WorldPosition want{
                static_cast<double>(n.x) * kFtPerGrid,
                static_cast<double>(n.y) * kFtPerGrid,
                tf->position.z   // sea level is not modeled; z keeps
                                 // the populate-time value
            };
            if (want.x != tf->position.x || want.y != tf->position.y) {
                tf->position = want;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// FID — the fidelity-tier machinery (Docs/FIDELITY_TIERS_PLAN.md)
// ---------------------------------------------------------------------------

void CampaignSession::advance_flights_() {
    if (flights_ == nullptr) return;

    // The engine's tick() accumulates on its own clock (the ground
    // war's shape): feed it the whole campaign seconds owed.
    if (flight_sec_accum_ >= 1.0) {
        const auto whole = static_cast<f4::campaign::CampaignTime>(
            flight_sec_accum_);
        flight_sec_accum_ -= static_cast<double>(whole);
        flights_->tick(whole);
    }

    // Mirror whenever the engine actually advanced, then one tier pass
    // (per campaign second — O(flights), all distance tests).
    if (flights_->stats().updates != flight_synced_updates_) {
        flight_synced_updates_ = flights_->stats().updates;
        sync_flight_entities_();
    }
    evaluate_tiers_();
}

void CampaignSession::sync_flight_entities_() {
    // The engine's state is campaign truth; the sim's flight entities
    // are its mirror (the one-world rule, the ground mirror's twin).
    // Only changed values write. Deaggregated flights are skipped —
    // their aircraft own the truth while materialized.
    auto& world = sim_->world();
    for (const auto& f : flights_->flights()) {
        if (f.suspended) continue;
        const auto it = unit_id_map_.find(f.vu);
        if (it == unit_id_map_.end()) continue;
        f4::entities::EntityHandle h(it->second, &world);

        if (auto* tf = h.get<f4::entities::TransformComponent>()) {
            const f4::geo::WorldPosition want{
                f.fx * kFtPerGrid, f.fy * kFtPerGrid,
                static_cast<double>(f.altitude_ft)};
            if (want.x != tf->position.x || want.y != tf->position.y ||
                want.z != tf->position.z) {
                tf->position = want;
            }
        }
        if (auto* fp = h.get<f4::entities::FlightPlanComponent>()) {
            if (fp->fuel_burnt != f.fuel_burnt) {
                fp->fuel_burnt = f.fuel_burnt;
            }
            if (fp->altitude != f.altitude_ft) {
                fp->altitude = f.altitude_ft;
            }
        }
    }
}

void CampaignSession::evaluate_tiers_() {
    if (flights_ == nullptr) return;
    const std::int64_t now = campaign_time();
    const auto& fleet = flights_->flights();

    for (std::size_t i = 0; i < fleet.size(); ++i) {
        const auto& f = fleet[i];
        if (f.destroyed) continue;

        auto rec = deaggregated_.find(f.vu);
        if (rec != deaggregated_.end()) {
            // --- the reagg rules (FID-3) ---
            if (rec->second.trigger ==
                    DeaggregatedFlight::Trigger::Force) {
                continue;   // force pins until force_reaggregate_flight
            }
            if (now < rec->second.pinned_until) {
                continue;   // the ops window holds
            }
            // The bubble rule governs every non-force, unpinned
            // record: reaggregate when unobserved (the bubble dropped)
            // or outside the hysteresis band, cooldown permitting.
            const double dx = f.fx * kFtPerGrid - air_bubble_center_.x;
            const double dy = f.fy * kFtPerGrid - air_bubble_center_.y;
            const double dist_ft = std::sqrt(dx * dx + dy * dy);
            const double reagg_r =
                air_bubble_radius_ft_ * air_reagg_factor_;
            const bool unobserved =
                !air_bubble_active_ || dist_ft > reagg_r;
            const bool cooled =
                now >= rec->second.deagg_time +
                           static_cast<std::int64_t>(deagg_cooldown_sec_);
            if (unobserved && cooled) {
                reaggregate_flight_(f.vu);
            }
            continue;
        }

        // --- the deagg triggers (FID-3): ops > bubble ---
        // CAMP-CMD-2: a scrubbed sortie and an aborted flight never
        // deaggregate again (the books closed; the intent is gone or
        // stale — a resurrected sortie would fork the war).
        if (f.scrubbed || aborted_flights_.count(f.vu) != 0) continue;
        // FID-5 adds the TOT arm: a flight approaching its TIME ON
        // TARGET deaggregates to fly the attack (the delivery is a
        // per-aircraft phase — §4.3's mission-phase pinning; the same
        // window the takeoff/recovery arms ride). The generated war's
        // synthetic missions depend on it: a flight that folded back
        // mid-ingress re-deaggregates here and still delivers.
        const std::int32_t to_depart = flights_->seconds_to_depart(i);
        const std::int32_t to_over =
            flights_->seconds_to_mission_over(i);
        const std::int32_t to_tot =
            flights_->seconds_to_time_on_target(i);
        const auto window = static_cast<std::int32_t>(
            std::min<std::int64_t>(std::max(0, ops_window_sec_),
                                   2147483647));
        const bool takeoff_window = to_depart > 0 && to_depart <= window;
        const bool recovery_window = to_over >= 0 && to_over <= window;
        const bool tot_window = to_tot > 0 && to_tot <= window;
        bool in_bubble = false;
        if (air_bubble_active_) {
            const double dx = f.fx * kFtPerGrid - air_bubble_center_.x;
            const double dy = f.fy * kFtPerGrid - air_bubble_center_.y;
            in_bubble = dx * dx + dy * dy <=
                        air_bubble_radius_ft_ * air_bubble_radius_ft_;
        }
        if (!takeoff_window && !recovery_window && !tot_window &&
            !in_bubble) continue;

        deaggregate_flight_(
            i, (takeoff_window || recovery_window || tot_window)
                   ? DeaggregatedFlight::Trigger::Ops
                   : DeaggregatedFlight::Trigger::Bubble);
    }
}

// ---------------------------------------------------------------------------
// CAMP-HOST-2 — the event pump (see the header's block comment)
// ---------------------------------------------------------------------------

void CampaignSession::emit_mission_filed_(
    const f4::campaign::MissionIntent& intent) {
    f4::campaign::api::CampaignEvent e;
    e.kind = f4::campaign::api::CampaignEvent::Kind::MissionFiled;
    e.mission_filed.t = intent.issued_time;   // relative ladder seconds
    e.mission_filed.package_id = intent.package_id;
    e.mission_filed.flight_id = intent.flight_id;
    e.mission_filed.team = intent.team;
    e.mission_filed.mission_byte = intent.mission_byte;
    e.mission_filed.mission_name = intent.mission_name;
    e.mission_filed.target_objective_id = intent.target_objective_id;
    e.mission_filed.synthetic = intent.synthetic;
    sim_->bus().publish(e);
}

void CampaignSession::emit_cadence_events_() {
    namespace api = f4::campaign::api;

    // tasking_cycle — the diff of the fired-cycle counter IS this
    // whole-second block's fires (the clock can chunk several seconds
    // into one tick; the event says how many rode together).
    const auto fired = ladder_->cycles_fired() - last_cycles_fired_;
    if (fired > 0) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::TaskingCycle;
        e.tasking_cycle.t = ladder_->clock();
        e.tasking_cycle.cycles = static_cast<int>(fired);
        e.tasking_cycle.next_tasking_sec =
            static_cast<int>(ladder_->seconds_to_next_cycle());
        e.tasking_cycle.intents =
            static_cast<int>(ladder_->intents().size());
        sim_->bus().publish(e);
        last_cycles_fired_ = ladder_->cycles_fired();
    }

    // reinforcement_delivered — the reinforcement log's tail, grouped by
    // fire (records of one delivery share their booked second; one
    // event per fire with the delivery's totals). A fire that found no
    // deficit pushes no record and files no event — the family's name
    // is "delivered", and the books agree.
    const auto& rlog = ledger_->reinforcement_log();
    for (auto i = last_reinforcement_record_; i < rlog.size();) {
        const double t_s = rlog[i].t_s;
        int aircraft = 0;
        auto j = i;
        while (j < rlog.size() && rlog[j].t_s == t_s) {
            aircraft += rlog[j].delivered;
            ++j;
        }
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::ReinforcementDelivered;
        e.reinforcement_delivered.t =
            static_cast<std::int64_t>(std::llround(t_s));
        e.reinforcement_delivered.aircraft = aircraft;
        e.reinforcement_delivered.squadrons_touched =
            static_cast<int>(j - i);
        sim_->bus().publish(e);
        i = j;
    }
    last_reinforcement_record_ = rlog.size();
}

void CampaignSession::emit_capture_events_() {
    namespace api = f4::campaign::api;
    const auto& clog = ledger_->objective_captures();
    for (auto i = last_capture_record_; i < clog.size(); ++i) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::ObjectiveCaptured;
        e.objective_captured.t =
            static_cast<std::int64_t>(std::llround(clog[i].t_s));
        e.objective_captured.objective_id = clog[i].objective;
        e.objective_captured.new_owner = clog[i].to_team;
        sim_->bus().publish(e);
    }
    last_capture_record_ = clog.size();
}

void CampaignSession::emit_repair_events_() {
    namespace api = f4::campaign::api;
    // CAMP-DOM-2 — the repair log's tail: one event per repaired
    // objective per fire, in the ledger's arrival order. The event is
    // the books' summary; the repaired BITMAP itself reached the
    // ledger's damage-state face (apply_objective_repair) and rides
    // the write-back with the bomb damage it heals.
    const auto& rlog = ledger_->repair_log();
    if (last_repair_record_ < rlog.size()) {
        // The sim-side mirror: the engine's repaired face must join
        // the objective ENTITY's face, or the next damage sync diffs
        // stale rubble (state-3 features the engine already healed)
        // and re-books the pre-repair truth over the repair. Same
        // one-world rule sync_ground_entities_ keeps for battalions.
        auto& world = sim_->world();
        for (auto i = last_repair_record_; i < rlog.size(); ++i) {
            const auto& rp = rlog[i];
            const auto it = objective_id_map_.find(rp.objective);
            if (it != objective_id_map_.end()) {
                f4::entities::EntityHandle h(it->second, &world);
                auto* fs = h.get<f4::entities::FeatureSetComponent>();
                if (fs != nullptr) {
                    // Repaired indices: the save-side face said
                    // damaged/destroyed, the record's face says
                    // repaired (VIS_REPAIRED — the honest state: not
                    // rubble, not pristine). Restored hit points give
                    // the feature its structure back (a full-hp
                    // feature re-kills like a fresh one; the hp
                    // ledger's lazy init already reads the face).
                    const int capacity =
                        static_cast<int>(rp.fstatus.size()) * 4;
                    const int n = static_cast<int>(fs->features.size());
                    const int walk = std::min(n, capacity);
                    for (int fi = 0; fi < walk; ++fi) {
                        const std::size_t bi = static_cast<std::size_t>(fi);
                        const std::uint8_t now =
                            static_cast<std::uint8_t>(
                                (rp.fstatus[bi / 4] >> ((bi % 4) * 2)) &
                                0x03);
                        auto& f = fs->features[bi];
                        if (now != 1) continue;
                        if (f.damage_state == 2 || f.damage_state == 3) {
                            f.damage_state = 1;
                            if (f.hit_points > 0.0 &&
                                fs->feature_hp.size() == fs->features.size()) {
                                fs->feature_hp[bi] = f.hit_points;
                            }
                        }
                    }
                }
                auto* db = h.get<f4::entities::DamageBitmapComponent>();
                if (db == nullptr) db = &h.add<f4::entities::DamageBitmapComponent>();
                db->fstatus = rp.fstatus;
            }

            api::CampaignEvent e;
            e.kind = api::CampaignEvent::Kind::ObjectiveRepaired;
            e.objective_repaired.t =
                static_cast<std::int64_t>(std::llround(rp.t_s));
            e.objective_repaired.objective_id = rp.objective;
            e.objective_repaired.owner = rp.owner;
            e.objective_repaired.features_repaired =
                static_cast<std::uint32_t>(rp.features_repaired);
            e.objective_repaired.features_destroyed =
                static_cast<std::uint32_t>(rp.features_destroyed);
            e.objective_repaired.supply = rp.supply;
            sim_->bus().publish(e);
        }
    }
    last_repair_record_ = rlog.size();
}

void CampaignSession::emit_action_filed_events_() {
    namespace api = f4::campaign::api;
    // CAMP-ATM-1 — the ACTION tables' filing log tail: one event per
    // filing, in the ledger's arrival order (the order the ATM scanned
    // the objectives). The tasking_cycle event has already published
    // for this whole-second block, so the stream's order stays the
    // engine's: the cycle fired, THEN its filings name themselves.
    const auto& alog = ledger_->action_filing_log();
    for (auto i = last_action_record_; i < alog.size(); ++i) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::ActionFiled;
        e.action_filed.t =
            static_cast<std::int64_t>(std::llround(alog[i].t_s));
        e.action_filed.team = alog[i].team;
        e.action_filed.mission_byte = alog[i].mission;
        e.action_filed.mission_name =
            std::string(f4::campaign::mission_type_name(alog[i].mission));
        e.action_filed.action_type = alog[i].action_type;
        e.action_filed.context = alog[i].context;
        e.action_filed.objective_id = alog[i].objective;
        e.action_filed.damage_pct = alog[i].damage_pct;
        sim_->bus().publish(e);
    }
    last_action_record_ = alog.size();
}

void CampaignSession::emit_pilot_events_() {
    namespace api = f4::campaign::api;
    // CAMP-DOM-3 — the personnel logs' tails, one event per record in
    // the ledger's arrival order. The assignment log grows at the
    // tasking cadence (the Campaign's draw booking), the loss log at
    // the result sink (a crewed flight's death), the recovery log at
    // the mission-recovery pass (a crewed flight came home). The event
    // IS the books' face — no session-side state beyond the cursors.
    const auto& asign = ledger_->pilot_assignment_log();
    for (auto i = last_pilot_assignment_record_; i < asign.size(); ++i) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::PilotAssigned;
        e.pilot_assigned.t =
            static_cast<std::int64_t>(std::llround(asign[i].t_s));
        e.pilot_assigned.team = asign[i].team;
        e.pilot_assigned.squadron = asign[i].squadron;
        e.pilot_assigned.flight = asign[i].flight;
        e.pilot_assigned.pilots = asign[i].crew;
        sim_->bus().publish(e);
    }
    last_pilot_assignment_record_ = asign.size();

    const auto& lost = ledger_->pilot_loss_log();
    for (auto i = last_pilot_loss_record_; i < lost.size(); ++i) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::PilotLost;
        e.pilot_lost.t =
            static_cast<std::int64_t>(std::llround(lost[i].t_s));
        e.pilot_lost.team = lost[i].team;
        e.pilot_lost.squadron = lost[i].squadron;
        e.pilot_lost.flight = lost[i].flight;
        e.pilot_lost.pilot = lost[i].slot;
        sim_->bus().publish(e);
    }
    last_pilot_loss_record_ = lost.size();

    const auto& rec = ledger_->pilot_recovery_log();
    for (auto i = last_pilot_recovery_record_; i < rec.size(); ++i) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::PilotRecovered;
        e.pilot_recovered.t =
            static_cast<std::int64_t>(std::llround(rec[i].t_s));
        e.pilot_recovered.team = rec[i].team;
        e.pilot_recovered.squadron = rec[i].squadron;
        e.pilot_recovered.flight = rec[i].flight;
        e.pilot_recovered.pilot = rec[i].slot;
        e.pilot_recovered.missions_run = rec[i].missions_run;
        sim_->bus().publish(e);
    }
    last_pilot_recovery_record_ = rec.size();
}

void CampaignSession::emit_slot_denied_events_() {
    namespace api = f4::campaign::api;
    // CAMP-DOM-4 — the scheduling books' tail: one event per refusal in
    // the ledger's arrival order. The log grows only when the
    // scheduling arm is on (the Campaign's drain gates the booking),
    // so a disarmed session's stream stays the pre-DOM-4 shape.
    const auto& dlog = ledger_->slot_denial_log();
    for (auto i = last_slot_denial_record_; i < dlog.size(); ++i) {
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::SlotDenied;
        e.slot_denied.t =
            static_cast<std::int64_t>(std::llround(dlog[i].t_s));
        e.slot_denied.team = dlog[i].team;
        e.slot_denied.airbase = dlog[i].airbase;
        e.slot_denied.reason = dlog[i].reason;
        sim_->bus().publish(e);
    }
    last_slot_denial_record_ = dlog.size();
}

void CampaignSession::emit_damage_events_() {
    namespace api = f4::campaign::api;
    for (const auto& d : sink_->damage_synced()) {
        // The owner AFTER the damage — the WorldState's objective row
        // (the damage pass never moves ownership; the capture family
        // does, and it publishes from its own site).
        std::uint8_t owner = 0;
        for (const auto& o : ws_.objectives) {
            if (o.id_num == d.vu) {
                owner = o.owner;
                break;
            }
        }
        api::CampaignEvent e;
        e.kind = api::CampaignEvent::Kind::ObjectiveDamage;
        e.objective_damage.t = ladder_->clock();
        e.objective_damage.objective_id = d.vu;
        e.objective_damage.owner = owner;
        e.objective_damage.features_damaged = d.features_damaged;
        sim_->bus().publish(e);
    }
}

// ---------------------------------------------------------------------------
// CAMP-DOM-1 — the verdict (see the header's block comment)
// ---------------------------------------------------------------------------

f4::campaign::TheaterVerdict CampaignSession::verdict() const {
    // The LIVE owner per objective: the ground war's mirror when one
    // runs (it IS the engine's live truth — the WorldState's owner row
    // only catches up at the write-back), the WorldState otherwise.
    // Both walks preserve wire order, so the opening baseline's
    // indexing holds either way.
    std::vector<f4::campaign::VerdictObjective> live;
    if (ground_ != nullptr) {
        live.reserve(ground_->objectives().size());
        for (const auto& o : ground_->objectives()) {
            live.push_back(f4::campaign::VerdictObjective{o.owner,
                                                          o.priority});
        }
    } else {
        live.reserve(ws_.objectives.size());
        for (const auto& o : ws_.objectives) {
            live.push_back(f4::campaign::VerdictObjective{o.owner,
                                                          o.priority});
        }
    }
    auto v = f4::campaign::compute_theater_verdict(
        live, verdict_opening_owners_, *ledger_,
        ladder_->belligerent_teams(), ws_.campaign.te_victory_points);
    return v;
}

void CampaignSession::emit_verdict_events_() {
    namespace api = f4::campaign::api;
    const auto v = verdict();
    // Coarse-state diff: the event IS the change signal (the full rows
    // live on the query). A fresh session starts stalemate/no-lead, so
    // a war that never moves publishes nothing.
    if (v.band == last_verdict_band_ && v.leader_slot == last_verdict_leader_ &&
        v.leader_swing == last_verdict_swing_) {
        return;
    }
    last_verdict_band_ = v.band;
    last_verdict_leader_ = v.leader_slot;
    last_verdict_swing_ = v.leader_swing;
    api::CampaignEvent e;
    e.kind = api::CampaignEvent::Kind::Verdict;
    e.verdict.t = ladder_->clock();
    e.verdict.band = f4::campaign::band_name(v.band);
    e.verdict.leader = v.leader_slot;
    e.verdict.swing = v.leader_swing;
    sim_->bus().publish(e);
}

// ---------------------------------------------------------------------------
// CAMP-CMD-1 — the roe_set command path (see the header's block comment)
// ---------------------------------------------------------------------------

bool CampaignSession::roe_scope_matches_(
    const f4::campaign::api::RoEScope& scope,
    const CampaignOriginComponent& origin) const {
    namespace api = f4::campaign::api;
    switch (scope.kind) {
        case api::RoEScopeKind::Team:
            return origin.team_slot == scope.team;
        case api::RoEScopeKind::Mission:
            return origin.team_slot == scope.team &&
                   origin.mission_byte == scope.mission;
        case api::RoEScopeKind::Flight:
            return origin.flight_vu == scope.flight;
    }
    return false;
}

std::uint8_t CampaignSession::effective_flight_roe_(
    const CampaignOriginComponent& origin) const {
    // The TIGHTEST wins: the carried P7 byte is the floor the save
    // asked for, every matching command scope raises it, and a wider
    // scope's hold is a ceiling a narrower scope cannot cut through
    // (doctrine cascades down; loosening happens at the scope that
    // tightened). Two commands on the SAME scope: the later one wins
    // because it is that scope's current level.
    std::uint8_t roe = spawner_->flight_roe(origin.flight_vu);
    for (const auto& rc : roe_commands_) {
        if (roe_scope_matches_(rc.scope, origin)) {
            roe = std::max(roe, rc.level);
        }
    }
    return roe;
}

void CampaignSession::apply_effective_roe_(f4::entities::EntityId id) {
    auto* origin = f4::entities::EntityHandle(id, &sim_->world())
                       .get<CampaignOriginComponent>();
    if (origin == nullptr) return;
    sim_->set_flight_roe(id, effective_flight_roe_(*origin));
}

void CampaignSession::reapply_roe_() {
    // Every campaign aircraft: the spawner's spawns in arrival order,
    // then the FID-5 deagg aircraft. The per-entity writes are pure
    // per-brain gate assignments (no RNG, no cross-entity terms), so
    // the result is walk-order independent; the arrival orders are
    // pinned anyway. Entities gone (retired wrecks) or without an
    // origin (scenario aircraft) are skipped by the guards.
    for (const auto id : campaign_aircraft()) {
        apply_effective_roe_(id);
    }
}

std::vector<f4::entities::EntityId> CampaignSession::campaign_aircraft()
    const {
    std::vector<f4::entities::EntityId> out;
    out.reserve(spawner_->spawned().size() + deaggregated_.size());
    for (const auto id : spawner_->spawned()) out.push_back(id);
    for (const auto& [vu, rec] : deaggregated_) out.push_back(rec.aircraft);
    return out;
}

std::size_t CampaignSession::apply_roe_command(
    const f4::campaign::api::RoEScope& scope,
    f4::campaign::api::RoeLevel roe) {
    namespace api = f4::campaign::api;
    // One level per scope: a roe_set on the same scope REPLACES that
    // scope's level (the later command is the commander's latest word —
    // without this, max() over the diary would let a loosened scope's
    // own earlier hold bind forever). Distinct scopes compose by the
    // tightest-wins rule in effective_flight_roe_.
    roe_commands_.erase(
        std::remove_if(roe_commands_.begin(), roe_commands_.end(),
                       [&](const RoeCommand& rc) {
                           return rc.scope.kind == scope.kind &&
                                  rc.scope.team == scope.team &&
                                  rc.scope.mission == scope.mission &&
                                  rc.scope.flight == scope.flight;
                       }),
        roe_commands_.end());
    roe_commands_.push_back(RoeCommand{scope, static_cast<std::uint8_t>(roe)});

    // Recompute every aircraft's effective RoE from the doctrine
    // baseline (the full write — commands lower as well as tighten),
    // then count the live aircraft the scope matched (the ack's
    // context; a doctrine for future flights matches zero today).
    reapply_roe_();
    std::size_t matched = 0;
    const auto count_one = [&](f4::entities::EntityId id) {
        auto* origin = f4::entities::EntityHandle(id, &sim_->world())
                           .get<CampaignOriginComponent>();
        if (origin == nullptr) return;
        if (!roe_scope_matches_(scope, *origin)) return;
        auto* brain = f4::entities::EntityHandle(id, &sim_->world())
                          .get<f4::ai::BrainComponent>();
        if (brain != nullptr) ++matched;
    };
    for (const auto id : campaign_aircraft()) count_one(id);

    // roe_changed — the pinned encoder's publisher (HOST-2 note 4's
    // "waits for CAMP-CMD-1"). t = the ladder's relative seconds, the
    // books' own axis; the scope echoes the command's verbatim.
    api::CampaignEvent e;
    e.kind = api::CampaignEvent::Kind::RoeChanged;
    e.roe_changed.t = ladder_->clock();
    e.roe_changed.scope = scope;
    e.roe_changed.roe = roe;
    sim_->bus().publish(e);
    return matched;
}

// ---------------------------------------------------------------------------
// CAMP-CMD-2 — the retask / abort / priority writes
// ---------------------------------------------------------------------------

namespace {

// The built-route → engine-waypoint conversion (the synthetic seed's own
// mapping, factored for the retask write).
std::vector<f4::entities::WaypointState> to_engine_route(
        const std::vector<f4::campaign::RouteWaypoint>& route) {
    std::vector<f4::entities::WaypointState> out;
    out.reserve(route.size());
    for (const auto& wp : route) {
        f4::entities::WaypointState w;
        w.x = wp.x;
        w.y = wp.y;
        w.z = static_cast<std::int16_t>(
            std::clamp<std::int32_t>(wp.altitude_ft, -32768, 32767));
        w.action = wp.action;
        w.flags = static_cast<std::int16_t>(wp.flags);
        w.target_num = wp.target_num;
        out.push_back(w);
    }
    return out;
}

// The engine-waypoint → built-route conversion (the RTB land waypoint's
// ride back into the plan builder's vocabulary).
f4::campaign::RouteWaypoint to_route_waypoint(
        const f4::entities::WaypointState& w) {
    f4::campaign::RouteWaypoint rw;
    rw.x = w.x;
    rw.y = w.y;
    rw.altitude_ft = w.z;
    rw.action = w.action;
    rw.flags = static_cast<std::uint16_t>(w.flags);
    rw.target_num = w.target_num;
    return rw;
}

} // namespace

CampaignSession::FlightShape CampaignSession::find_flight_shape_(
    std::uint32_t vu) const {
    FlightShape shape;
    if (flights_ != nullptr) {
        const auto idx = flights_->index_of(vu);
        if (idx != static_cast<std::size_t>(-1)) {
            shape.found = true;
            shape.aggregate = true;
            shape.agg_index = idx;
            shape.team = flights_->flights()[idx].team;
        }
    }
    // The live aircraft (both shapes can coexist — a suspended flight's
    // aggregate row and its materialized complement). Arrival order.
    for (const auto id : campaign_aircraft()) {
        auto* origin = f4::entities::EntityHandle(id, &sim_->world())
                           .get<CampaignOriginComponent>();
        if (origin == nullptr || origin->flight_vu != vu) continue;
        shape.live.push_back(id);
        if (!shape.aggregate && shape.team == 0) {
            shape.team = origin->team_slot;
        }
    }
    shape.found = shape.aggregate || !shape.live.empty();
    return shape;
}

std::uint32_t CampaignSession::home_airbase_for_flight_(
    std::uint32_t vu) const {
    // The squadron's airbase field is an EntityId; the route builder
    // wants the objective VU. Reverse the session's own objective map.
    const auto vu_of_base = [&](f4::entities::EntityId base) -> std::uint32_t {
        if (!base.valid()) return 0;
        for (const auto& [obj_vu, eid] : objective_id_map_) {
            if (eid == base) return obj_vu;
        }
        return 0;
    };
    // 1. The ATM booking (the commit's own record — the authority for
    //    every filed flight, aggregate or spawned complement).
    if (const auto* booked = ladder_->atm_booked_flights()) {
        const auto flight_id = booking_flight_id_(vu);
        for (const auto& ft : *booked) {
            if (ft.flight_id == flight_id) return ft.airbase_vu;
        }
    }
    // 2. A synthetic's squadron entity (the deagg path's rule).
    const auto intent_it = synthetic_intents_.find(vu);
    if (intent_it != synthetic_intents_.end()) {
        const auto sq_it = unit_id_map_.find(intent_it->second.squadron_id);
        if (sq_it != unit_id_map_.end() && sq_it->second.valid()) {
            auto* sq = f4::entities::EntityHandle(sq_it->second,
                                                  &sim_->world())
                           .get<f4::entities::SquadronComponent>();
            if (sq != nullptr) return vu_of_base(sq->airbase);
        }
        return 0;
    }
    // 3. A save-carried flight: the world flight entity's plan names
    //    the squadron; the squadron names the base.
    const auto it = unit_id_map_.find(vu);
    if (it != unit_id_map_.end() && it->second.valid()) {
        auto* fp = f4::entities::EntityHandle(it->second, &sim_->world())
                       .get<f4::entities::FlightPlanComponent>();
        if (fp != nullptr && fp->squadron.valid()) {
            auto* sq = f4::entities::EntityHandle(fp->squadron,
                                                  &sim_->world())
                           .get<f4::entities::SquadronComponent>();
            if (sq != nullptr) return vu_of_base(sq->airbase);
        }
        return 0;
    }
    // 4. A full-fidelity synthetic with no booking: the origin stamp.
    for (const auto id : campaign_aircraft()) {
        auto* origin = f4::entities::EntityHandle(id, &sim_->world())
                           .get<CampaignOriginComponent>();
        if (origin != nullptr && origin->flight_vu == vu &&
            origin->home_airbase_vu != 0) {
            return origin->home_airbase_vu;
        }
    }
    return 0;
}

CampaignSession::CommandWriteResult CampaignSession::apply_retask_command(
    std::uint32_t flight_vu, std::uint8_t mission_byte,
    std::uint32_t target_vu) {
    CommandWriteResult res;

    // The mission's profile (the route builder's altitudes/actions and
    // the loiter term) — an untasked byte or a profile-less one refuses
    // (for_mission fails loudly; the refusal is the data).
    const f4::campaign::MissionProfile* profile = nullptr;
    try {
        profile = &profiles_.for_mission(mission_byte);
    } catch (const std::exception&) {
        res.status = CommandWrite::InvalidArgument;
        res.detail = "no mission profile for byte " +
                     std::to_string(mission_byte) + " (" +
                     std::string(f4::campaign::mission_type_name(
                         mission_byte)) + ")";
        return res;
    }

    // The target must be a real objective (the route builder anchors
    // the new plan's delivery on it).
    bool target_found = false;
    for (const auto& o : ws_.objectives) {
        if (o.id_num == target_vu) {
            target_found = true;
            break;
        }
    }
    if (!target_found) {
        res.status = CommandWrite::UnknownObjective;
        res.detail = "no objective carries id " +
                     std::to_string(target_vu);
        return res;
    }

    const FlightShape shape = find_flight_shape_(flight_vu);
    if (!shape.found) {
        res.status = CommandWrite::UnknownFlight;
        res.detail = "no such flight in the session's war";
        return res;
    }

    // The aggregate row's own terminal guards (a live-only flight has
    // no row; its brains refuse through the retask path below).
    if (shape.aggregate) {
        const auto& st = flights_->flights()[shape.agg_index];
        if (st.arrived) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "flight has arrived";
            return res;
        }
        if (st.destroyed) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "flight is destroyed";
            return res;
        }
        if (st.scrubbed) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "flight is aborted";
            return res;
        }
    } else if (shape.live.empty()) {
        res.status = CommandWrite::UnknownFlight;
        res.detail = "no such flight in the session's war";
        return res;
    }

    // The home airbase anchors the new route's build.
    const std::uint32_t airbase = home_airbase_for_flight_(flight_vu);
    if (airbase == 0) {
        res.status = CommandWrite::InvalidArgument;
        res.detail = "flight has no resolvable home airbase";
        return res;
    }

    // The head position: the aggregate's own truth, or — for a live
    // complement — the lead aircraft's real position (the suspended
    // row's state is frozen at deagg time).
    double hx = 0.0;
    double hy = 0.0;
    float halt = 0.0f;
    bool suspended = false;
    if (shape.aggregate) {
        const auto& st = flights_->flights()[shape.agg_index];
        hx = st.fx;
        hy = st.fy;
        halt = st.altitude_ft;
        suspended = st.suspended;
    }
    if (suspended || !shape.aggregate) {
        for (const auto id : shape.live) {
            auto* tf = f4::entities::EntityHandle(id, &sim_->world())
                           .get<f4::entities::TransformComponent>();
            if (tf != nullptr) {
                hx = tf->position.x / kFtPerGrid;
                hy = tf->position.y / kFtPerGrid;
                halt = static_cast<float>(tf->position.z);
                break;
            }
        }
    }

    // The new plan's route: home airbase → target through the threat
    // map (the session's own builder — the C3 planner, deterministic),
    // spliced at the ingress point (the new plan's IP) and headed with
    // the flight's CURRENT position. No IP (route-only shapes) splices
    // after the takeoff waypoint.
    const auto rb = route_builder_->build(shape.team, *profile, airbase,
                                          target_vu);
    if (rb.waypoints.size() < 2) {
        res.status = CommandWrite::InvalidArgument;
        res.detail = "route build failed for the new tasking";
        return res;
    }
    std::size_t splice = 1;
    for (std::size_t i = 0; i < rb.waypoints.size(); ++i) {
        if ((rb.waypoints[i].flags & f4::campaign::kWpfIp) != 0) {
            splice = i;
            break;
        }
    }
    std::vector<f4::campaign::RouteWaypoint> new_route;
    new_route.reserve(1 + rb.waypoints.size() - splice);
    f4::campaign::RouteWaypoint head;
    head.x = static_cast<std::int16_t>(
        std::clamp<std::int64_t>(std::llround(hx), -32768, 32767));
    head.y = static_cast<std::int16_t>(
        std::clamp<std::int64_t>(std::llround(hy), -32768, 32767));
    head.altitude_ft = static_cast<std::int32_t>(halt);
    head.action = 0;
    head.flags = f4::campaign::kWpfTurnPoint;
    new_route.push_back(head);
    for (std::size_t i = splice; i < rb.waypoints.size(); ++i) {
        new_route.push_back(rb.waypoints[i]);
    }

    // TOT + mission-over: the ATM's own arithmetic on the cruise-speed
    // estimate (straight-line head → target grid distance; the out and
    // home legs share it; the profile's loiter + the doubled reserve
    // close the shape — compose_packages' formula, retask-shaped).
    const double cruise = flights_ != nullptr
                              ? flights_->cruise_grid_per_min()
                              : atm_cruise_grid_per_min_;
    auto target_wp = rb.waypoints.end() - 1;
    for (auto it = rb.waypoints.begin(); it != rb.waypoints.end(); ++it) {
        if ((it->flags & f4::campaign::kWpfTarget) != 0 ||
            it->target_num == target_vu) {
            target_wp = it;
            break;
        }
    }
    const double tgx = static_cast<double>(target_wp->x) - hx;
    const double tgy = static_cast<double>(target_wp->y) - hy;
    const int travel_sec = std::max(
        60, static_cast<int>(std::ceil(
                std::sqrt(tgx * tgx + tgy * tgy) /
                (cruise > 0.0 ? cruise : 12.0) * 60.0)));
    const std::int64_t now_rel = ladder_->clock();
    const std::int64_t tot_rel = now_rel + travel_sec;
    const std::int64_t over_rel =
        tot_rel + static_cast<std::int64_t>(
                      std::max(0, profile->loitertime)) * 60 +
        travel_sec +
        2 * static_cast<std::int64_t>(std::max(0, atm_reserve_min_)) * 60;
    const auto clamp32 = [](std::int64_t v) {
        return static_cast<std::int32_t>(
            std::clamp<std::int64_t>(v, 0, 2147483647));
    };

    // 1. The aggregate row (tiered): the route, mission, and times.
    //    The head keeps the ORIGINAL departure on an un-launched
    //    flight (the takeoff gate holds it to its slot); a departed
    //    flight's head depart is inert. The value rides to 2b (the
    //    entity mirror keeps the same takeoff gate).
    std::int32_t engine_head_depart = 0;
    if (shape.aggregate) {
        auto engine_route = to_engine_route(new_route);
        if (!suspended && flights_->seconds_to_depart(shape.agg_index) > 0 &&
            !flights_->routes()[shape.agg_index].empty()) {
            engine_head_depart =
                flights_->routes()[shape.agg_index].front().depart;
            engine_route.front().depart = engine_head_depart;
        }
        flights_->retask(flight_vu, mission_byte, std::move(engine_route),
                         clamp32(epoch_ + tot_rel),
                         clamp32(epoch_ + over_rel));
    }

    // 2. The stored synthetic intent: a later deagg spawns the NEW
    //    plan (mission, target, relative TOT, route — the head rides
    //    as a turnpoint, the plan builder's takeoff drop ignores it).
    const auto intent_it = synthetic_intents_.find(flight_vu);
    if (intent_it != synthetic_intents_.end()) {
        auto& intent = intent_it->second;
        intent.mission_byte = mission_byte;
        intent.mission_name = std::string(
            f4::campaign::mission_type_name(mission_byte));
        intent.target_objective_id = target_vu;
        intent.time_on_target = clamp32(tot_rel);
        intent.route = new_route;
    }

    // 2b. A save-carried flight's world entity: the WaypointPlanComponent
    //    is what a later deagg spawn builds the aircraft's plan from —
    //    without this write the materialized sortie would fly the OLD
    //    route. The engine route's own form (WaypointState) drops in
    //    directly; the FID-2 mirror keeps the entity's position/fuel
    //    truth flowing as before.
    if (shape.aggregate) {
        const auto unit_it = unit_id_map_.find(flight_vu);
        if (unit_it != unit_id_map_.end() && unit_it->second.valid()) {
            auto* wp = f4::entities::EntityHandle(unit_it->second,
                                                  &sim_->world())
                           .get<f4::entities::WaypointPlanComponent>();
            if (wp != nullptr) {
                wp->waypoints = to_engine_route(new_route);
                if (engine_head_depart != 0) {
                    wp->waypoints.front().depart = engine_head_depart;
                }
            }
        }
    }

    // 3. The booking: the recovery clock follows the new plan (the
    //    draw stands — the books close when the retasked mission does).
    const auto flight_id = booking_flight_id_(flight_vu);
    const bool rebooked = ladder_->reschedule_flight(
        flight_id, mission_byte, target_vu, tot_rel, over_rel);

    // 4. The live complement: the brains re-plan from where they are.
    res.matched = 0;
    if (!shape.live.empty()) {
        auto plan_opt = f4::simulation::build_mission_plan_from_route(
            new_route, target_vu, &objective_id_map_, &unit_id_map_);
        if (!plan_opt.has_value()) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "the new plan does not build into a mission";
            return res;
        }
        const auto& plan = *plan_opt;
        for (const auto id : shape.live) {
            auto* brain = f4::entities::EntityHandle(id, &sim_->world())
                              .get<f4::ai::BrainComponent>();
            if (brain == nullptr) continue;
            if (brain->retask(plan)) ++res.matched;
        }
    }

    res.detail =
        "retasked to " +
        std::string(f4::campaign::mission_type_name(mission_byte)) +
        " vs objective " + std::to_string(target_vu) + " (TOT +" +
        std::to_string(travel_sec) + "s" +
        (rebooked ? ", booking rescheduled" : "") +
        (res.matched > 0
             ? ", " + std::to_string(res.matched) + " aircraft re-planned"
             : "") +
        ")";
    return res;
}

CampaignSession::CommandWriteResult CampaignSession::apply_abort_command(
    std::uint32_t flight_vu) {
    CommandWriteResult res;

    const FlightShape shape = find_flight_shape_(flight_vu);
    if (!shape.found) {
        res.status = CommandWrite::UnknownFlight;
        res.detail = "no such flight in the session's war";
        return res;
    }
    if (shape.aggregate) {
        const auto& st = flights_->flights()[shape.agg_index];
        if (st.arrived) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "flight has arrived";
            return res;
        }
        if (st.destroyed) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "flight is destroyed";
            return res;
        }
        if (st.scrubbed || aborted_flights_.count(flight_vu) != 0) {
            res.status = CommandWrite::InvalidArgument;
            res.detail = "flight is already aborted";
            return res;
        }
    }

    // The brains' phase decides the scrub-vs-RTB split for a live
    // complement (a tiered suspended flight materializes ONE aircraft;
    // full fidelity flies the whole complement).
    bool any_live = false;
    bool any_airborne = false;
    for (const auto id : shape.live) {
        any_live = true;
        auto* brain = f4::entities::EntityHandle(id, &sim_->world())
                          .get<f4::ai::BrainComponent>();
        if (brain != nullptr &&
            brain->phase() != f4::ai::BrainComponent::Phase::Ground) {
            any_airborne = true;
        }
    }
    const bool departed = shape.aggregate
                              ? (any_airborne ||
                                 flights_->seconds_to_depart(
                                     shape.agg_index) <= 0)
                              : any_airborne;

    // The RTB landing waypoint (the departed branch): the flight's own
    // route home — the aggregate route's last waypoint, the stored
    // intent's, or the home airbase objective's position.
    std::optional<f4::campaign::RouteWaypoint> land;
    double hx = 0.0;
    double hy = 0.0;
    float halt = 0.0f;
    if (shape.aggregate) {
        const auto& st = flights_->flights()[shape.agg_index];
        hx = st.fx;
        hy = st.fy;
        halt = st.altitude_ft;
        if (departed && !flights_->routes()[shape.agg_index].empty()) {
            land = to_route_waypoint(
                flights_->routes()[shape.agg_index].back());
        }
    }
    if (departed || any_live) {
        for (const auto id : shape.live) {
            auto* tf = f4::entities::EntityHandle(id, &sim_->world())
                           .get<f4::entities::TransformComponent>();
            if (tf != nullptr) {
                hx = tf->position.x / kFtPerGrid;
                hy = tf->position.y / kFtPerGrid;
                halt = static_cast<float>(tf->position.z);
                break;
            }
        }
    }
    if (departed && !land.has_value()) {
        const auto intent_it = synthetic_intents_.find(flight_vu);
        if (intent_it != synthetic_intents_.end() &&
            !intent_it->second.route.empty()) {
            land = intent_it->second.route.back();
        }
    }
    if (departed && !land.has_value()) {
        const std::uint32_t airbase = home_airbase_for_flight_(flight_vu);
        for (const auto& o : ws_.objectives) {
            if (o.id_num != airbase) continue;
            f4::campaign::RouteWaypoint rw;
            rw.x = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(o.x, -32768, 32767));
            rw.y = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(o.y, -32768, 32767));
            rw.altitude_ft = 0;
            rw.action = 7;   // WP_LAND (the fixtures' vocabulary)
            rw.flags = f4::campaign::kWpfLand;
            land = rw;
            break;
        }
    }
    if (departed && !land.has_value()) {
        res.status = CommandWrite::InvalidArgument;
        res.detail = "no route home for the flight";
        return res;
    }

    // The books close FIRST (the command's own semantics — the scrub's
    // recovery rides the current clock whether or not the air frames
    // keep flying): an ATM booking releases its survivors; a
    // save-carried flight has no booking in this session's ledger.
    const auto release = ladder_->scrub_flight(booking_flight_id_(flight_vu));

    std::int32_t scrubbed_count = 0;
    if (!departed) {
        // The sortie never launches. A live parked complement folds
        // back first (the parked aircraft rolls into the aggregate and
        // retires with the fold), then the aggregate row scrubs; a
        // full-fidelity parked complement retires directly.
        if (shape.aggregate) {
            if (deaggregated_.count(flight_vu) != 0) {
                (void)reaggregate_flight_(flight_vu);
            }
            if (flights_->scrub(flight_vu)) {
                ++scrubbed_count;
            }
        } else {
            for (const auto id : shape.live) {
                sim_->retire_aircraft(id);
                ++res.matched;
            }
        }
    } else {
        // RTB: the aggregate flies home (retask onto [head → land]),
        // the brains re-plan onto the same leg.
        if (shape.aggregate) {
            const double cruise = flights_->cruise_grid_per_min();
            const double dgx =
                static_cast<double>(land->x) - hx;
            const double dgy =
                static_cast<double>(land->y) - hy;
            const int home_sec = std::max(
                60, static_cast<int>(std::ceil(
                        std::sqrt(dgx * dgx + dgy * dgy) /
                        (cruise > 0.0 ? cruise : 12.0) * 60.0)));
            const std::int64_t now_rel = ladder_->clock();
            const std::int64_t over_rel =
                now_rel + home_sec +
                2 * static_cast<std::int64_t>(
                        std::max(0, atm_reserve_min_)) * 60;
            std::vector<f4::entities::WaypointState> rtb;
            rtb.reserve(2);
            f4::entities::WaypointState head;
            head.x = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(std::llround(hx), -32768,
                                         32767));
            head.y = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(std::llround(hy), -32768,
                                         32767));
            head.z = static_cast<std::int16_t>(
                std::clamp<std::int32_t>(
                    static_cast<std::int32_t>(halt), -32768, 32767));
            rtb.push_back(head);
            rtb.push_back(to_engine_route(
                              std::vector<f4::campaign::RouteWaypoint>{
                                  land.value()})
                              .front());
            flights_->retask(flight_vu,
                             flights_->flights()[shape.agg_index].mission,
                             std::move(rtb),
                             /*time_on_target_abs=*/0,
                             static_cast<std::int32_t>(
                                 std::clamp<std::int64_t>(
                                     over_rel + epoch_, 0, 2147483647)));
        }
        if (!shape.live.empty()) {
            std::vector<f4::campaign::RouteWaypoint> rtb_route;
            rtb_route.reserve(2);
            f4::campaign::RouteWaypoint head;
            head.x = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(std::llround(hx), -32768,
                                         32767));
            head.y = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(std::llround(hy), -32768,
                                         32767));
            head.altitude_ft = static_cast<std::int32_t>(halt);
            head.flags = f4::campaign::kWpfTurnPoint;
            rtb_route.push_back(head);
            rtb_route.push_back(land.value());
            auto plan_opt = f4::simulation::build_mission_plan_from_route(
                rtb_route, 0, &objective_id_map_, &unit_id_map_);
            if (plan_opt.has_value()) {
                const auto& plan = *plan_opt;
                for (const auto id : shape.live) {
                    auto* brain = f4::entities::EntityHandle(
                                      id, &sim_->world())
                                      .get<f4::ai::BrainComponent>();
                    if (brain == nullptr) continue;
                    if (brain->retask(plan)) ++res.matched;
                }
            }
        }
    }

    // The abort record: no future deagg trigger ever resurrects the
    // sortie, and the stored intent (if any) goes with it.
    aborted_flights_.insert(flight_vu);
    synthetic_intents_.erase(flight_vu);

    res.detail = departed ? "flight aborted — RTB; the books closed"
                          : "flight aborted before launch — the books closed";
    if (scrubbed_count > 0) {
        res.detail += " (sortie scrubbed)";
    }
    if (release.has_value()) {
        res.detail += " (" + std::to_string(release->survivors) +
                      " aircraft returned to the pool)";
    }
    return res;
}

CampaignSession::CommandWriteResult
CampaignSession::apply_objective_priority(std::uint32_t objective_vu,
                                          std::uint8_t weight) {
    CommandWriteResult res;
    for (auto& o : ws_.objectives) {
        if (o.id_num != objective_vu) continue;
        const auto before = o.priority;
        o.priority = weight;
        res.detail = "objective " + std::to_string(objective_vu) +
                     " priority " + std::to_string(before) + " → " +
                     std::to_string(weight);
        return res;
    }
    res.status = CommandWrite::UnknownObjective;
    res.detail = "no objective carries id " + std::to_string(objective_vu);
    return res;
}

// ---------------------------------------------------------------------------
// FID-5 — event-driven combat deagg (Docs/FIDELITY_TIERS_PLAN.md §4.5–4.6)
// ---------------------------------------------------------------------------

void CampaignSession::handle_mission_intent_(
    const f4::campaign::MissionIntent& intent) {
    // CAMP-HOST-2: EVERY intent the ladder publishes files its event
    // FIRST (before the FID-5 gates — a full-fidelity session files
    // missions too; it just never aggregates them). The publish is a
    // nested one inside the intent's own bus fan-out: the bus defers it
    // to the outer publish's end, so the stream order stays the engine's.
    emit_mission_filed_(intent);

    // Tiered + the synthetic arm only; full-fidelity sessions never
    // touch this path (the spawner's behavior is byte-identical).
    if (flights_ == nullptr || !synthetic_as_aggregates_) return;
    if (!intent.synthetic || intent.route.empty()) return;

    // Duplicate guard: a republished intent re-registers nothing (the
    // engine's own refusal is the second line of defense).
    const std::uint32_t vu =
        kSyntheticVuBase | (intent.flight_id & 0xFFFFu);
    if (flights_->find(vu) != nullptr) return;

    f4::campaign::SyntheticFlightSeed seed;
    seed.vu = vu;
    seed.team = intent.team;
    seed.mission = intent.mission_byte;
    seed.aircraft_count = intent.aircraft_count;
    // TOT: the intent's time is campaign-RELATIVE (the ladder's clock);
    // the engine's times are ABSOLUTE (the save epoch + advanced clock)
    // — the same anchor the waypoints' arrive/depart run on.
    const std::int64_t tot_abs =
        epoch_ + static_cast<std::int64_t>(intent.time_on_target);
    seed.time_on_target = static_cast<std::int32_t>(
        std::clamp<std::int64_t>(tot_abs, 0, 2147483647));
    seed.route.reserve(intent.route.size());
    for (const auto& wp : intent.route) {
        f4::entities::WaypointState w;
        w.x = wp.x;
        w.y = wp.y;
        w.z = static_cast<std::int16_t>(
            std::clamp<std::int32_t>(wp.altitude_ft, -32768, 32767));
        w.action = wp.action;
        w.flags = static_cast<std::int16_t>(wp.flags);
        w.target_num = wp.target_num;
        seed.route.push_back(w);
    }
    // The takeoff gate: the flight holds at its base until the first
    // waypoint departs. Two ops windows before TOT — the ATC's whole
    // window to fly it off before the delivery — clamped forward so a
    // late TOT never walks the aggregate immediately (the TOT window
    // arms the ground spawn for late missions anyway).
    // CAMP-DOM-4: the scheduling arm arms the window against the
    // flight's SCHEDULED slot instead — the aggregate materializes one
    // ops window before the grid's own minute and rolls ON it (the
    // delivery then lands at slot + travel, the engine's own TOT
    // estimate — the FIDELITY_TIERS §7 divergence closes). Flights
    // without a slot (the save's own, the legacy ladder, a base-less
    // filing) and disarmed sessions keep the TOT-anchored gate.
    if (!seed.route.empty()) {
        const std::int64_t earliest = campaign_time() + 1;
        std::int64_t depart;
        if (airbase_scheduling_ && intent.takeoff > 0) {
            const std::int64_t takeoff_abs =
                epoch_ + static_cast<std::int64_t>(intent.takeoff);
            depart = std::max(takeoff_abs, earliest);
        } else {
            depart =
                std::max(tot_abs - 2 * static_cast<std::int64_t>(
                                           std::max(0, ops_window_sec_)),
                         earliest);
        }
        seed.route.front().depart = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(depart, 1, 2147483647));
    }
    if (flights_->register_synthetic(seed) ==
        static_cast<std::size_t>(-1)) {
        return;   // unusable seed — the loud refusal, nothing registered
    }
    synthetic_intents_.emplace(vu, intent);
    ++synthetic_registered_;
    // The one-frame numbers go live immediately (the handler fires
    // outside the advance() cadence — a host reading stats between
    // frames sees the registration the same frame it happened).
    refresh_stats_();
}

void CampaignSession::rebuild_aggregate_feed_() {
    aggregate_contacts_.clear();
    aggregate_vu_set_.clear();
    if (flights_ == nullptr || !combat_deagg_) return;
    const auto& fleet = flights_->flights();
    aggregate_contacts_.reserve(fleet.size());
    const double cruise_fps =
        flights_->cruise_grid_per_min() * kFtPerGrid / 60.0;
    for (std::size_t i = 0; i < fleet.size(); ++i) {
        const auto& f = fleet[i];
        // The picture rule (§4.6, the walk's own clutter semantics made
        // coarse): airborne, progressing aggregates only — a ground-held
        // or arrived flight is the ramp, not the air picture; a
        // suspended flight's truth is its live aircraft (real contacts).
        // CAMP-CMD-2: a scrubbed sortie and an aborted (RTB-ing) flight
        // never re-enter the picture — the commit/convergence triggers
        // could otherwise resurrect a closed sortie.
        if (f.suspended || f.destroyed || f.arrived || f.scrubbed) continue;
        if (aborted_flights_.count(f.vu) != 0) continue;
        if (f.altitude_ft < 8000.0f) continue;
        f4::ai::AggregateContact c;
        c.flight_vu = f.vu;
        c.position = f4::geo::WorldPosition{
            f.fx * kFtPerGrid, f.fy * kFtPerGrid,
            static_cast<double>(f.altitude_ft)};
        // Cruise velocity along the leg: the aggregate's own heading ×
        // the engine's cruise constant. Compass → ENU (0 = north/+y).
        const double hdg = flights_->current_heading_rad(i);
        c.velocity = f4::geo::WorldPosition{
            std::sin(hdg) * cruise_fps, std::cos(hdg) * cruise_fps, 0.0};
        // The sim's own team vocabulary (blue/red/green) — the same
        // mapping the spawned aircraft's TEAM tags carry, so the
        // fusion's own-relative hostility sees aggregates exactly as it
        // sees materialized aircraft.
        c.team = owner_team_string(sim_->world(), f.team);
        aggregate_contacts_.push_back(c);
        aggregate_vu_set_.insert(f.vu);
    }
}

void CampaignSession::evaluate_combat_() {
    if (flights_ == nullptr || !combat_deagg_) return;
    const auto& fleet = flights_->flights();

    // --- Trigger A: a Tier-B fighter COMMITS against a Tier-A contact.
    // The brain's engagement id is the aggregate's flight VU (the
    // contact id the picture published). The roster is COPIED first —
    // the deagg below spawns entities into it.
    if (!aggregate_vu_set_.empty()) {
        const auto roster = sim_->aircraft_entities();   // copy
        for (const auto eid : roster) {
            f4::entities::EntityHandle h(eid, &sim_->world());
            auto* brain = h.get<f4::ai::BrainComponent>();
            if (brain == nullptr) continue;
            const std::uint64_t engaged = brain->combat_engagement_id();
            if (engaged == 0) continue;
            if (aggregate_vu_set_.count(engaged) == 0) continue;
            const std::size_t idx = flights_->index_of(
                static_cast<std::uint32_t>(engaged));
            if (idx == static_cast<std::size_t>(-1)) continue;
            deaggregate_flight_(idx, DeaggregatedFlight::Trigger::Combat);
        }
    }

    // --- Trigger B: two Tier-A tracks CONVERGE inside the engagement
    // envelope (§4.5). Predicted positions (current + cruise velocity ×
    // the lookahead) within the envelope and closing — both flights
    // deaggregate and the fight runs in-sim. Wire order, deterministic.
    if (combat_envelope_ft_ <= 0.0 || combat_lookahead_sec_ <= 0) return;
    const double T = static_cast<double>(combat_lookahead_sec_);
    const double cruise_fps =
        flights_->cruise_grid_per_min() * kFtPerGrid / 60.0;
    // The eligible set: airborne, progressing, opposing-team aggregates
    // (the same rule the picture feed applies, plus the belligerent
    // gate — the belligerents are the war's combatant slots).
    const auto belligerents = ladder_->belligerent_teams();
    const auto at_war = [&belligerents](std::uint8_t team) {
        for (const int b : belligerents) {
            if (b == static_cast<int>(team)) return true;
        }
        return false;
    };
    std::vector<std::size_t> eligible;
    eligible.reserve(fleet.size());
    for (std::size_t i = 0; i < fleet.size(); ++i) {
        const auto& f = fleet[i];
        if (f.suspended || f.destroyed || f.arrived || f.scrubbed) continue;
        if (aborted_flights_.count(f.vu) != 0) continue;   // CAMP-CMD-2
        if (f.altitude_ft < 8000.0f) continue;
        if (!at_war(f.team)) continue;
        eligible.push_back(i);
    }
    for (std::size_t a = 0; a < eligible.size(); ++a) {
        const std::size_t i = eligible[a];
        const auto& fi = fleet[i];
        if (fi.suspended) continue;   // a trigger-A deagg this pass
        const double hdg_i = flights_->current_heading_rad(i);
        const double vx_i = std::sin(hdg_i) * cruise_fps;
        const double vy_i = std::cos(hdg_i) * cruise_fps;
        const double px_i = fi.fx * kFtPerGrid + vx_i * T;
        const double py_i = fi.fy * kFtPerGrid + vy_i * T;
        for (std::size_t b = a + 1; b < eligible.size(); ++b) {
            const std::size_t j = eligible[b];
            const auto& fj = fleet[j];
            if (fj.suspended) continue;
            if (fj.team == fi.team) continue;   // allies do not merge
            // Current separation + closing rate (relative velocity along
            // the line of sight — negative = closing).
            const double rx = (fi.fx - fj.fx) * kFtPerGrid;
            const double ry = (fi.fy - fj.fy) * kFtPerGrid;
            const double rz = static_cast<double>(fi.altitude_ft) -
                              static_cast<double>(fj.altitude_ft);
            const double hdg_j = flights_->current_heading_rad(j);
            const double vx_j = std::sin(hdg_j) * cruise_fps;
            const double vy_j = std::cos(hdg_j) * cruise_fps;
            const double rvx = vx_i - vx_j;
            const double rvy = vy_i - vy_j;
            const double r_now =
                std::sqrt(rx * rx + ry * ry + rz * rz);
            if (r_now > combat_envelope_ft_ + cruise_fps * T) continue;
            if (rx * rvx + ry * rvy >= 0.0) continue;   // opening
            const double px_j = fj.fx * kFtPerGrid + vx_j * T;
            const double py_j = fj.fy * kFtPerGrid + vy_j * T;
            const double miss_x = px_i - px_j;
            const double miss_y = py_i - py_j;
            const double miss = std::sqrt(miss_x * miss_x +
                                          miss_y * miss_y + rz * rz);
            if (miss > combat_envelope_ft_) continue;
            deaggregate_flight_(i, DeaggregatedFlight::Trigger::Combat);
            deaggregate_flight_(j, DeaggregatedFlight::Trigger::Combat);
        }
    }

    // The feed LAST: the picture + the veto set always describe the
    // state this pass left behind (deaggregated flights drop out —
    // their aircraft are real contacts now).
    rebuild_aggregate_feed_();
}

void CampaignSession::deaggregate_flight_(
    std::size_t index, DeaggregatedFlight::Trigger trigger) {
    const auto& f = flights_->flights()[index];
    if (deaggregated_.count(f.vu) > 0) return;

    // FID-5: two flight identities — a SAVE flight resolves through the
    // world's unit map (the FID-4 bridge path); a SYNTHETIC flight
    // (the generated war's aggregate, §4.5) carries its MissionIntent
    // and spawns through the intent path.
    const auto entity_it = unit_id_map_.find(f.vu);
    const auto intent_it = synthetic_intents_.find(f.vu);
    const bool synthetic = entity_it == unit_id_map_.end();
    if (!synthetic) {
        // A save flight materializes through its world entity; an
        // invalid id (corrupt map) is the FID-4 loud-skip as before.
        if (!entity_it->second.valid()) return;
    } else if (intent_it == synthetic_intents_.end()) {
        return;   // no world flight, no intent — skip
    }

    // The handoff's spawn half (FID-4 §4.4): a GROUND spawn while the
    // flight has not departed (the ops takeoff — the ATC flies it off
    // through the takeoff modules) or when it has ARRIVED home (it is
    // parked at its base); an AIR spawn at the aggregate state otherwise
    // (the bridge's AirSpawnPose: in-air FM init, the plan's Enroute
    // start phase, the handoff's fuel).
    const auto& st = flights_->flights()[index];
    const std::int32_t to_depart = flights_->seconds_to_depart(index);
    const bool ground_spawn = to_depart > 0 || st.arrived;
    std::optional<f4::entities::EntityId> spawned;
    if (synthetic) {
        const auto& intent = intent_it->second;
        if (ground_spawn) {
            // Parking slot keyed on the squadron's airbase (the
            // spawner's own keying; its counters stay untouched under
            // deferral — the session owns the synthetic slots).
            std::uint64_t base_key = 0;
            const auto sq_it = unit_id_map_.find(intent.squadron_id);
            if (sq_it != unit_id_map_.end() && sq_it->second.valid()) {
                auto* sq = f4::entities::EntityHandle(sq_it->second,
                                                      &sim_->world())
                               .get<f4::entities::SquadronComponent>();
                if (sq && sq->airbase.value != 0) {
                    base_key = sq->airbase.value;
                }
            }
            const int slot = synthetic_parking_index_[base_key]++;
            spawned = f4::simulation::spawn_aircraft_for_intent(
                sim_->world(), intent, unit_id_map_, ct_, cfg_, airfield_,
                spawn_tpl_, slot,
                airbase_airfields_.empty() ? nullptr
                                           : &airbase_airfields_,
                &objective_id_map_, &weapon_table_, &unit_id_map_,
                /*air_pose=*/nullptr,
                sim_->theater_tables(), pilot_skill_flow_);
        } else {
            f4::simulation::AirSpawnPose pose;
            pose.position = f4::geo::WorldPosition{
                f.fx * kFtPerGrid, f.fy * kFtPerGrid,
                static_cast<double>(f.altitude_ft)};
            pose.heading_rad = flights_->current_heading_rad(index);
            // Cruise: the engine's speed-mode constant (grid/min → ft/s).
            pose.vt_fps =
                flights_->cruise_grid_per_min() * kFtPerGrid / 60.0;
            const double capacity = cfg_.geometry.internalFuel.value();
            pose.fuel_lbs = std::max(
                0.0, capacity - static_cast<double>(f.fuel_burnt));
            spawned = f4::simulation::spawn_aircraft_for_intent(
                sim_->world(), intent, unit_id_map_, ct_, cfg_, airfield_,
                spawn_tpl_, 0,
                airbase_airfields_.empty() ? nullptr
                                           : &airbase_airfields_,
                &objective_id_map_, &weapon_table_, &unit_id_map_,
                &pose, sim_->theater_tables(), pilot_skill_flow_);
        }
    } else if (ground_spawn) {
        spawned = f4::simulation::spawn_aircraft_for_flight(
            sim_->world(), entity_it->second, ct_, cfg_, airfield_,
            spawn_tpl_,
            /*parking_slot=*/0,
            airbase_airfields_.empty() ? nullptr : &airbase_airfields_,
            &objective_id_map_, &weapon_table_, &unit_id_map_,
            /*air_pose=*/nullptr, sim_->theater_tables(),
            pilot_skill_flow_);
    } else {
        f4::simulation::AirSpawnPose pose;
        pose.position = f4::geo::WorldPosition{
            f.fx * kFtPerGrid, f.fy * kFtPerGrid,
            static_cast<double>(f.altitude_ft)};
        pose.heading_rad = flights_->current_heading_rad(index);
        // Cruise: the engine's speed-mode constant (grid/min → ft/s).
        pose.vt_fps = flights_->cruise_grid_per_min() * kFtPerGrid / 60.0;
        // The handoff's fuel: capacity − the aggregate's per-aircraft
        // burnt. ≤ 0 (exhausted or capacity-less configs) keeps the
        // config default — the scenario path's own rule.
        const double capacity = cfg_.geometry.internalFuel.value();
        pose.fuel_lbs =
            std::max(0.0, capacity - static_cast<double>(f.fuel_burnt));
        spawned = f4::simulation::spawn_aircraft_for_flight(
            sim_->world(), entity_it->second, ct_, cfg_, airfield_,
            spawn_tpl_,
            0, airbase_airfields_.empty() ? nullptr : &airbase_airfields_,
            &objective_id_map_, &weapon_table_, &unit_id_map_, &pose,
            sim_->theater_tables(), pilot_skill_flow_);
    }
    if (!spawned.has_value() || !spawned->valid()) return;

    // The adopt cadence's own pairing: roster + doctrine arm (both
    // idempotent — the same calls adopt_new_spawns_ makes).
    sim_->register_aircraft(*spawned);
    sim_->arm_campaign_aircraft(*spawned);
    // CAMP-CMD-1: the deagged aircraft inherits the flight's doctrine
    // too — the effective RoE (carried byte + the command store) rides
    // on top of the fresh arm exactly as the adopt cadence would apply
    // it at the next whole-second boundary.
    apply_effective_roe_(*spawned);

    flights_->set_suspended(f.vu, true);
    DeaggregatedFlight rec;
    rec.aircraft = *spawned;
    rec.trigger = trigger;
    rec.deagg_time = campaign_time();
    // The pins: an ops deagg holds through its window (2×); a COMBAT
    // deagg holds the transient fight window (§4.5's phase pin) before
    // the standard reagg rules may fold it; a bubble deagg obeys the
    // hysteresis + cooldown immediately.
    rec.pinned_until =
        trigger == DeaggregatedFlight::Trigger::Ops
            ? rec.deagg_time +
                  2 * static_cast<std::int64_t>(std::max(0, ops_window_sec_))
        : trigger == DeaggregatedFlight::Trigger::Combat
            ? rec.deagg_time +
                  static_cast<std::int64_t>(
                      std::max(0, combat_window_sec_))
            : 0;
    deaggregated_.emplace(f.vu, rec);
    ++tier_deaggs_;
    if (trigger == DeaggregatedFlight::Trigger::Combat) {
        ++combat_deaggs_;
    }
}

bool CampaignSession::reaggregate_flight_(std::uint32_t vu) {
    auto rec = deaggregated_.find(vu);
    if (rec == deaggregated_.end()) return false;

    // The handoff's fold half (FID-4 §4.4): the LEAD aircraft's state
    // rolls up — position (the transform's ENU, grid-divided),
    // altitude, fuel (capacity − remaining, monotone into the engine).
    // An entity that died in-sim (killed, or already reaped) folds as
    // DESTROYED — the flight closes in the aggregate layer; its loss
    // is already booked at the C1 sink (the EntityKilled path).
    bool folded_live = false;
    if (rec->second.aircraft.valid()) {
        f4::entities::EntityHandle h(rec->second.aircraft, &sim_->world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (fm != nullptr) {
            const auto alive = h.get_tag(f4::entities::tags::ALIVE);
            const bool is_alive = !alive.has_value() || alive->as_bool();
            auto* tf = h.get<f4::entities::TransformComponent>();
            if (is_alive && tf != nullptr) {
                const double capacity = cfg_.geometry.internalFuel.value();
                const std::int32_t burnt = static_cast<std::int32_t>(
                    std::max(0.0, capacity - fm->fuel_lbs()));
                flights_->reaggregate(
                    vu, tf->position.x / kFtPerGrid,
                    tf->position.y / kFtPerGrid,
                    static_cast<float>(tf->position.z), burnt);
                folded_live = true;
            }
        }
    }
    if (!folded_live) {
        flights_->mark_destroyed(vu);
    }
    // The aircraft leaves the roster + the world (the reaper's own
    // mechanics; idempotent when the reaper already retired it).
    sim_->retire_aircraft(rec->second.aircraft);
    deaggregated_.erase(rec);
    ++tier_reaggs_;
    return true;
}

std::vector<CampaignSession::FlightTierView>
CampaignSession::flight_tiers() const {
    std::vector<FlightTierView> out;
    if (flights_ == nullptr) return out;
    out.reserve(flights_->flights().size());
    for (std::size_t i = 0; i < flights_->flights().size(); ++i) {
        const auto& f = flights_->flights()[i];
        FlightTierView v;
        v.vu = f.vu;
        v.team = f.team;
        v.mission = f.mission;
        v.aircraft_count = f.aircraft_count;
        v.x_grid = f.fx;
        v.y_grid = f.fy;
        v.altitude_ft = f.altitude_ft;
        v.fuel_burnt = f.fuel_burnt;
        v.live = f.suspended;
        v.arrived = f.arrived;
        v.destroyed = f.destroyed;
        v.to_depart = flights_->seconds_to_depart(i);
        v.to_mission_over = flights_->seconds_to_mission_over(i);
        // CAMP-CMD-2: the abort record (the engine's scrub state or the
        // session's own abort set — an RTB-ing flight is aborted too).
        v.aborted = f.scrubbed || aborted_flights_.count(f.vu) != 0;
        out.push_back(v);
    }
    return out;
}

std::optional<double> CampaignSession::flight_heading_rad(
    std::uint32_t vu) const {
    if (flights_ == nullptr) return std::nullopt;
    const std::size_t idx = flights_->index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return std::nullopt;
    return flights_->current_heading_rad(idx);
}

void CampaignSession::force_deaggregate_flight(std::uint32_t vu) {
    if (flights_ == nullptr) return;
    const std::size_t idx = flights_->index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    // CAMP-CMD-2: an aborted flight never materializes again (the
    // books closed; the same rule the tier triggers obey).
    if (flights_->flights()[idx].scrubbed ||
        aborted_flights_.count(vu) != 0) {
        return;
    }
    if (deaggregated_.count(vu) > 0) {
        // Already live: pin it (force beats every automatic trigger).
        deaggregated_[vu].trigger = DeaggregatedFlight::Trigger::Force;
        deaggregated_[vu].pinned_until = 0;
        return;
    }
    // Immediate (the UI calls this under the session lock — a paused
    // session still deaggregates on request, the V-3DLIVE rule).
    deaggregate_flight_(idx, DeaggregatedFlight::Trigger::Force);
    refresh_stats_();
}

void CampaignSession::force_reaggregate_flight(std::uint32_t vu) {
    if (flights_ == nullptr) return;
    if (reaggregate_flight_(vu)) {
        refresh_stats_();
    }
}

void CampaignSession::refresh_stats_() {
    stats_ = {};
    if (!sim_) return;
    stats_.cycles = ladder_->cycles_fired();
    // The tasking countdown (the campaign view's "next ATO wave"
    // readout — generated missions first land a full cycle in).
    stats_.next_tasking_sec = static_cast<int>(std::min<std::int64_t>(
        ladder_->seconds_to_next_cycle(), 2147483647));
    stats_.intents = static_cast<int>(ladder_->intents().size());
    stats_.routes_built = ladder_->routes_built();
    stats_.routes_failed = ladder_->routes_failed();
    for (const auto& in : ladder_->intents()) {
        if (!in.route.empty()) {
            stats_.route_waypoints += static_cast<int>(in.route.size());
        }
    }
    stats_.drawn_aircraft = ledger_->mission_draw_aircraft();
    stats_.air_losses = ledger_->air_losses();
    stats_.reinforce_fires = ledger_->reinforcement_fires();
    stats_.reinforced = ledger_->aircraft_reinforced();
    // C4: the ATM pipeline's own numbers (packages/escorts/recovery).
    if (const auto* atm = ladder_->atm_stats(); atm != nullptr) {
        stats_.packages = atm->packages_built;
        stats_.escorts = atm->escorts_built;
    }
    stats_.recovered = ledger_->aircraft_recovered();
    // C6: the campaign-combat counters (the armed doctrine's shape and
    // its ledger-side result — the war's A/A story in one place).
    stats_.armed_aircraft = sim_->campaign_armed_aircraft();
    stats_.armed_fighters = sim_->campaign_armed_fighters();
    stats_.armed_defensive = sim_->campaign_armed_defensive();
    stats_.aa_kills = ledger_->air_losses();
    // G1: the ground war's one-frame numbers (engine state + ledger
    // books — the panel's ground row).
    if (ground_ != nullptr) {
        const auto& gs = ground_->stats();
        stats_.ground_updates = gs.updates;
        stats_.ground_battalions = gs.battalions_alive;
        stats_.ground_mobile = gs.battalions_mobile;
        stats_.ground_engaged = gs.update_engaged;
        stats_.ground_front_columns = gs.front_columns;
        stats_.ground_losses = ledger_->ground_vehicle_losses();
        stats_.ground_destroyed = ledger_->ground_battalions_destroyed();
        stats_.ground_captures = ledger_->ground_objectives_captured();
    }
    // G2: the interdiction number reads the ledger directly (it books
    // with unit_strike on, with or without the engine — air-caused
    // losses are state even when nobody applies them).
    stats_.ground_losses_air = ledger_->ground_vehicle_losses_air();
    stats_.synthetic_spawned = spawner_->stats().synthetic_spawned;
    // FID: the tier numbers (inert without the engine).
    if (flights_ != nullptr) {
        const auto& fs = flights_->stats();
        stats_.agg_updates = fs.updates;
        stats_.agg_flights = fs.flights;
        stats_.agg_live = fs.suspended;
        stats_.agg_arrived = fs.arrived;
        stats_.agg_destroyed = fs.destroyed;
        stats_.tier_deaggs = tier_deaggs_;
        stats_.tier_reaggs = tier_reaggs_;
        // FID-5: the combat numbers (the triggers' bookkeeping + the
        // picture feed the sim is publishing this frame).
        stats_.combat_deaggs = combat_deaggs_;
        stats_.synthetic_aggregates = synthetic_registered_;
        stats_.agg_contacts = static_cast<int>(aggregate_contacts_.size());
        stats_.deferred_releases = sim_->deferred_releases();
    }
    stats_.live_aircraft = static_cast<int>(sim_->aircraft_entities().size());
    stats_.retired = sim_->retired_aircraft();
    for (const auto eid : sim_->aircraft_entities()) {
        auto h = f4::entities::EntityHandle(eid, &sim_->world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (fm && fm->model().state().gear.inAir) ++stats_.airborne;
    }
    stats_.sim_time_s = sim_->sim_time_s();
}

} // namespace f4::simulation

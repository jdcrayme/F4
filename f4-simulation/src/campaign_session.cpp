// f4-simulation/src/campaign_session.cpp
//
// CampaignSession — see campaign_session.hpp for the design. Headless
// orchestration: no rendering, no clocks of its own, deterministic.

#include <f4/simulation/campaign_session.hpp>

#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/io/read_file.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

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
        double sim_dt) {
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
    out << "  \"combat\": {\"enabled\": true},\n";
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
        out << ", \"mission\": " << filter.mission;
        out << ", \"max_flights\": " << filter.max_flights << "},\n";
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
            session->ct_.load(opts.class_table.string());
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

    // 6. The scenario (temp dir; world_json_path ABSOLUTE — the QC's
    //    relative-path lesson: the sim resolves it against the scenario
    //    file's directory).
    std::error_code ec;
    session->scenario_temp_dir_ =
        std::filesystem::temp_directory_path(ec) / "f4_viewer_session";
    if (!ec) {
        std::filesystem::create_directories(session->scenario_temp_dir_, ec);
    }
    if (ec) {
        return fail("cannot create temp dir " +
                    session->scenario_temp_dir_.string() + ": " +
                    ec.message());
    }
    const auto world_abs = std::filesystem::absolute(opts.world_json);
    const auto ct_abs =
        opts.class_table.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(opts.class_table);
    f4::simulation::FlightSpawnFilter filter;
    filter.team = opts.team;
    filter.mission = opts.mission;
    filter.max_flights = opts.max_flights;
    const bool have_flights = count_flights(session->ws_) > 0;
    const auto scenario_path =
        session->scenario_temp_dir_ / "scenario.json";
    {
        std::ofstream out(scenario_path);
        out << session_scenario_json(world_abs, ct_abs,
                                     opts.aircraft_config, have_flights,
                                     filter, opts.sim_dt);
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

    // 8. Cross-reference maps over the SIM's world (the spawner's
    //    squadron/parking resolution + the objective map for strike
    //    arming — the same maps the QC gets from populate_world).
    build_id_maps(session->sim_->world(), session->unit_id_map_,
                  session->objective_id_map_);

    // 9. The airfield + per-airbase map (spawn parking): derived from
    //    the session's WorldState, the same rule the QC applies.
    f4::simulation::ScenarioAirfield airfield;
    bool have_airfield = false;
    f4::simulation::AirbaseAirfieldMap airbase_airfields;
    for (const auto& obj : session->ws_.objectives) {
        if (auto af = f4::simulation::derive_airfield_from_objective(
                obj, 36)) {
            if (!have_airfield) {
                airfield = *af;
                have_airfield = true;
            }
            if (obj.id_num != 0) {
                airbase_airfields[obj.id_num] = std::move(*af);
            }
        }
    }
    if (!have_airfield) {
        // The QC hard-fails here; a SESSION degrades instead — the
        // spawner's own fallback ladder (route takeoff waypoint →
        // template threshold) still parks every generated mission, so
        // the loop runs on worlds the QC would reject.
        airfield = f4::simulation::ScenarioAirfield{};
    }

    // 10. The spawner — feeding THE SIM'S WORLD + BUS. Intents the
    //     ladder publishes materialize as aircraft in the same world
    //     the physics ticks. (The template shares the scenario's.)
    f4::simulation::ScenarioAircraft tpl;
    tpl.callsign = "CAMPAIGN";
    tpl.vis_type_index = 1052;
    tpl.aircraft_config_path = opts.aircraft_config.string();
    session->spawner_ =
        std::make_unique<f4::simulation::CampaignSimSpawner>(
            session->sim_->world(), session->unit_id_map_,
            session->ct_, session->db_, session->cfg_, airfield, tpl,
            filter);
    // Arming + parking: the builtin weapon table is a MEMBER (the
    // spawner borrows it; a temporary would dangle) — the QC keeps a
    // named local alive for the whole run, the session stores one.
    session->weapon_table_ = f4::weapons::WeaponClassTable::with_builtins();
    session->spawner_->set_objective_id_map(&session->objective_id_map_);
    session->spawner_->set_weapon_table(&session->weapon_table_);
    session->spawner_->set_airbase_airfields(
        airbase_airfields.empty() ? nullptr : &airbase_airfields);
    session->spawner_->attach(session->sim_->bus());

    // 11. The ladder (C2's one-pool tasking) over the SAME bus.
    f4::campaign::CampaignConfig ladder_cfg;
    ladder_cfg.air_task_cycle_sec = opts.tasking_cycle_sec;
    ladder_cfg.reinforcement_period_sec = opts.reinforce_period_sec;
    ladder_cfg.tasking_role_fallback = opts.role_fallback;
    session->ladder_ = std::make_unique<f4::campaign::Campaign>(
        static_cast<const f4::world::ICampaignSource&>(
            session->adapters_->campaign),
        static_cast<const f4::world::ITeamSource&>(
            session->adapters_->teams),
        static_cast<const f4::world::IUnitCoreSource&>(
            session->adapters_->units),
        session->profiles_, session->sim_->bus(), ladder_cfg);
    session->ladder_->set_result_ledger(session->ledger_.get());

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
    session->sink_->attach(session->sim_->bus());

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
    }
}

// ---------------------------------------------------------------------------
// advance()
// ---------------------------------------------------------------------------

bool CampaignSession::advance(double real_seconds) {
    if (paused_ || real_seconds <= 0.0) {
        refresh_stats_();
        return false;
    }
    accumulator_ += real_seconds;

    int steps = 0;
    bool capped = false;
    while (accumulator_ >= sim_dt_) {
        if (steps >= max_steps_per_advance_) {
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
            // The damage sync rides the same cadence: final-state diff
            // of every damaged objective (cheap — the diff walks only
            // objectives with damage components).
            sink_->sync_objective_damage();
            adopt_new_spawns_();
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
    // roster is a no-op.
    const auto& spawned = spawner_->spawned();
    while (registered_spawns_ < spawned.size()) {
        sim_->register_aircraft(spawned[registered_spawns_]);
        ++registered_spawns_;
    }
}

void CampaignSession::refresh_stats_() {
    stats_ = {};
    if (!sim_) return;
    stats_.cycles = ladder_->cycles_fired();
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
    stats_.synthetic_spawned = spawner_->stats().synthetic_spawned;
    stats_.live_aircraft = static_cast<int>(sim_->aircraft_entities().size());
    for (const auto eid : sim_->aircraft_entities()) {
        auto h = f4::entities::EntityHandle(eid, &sim_->world());
        auto* fm = h.get<f4::flight::FlightModelComponent>();
        if (fm && fm->model().state().gear.inAir) ++stats_.airborne;
    }
    stats_.sim_time_s = sim_->sim_time_s();
}

} // namespace f4::simulation

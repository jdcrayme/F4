// f4-simulation/src/campaign_session.cpp
//
// CampaignSession — see campaign_session.hpp for the design. Headless
// orchestration: no rendering, no clocks of its own, deterministic.

#include <f4/simulation/campaign_session.hpp>

#include <f4/campaign/ground_writeback.hpp>
#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/world_writeback.hpp>
#include <f4/simulation/scenario.hpp>
#include <f4/flight/flight_model_component.hpp>
#include <f4/data/config_loader.hpp>
#include <f4/io/read_file.hpp>
#include <f4/weapons/messages.hpp>
#include <f4/world/world_loader.hpp>   // populate_world (G1 mirror)

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
        double sim_dt,
        bool aa_combat,
        const std::filesystem::path& brain_data) {
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
               " \"guns_hold\": false},\n";
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
    const auto brain_abs =
        opts.brain_data.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(opts.brain_data);
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
                                     filter, opts.sim_dt, opts.aa_combat,
                                     brain_abs);
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
            session->ct_, session->db_, session->cfg_, session->airfield_,
            session->spawn_tpl_, filter);
    // Arming + parking: the builtin weapon table is a MEMBER (the
    // spawner borrows it; a temporary would dangle) — the QC keeps a
    // named local alive for the whole run, the session stores one.
    session->weapon_table_ = f4::weapons::WeaponClassTable::with_builtins();
    session->spawner_->set_objective_id_map(&session->objective_id_map_);
    session->spawner_->set_weapon_table(&session->weapon_table_);
    session->spawner_->set_airbase_airfields(
        session->airbase_airfields_.empty()
            ? nullptr
            : &session->airbase_airfields_);
    session->spawner_->attach(session->sim_->bus());

    // 11. The ladder (C2's one-pool tasking + C4's ATM pipeline) over
    //     the SAME bus.
    f4::campaign::CampaignConfig ladder_cfg;
    ladder_cfg.air_task_cycle_sec = opts.tasking_cycle_sec;
    ladder_cfg.reinforcement_period_sec = opts.reinforce_period_sec;
    ladder_cfg.atm_pipeline = opts.atm_pipeline;
    ladder_cfg.atm.min_seadescort_threat = opts.atm_seadescort_threat;
    // G2: the interdiction arm — BOTH ladders (legacy + ATM) and the
    // sink's unit-loss booking ride this one flag (the aa_combat /
    // ground_war opt-in contract).
    ladder_cfg.unit_strike = opts.unit_strike;
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
            // G1: the ground war rides the same whole-second cadence
            // (its own accumulator gates on the update granularity).
            if (ground_ != nullptr) {
                ground_sec_accum_ += static_cast<double>(whole);
                advance_ground_();
            }
            // The damage sync rides the same cadence: final-state diff
            // of every damaged objective (cheap — the diff walks only
            // objectives with damage components).
            sink_->sync_objective_damage();
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

// f4-simulation/src/campaign_session_host.cpp
//
// EngineSessionHost — the contract↔engine translation (see the header).
// Every method here is a MAP, not a policy: query rows come straight off
// the engine's own views (flight_tiers, intents, the WorldState's
// objectives, the ledger's byte-stable JSON), commands forward to the
// FID machinery the engine already exposes, and everything the engine
// cannot do yet comes back as a TYPED refusal naming its tranche.

#include <f4/simulation/campaign_session_host.hpp>

#include <f4/campaign/api/commands.hpp>
#include <f4/campaign/api/dto.hpp>
#include <f4/campaign/api/identity.hpp>
#include <f4/campaign/api/protocol.hpp>
#include <f4/geo/f4_geo.hpp>

#include <fstream>
#include <utility>
#include <vector>

namespace f4::simulation {

namespace api = f4::campaign::api;

namespace {

// The v1 query set the ADAPTER serves (the protocol whitelist is the
// contract's; this is the engine's answer). Every name here maps onto
// state the engine exposes TODAY — no stubs, no placeholders.
[[nodiscard]] bool engine_serves_query(std::string_view name) noexcept {
    return name == "time" || name == "stats" || name == "flights" ||
           name == "tasking" || name == "books" || name == "objectives";
}

} // namespace

std::unique_ptr<EngineSessionHost> EngineSessionHost::create(
    const CampaignSessionOptions& opts, std::string* error) {
    auto session = CampaignSession::create(opts, error);
    if (session == nullptr) {
        return nullptr;
    }
    auto host = std::unique_ptr<EngineSessionHost>(new EngineSessionHost());
    host->opts_ = opts;
    host->session_ = std::move(session);
    return host;
}

// --- lifecycle -----------------------------------------------------------

api::IdentityFingerprint EngineSessionHost::identity() const {
    api::IdentityFingerprint id;
    id.protocol_version = api::kProtocolVersion;
    id.campaign_time_s = session_->campaign_time();
    id.ledger_fnv =
        api::to_hex16(api::fnv1a64(session_->ledger_json()));
    return id;
}

api::StepResult EngineSessionHost::step(std::uint32_t ticks) {
    api::StepResult res;
    if (ticks == 0) {
        return res;
    }
    // The engine's accumulator drains whole ticks and carries the
    // sub-tick residue forward (advance()'s own discipline), and the
    // override caps the request so rounding can never overshoot. The
    // boolean is the cap-hit flag — the plan's dilation signal.
    res.dilated = session_->advance(
        static_cast<double>(ticks) * opts_.sim_dt, static_cast<int>(ticks));
    return res;
}

void EngineSessionHost::set_time_scale(double scale) {
    time_scale_ = scale;
}

void EngineSessionHost::set_paused(bool on) { session_->set_paused(on); }

api::SaveResult EngineSessionHost::save(std::string_view path) {
    api::SaveResult res;
    // The runtime-safe save (the C6 --save-write shape): write the
    // ledger's state back into the WorldState (air + ground books), then
    // emit the WorldState JSON. The .cam assembly stays the importer's
    // process — json2cam --reencode-all — the F4_SIDE boundary keeps it
    // out of this link closure (the plan §3.1 note).
    try {
        const auto writeback = session_->apply_writeback();
        (void)session_->apply_ground_writeback();
        const std::string json = session_->world_state().to_json_string();
        std::ofstream out(std::string(path), std::ios::binary);
        if (!out) {
            res.detail = "cannot open " + std::string(path);
            return res;
        }
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        out.close();
        if (!out) {
            res.detail = "write failed for " + std::string(path);
            return res;
        }
        res.ok = true;
        res.bytes = json.size();
        res.detail = std::string(path);
        // surface the write-back shape in the detail — the same numbers
        // the QC's save-write line reports (pools/squadrons/objectives)
        res.detail += " (writeback pools=" +
                      std::to_string(writeback.team_pools_written) + " sq=" +
                      std::to_string(writeback.squadrons_written) + " obj=" +
                      std::to_string(writeback.objectives_written) + ")";
    } catch (const std::exception& e) {
        res.detail = e.what();
    }
    return res;
}

// --- queries --------------------------------------------------------------

api::QueryResult EngineSessionHost::query(const api::QuerySpec& spec) {
    api::QueryResult res;
    if (!engine_serves_query(spec.name)) {
        // the protocol whitelist should have caught this; defensive and
        // loud (the loud-failure discipline, never a silent empty set)
        res.detail = "engine does not serve query: " + spec.name;
        return res;
    }

    const auto& stats = session_->stats();

    if (spec.name == "time") {
        api::TimeView t;
        t.tick_sec = opts_.sim_dt;
        t.campaign_time_s = session_->campaign_time();
        t.sim_time_s = stats.sim_time_s;
        t.paused = session_->paused();
        t.next_tasking_sec = stats.next_tasking_sec;
        t.time_scale = time_scale_;
        f4::json::Writer w;
        api::encode(w, t);
        res.ok = true;
        res.data_json = std::move(w).str();
        return res;
    }

    if (spec.name == "stats") {
        api::StatsView s;
        s.cycles = stats.cycles;
        s.next_tasking_sec = stats.next_tasking_sec;
        s.intents = stats.intents;
        s.routes_built = stats.routes_built;
        s.routes_failed = stats.routes_failed;
        s.route_waypoints = stats.route_waypoints;
        s.drawn_aircraft = stats.drawn_aircraft;
        s.air_losses = stats.air_losses;
        s.reinforce_fires = stats.reinforce_fires;
        s.reinforced = stats.reinforced;
        s.synthetic_spawned = stats.synthetic_spawned;
        s.live_aircraft = stats.live_aircraft;
        s.airborne = stats.airborne;
        s.sim_time_s = stats.sim_time_s;
        s.retired = stats.retired;
        s.packages = stats.packages;
        s.escorts = stats.escorts;
        s.recovered = stats.recovered;
        s.armed_aircraft = stats.armed_aircraft;
        s.armed_fighters = stats.armed_fighters;
        s.armed_defensive = stats.armed_defensive;
        s.aa_kills = stats.aa_kills;
        s.ground_updates = stats.ground_updates;
        s.ground_battalions = stats.ground_battalions;
        s.ground_mobile = stats.ground_mobile;
        s.ground_losses = stats.ground_losses;
        s.ground_losses_air = stats.ground_losses_air;
        s.ground_destroyed = stats.ground_destroyed;
        s.ground_captures = stats.ground_captures;
        s.ground_engaged = stats.ground_engaged;
        s.ground_front_columns = stats.ground_front_columns;
        s.agg_updates = stats.agg_updates;
        s.agg_flights = stats.agg_flights;
        s.agg_live = stats.agg_live;
        s.agg_arrived = stats.agg_arrived;
        s.agg_destroyed = stats.agg_destroyed;
        s.tier_deaggs = stats.tier_deaggs;
        s.tier_reaggs = stats.tier_reaggs;
        s.combat_deaggs = stats.combat_deaggs;
        s.synthetic_aggregates = stats.synthetic_aggregates;
        s.agg_contacts = stats.agg_contacts;
        s.deferred_releases = stats.deferred_releases;
        f4::json::Writer w;
        api::encode(w, s);
        res.ok = true;
        res.data_json = std::move(w).str();
        return res;
    }

    if (spec.name == "flights") {
        // The FID tier view — the aggregate picture by default (plan §4:
        // a host that never focuses sees exactly this).
        std::vector<api::FlightView> rows;
        for (const auto& ft : session_->flight_tiers()) {
            if (spec.team >= 0 && ft.team != spec.team) continue;
            api::FlightView f;
            f.vu = ft.vu;
            f.team = ft.team;
            f.mission = ft.mission;
            f.aircraft_count = ft.aircraft_count;
            f.x_grid = ft.x_grid;
            f.y_grid = ft.y_grid;
            f.altitude_ft = ft.altitude_ft;
            f.fuel_burnt = ft.fuel_burnt;
            f.live = ft.live;
            f.arrived = ft.arrived;
            f.destroyed = ft.destroyed;
            f.to_depart = ft.to_depart;
            f.to_mission_over = ft.to_mission_over;
            rows.push_back(f);
            if (spec.limit > 0 && rows.size() >= spec.limit) break;
        }
        f4::json::Writer w;
        api::encode(w, rows);
        res.ok = true;
        res.data_json = std::move(w).str();
        return res;
    }

    if (spec.name == "tasking") {
        std::vector<api::IntentView> rows;
        for (const auto& mi : session_->intents()) {
            if (spec.team >= 0 && mi.team != spec.team) continue;
            api::IntentView v;
            v.issued_time = mi.issued_time;
            v.time_on_target = mi.time_on_target;
            v.team = mi.team;
            v.team_name = mi.team_name;
            v.mission_byte = mi.mission_byte;
            v.mission_name = mi.mission_name;
            v.aircraft_count = mi.aircraft_count;
            v.squadron_id = mi.squadron_id;
            v.squadron_name = mi.squadron_name;
            v.package_id = mi.package_id;
            v.flight_id = mi.flight_id;
            v.target_objective_id = mi.target_objective_id;
            v.synthetic = mi.synthetic;
            rows.push_back(v);
            if (spec.limit > 0 && rows.size() >= spec.limit) break;
        }
        f4::json::Writer w;
        api::encode(w, rows);
        res.ok = true;
        res.data_json = std::move(w).str();
        return res;
    }

    if (spec.name == "books") {
        // The ledger's byte-stable JSON, wrapped as ONE escaped string —
        // the ledger's own writer is pretty-printed (multi-line), so
        // embedding it raw would break the wire's one-line framing. One
        // client-side string decode returns the EXACT ledger bytes; the
        // identity fingerprint hashes those same bytes.
        res.ok = true;
        res.data_json =
            "{\"ledger_json\":\"" +
            f4::json::escape_string(session_->ledger_json()) + "\"}";
        return res;
    }

    // objectives
    const auto& objectives = session_->world_state().objectives;
    std::vector<api::ObjectiveView> rows;
    for (const auto& o : objectives) {
        if (spec.team >= 0 && o.owner != spec.team) continue;
        api::ObjectiveView v;
        v.id_creator = o.id_creator;
        v.id_num = o.id_num;
        v.type = o.type;
        v.objective_type = o.objective_type;
        v.entity_type = o.entity_type;
        v.x = o.x;
        v.y = o.y;
        v.z = o.z;
        v.owner = o.owner;
        v.first_owner = o.first_owner;
        v.priority = o.priority;
        v.nameid = o.nameid;
        v.obj_flags = o.obj_flags;
        v.parent_id = o.parent_id;
        v.supply = o.supply;
        v.fuel = o.fuel;
        v.losses = o.losses;
        v.last_repair = o.last_repair;
        v.has_radar = o.has_radar;
        v.radar_range_km = o.radar_range_km;
        v.fstatus = o.fstatus;
        rows.push_back(v);
        if (spec.limit > 0 && rows.size() >= spec.limit) break;
    }
    f4::json::Writer w;
    api::encode(w, rows);
    res.ok = true;
    res.data_json = std::move(w).str();
    return res;
}

// --- commands -------------------------------------------------------------

api::CommandAck EngineSessionHost::submit(
    const api::CommandIntent& intent) {
    api::CommandAck ack;
    ack.apply_tick = session_->campaign_time();

    switch (intent.kind) {
        case api::CommandIntent::Kind::Focus: {
            if (!(intent.radius_ft > 0.0)) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::InvalidArgument;
                ack.detail = "focus needs radius_ft > 0";
                return ack;
            }
            // The FID camera bubble, as a command (plan §4): a paused
            // session still deaggregates what the host zooms into —
            // the V-3DLIVE rule, applied immediately.
            session_->set_view_bubble(
                intent.radius_ft, f4::geo::WorldPosition{
                                      intent.x, intent.y, intent.z});
            ack.detail = "view bubble set";
            return ack;
        }

        case api::CommandIntent::Kind::ClearFocus:
            session_->clear_view_bubble();
            ack.detail = "view bubble cleared";
            return ack;

        case api::CommandIntent::Kind::SelectDeagg: {
            // FID-4's force-deagg, command-shaped. Unknown vu = the
            // engine no-ops; the CONTRACT answers with a typed refusal
            // instead (refusal is data — the plan §3.3 rule).
            bool found = false;
            for (const auto& ft : session_->flight_tiers()) {
                if (ft.vu == intent.flight) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::UnknownFlight;
                ack.detail = "no such flight in the session's roster";
                return ack;
            }
            session_->force_deaggregate_flight(intent.flight);
            ack.detail = "flight deaggregated (force)";
            return ack;
        }

        case api::CommandIntent::Kind::SelectReagg: {
            bool found = false;
            bool live = false;
            for (const auto& ft : session_->flight_tiers()) {
                if (ft.vu == intent.flight) {
                    found = true;
                    live = ft.live;
                    break;
                }
            }
            if (!found) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::UnknownFlight;
                ack.detail = "no such flight in the session's roster";
                return ack;
            }
            if (!live) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::InvalidArgument;
                ack.detail = "flight is not deaggregated";
                return ack;
            }
            session_->force_reaggregate_flight(intent.flight);
            ack.detail = "flight reaggregated (force)";
            return ack;
        }

        case api::CommandIntent::Kind::RoeSet:
            // The CAMP-CMD queue — refused with the tranche named. The
            // wire is stable now so the commands land WITHOUT a
            // protocol bump (plan §3.3).
            ack.status = api::CommandAck::Status::Refused;
            ack.refusal = api::CommandAck::Refusal::NotImplemented;
            ack.detail = "CAMP-CMD-1 lands roe_set";
            return ack;

        case api::CommandIntent::Kind::FlightRetask:
            ack.status = api::CommandAck::Status::Refused;
            ack.refusal = api::CommandAck::Refusal::NotImplemented;
            ack.detail = "CAMP-CMD-2 lands flight_retask";
            return ack;

        case api::CommandIntent::Kind::FlightAbort:
            ack.status = api::CommandAck::Status::Refused;
            ack.refusal = api::CommandAck::Refusal::NotImplemented;
            ack.detail = "CAMP-CMD-2 lands flight_abort";
            return ack;

        case api::CommandIntent::Kind::ObjectivePriority:
            ack.status = api::CommandAck::Status::Refused;
            ack.refusal = api::CommandAck::Refusal::NotImplemented;
            ack.detail = "CAMP-CMD-2 lands objective_priority";
            return ack;
    }
    // unreachable (the Kind set is closed)
    return ack;
}

} // namespace f4::simulation

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
#include <f4/campaign/threat_map.hpp>  // CAMP-HOST-3: kThreatMapRatio echo
#include <f4/geo/f4_geo.hpp>

#include <algorithm>
#include <cmath>
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
           name == "tasking" || name == "books" ||
           name == "objectives" || name == "threat" ||
           name == "verdict";
}

// The session's command-write outcome → the contract's typed refusal
// (CAMP-CMD-2's mapping; one place, the dispatch's only policy).
[[nodiscard]] api::CommandAck::Refusal map_write_refusal_(
    CampaignSession::CommandWrite status) noexcept {
    switch (status) {
        case CampaignSession::CommandWrite::UnknownFlight:
            return api::CommandAck::Refusal::UnknownFlight;
        case CampaignSession::CommandWrite::UnknownObjective:
            return api::CommandAck::Refusal::UnknownObjective;
        case CampaignSession::CommandWrite::InvalidArgument:
        case CampaignSession::CommandWrite::Applied:
            break;
    }
    return api::CommandAck::Refusal::InvalidArgument;
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

EngineSessionHost::~EngineSessionHost() {
    // Unsubscribe EVERYTHING this host installed before the session
    // (and the bus it owns) dies — the same detach-first discipline the
    // session's own destructor follows.
    if (session_ == nullptr) return;
    auto& bus = session_->sim().bus();
    if (buffer_subscription_ != static_cast<std::size_t>(-1)) {
        bus.unsubscribe<f4::campaign::api::CampaignEvent>(
            buffer_subscription_);
        buffer_subscription_ = static_cast<std::size_t>(-1);
    }
    for (const auto& s : sinks_) {
        bus.unsubscribe<f4::campaign::api::CampaignEvent>(s.subscription);
    }
    sinks_.clear();
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
    if (replay_cursor_ >= replay_.size()) {
        // The engine's accumulator drains whole ticks and carries the
        // sub-tick residue forward (advance()'s own discipline), and the
        // override caps the request so rounding can never overshoot. The
        // boolean is the cap-hit flag — the plan's dilation signal.
        res.dilated = session_->advance(
            static_cast<double>(ticks) * opts_.sim_dt, static_cast<int>(ticks));
        return res;
    }
    // CAMP-CMD-1 — the replay path: segment the request around the
    // pending apply ticks so every replayed command lands at EXACTLY
    // the engine tick the record applied it at, no matter how the
    // replay's step requests are chunked (step(200) and 2×step(100)
    // reproduce the same war). A paused session absorbs the budget
    // without moving ticks (the HOST-1 semantics) — the commands stay
    // pending until the clock moves again.
    if (session_->paused()) {
        apply_due_replay_commands_();
        return res;
    }
    bool dilated = false;
    std::uint32_t remaining = ticks;
    while (remaining > 0) {
        apply_due_replay_commands_();
        if (replay_cursor_ >= replay_.size()) break;
        const std::uint64_t now = engine_ticks();
        const std::uint64_t next = replay_[replay_cursor_].apply_tick;
        if (next <= now) {
            // unreachable after apply_due unless the cap dropped the
            // debt below a pending tick (a replay whose stepping does
            // not match the record — the footer check fails loudly at
            // EOF); bail instead of spinning.
            break;
        }
        const std::uint32_t run = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, next - now));
        dilated = session_->advance(
                      static_cast<double>(run) * opts_.sim_dt,
                      static_cast<int>(run)) || dilated;
        remaining -= run;
    }
    if (replay_cursor_ >= replay_.size() && remaining > 0) {
        // the journal exhausted mid-request: finish the budget plainly
        dilated = session_->advance(
                      static_cast<double>(remaining) * opts_.sim_dt,
                      static_cast<int>(remaining)) || dilated;
    }
    res.dilated = dilated;
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
            // CAMP-CMD-2: the abort record rides the row's additive
            // tail (the DTO rule — new fields land at the END).
            f.aborted = ft.aborted;
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
            // CAMP-HOST-3: the C3 route leg count (the window's "wps"
            // column) + the package role (the pairing the ATM composed)
            // — additive fields, riding at the END of the row.
            v.route_waypoints = static_cast<int>(mi.route.size());
            v.flight_role = mi.flight_role;
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

    if (spec.name == "threat") {
        // The route-builder's threat map (C3) from the session's viewer
        // team — the SAM-ring picture behind the route lines, painted by
        // any UX that asks. The map is a pure function of the world (no
        // RNG), rebuilt by the route builder per tasking cycle; an
        // unbuilt map (no cycle yet) reads as an empty grid.
        api::ThreatView t;
        t.viewer_team = session_->threat_viewer_team();
        const auto& map = session_->route_builder().threat_map();
        t.cell_grid = f4::campaign::kThreatMapRatio;
        t.cells_x = map.cells_x();
        t.cells_y = map.cells_y();
        const std::size_t cells =
            static_cast<std::size_t>(t.cells_x) *
            static_cast<std::size_t>(t.cells_y);
        t.low.reserve(cells);
        t.high.reserve(cells);
        for (int cy = 0; cy < t.cells_y; ++cy) {
            for (int cx = 0; cx < t.cells_x; ++cx) {
                t.low.push_back(map.low_band_density(cx, cy, t.viewer_team));
                t.high.push_back(
                    map.high_band_density(cx, cy, t.viewer_team));
            }
        }
        f4::json::Writer w;
        api::encode(w, t);
        res.ok = true;
        res.data_json = std::move(w).str();
        return res;
    }

    if (spec.name == "verdict") {
        // CAMP-DOM-1: the books' projection, computed fresh per ask —
        // the same state the events' coarse diff reads. `t` rides the
        // engine's RELATIVE clock (the events' axis; the books the
        // verdict sums are run-scoped).
        const auto v = session_->verdict();
        api::VerdictView view;
        view.t = static_cast<std::int64_t>(session_->campaign().clock());
        view.threshold = v.victory_threshold;
        view.band = f4::campaign::band_name(v.band);
        view.leader = v.leader_slot;
        view.leader_swing = v.leader_swing;
        view.teams.reserve(v.teams.size());
        for (const auto& r : v.teams) {
            api::VerdictTeamRow row;
            row.slot = r.slot;
            row.name = r.name;
            row.owned = r.owned;
            row.gained = r.gained;
            row.lost = r.lost;
            row.gained_value = r.gained_value;
            row.lost_value = r.lost_value;
            row.swing = r.swing;
            row.captures = r.captures;
            row.air_losses = r.air_losses;
            row.ground_losses = r.ground_losses;
            row.battalions_destroyed = r.battalions_destroyed;
            row.aircraft_remaining = r.aircraft_remaining;
            view.teams.push_back(std::move(row));
        }
        f4::json::Writer w;
        api::encode(w, view);
        res.ok = true;
        res.data_json = std::move(w).str();
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

// --- events (§3.4, CAMP-HOST-2) -------------------------------------------

void EngineSessionHost::arm_buffer_() {
    if (buffer_armed_) return;
    buffer_subscription_ = session_->sim().bus()
        .subscribe<f4::campaign::api::CampaignEvent>(
            [this](const f4::campaign::api::CampaignEvent& e) {
                // push-time filtering: a kind/team the host did not
                // subscribe to never buffers (arms by use, stays small)
                if (f4::campaign::api::matches(filter_, e)) {
                    buffer_.push_back(e);
                }
            });
    buffer_armed_ = true;
}

void EngineSessionHost::set_event_filter(
    const f4::campaign::api::EventFilter& filter) {
    filter_ = filter;
    arm_buffer_();
}

std::vector<f4::campaign::api::CampaignEvent>
EngineSessionHost::drain_events() {
    // the events since the last drain, in engine occurrence order
    return std::exchange(buffer_, {});
}

std::size_t EngineSessionHost::add_event_sink(
    std::function<void(const f4::campaign::api::CampaignEvent&)> sink) {
    const auto subscription =
        session_->sim().bus().subscribe<f4::campaign::api::CampaignEvent>(
            sink);
    const auto handle = next_sink_handle_++;
    sinks_.push_back(EventSink{handle, subscription, std::move(sink)});
    return handle;
}

void EngineSessionHost::remove_event_sink(std::size_t handle) {
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
        if (it->handle == handle) {
            session_->sim().bus()
                .unsubscribe<f4::campaign::api::CampaignEvent>(
                    it->subscription);
            sinks_.erase(it);
            return;
        }
    }
}

// --- commands -------------------------------------------------------------

api::CommandAck EngineSessionHost::submit(
    const api::CommandIntent& intent) {
    if (replay_active_) {
        // CAMP-CMD-1: the journal is the command source during replay —
        // a wire command would fork the record (refusal is data).
        api::CommandAck ack;
        ack.apply_tick = session_->campaign_time();
        ack.status = api::CommandAck::Status::Refused;
        ack.refusal = api::CommandAck::Refusal::InvalidArgument;
        ack.detail =
            "command replay is active — this session's commands come "
            "from its journal";
        return ack;
    }
    return apply_command_(intent);
}

api::CommandAck EngineSessionHost::apply_command_(
    const api::CommandIntent& intent) {
    api::CommandAck ack = dispatch_command_(intent);
    if (ack.status == api::CommandAck::Status::Applied && journal_sink_) {
        // the intervention journal: the tick index (the replay axis) +
        // the campaign seconds (the ack's axis) + the intent. Refused
        // commands never journal — they mutate nothing.
        journal_sink_(engine_ticks(), session_->campaign_time(), intent);
    }
    return ack;
}

void EngineSessionHost::set_command_journal_sink(CommandJournalSink sink) {
    journal_sink_ = std::move(sink);
}

std::uint64_t EngineSessionHost::engine_ticks() const {
    // whole sim_dt steps drained since session start, from the engine's
    // own accumulated sim seconds (llround absorbs the per-tick fp
    // accumulation drift; a paused session moved no ticks)
    if (!(opts_.sim_dt > 0.0)) return 0;
    return static_cast<std::uint64_t>(
        std::llround(session_->stats().sim_time_s / opts_.sim_dt));
}

bool EngineSessionHost::start_command_replay(
    std::vector<api::CommandJournalEntry> entries, std::string* error) {
    if (replay_active_ || replay_cursor_ < replay_.size()) {
        if (error != nullptr) *error = "command replay already active";
        return false;
    }
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].apply_tick < entries[i - 1].apply_tick) {
            if (error != nullptr) {
                *error = "replay entries are not in apply order (tick " +
                         std::to_string(entries[i].apply_tick) + " < " +
                         std::to_string(entries[i - 1].apply_tick) + ")";
            }
            return false;
        }
    }
    replay_ = std::move(entries);
    replay_cursor_ = 0;
    replay_active_ = true;
    // anything already due applies NOW (the record applied it at this
    // same boundary — a tick-0 command lands before the first step)
    apply_due_replay_commands_();
    return true;
}

void EngineSessionHost::apply_due_replay_commands_() {
    const std::uint64_t now = engine_ticks();
    while (replay_cursor_ < replay_.size() &&
           replay_[replay_cursor_].apply_tick <= now) {
        (void)apply_command_(replay_[replay_cursor_].intent);
        ++replay_cursor_;
    }
}

api::CommandAck EngineSessionHost::dispatch_command_(
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

        case api::CommandIntent::Kind::RoeSet: {
            // CAMP-CMD-1 — roe_set rides the P7 fire-control path: the
            // scope joins the session's doctrine store, every campaign
            // aircraft's effective RoE recomputes (the full write — a
            // command can LOWER as well as tighten), and roe_changed
            // publishes. Validation first (refusal is data): a scope's
            // zero-values are non-targets (team 0 = no team, mission 0
            // = untasked, flight 0 = no VU), and a flight scope must
            // name a roster flight (doctrine for FUTURE flights goes
            // through the team/mission scopes it will spawn under).
            const auto level_name = [](api::RoeLevel l) {
                switch (l) {
                    case api::RoeLevel::Tight: return "tight";
                    case api::RoeLevel::Hold:  return "hold";
                    case api::RoeLevel::Free:  break;
                }
                return "free";
            };
            std::string scope_text;
            switch (intent.scope.kind) {
                case api::RoEScopeKind::Team:
                    if (intent.scope.team == 0) {
                        ack.status = api::CommandAck::Status::Refused;
                        ack.refusal =
                            api::CommandAck::Refusal::InvalidArgument;
                        ack.detail = "team 0 is not a doctrine target";
                        return ack;
                    }
                    scope_text = "team " +
                        std::to_string(intent.scope.team);
                    break;
                case api::RoEScopeKind::Mission:
                    if (intent.scope.team == 0 ||
                        intent.scope.mission == 0) {
                        ack.status = api::CommandAck::Status::Refused;
                        ack.refusal =
                            api::CommandAck::Refusal::InvalidArgument;
                        ack.detail =
                            "mission scope needs team >= 1 and "
                            "mission >= 1";
                        return ack;
                    }
                    scope_text = "team " +
                        std::to_string(intent.scope.team) + " mission " +
                        std::to_string(intent.scope.mission);
                    break;
                case api::RoEScopeKind::Flight:
                    if (intent.scope.flight == 0) {
                        ack.status = api::CommandAck::Status::Refused;
                        ack.refusal =
                            api::CommandAck::Refusal::UnknownFlight;
                        ack.detail = "flight VU 0 is not a roster id";
                        return ack;
                    }
                    {
                        bool found = false;
                        for (const auto& ft : session_->flight_tiers()) {
                            if (ft.vu == intent.scope.flight) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            ack.status =
                                api::CommandAck::Status::Refused;
                            ack.refusal =
                                api::CommandAck::Refusal::UnknownFlight;
                            ack.detail =
                                "no such flight in the session's roster";
                            return ack;
                        }
                    }
                    scope_text = "flight VU " +
                        std::to_string(intent.scope.flight);
                    break;
            }
            const auto matched =
                session_->apply_roe_command(intent.scope, intent.roe);
            ack.detail =
                "roe_set applied (" + scope_text + " = " +
                std::string(level_name(intent.roe)) + "): " +
                std::to_string(matched) + " live aircraft matched";
            return ack;
        }

        case api::CommandIntent::Kind::FlightRetask: {
            // CAMP-CMD-2 — the replan write. Wire-level validation
            // first (refusal is data): the mission byte must carry a
            // real, profiled mission and the target must be a real
            // objective; the session answers for the flight shape.
            if (intent.mission_byte == 0 ||
                !f4::campaign::mission_type_byte(
                     f4::campaign::mission_type_name(intent.mission_byte))
                     .has_value()) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::InvalidArgument;
                ack.detail = "mission byte " +
                             std::to_string(intent.mission_byte) +
                             " is not a taskable mission";
                return ack;
            }
            if (intent.target_objective_id == 0) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::InvalidArgument;
                ack.detail = "retask needs a target objective";
                return ack;
            }
            const auto res = session_->apply_retask_command(
                intent.flight, intent.mission_byte,
                intent.target_objective_id);
            if (res.status != CampaignSession::CommandWrite::Applied) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = map_write_refusal_(res.status);
                ack.detail = res.detail;
                return ack;
            }
            ack.detail = res.detail;
            return ack;
        }

        case api::CommandIntent::Kind::FlightAbort: {
            const auto res = session_->apply_abort_command(intent.flight);
            if (res.status != CampaignSession::CommandWrite::Applied) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = map_write_refusal_(res.status);
                ack.detail = res.detail;
                return ack;
            }
            ack.detail = res.detail;
            return ack;
        }

        case api::CommandIntent::Kind::ObjectivePriority: {
            // The save's own priority scale is 0..100 (the byte the
            // tasking scores scale against).
            if (intent.weight < 0 || intent.weight > 100) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = api::CommandAck::Refusal::InvalidArgument;
                ack.detail = "weight must be 0..100 (the objective "
                             "priority scale)";
                return ack;
            }
            const auto res = session_->apply_objective_priority(
                intent.objective_id,
                static_cast<std::uint8_t>(intent.weight));
            if (res.status != CampaignSession::CommandWrite::Applied) {
                ack.status = api::CommandAck::Status::Refused;
                ack.refusal = map_write_refusal_(res.status);
                ack.detail = res.detail;
                return ack;
            }
            ack.detail = res.detail;
            return ack;
        }
    }
    // unreachable (the Kind set is closed)
    return ack;
}

} // namespace f4::simulation

// f4-world-viewer/src/campaign_queries.cpp
//
// The contract-plane query walks (see the header). One helper per DTO
// shape, each an order-independent, additive-tolerant key walk.

#include "campaign_queries.hpp"

#include <f4/json/reader.hpp>

#include <cmath>
#include <utility>

namespace f4::viewer {

namespace {

// --- the shared object-walk pattern ---------------------------------------
//
// The DTOs are flat objects of scalar fields (plus the row arrays the
// per-query helpers handle). The walk: at the loop top '}' ends the
// object; each "key":value is dispatched; unknown keys skip_value();
// after each value ',' continues and '}' ends.

template <class FieldFn>
[[nodiscard]] bool walk_object(f4::json::Reader& r, FieldFn&& field) {
    r.expect('{');
    while (true) {
        if (r.consume('}')) return true;
        const auto key = r.read_string();
        r.expect(':');
        if (!field(key)) {
            r.skip_value();
        }
        if (r.consume(',')) continue;
        r.expect('}');
        return true;
    }
}

[[nodiscard]] int read_i(f4::json::Reader& r) {
    return static_cast<int>(r.read_int());
}

// The loud-failure discipline, client side: a malformed payload never
// yields partial garbage — the walk either completes or the fetch
// returns the zero value (the caller's !ok / empty-set path handles
// it; the same rule the engine's queries follow in reverse).
template <class Fn>
[[nodiscard]] auto guarded(Fn&& fn) noexcept ->
    decltype(fn()) {
    try {
        return fn();
    } catch (const std::exception&) {
        return decltype(fn()){};
    } catch (...) {
        return decltype(fn()){};
    }
}

[[nodiscard]] std::uint32_t read_u32(f4::json::Reader& r) {
    return static_cast<std::uint32_t>(r.read_int());
}

[[nodiscard]] std::int64_t read_i64(f4::json::Reader& r) {
    return static_cast<std::int64_t>(r.read_int());
}

// [ n, n, ... ] — the threat bands
[[nodiscard]] std::vector<int> read_int_array(f4::json::Reader& r) {
    std::vector<int> out;
    r.expect('[');
    if (!r.consume(']')) {
        while (true) {
            out.push_back(read_i(r));
            if (r.consume(',')) continue;
            r.expect(']');
            break;
        }
    }
    return out;
}

} // namespace

// --- time ------------------------------------------------------------------

SessionTime fetch_time(f4::campaign::api::ICampaignSession& s) {
    return guarded([&]() -> SessionTime {
        SessionTime t;
        const auto res = s.query({"time", -1, 0});
        if (!res.ok) return t;
        f4::json::Reader r(res.data_json);
        (void)walk_object(r, [&](const std::string& key) {
            if (key == "tick_sec") t.tick_sec = r.read_number();
            else if (key == "campaign_time_s") t.campaign_time_s = read_i64(r);
            else if (key == "sim_time_s") t.sim_time_s = r.read_number();
            else if (key == "paused") t.paused = r.read_int() != 0;
            else if (key == "next_tasking_sec") t.next_tasking_sec = read_i(r);
            else if (key == "time_scale") t.time_scale = r.read_number();
            else return false;
            return true;
        });
        t.ok = true;
        return t;
    });
}

// --- stats -------------------------------------------------------------------

SessionStats fetch_stats(f4::campaign::api::ICampaignSession& s) {
    return guarded([&]() -> SessionStats {
        SessionStats v;
        const auto res = s.query({"stats", -1, 0});
        if (!res.ok) return v;
        f4::json::Reader r(res.data_json);
        (void)walk_object(r, [&](const std::string& key) {
        if (key == "cycles") v.cycles = read_i(r);
        else if (key == "next_tasking_sec") v.next_tasking_sec = read_i(r);
        else if (key == "intents") v.intents = read_i(r);
        else if (key == "routes_built") v.routes_built = read_i(r);
        else if (key == "routes_failed") v.routes_failed = read_i(r);
        else if (key == "route_waypoints") v.route_waypoints = read_i(r);
        else if (key == "drawn_aircraft") v.drawn_aircraft = read_i(r);
        else if (key == "air_losses") v.air_losses = read_i(r);
        else if (key == "reinforce_fires") v.reinforce_fires = read_i(r);
        else if (key == "reinforced") v.reinforced = read_i(r);
        else if (key == "synthetic_spawned") v.synthetic_spawned = read_i(r);
        else if (key == "live_aircraft") v.live_aircraft = read_i(r);
        else if (key == "airborne") v.airborne = read_i(r);
        else if (key == "sim_time_s") v.sim_time_s = r.read_number();
        else if (key == "retired") v.retired = read_i(r);
        else if (key == "packages") v.packages = read_i(r);
        else if (key == "escorts") v.escorts = read_i(r);
        else if (key == "recovered") v.recovered = read_i(r);
        else if (key == "armed_aircraft") v.armed_aircraft = read_i(r);
        else if (key == "armed_fighters") v.armed_fighters = read_i(r);
        else if (key == "armed_defensive") v.armed_defensive = read_i(r);
        else if (key == "aa_kills") v.aa_kills = read_i(r);
        else if (key == "ground_updates") v.ground_updates = read_i(r);
        else if (key == "ground_battalions") v.ground_battalions = read_i(r);
        else if (key == "ground_mobile") v.ground_mobile = read_i(r);
        else if (key == "ground_losses") v.ground_losses = read_i(r);
        else if (key == "ground_losses_air") v.ground_losses_air = read_i(r);
        else if (key == "ground_destroyed") v.ground_destroyed = read_i(r);
        else if (key == "ground_captures") v.ground_captures = read_i(r);
        else if (key == "ground_engaged") v.ground_engaged = read_i(r);
        else if (key == "ground_front_columns")
            v.ground_front_columns = read_i(r);
        else if (key == "agg_updates") v.agg_updates = read_i(r);
        else if (key == "agg_flights") v.agg_flights = read_i(r);
        else if (key == "agg_live") v.agg_live = read_i(r);
        else if (key == "agg_arrived") v.agg_arrived = read_i(r);
        else if (key == "agg_destroyed") v.agg_destroyed = read_i(r);
        else if (key == "tier_deaggs") v.tier_deaggs = read_i(r);
        else if (key == "tier_reaggs") v.tier_reaggs = read_i(r);
        else if (key == "combat_deaggs") v.combat_deaggs = read_i(r);
        else if (key == "synthetic_aggregates")
            v.synthetic_aggregates = read_i(r);
        else if (key == "agg_contacts") v.agg_contacts = read_i(r);
        else if (key == "deferred_releases")
            v.deferred_releases = read_i(r);
        else return false;
        return true;
        });
    v.ok = true;
    return v;
    });
}

// --- flights -----------------------------------------------------------------

namespace {

[[nodiscard]] FlightRow read_flight(f4::json::Reader& r) {
    FlightRow f;
    (void)walk_object(r, [&](const std::string& key) {
        if (key == "vu") f.vu = read_u32(r);
        else if (key == "team") f.team = read_i(r);
        else if (key == "mission") f.mission = read_i(r);
        else if (key == "aircraft_count") f.aircraft_count = read_i(r);
        else if (key == "x_grid") f.x_grid = r.read_number();
        else if (key == "y_grid") f.y_grid = r.read_number();
        else if (key == "altitude_ft")
            f.altitude_ft = static_cast<float>(r.read_number());
        else if (key == "fuel_burnt")
            f.fuel_burnt = static_cast<std::int32_t>(r.read_int());
        else if (key == "live") f.live = r.read_int() != 0;
        else if (key == "arrived") f.arrived = r.read_int() != 0;
        else if (key == "destroyed") f.destroyed = r.read_int() != 0;
        else if (key == "to_depart")
            f.to_depart = static_cast<std::int32_t>(r.read_int());
        else if (key == "to_mission_over")
            f.to_mission_over = static_cast<std::int32_t>(r.read_int());
        else return false;
        return true;
    });
    return f;
}

[[nodiscard]] IntentRow read_intent(f4::json::Reader& r) {
    IntentRow in;
    (void)walk_object(r, [&](const std::string& key) {
        if (key == "issued_time") in.issued_time = read_i64(r);
        else if (key == "time_on_target") in.time_on_target = read_i64(r);
        else if (key == "team") in.team = read_i(r);
        else if (key == "team_name") in.team_name = r.read_string();
        else if (key == "mission_byte") in.mission_byte = read_i(r);
        else if (key == "mission_name") in.mission_name = r.read_string();
        else if (key == "aircraft_count") in.aircraft_count = read_i(r);
        else if (key == "squadron_id") in.squadron_id = read_u32(r);
        else if (key == "squadron_name") in.squadron_name = r.read_string();
        else if (key == "package_id") in.package_id = read_u32(r);
        else if (key == "flight_id") in.flight_id = read_u32(r);
        else if (key == "target_objective_id")
            in.target_objective_id = read_u32(r);
        else if (key == "synthetic") in.synthetic = r.read_int() != 0;
        else if (key == "route_waypoints") in.route_waypoints = read_i(r);
        else if (key == "flight_role") in.flight_role = read_i(r);
        else return false;
        return true;
    });
    return in;
}

template <class Row, class ReadRow>
[[nodiscard]] std::vector<Row> read_rows(f4::campaign::api::ICampaignSession& s,
                                         const char* name, ReadRow&& read) {
    return guarded([&]() -> std::vector<Row> {
        std::vector<Row> out;
        const auto res = s.query({name, -1, 0});
        if (!res.ok) return out;
        f4::json::Reader r(res.data_json);
        r.expect('[');
        if (!r.consume(']')) {
            while (true) {
                out.push_back(read(r));
                if (r.consume(',')) continue;
                r.expect(']');
                break;
            }
        }
        return out;
    });
}

} // namespace

std::vector<FlightRow> fetch_flights(f4::campaign::api::ICampaignSession& s) {
    return read_rows<FlightRow>(s, "flights", read_flight);
}

std::vector<IntentRow> fetch_tasking(f4::campaign::api::ICampaignSession& s) {
    return read_rows<IntentRow>(s, "tasking", read_intent);
}

// --- threat -------------------------------------------------------------------

ThreatGrid fetch_threat(f4::campaign::api::ICampaignSession& s) {
    return guarded([&]() -> ThreatGrid {
        ThreatGrid t;
        const auto res = s.query({"threat", -1, 0});
        if (!res.ok) return t;
        f4::json::Reader r(res.data_json);
        (void)walk_object(r, [&](const std::string& key) {
            if (key == "viewer_team") t.viewer_team = read_i(r);
            else if (key == "cell_grid") t.cell_grid = read_i(r);
            else if (key == "cells_x") t.cells_x = read_i(r);
            else if (key == "cells_y") t.cells_y = read_i(r);
            else if (key == "low") t.low = read_int_array(r);
            else if (key == "high") t.high = read_int_array(r);
            else return false;
            return true;
        });
        t.ok = true;
        return t;
    });
}

// --- books ---------------------------------------------------------------------

std::string fetch_books_ledger(f4::campaign::api::ICampaignSession& s) {
    return guarded([&]() -> std::string {
        const auto res = s.query({"books", -1, 0});
        if (!res.ok) return {};
        // {"ledger_json":"<the escaped ledger>"} — read_string()
        // unescapes, returning the EXACT ledger bytes (the identity
        // hashes these).
        f4::json::Reader r(res.data_json);
        std::string ledger;
        (void)walk_object(r, [&](const std::string& key) {
            if (key == "ledger_json") {
                ledger = r.read_string();
                return true;
            }
            return false;
        });
        return ledger;
    });
}

// --- the snapshot ----------------------------------------------------------------

SessionSnapshot fetch_snapshot(f4::campaign::api::ICampaignSession& s,
                               bool with_threat) {
    return guarded([&]() -> SessionSnapshot {
        SessionSnapshot snap;
        snap.time = fetch_time(s);
        snap.stats = fetch_stats(s);
        snap.flights = fetch_flights(s);
        snap.tasking = fetch_tasking(s);
        if (with_threat) {
            snap.threat = fetch_threat(s);
        }
        return snap;
    });
}

} // namespace f4::viewer

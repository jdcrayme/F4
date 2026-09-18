// f4-campaign-api/include/f4/campaign/api/dto.hpp
//
// Surface 2 — the v1 query DTOs (CAMP_HOST_PLAN.md §3.2) and their
// byte-stable encoders. THE CANONICAL KEY ORDER IS THE CONTRACT: goldens
// pin these bytes, adapters on both sides diff them. Do not reorder, do
// not pretty-print, do not add a key mid-sequence — additive fields go at
// the END of an object and the protocol version decides when clients must
// care (plan §11).
//
// Field naming follows the wire (snake_case), not the engine's member
// names; the adapters own the mapping. Every view is engine-agnostic —
// plain numbers and strings — so any language implements a client.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <f4/json/writer.hpp>

namespace f4::campaign::api {

// --- time ---------------------------------------------------------------
//
// The `time` query: the session clock + the pacing presentation. tick_sec
// is the engine's fixed dt (never scaled — plan §2.2); time_scale is what
// set_time_scale last carried.

struct TimeView {
    double tick_sec{0.0};
    std::int64_t campaign_time_s{0};
    double sim_time_s{0.0};
    bool paused{false};
    int next_tasking_sec{0};
    double time_scale{1.0};
};

inline void encode(f4::json::Writer& w, const TimeView& t) {
    w.raw("{\"tick_sec\":");
    w.number(t.tick_sec);
    w.raw(",\"campaign_time_s\":");
    w.number(static_cast<long long>(t.campaign_time_s));
    w.raw(",\"sim_time_s\":");
    w.number(t.sim_time_s);
    w.raw(",\"paused\":");
    w.raw(t.paused ? "1" : "0");
    w.raw(",\"next_tasking_sec\":");
    w.number(t.next_tasking_sec);
    w.raw(",\"time_scale\":");
    w.number(t.time_scale);
    w.put('}');
}

// --- stats --------------------------------------------------------------
//
// The `stats` query: the engine's one-frame counters (the Campaign
// window's vocabulary), verbatim. Groups arrive in the engine's own
// order — tasking, ledger, ground, tiers — so the wire reads the same
// story the as-built docs tell.

struct StatsView {
    int cycles{0};
    int next_tasking_sec{0};
    int intents{0};
    int routes_built{0};
    int routes_failed{0};
    int route_waypoints{0};
    int drawn_aircraft{0};
    int air_losses{0};
    int reinforce_fires{0};
    int reinforced{0};
    int synthetic_spawned{0};
    int live_aircraft{0};
    int airborne{0};
    double sim_time_s{0.0};
    int retired{0};
    int packages{0};
    int escorts{0};
    int recovered{0};
    int armed_aircraft{0};
    int armed_fighters{0};
    int armed_defensive{0};
    int aa_kills{0};
    int ground_updates{0};
    int ground_battalions{0};
    int ground_mobile{0};
    int ground_losses{0};
    int ground_losses_air{0};
    int ground_destroyed{0};
    int ground_captures{0};
    int ground_engaged{0};
    int ground_front_columns{0};
    int agg_updates{0};
    int agg_flights{0};
    int agg_live{0};
    int agg_arrived{0};
    int agg_destroyed{0};
    int tier_deaggs{0};
    int tier_reaggs{0};
    int combat_deaggs{0};
    int synthetic_aggregates{0};
    int agg_contacts{0};
    int deferred_releases{0};
};

namespace detail {

inline void int_key(f4::json::Writer& w, const char* key, int v) {
    w.put('"');
    w.raw(key);
    w.raw("\":");
    w.number(v);
    w.put(',');
}

} // namespace detail

inline void encode(f4::json::Writer& w, const StatsView& s) {
    w.put('{');
    detail::int_key(w, "cycles", s.cycles);
    detail::int_key(w, "next_tasking_sec", s.next_tasking_sec);
    detail::int_key(w, "intents", s.intents);
    detail::int_key(w, "routes_built", s.routes_built);
    detail::int_key(w, "routes_failed", s.routes_failed);
    detail::int_key(w, "route_waypoints", s.route_waypoints);
    detail::int_key(w, "drawn_aircraft", s.drawn_aircraft);
    detail::int_key(w, "air_losses", s.air_losses);
    detail::int_key(w, "reinforce_fires", s.reinforce_fires);
    detail::int_key(w, "reinforced", s.reinforced);
    detail::int_key(w, "synthetic_spawned", s.synthetic_spawned);
    detail::int_key(w, "live_aircraft", s.live_aircraft);
    detail::int_key(w, "airborne", s.airborne);
    w.raw("\"sim_time_s\":");
    w.number(s.sim_time_s);
    w.put(',');
    detail::int_key(w, "retired", s.retired);
    detail::int_key(w, "packages", s.packages);
    detail::int_key(w, "escorts", s.escorts);
    detail::int_key(w, "recovered", s.recovered);
    detail::int_key(w, "armed_aircraft", s.armed_aircraft);
    detail::int_key(w, "armed_fighters", s.armed_fighters);
    detail::int_key(w, "armed_defensive", s.armed_defensive);
    detail::int_key(w, "aa_kills", s.aa_kills);
    detail::int_key(w, "ground_updates", s.ground_updates);
    detail::int_key(w, "ground_battalions", s.ground_battalions);
    detail::int_key(w, "ground_mobile", s.ground_mobile);
    detail::int_key(w, "ground_losses", s.ground_losses);
    detail::int_key(w, "ground_losses_air", s.ground_losses_air);
    detail::int_key(w, "ground_destroyed", s.ground_destroyed);
    detail::int_key(w, "ground_captures", s.ground_captures);
    detail::int_key(w, "ground_engaged", s.ground_engaged);
    detail::int_key(w, "ground_front_columns", s.ground_front_columns);
    detail::int_key(w, "agg_updates", s.agg_updates);
    detail::int_key(w, "agg_flights", s.agg_flights);
    detail::int_key(w, "agg_live", s.agg_live);
    detail::int_key(w, "agg_arrived", s.agg_arrived);
    detail::int_key(w, "agg_destroyed", s.agg_destroyed);
    detail::int_key(w, "tier_deaggs", s.tier_deaggs);
    detail::int_key(w, "tier_reaggs", s.tier_reaggs);
    detail::int_key(w, "combat_deaggs", s.combat_deaggs);
    detail::int_key(w, "synthetic_aggregates", s.synthetic_aggregates);
    detail::int_key(w, "agg_contacts", s.agg_contacts);
    // last key — no trailing comma
    w.raw("\"deferred_releases\":");
    w.number(s.deferred_releases);
    w.put('}');
}

// --- flights (the FID tier view) -----------------------------------------
//
// The `flights` query: one row per campaign flight, the aggregate picture
// by default (plan §4 — a host that never focuses sees exactly this).
// `live` marks the flights currently deaggregated (Tier-B).

struct FlightView {
    std::uint32_t vu{0};
    std::uint8_t team{0};
    std::uint8_t mission{0};
    int aircraft_count{0};
    double x_grid{0.0};
    double y_grid{0.0};
    float altitude_ft{0.0f};
    std::int32_t fuel_burnt{0};
    bool live{false};
    bool arrived{false};
    bool destroyed{false};
    std::int32_t to_depart{-1};
    std::int32_t to_mission_over{-1};
    /// CAMP-CMD-2 (v1.1 additive, the row's tail): the flight was
    /// aborted before launch — a scrubbed sortie that never flies (the
    /// books closed; the row stays for the war's history).
    bool aborted{false};
};

inline void encode_flight(f4::json::Writer& w, const FlightView& f) {
    w.raw("{\"vu\":");
    w.number(static_cast<std::uint64_t>(f.vu));
    w.raw(",\"team\":");
    w.number(f.team);
    w.raw(",\"mission\":");
    w.number(f.mission);
    w.raw(",\"aircraft_count\":");
    w.number(f.aircraft_count);
    w.raw(",\"x_grid\":");
    w.number(f.x_grid);
    w.raw(",\"y_grid\":");
    w.number(f.y_grid);
    w.raw(",\"altitude_ft\":");
    w.number(static_cast<double>(f.altitude_ft));
    w.raw(",\"fuel_burnt\":");
    w.number(static_cast<long long>(f.fuel_burnt));
    w.raw(",\"live\":");
    w.raw(f.live ? "1" : "0");
    w.raw(",\"arrived\":");
    w.raw(f.arrived ? "1" : "0");
    w.raw(",\"destroyed\":");
    w.raw(f.destroyed ? "1" : "0");
    w.raw(",\"to_depart\":");
    w.number(static_cast<long long>(f.to_depart));
    w.raw(",\"to_mission_over\":");
    w.number(static_cast<long long>(f.to_mission_over));
    // CAMP-CMD-2 — the additive tail (the DTO rule: new fields ride at
    // the END, always present — the canonical form never elides).
    w.raw(",\"aborted\":");
    w.raw(f.aborted ? "1" : "0");
    w.put('}');
}

inline void encode(f4::json::Writer& w, const std::vector<FlightView>& flights) {
    w.put('[');
    for (std::size_t i = 0; i < flights.size(); ++i) {
        if (i != 0) w.put(',');
        encode_flight(w, flights[i]);
    }
    w.put(']');
}

// --- tasking (the ATO) ----------------------------------------------------
//
// The `tasking` query: the generated missions, oldest last (the engine's
// intents() order — the Campaign window's table).
//
// CAMP-HOST-3 (additive, at the END per the header rule): route_waypoints
// — the intent's C3 route leg count (the window's "wps" column). Saved-
// flight intents (emit_flight_intents) carry 0 — their routes live in
// the save's own waypoint list, not the generation plan.

struct IntentView {
    std::int64_t issued_time{0};
    std::int64_t time_on_target{0};
    std::uint8_t team{0};
    std::string team_name;
    std::uint8_t mission_byte{0};
    std::string mission_name;
    int aircraft_count{0};
    std::uint32_t squadron_id{0};
    std::string squadron_name;
    std::uint32_t package_id{0};
    std::uint32_t flight_id{0};
    std::uint32_t target_objective_id{0};
    bool synthetic{false};
    int route_waypoints{0};
    std::uint8_t flight_role{0};
    // CAMP-DOM-4 (additive, at the END per the header rule): takeoff —
    // the flight's SCHEDULED takeoff slot (the phase-7 snap's output,
    // campaign-relative seconds; the engine's own RELATIVE axis, like
    // the other times here). 0 = never slotted (the legacy ladder's
    // intents, the save's own flights, a base-less filing).
    std::int64_t takeoff{0};
};

inline void encode_intent(f4::json::Writer& w, const IntentView& m) {
    w.raw("{\"issued_time\":");
    w.number(static_cast<long long>(m.issued_time));
    w.raw(",\"time_on_target\":");
    w.number(static_cast<long long>(m.time_on_target));
    w.raw(",\"team\":");
    w.number(m.team);
    w.raw(",\"team_name\":\"");
    w.put(f4::json::escape_string(m.team_name));
    w.raw("\",\"mission_byte\":");
    w.number(m.mission_byte);
    w.raw(",\"mission_name\":\"");
    w.put(f4::json::escape_string(m.mission_name));
    w.raw("\",\"aircraft_count\":");
    w.number(m.aircraft_count);
    w.raw(",\"squadron_id\":");
    w.number(static_cast<std::uint64_t>(m.squadron_id));
    w.raw(",\"squadron_name\":\"");
    w.put(f4::json::escape_string(m.squadron_name));
    w.raw("\",\"package_id\":");
    w.number(static_cast<std::uint64_t>(m.package_id));
    w.raw(",\"flight_id\":");
    w.number(static_cast<std::uint64_t>(m.flight_id));
    w.raw(",\"target_objective_id\":");
    w.number(static_cast<std::uint64_t>(m.target_objective_id));
    w.raw(",\"synthetic\":");
    w.raw(m.synthetic ? "1" : "0");
    w.raw(",\"route_waypoints\":");
    w.number(m.route_waypoints);
    w.raw(",\"flight_role\":");
    w.number(m.flight_role);
    w.raw(",\"takeoff\":");
    w.number(static_cast<long long>(m.takeoff));
    w.put('}');
}

inline void encode(f4::json::Writer& w, const std::vector<IntentView>& intents) {
    w.put('[');
    for (std::size_t i = 0; i < intents.size(); ++i) {
        if (i != 0) w.put(',');
        encode_intent(w, intents[i]);
    }
    w.put(']');
}

inline void encode(f4::json::Writer& w, const IntentView& m) {
    encode_intent(w, m);
}

// --- objectives (the theater's static + damage state) ---------------------
//
// The `objectives` query: the WorldState's objectives — ownership,
// priority, the logistics numbers (supply/fuel/losses — the DOM-2 seed),
// and the per-feature damage bitmap (2 bits per feature, raw).

struct ObjectiveView {
    std::uint32_t id_creator{0};
    std::uint32_t id_num{0};
    std::int16_t type{0};
    std::uint8_t objective_type{0};
    std::uint16_t entity_type{0};
    std::int16_t x{0};
    std::int16_t y{0};
    float z{0.0f};
    std::uint8_t owner{0};
    std::uint8_t first_owner{0};
    std::uint8_t priority{0};
    std::int16_t nameid{0};
    std::uint32_t obj_flags{0};
    std::uint32_t parent_id{0};
    std::uint8_t supply{0};
    std::uint8_t fuel{0};
    std::uint8_t losses{0};
    std::int32_t last_repair{0};
    bool has_radar{false};
    float radar_range_km{0.0f};
    std::vector<std::uint8_t> fstatus;
};

inline void encode_objective(f4::json::Writer& w, const ObjectiveView& o) {
    w.raw("{\"id_creator\":");
    w.number(static_cast<std::uint64_t>(o.id_creator));
    w.raw(",\"id_num\":");
    w.number(static_cast<std::uint64_t>(o.id_num));
    w.raw(",\"type\":");
    w.number(o.type);
    w.raw(",\"objective_type\":");
    w.number(o.objective_type);
    w.raw(",\"entity_type\":");
    w.number(o.entity_type);
    w.raw(",\"x\":");
    w.number(o.x);
    w.raw(",\"y\":");
    w.number(o.y);
    w.raw(",\"z\":");
    w.number(static_cast<double>(o.z));
    w.raw(",\"owner\":");
    w.number(o.owner);
    w.raw(",\"first_owner\":");
    w.number(o.first_owner);
    w.raw(",\"priority\":");
    w.number(o.priority);
    w.raw(",\"nameid\":");
    w.number(o.nameid);
    w.raw(",\"obj_flags\":");
    w.number(static_cast<std::uint64_t>(o.obj_flags));
    w.raw(",\"parent_id\":");
    w.number(static_cast<std::uint64_t>(o.parent_id));
    w.raw(",\"supply\":");
    w.number(o.supply);
    w.raw(",\"fuel\":");
    w.number(o.fuel);
    w.raw(",\"losses\":");
    w.number(o.losses);
    w.raw(",\"last_repair\":");
    w.number(static_cast<long long>(o.last_repair));
    w.raw(",\"has_radar\":");
    w.raw(o.has_radar ? "1" : "0");
    w.raw(",\"radar_range_km\":");
    w.number(static_cast<double>(o.radar_range_km));
    w.raw(",\"fstatus\":[");
    for (std::size_t i = 0; i < o.fstatus.size(); ++i) {
        if (i != 0) w.put(',');
        w.number(o.fstatus[i]);
    }
    w.raw("]}");
}

inline void encode(f4::json::Writer& w, const std::vector<ObjectiveView>& objectives) {
    w.put('[');
    for (std::size_t i = 0; i < objectives.size(); ++i) {
        if (i != 0) w.put(',');
        encode_objective(w, objectives[i]);
    }
    w.put(']');
}

inline void encode(f4::json::Writer& w, const ObjectiveView& o) {
    encode_objective(w, o);
}

// --- threat (the C3 SAM-ring picture, CAMP-HOST-3) ------------------------
//
// The `threat` query (plan §3.2 named it v1.1-additive; it lands with the
// viewer's HOST-3 move): the route-builder's threat map viewed from ONE
// team — the half of each cell's air-defense density that threatens
// `viewer_team`. Cell (cx,cy) covers the grid square
// [cx*cell_grid,(cx+1)*cell_grid) × same for y (grid = 1024 ft — the
// map's own kThreatMapRatio is ECHOED as cell_grid so a client never
// hardcodes the constant).
//
// Bands stay SEPARATE on the wire (low = low-alt threats, high = high-
// alt) even though today's overlay paints their sum — splitting them is
// a display decision, merging them here would be a lossy one.

struct ThreatView {
    std::uint8_t viewer_team{0};
    int cell_grid{0};
    int cells_x{0};
    int cells_y{0};
    std::vector<int> low;   ///< cells_y * cells_x densities, row-major
    std::vector<int> high;  ///< same layout, high-alt band
};

inline void encode(f4::json::Writer& w, const ThreatView& t) {
    w.raw("{\"viewer_team\":");
    w.number(t.viewer_team);
    w.raw(",\"cell_grid\":");
    w.number(t.cell_grid);
    w.raw(",\"cells_x\":");
    w.number(t.cells_x);
    w.raw(",\"cells_y\":");
    w.number(t.cells_y);
    w.raw(",\"low\":[");
    for (std::size_t i = 0; i < t.low.size(); ++i) {
        if (i != 0) w.put(',');
        w.number(t.low[i]);
    }
    w.raw("],\"high\":[");
    for (std::size_t i = 0; i < t.high.size(); ++i) {
        if (i != 0) w.put(',');
        w.number(t.high[i]);
    }
    w.raw("]}");
}

// --- verdict (the war's outcome picture, CAMP-DOM-1) ---------------------
//
// The `verdict` query (plan §3.2's DOM-1 row): the books' projection —
// who is winning, derived from state the engine already keeps, with no
// new engine truth behind it. Per team: the territorial census (the
// LIVE owner of every objective against the session's OPENING owner,
// weighted by the objective's own priority byte) plus the ledger's own
// run books (captures, air losses, ground losses, battalions
// destroyed, the aircraft pool's existence view).
//
// `band` is territorial only — stalemate / advantage / decisive, the
// leader's net gained priority deciding (>= 100 = decisive; a tie for
// the lead is no lead). The attrition books ride the rows; weighing
// them is a display decision. `threshold` is the .cmp header's own
// te_victory_points (0 = the save sets none) — reported as context.
// `t` is the engine's RELATIVE clock (the events' axis — the books the
// verdict sums are run-scoped); a host that wants the war's absolute
// time adds the save epoch it already holds.

struct VerdictTeamRow {
    int slot{0};
    std::string name;
    int owned{0};                 ///< objectives held now
    int gained{0};                ///< held now, not at opening
    int lost{0};                  ///< held at opening, not now
    int gained_value{0};          ///< Σ priority(gained)
    int lost_value{0};            ///< Σ priority(lost)
    int swing{0};                 ///< gained_value − lost_value
    int captures{0};              ///< ledger: objectives taken this run
    int air_losses{0};            ///< ledger: aircraft lost this run
    int ground_losses{0};         ///< ledger: vehicles lost this run
    int battalions_destroyed{0};  ///< ledger: battalions lost this run
    int aircraft_remaining{0};    ///< ledger: the pool's existence view
};

struct VerdictView {
    std::int64_t t{0};            ///< engine-relative seconds
    int threshold{0};             ///< the save's te_victory_points
    std::string band;             ///< stalemate | advantage | decisive
    int leader{-1};               ///< the leading team's slot, -1 none
    int leader_swing{0};          ///< the leader's swing (0 when none)
    std::vector<VerdictTeamRow> teams;  ///< slot order
};

inline void encode_verdict_row(f4::json::Writer& w,
                               const VerdictTeamRow& r) {
    w.raw("{\"slot\":");
    w.number(r.slot);
    w.raw(",\"name\":\"");
    w.put(f4::json::escape_string(r.name));
    w.raw("\",\"owned\":");
    w.number(r.owned);
    w.raw(",\"gained\":");
    w.number(r.gained);
    w.raw(",\"lost\":");
    w.number(r.lost);
    w.raw(",\"gained_value\":");
    w.number(r.gained_value);
    w.raw(",\"lost_value\":");
    w.number(r.lost_value);
    w.raw(",\"swing\":");
    w.number(r.swing);
    w.raw(",\"captures\":");
    w.number(r.captures);
    w.raw(",\"air_losses\":");
    w.number(r.air_losses);
    w.raw(",\"ground_losses\":");
    w.number(r.ground_losses);
    w.raw(",\"battalions_destroyed\":");
    w.number(r.battalions_destroyed);
    w.raw(",\"aircraft_remaining\":");
    w.number(r.aircraft_remaining);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const VerdictView& v) {
    w.raw("{\"t\":");
    w.number(static_cast<long long>(v.t));
    w.raw(",\"threshold\":");
    w.number(v.threshold);
    w.raw(",\"band\":\"");
    w.put(f4::json::escape_string(v.band));
    w.raw("\",\"leader\":");
    w.number(v.leader);
    w.raw(",\"leader_swing\":");
    w.number(v.leader_swing);
    w.raw(",\"teams\":[");
    for (std::size_t i = 0; i < v.teams.size(); ++i) {
        if (i != 0) w.put(',');
        encode_verdict_row(w, v.teams[i]);
    }
    w.raw("]}");
}

// --- the squadrons view (CAMP-DOM-3 — the personnel face) ----------------
//
// One row per squadron in WIRE ORDER (the same order the books and the
// write-back walk). Identity + the tasking availability + the pilot
// roster's overlay face: the wire's own counts with the run's deltas
// applied (dead = the ledger's crewed-flight losses; missions = the
// wire's history + the run's credited sorties). ratings is the
// squadron's per-role effectiveness table — the LIVE view when the
// rating-decay arm ran, else the wire's own rating[16] (zero when the
// save carries none; the array is always 16 wide).
struct SquadronView {
    std::uint32_t vu{0};          ///< VU_ID.num (the campaign key)
    std::uint8_t team{0};         ///< owner slot
    std::string name;             ///< display name
    std::uint32_t airbase_id{0};  ///< home airbase VU_ID.num (0 = none)
    std::uint8_t specialty{0};    ///< wire byte (0/1/2: none/AA/AG)
    int available{0};             ///< tasking availability (the live pool)
    int pilots_total{0};          ///< the wire roster's size
    int pilots_available{0};      ///< status 0 and not dead/out this run
    int pilots_dead{0};           ///< lost this run (crewed-flight deaths)
    int missions_flown{0};        ///< the wire aggregate (unchanged this run)
    std::array<std::uint8_t, 16> ratings{};  ///< the per-role table
};

inline void encode(f4::json::Writer& w, const SquadronView& s) {
    w.raw("{\"vu\":");
    w.number(static_cast<std::uint64_t>(s.vu));
    w.raw(",\"team\":");
    w.number(s.team);
    w.raw(",\"name\":\"");
    w.put(f4::json::escape_string(s.name));
    w.raw("\",\"airbase_id\":");
    w.number(static_cast<std::uint64_t>(s.airbase_id));
    w.raw(",\"specialty\":");
    w.number(s.specialty);
    w.raw(",\"available\":");
    w.number(s.available);
    w.raw(",\"pilots_total\":");
    w.number(s.pilots_total);
    w.raw(",\"pilots_available\":");
    w.number(s.pilots_available);
    w.raw(",\"pilots_dead\":");
    w.number(s.pilots_dead);
    w.raw(",\"missions_flown\":");
    w.number(s.missions_flown);
    w.raw(",\"ratings\":[");
    for (std::size_t i = 0; i < s.ratings.size(); ++i) {
        if (i) w.raw(",");
        w.number(s.ratings[i]);
    }
    w.raw("]}");
}

inline void encode(f4::json::Writer& w,
                   const std::vector<SquadronView>& squadrons) {
    w.put('[');
    for (std::size_t i = 0; i < squadrons.size(); ++i) {
        if (i != 0) w.put(',');
        encode(w, squadrons[i]);
    }
    w.put(']');
}

// --- the airfields view (CAMP-DOM-4 — the scheduling face) --------------
//
// One row per airbase the tasking pipeline holds a schedule book for,
// in WIRE order (the decode walk; lazily-created bases join at the
// tail). The grid is the 32-block takeoff bitmask as the ATM holds it
// RIGHT NOW — `schedule` is its 32 bytes as 64 lowercase hex chars
// (block 0's byte first), `epoch_min` the campaign-minute block 0
// currently maps to (0 = the campaign-start anchor; nonzero = the
// scheduling arm slid the grid), `booked` the set-bit count. The
// denial books are the base's own: `denied` counts pick-gate skips,
// `overflowed` horizon refusals. Teamless rows (the grid is the
// BASE's truth — every side's flights deconflict against it).
struct AirfieldView {
    std::uint32_t vu{0};          ///< VU_ID.num (the campaign key)
    std::int64_t epoch_min{0};    ///< block 0's campaign-minute
    std::string schedule;         ///< the 32 block bytes, 64 hex chars
    int booked{0};                ///< set bits (the booked slots)
    int denied{0};                ///< pick-gate skips on this base
    int overflowed{0};            ///< horizon refusals on this base
};

inline void encode_airfield(f4::json::Writer& w, const AirfieldView& a) {
    w.raw("{\"vu\":");
    w.number(static_cast<std::uint64_t>(a.vu));
    w.raw(",\"epoch_min\":");
    w.number(static_cast<long long>(a.epoch_min));
    w.raw(",\"schedule\":\"");
    w.put(a.schedule);
    w.raw("\",\"booked\":");
    w.number(a.booked);
    w.raw(",\"denied\":");
    w.number(a.denied);
    w.raw(",\"overflowed\":");
    w.number(a.overflowed);
    w.put('}');
}

inline void encode(f4::json::Writer& w, const AirfieldView& a) {
    encode_airfield(w, a);
}

inline void encode(f4::json::Writer& w,
                   const std::vector<AirfieldView>& airfields) {
    w.put('[');
    for (std::size_t i = 0; i < airfields.size(); ++i) {
        if (i != 0) w.put(',');
        encode_airfield(w, airfields[i]);
    }
    w.put(']');
}

} // namespace f4::campaign::api

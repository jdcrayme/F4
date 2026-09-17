// f4-world-viewer/src/campaign_queries.hpp
//
// The viewer's CONTRACT-PLANE data access (CAMP-HOST-3).
//
// Everything the Campaign Session window shows and does rides the
// f4-campaign-api query surface. These helpers call one query, walk the
// DTO's byte-stable JSON with f4::json::Reader, and hand the windows
// plain viewer-side structs — no engine types, no f4/simulation includes
// (the fetch layer compiles without the engine in the link; that is the
// two-plane proof in one build rule).
//
// Parsing discipline: the DTO encoders are byte-stable with a fixed key
// order, but the walks here are ORDER-INDEPENDENT and additive-tolerant
// (unknown keys skip_value() — the contract's additive-field rule means
// new keys appear at the END without notice; a client that hard-fails
// on them ossifies the wire). A malformed payload returns !ok / an
// empty set — never a partial garbage row.
//
// Snapshot cadence: "the numbers refresh once per advance(), never per
// draw" — the caller gates fetch_snapshot() on the runner's
// step_serial() and the windows read the cached structs.
#pragma once

#include <f4/campaign/api/session.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::viewer {

// --- the time query ------------------------------------------------------

struct SessionTime {
    double tick_sec{0.0};
    std::int64_t campaign_time_s{0};
    double sim_time_s{0.0};
    bool paused{false};
    int next_tasking_sec{0};
    double time_scale{1.0};
    bool ok{false};
};

/// The session clock + pacing echo. Called ONCE at session adopt (while
/// the session is paused at zero ticks) the returned campaign_time_s is
/// the SAVE'S EPOCH — the absolute base every relative TOT adds onto.
[[nodiscard]] SessionTime fetch_time(f4::campaign::api::ICampaignSession& s);

// --- the stats query (the war-status block's vocabulary) -----------------

struct SessionStats {
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
    bool ok{false};
};

[[nodiscard]] SessionStats fetch_stats(f4::campaign::api::ICampaignSession& s);

// --- the flights query (the flights table + the aggregate layer) ---------

struct FlightRow {
    std::uint32_t vu{0};
    int team{0};
    int mission{0};
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
};

[[nodiscard]] std::vector<FlightRow> fetch_flights(
    f4::campaign::api::ICampaignSession& s);

// --- the tasking query (the generated-missions table) --------------------

struct IntentRow {
    std::int64_t issued_time{0};
    std::int64_t time_on_target{0};
    int team{0};
    std::string team_name;
    int mission_byte{0};
    std::string mission_name;
    int aircraft_count{0};
    std::uint32_t squadron_id{0};
    std::string squadron_name;
    std::uint32_t package_id{0};
    std::uint32_t flight_id{0};
    std::uint32_t target_objective_id{0};
    bool synthetic{false};
    int route_waypoints{0};
    int flight_role{0};
};

[[nodiscard]] std::vector<IntentRow> fetch_tasking(
    f4::campaign::api::ICampaignSession& s);

// --- the threat query (the C3 SAM-ring overlay) ---------------------------

struct ThreatGrid {
    int viewer_team{-1};
    int cell_grid{0};
    int cells_x{0};
    int cells_y{0};
    std::vector<int> low;   ///< row-major, cells_y * cells_x
    std::vector<int> high;  ///< row-major, same layout
    bool ok{false};

    /// The cell's total density against the viewer team (the bands
    /// summed — the overlay's own read; they stay separate on the wire).
    [[nodiscard]] int density(int cx, int cy) const noexcept {
        if (cx < 0 || cy < 0 || cx >= cells_x || cy >= cells_y) return 0;
        const std::size_t i =
            static_cast<std::size_t>(cy) * static_cast<std::size_t>(cells_x) +
            static_cast<std::size_t>(cx);
        const int lo = i < low.size() ? low[i] : 0;
        const int hi = i < high.size() ? high[i] : 0;
        return lo + hi;
    }
};

[[nodiscard]] ThreatGrid fetch_threat(f4::campaign::api::ICampaignSession& s);

// --- the books query (the ledger's own bytes) -----------------------------

/// Returns the EXACT ledger JSON bytes (the wire wraps them as one
/// escaped string; this unwraps — the identity fingerprint hashes the
/// same bytes).
[[nodiscard]] std::string fetch_books_ledger(
    f4::campaign::api::ICampaignSession& s);

// --- the one-per-advance snapshot -----------------------------------------

struct SessionSnapshot {
    SessionTime time;
    SessionStats stats;
    std::vector<FlightRow> flights;
    std::vector<IntentRow> tasking;
    ThreatGrid threat;   // filled only when with_threat
};

/// One round of the session's display state. `with_threat` gates the
/// (theater-sized) threat walk — off for windows that never paint it.
[[nodiscard]] SessionSnapshot fetch_snapshot(
    f4::campaign::api::ICampaignSession& s, bool with_threat);

} // namespace f4::viewer

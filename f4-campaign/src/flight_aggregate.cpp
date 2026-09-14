// f4-campaign/src/flight_aggregate.cpp
//
// FlightAggregateEngine — see flight_aggregate.hpp for the design.
// The implementation rules this file pins:
//   * Determinism: no RNG, no wall-clock; wire order everywhere; the
//     same sources + tick sequence produce the same state.
//   * The engine advances only whole update ticks (the GroundWar
//     accumulator shape), so one big tick == N small ones (the C2 pin).
//   * TIME mode reproduces the save's own move schedule; SPEED mode is
//     the documented divergence for routes without times.

#include <f4/campaign/flight_aggregate.hpp>

#include <algorithm>
#include <cmath>

namespace f4::campaign {

namespace {

/// Linear interpolation factor of `now` between [a, b] (b > a), clamped.
double frac(std::int64_t now, std::int64_t a, std::int64_t b) {
    if (b <= a) return 0.0;
    const double t = static_cast<double>(now - a) /
                     static_cast<double>(b - a);
    return std::clamp(t, 0.0, 1.0);
}

} // namespace

// ============================================================================
// construction — the snapshot
// ============================================================================

FlightAggregateEngine::FlightAggregateEngine(
        const f4::world::ICampaignSource& campaign,
        const f4::world::IUnitCoreSource& units,
        const f4::world::IFlightSource& flights,
        const FlightAggregateConfig& cfg, const FlightAggregateFilter& filter)
    : cfg_(cfg) {
    epoch_ = campaign.current_time();
    if (cfg_.update_sec <= 0) cfg_.update_sec = 60;   // loud defaults stay loud
    if (cfg_.cruise_grid_per_min <= 0.0) cfg_.cruise_grid_per_min = 12.0;

    int taken = 0;
    const bool cap_active = filter.max_flights >= 0;
    for (int i = 0; i < units.unit_count(); ++i) {
        if (units.unit_class(i) != f4::entities::UnitClass::Flight) continue;
        // The spawn filter's own semantics (team/mission -1 = all).
        if (filter.team >= 0 &&
            static_cast<int>(units.owner(i)) != filter.team) {
            continue;
        }
        if (filter.mission >= 0 &&
            static_cast<int>(flights.mission(i)) != filter.mission) {
            continue;
        }
        if (cap_active && taken >= filter.max_flights) break;

        FlightAggregateState f;
        f.vu = units.id_num(i);
        f.team = units.owner(i);
        f.mission = flights.mission(i);
        f.time_on_target = flights.time_on_target(i);
        f.mission_over_time = flights.mission_over_time(i);
        f.fx = static_cast<double>(units.x(i));
        f.fy = static_cast<double>(units.y(i));
        f.altitude_ft = flights.flight_altitude(i);
        f.fuel_burnt = flights.fuel_burnt(i);
        f.last_move = static_cast<std::int32_t>(
            std::min<std::int64_t>(epoch_, 2147483647));

        // Aircraft count: the flight's vehicle groups (live_count from
        // the roster when the save carries one, else the nominal count).
        int count = 0;
        if (units.has_vehicle_groups(i)) {
            for (const auto& g : units.vehicle_groups(i)) {
                count += g.live_count > 0 ? g.live_count : g.count;
            }
        }
        f.aircraft_count = count > 0 ? count : 1;

        // The route: a copy of the save's waypoints (the engine owns
        // its cursor; the world's WaypointPlanComponent stays pristine
        // until the session's entity mirror touches it).
        routes_.emplace_back();
        if (units.has_waypoints(i)) {
            routes_.back() = units.waypoints(i);
        }
        f.has_route = !routes_.back().empty();

        // Mode: TIME when any arrival time is usable, SPEED otherwise.
        bool timed = false;
        for (const auto& wp : routes_.back()) {
            if (wp.arrive > 0) {
                timed = true;
                break;
            }
        }
        time_mode_.push_back(timed);

        flights_.push_back(f);
        ++taken;
    }

    refresh_stats();
}

// ============================================================================
// tick — the update loop (the GroundWar accumulator shape)
// ============================================================================

void FlightAggregateEngine::tick(CampaignTime delta_sec) {
    if (delta_sec <= 0) return;
    clock_ += delta_sec;
    while (clock_ >= next_update_ + cfg_.update_sec) {
        next_update_ += cfg_.update_sec;

        const std::int64_t now_abs = epoch_ + next_update_;
        for (std::size_t i = 0; i < flights_.size(); ++i) {
            FlightAggregateState& f = flights_[i];
            if (f.suspended || f.arrived || f.destroyed) continue;
            advance_flight_(f, i, routes_[i], now_abs);
        }

        ++stats_.updates;
        refresh_stats();
    }
}

void FlightAggregateEngine::refresh_stats() {
    stats_.flights = static_cast<int>(flights_.size());
    stats_.aggregate = 0;
    stats_.suspended = 0;
    stats_.arrived = 0;
    stats_.destroyed = 0;
    for (const auto& f : flights_) {
        if (f.destroyed) ++stats_.destroyed;
        else if (f.suspended) ++stats_.suspended;
        else if (f.arrived) ++stats_.arrived;
        else ++stats_.aggregate;
    }
}

// ============================================================================
// advance_flight_ — one flight, one update
// ============================================================================

void FlightAggregateEngine::advance_flight_(
        FlightAggregateState& f, std::size_t index,
        std::vector<f4::entities::WaypointState>& route,
        std::int64_t now_abs) {
    if (!f.has_route || route.empty()) return;   // nothing to fly

    // The takeoff gate: a flight holds at its save position until its
    // first waypoint's depart (both modes — the wire's own schedule).
    const std::int64_t first_depart = route.front().depart;
    if (first_depart > 0 && now_abs < first_depart) return;

    bool moved = false;
    if (time_mode_[index]) {
        // TIME mode — interpolate on the wire's own schedule.
        // Legs: wp[i-1] (depart) → wp[i] (arrive); hold at wp[i]
        // between its arrive and its depart. Walk the legs in order;
        // the last leg whose arrival has passed owns the position.
        for (std::size_t i = 1; i < route.size(); ++i) {
            const auto& from = route[i - 1];
            const auto& to = route[i];
            if (to.arrive <= 0) continue;   // leg without a schedule
            if (now_abs >= to.arrive) {
                // Past this leg's arrival: sit at (or beyond) the
                // waypoint — later legs override in order.
                f.fx = static_cast<double>(to.x);
                f.fy = static_cast<double>(to.y);
                f.altitude_ft = static_cast<float>(to.z);
                f.wp_index = i;
                moved = true;
                if (i + 1 == route.size()) {
                    f.arrived = true;   // last waypoint reached
                }
                continue;
            }
            if (now_abs > from.depart) {
                // Mid-leg: linear interpolation on the wire's times.
                const double t = frac(now_abs, from.depart, to.arrive);
                f.fx = static_cast<double>(from.x) +
                       t * (static_cast<double>(to.x) - from.x);
                f.fy = static_cast<double>(from.y) +
                       t * (static_cast<double>(to.y) - from.y);
                f.altitude_ft = static_cast<float>(
                    static_cast<double>(from.z) +
                    t * (static_cast<double>(to.z) - from.z));
                f.wp_index = i;
                moved = true;
                break;
            }
            break;   // before this leg's departure: holding at wp[i-1]
        }
    } else {
        // SPEED mode — walk the legs at the cruise speed toward the
        // current cursor waypoint. Start position → wp[0] → wp[1] → …
        // The cursor names the waypoint being flown TOWARD; altitude
        // lerps toward the target's z by the same distance fraction.
        double remaining = cfg_.cruise_grid_per_min *
                           (static_cast<double>(
                                std::min<CampaignTime>(cfg_.update_sec, 3600)) /
                            60.0);
        while (remaining > 0.0 && f.wp_index < route.size() &&
               !f.arrived) {
            const auto& target = route[f.wp_index];
            const double dx = static_cast<double>(target.x) - f.fx;
            const double dy = static_cast<double>(target.y) - f.fy;
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= remaining) {
                // Waypoint reached inside this update: snap, spend the
                // distance, advance the cursor.
                f.fx = static_cast<double>(target.x);
                f.fy = static_cast<double>(target.y);
                f.altitude_ft = static_cast<float>(target.z);
                remaining -= dist;
                moved = true;
                if (f.wp_index + 1 < route.size()) {
                    ++f.wp_index;
                } else {
                    f.arrived = true;   // last waypoint reached
                }
            } else {
                // Advance the fraction of the step toward the target.
                const double k = remaining / dist;
                f.fx += k * dx;
                f.fy += k * dy;
                f.altitude_ft = static_cast<float>(
                    static_cast<double>(f.altitude_ft) +
                    k * (static_cast<double>(target.z) -
                         static_cast<double>(f.altitude_ft)));
                moved = true;
                remaining = 0.0;
            }
        }
    }

    // Fuel accrual: cruise burn per aircraft per minute while the
    // flight is airborne-progressing (departed, not arrived).
    if (moved && !f.arrived && cfg_.fuel_burn_lbs_per_min > 0) {
        const double minutes =
            static_cast<double>(cfg_.update_sec) / 60.0;
        f.fuel_burnt += static_cast<std::int32_t>(
            std::llround(static_cast<double>(cfg_.fuel_burn_lbs_per_min) *
                         minutes));
        if (f.fuel_burnt < 0) f.fuel_burnt = 0;   // clamp absurd saves
    }

    if (moved) {
        f.last_move = static_cast<std::int32_t>(
            std::min<std::int64_t>(now_abs, 2147483647));
        f.dirty = true;
    }
}

// ============================================================================
// tier cooperation — suspend / fold-back
// ============================================================================

void FlightAggregateEngine::set_suspended(std::uint32_t vu, bool suspended) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    flights_[idx].suspended = suspended;
    refresh_stats();
}

void FlightAggregateEngine::reaggregate(std::uint32_t vu, double fx,
                                        double fy, float altitude_ft,
                                        std::int32_t fuel_burnt) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    FlightAggregateState& f = flights_[idx];
    f.fx = std::clamp(fx, -32768.0, 32767.0);
    f.fy = std::clamp(fy, -32768.0, 32767.0);
    f.altitude_ft = altitude_ft;
    // Monotonic fuel: the sim can only have burned more (the fold-back
    // never resurrects fuel the aggregate already booked).
    f.fuel_burnt = std::max(f.fuel_burnt, fuel_burnt);
    reset_cursor_(f, routes_[idx], epoch_ + clock_);
    f.suspended = false;
    f.dirty = true;
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(epoch_ + clock_, 2147483647));
    refresh_stats();
}

void FlightAggregateEngine::mark_destroyed(std::uint32_t vu) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    flights_[idx].destroyed = true;
    flights_[idx].suspended = false;
    flights_[idx].dirty = true;
    refresh_stats();
}

void FlightAggregateEngine::reset_cursor_(
        FlightAggregateState& f,
        const std::vector<f4::entities::WaypointState>& route,
        std::int64_t now_abs) {
    if (route.empty()) {
        f.wp_index = 0;
        return;
    }
    if (time_mode_[&f - flights_.data()]) {
        // First waypoint not yet reached on the wire's schedule.
        for (std::size_t i = 1; i < route.size(); ++i) {
            if (route[i].arrive > now_abs) {
                f.wp_index = i;
                return;
            }
        }
        f.wp_index = route.size() - 1;
        return;
    }
    // SPEED mode: the cursor targets the waypoint AFTER the nearest one
    // to the folded position (the leg it would fly next); when the fold
    // lands on the last waypoint the cursor stays there.
    std::size_t best = 0;
    double best_d = 0.0;
    for (std::size_t i = 0; i < route.size(); ++i) {
        const double dx = static_cast<double>(route[i].x) - f.fx;
        const double dy = static_cast<double>(route[i].y) - f.fy;
        const double d = dx * dx + dy * dy;
        if (i == 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    f.wp_index = best + 1 < route.size() ? best + 1 : best;
}

// ============================================================================
// queries
// ============================================================================

const FlightAggregateState* FlightAggregateEngine::find(
    std::uint32_t vu) const {
    for (const auto& f : flights_) {
        if (f.vu == vu) return &f;
    }
    return nullptr;
}

std::size_t FlightAggregateEngine::index_of(std::uint32_t vu) const {
    for (std::size_t i = 0; i < flights_.size(); ++i) {
        if (flights_[i].vu == vu) return i;
    }
    return static_cast<std::size_t>(-1);
}

std::int32_t FlightAggregateEngine::seconds_to_depart(
    std::size_t index) const {
    if (index >= flights_.size() || routes_[index].empty()) return -1;
    const std::int64_t depart = routes_[index].front().depart;
    if (depart <= 0) return -1;   // no usable schedule
    const std::int64_t d = depart - (epoch_ + clock_);
    return d > 2147483647 ? 2147483647
         : d < -2147483648 ? -2147483648
                           : static_cast<std::int32_t>(d);
}

std::int32_t FlightAggregateEngine::seconds_to_mission_over(
    std::size_t index) const {
    if (index >= flights_.size()) return -1;
    const std::int64_t over = flights_[index].mission_over_time;
    if (over <= 0) return -1;     // no mission-over time in the save
    const std::int64_t d = over - (epoch_ + clock_);
    return d > 2147483647 ? 2147483647
         : d < -2147483648 ? -2147483648
                           : static_cast<std::int32_t>(d);
}

double FlightAggregateEngine::current_heading_rad(std::size_t index) const {
    if (index >= flights_.size() || routes_[index].empty()) return 0.0;
    const auto& route = routes_[index];
    const auto& f = flights_[index];
    std::size_t target = f.wp_index < route.size() ? f.wp_index : 0;
    const double dx = static_cast<double>(route[target].x) - f.fx;
    const double dy = static_cast<double>(route[target].y) - f.fy;
    if (dx == 0.0 && dy == 0.0) return 0.0;
    // Compass bearing (0 = north = +grid-y), radians — the same
    // convention enu_quat_from_compass consumes at the spawn.
    return std::atan2(dx, dy);
}

} // namespace f4::campaign

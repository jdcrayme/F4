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

        // The TIME-mode schedule's terminal: the last waypoint carrying
        // an arrival (the unscheduled-tail arrival rule reads it).
        std::size_t sched_end = static_cast<std::size_t>(-1);
        if (timed) {
            for (std::size_t j = 0; j < routes_.back().size(); ++j) {
                if (routes_.back()[j].arrive > 0) sched_end = j;
            }
        }
        schedule_end_index_.push_back(sched_end);

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
            if (f.suspended || f.arrived || f.destroyed || f.scrubbed) {
                continue;
            }
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
    stats_.scrubbed = 0;
    for (const auto& f : flights_) {
        if (f.destroyed) ++stats_.destroyed;
        else if (f.scrubbed) ++stats_.scrubbed;
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

        // The unscheduled tail: legs after the last scheduled waypoint
        // have no arrival time to fire — the walk above skips them, so
        // a route whose last legs are unscheduled never marked the
        // flight arrived and it sat at the last scheduled waypoint
        // forever (a frozen glyph mid-map). Once the final scheduled
        // arrival is past, the flight IS at its route's end.
        const std::size_t end_i = schedule_end_index_[index];
        if (!f.arrived && end_i != static_cast<std::size_t>(-1) &&
            now_abs >= route[end_i].arrive) {
            f.arrived = true;
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
    // The schedule re-anchor runs BEFORE the cursor reset so the cursor
    // derives from the shifted wire (the fold's whole point: the
    // schedule now passes through the folded position — reset_cursor_'s
    // TIME-mode walk reads the shifted arrives).
    if (time_mode_[idx]) {
        reanchor_schedule_(f, routes_[idx], epoch_ + clock_);
    }
    reset_cursor_(f, routes_[idx], epoch_ + clock_);
    f.suspended = false;
    f.dirty = true;
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(epoch_ + clock_, 2147483647));
    refresh_stats();
}

void FlightAggregateEngine::update_live(std::uint32_t vu, double fx,
                                        double fy, float altitude_ft,
                                        std::int32_t fuel_burnt) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    FlightAggregateState& f = flights_[idx];
    if (!f.suspended) return;   // an aggregate owns its own kinematics
    f.fx = std::clamp(fx, -32768.0, 32767.0);
    f.fy = std::clamp(fy, -32768.0, 32767.0);
    f.altitude_ft = altitude_ft;
    f.fuel_burnt = std::max(f.fuel_burnt, fuel_burnt);
}

void FlightAggregateEngine::mark_destroyed(std::uint32_t vu) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    flights_[idx].destroyed = true;
    flights_[idx].suspended = false;
    flights_[idx].dirty = true;
    refresh_stats();
}

// ============================================================================
// CAMP-CMD-2 — the command writes
// ============================================================================

bool FlightAggregateEngine::retask(
        std::uint32_t vu, std::uint8_t mission,
        std::vector<f4::entities::WaypointState> route,
        std::int32_t time_on_target_abs, std::int32_t mission_over_abs) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return false;
    FlightAggregateState& f = flights_[idx];
    if (f.arrived || f.destroyed || f.scrubbed) return false;
    if (route.empty()) return false;   // loud: a retask flies SOMEWHERE

    // FID-P1: anchor the swap at the flight's TRUE position (the one
    // the serving face extrapolates) — the caller built the new route's
    // head from the same display position, so head == position and the
    // flight never snaps back to its last 60-s quanta point.
    catch_up_(f, idx, epoch_ + clock_);

    routes_[idx] = std::move(route);
    f.mission = mission;
    f.time_on_target = time_on_target_abs;
    f.mission_over_time = mission_over_abs;
    // The retask route carries no leg times (the intent vocabulary) —
    // a TIME-mode save flight retasks INTO speed mode. The takeoff
    // gate still reads the head waypoint's depart (the caller keeps
    // the original departure on an un-launched flight's head).
    time_mode_[idx] = false;
    f.has_route = true;
    f.arrived = false;
    f.dirty = true;
    if (!f.suspended) {
        // Flying toward the route's SECOND waypoint from the head (the
        // head IS the retask position — the first leg re-derives from
        // wherever the flight actually is).
        f.wp_index = routes_[idx].size() > 1 ? 1 : 0;
    }
    // A suspended flight keeps its cursor: the fold-back's
    // reset_cursor_ re-derives it on the new route from the lead
    // aircraft's true position.
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(epoch_ + clock_, 2147483647));
    refresh_stats();
    return true;
}

bool FlightAggregateEngine::scrub(std::uint32_t vu) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return false;
    FlightAggregateState& f = flights_[idx];
    if (f.arrived || f.destroyed || f.scrubbed) return false;
    f.scrubbed = true;
    f.suspended = false;
    f.dirty = true;
    refresh_stats();
    return true;
}

std::size_t FlightAggregateEngine::register_synthetic(
    const SyntheticFlightSeed& seed) {
    if (seed.vu == 0) return static_cast<std::size_t>(-1);
    if (index_of(seed.vu) != static_cast<std::size_t>(-1)) {
        return static_cast<std::size_t>(-1);   // duplicate — loud refusal
    }
    FlightAggregateState f;
    f.vu = seed.vu;
    f.team = seed.team;
    f.mission = seed.mission;
    f.aircraft_count = seed.aircraft_count > 0 ? seed.aircraft_count : 1;
    f.time_on_target = seed.time_on_target;
    // The recovery deadline rides the seed now (the ATM's own
    // mission_over, threaded through the intent): the recovery-ops
    // window arms for generated flights exactly as it does for the
    // save's own. 0 stays the no-window marker (an intent that never
    // carried one — the legacy ladder's).
    routes_.push_back(seed.route);
    const auto& route = routes_.back();
    // The flight holds at its route's first waypoint — the takeoff
    // waypoint IS the base the planner launched from (the intent path's
    // own parking fallback). Has no route = a parked aggregate.
    f.fx = route.empty() ? 0.0 : static_cast<double>(route.front().x);
    f.fy = route.empty() ? 0.0 : static_cast<double>(route.front().y);
    f.altitude_ft =
        route.empty() ? 0.0f : static_cast<float>(route.front().z);
    f.fuel_burnt = 0;
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(epoch_ + clock_, 2147483647));
    f.has_route = !route.empty();
    f.mission_over_time = seed.mission_over_time;
    // SPEED mode always: the intent's route carries no leg times (the
    // ATM's own takeoff estimate is the TOT anchor, not a wire schedule).
    time_mode_.push_back(false);
    schedule_end_index_.push_back(static_cast<std::size_t>(-1));
    flights_.push_back(f);
    refresh_stats();
    return flights_.size() - 1;
}

void FlightAggregateEngine::catch_up_(FlightAggregateState& f,
                                      std::size_t index,
                                      std::int64_t now_abs) {
    if (f.suspended) return;   // the sim owns the truth; the fold anchors
    double fx = f.fx, fy = f.fy;
    float alt = f.altitude_ft;
    display_position(index, now_abs, fx, fy, alt);
    f.fx = std::clamp(fx, -32768.0, 32767.0);
    f.fy = std::clamp(fy, -32768.0, 32767.0);
    f.altitude_ft = alt;
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(now_abs, 2147483647));
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

void FlightAggregateEngine::reanchor_schedule_(
        FlightAggregateState& f,
        std::vector<f4::entities::WaypointState>& route,
        std::int64_t now_abs) {
    if (route.size() < 2) return;
    // A pre-departure fold keeps the wire: the flight has not started
    // flying the schedule, so the takeoff gate still owns it.
    const std::int64_t first_depart = route.front().depart;
    if (first_depart > 0 && now_abs < first_depart) return;

    // Along-route distances (grids) of the waypoints.
    std::vector<double> dist(route.size(), 0.0);
    for (std::size_t i = 1; i < route.size(); ++i) {
        const double dx =
            static_cast<double>(route[i].x) - static_cast<double>(route[i - 1].x);
        const double dy =
            static_cast<double>(route[i].y) - static_cast<double>(route[i - 1].y);
        dist[i] = dist[i - 1] + std::sqrt(dx * dx + dy * dy);
    }

    // Project the folded position onto the polyline — combat drift can
    // leave the route — keeping the closest leg's touch point.
    double d_p = 0.0;
    double best = -1.0;
    for (std::size_t i = 1; i < route.size(); ++i) {
        const double ax = static_cast<double>(route[i - 1].x);
        const double ay = static_cast<double>(route[i - 1].y);
        const double bx = static_cast<double>(route[i].x);
        const double by = static_cast<double>(route[i].y);
        const double ex = bx - ax, ey = by - ay;
        const double len2 = ex * ex + ey * ey;
        double t = 0.0;
        if (len2 > 0.0) {
            t = ((f.fx - ax) * ex + (f.fy - ay) * ey) / len2;
            t = std::clamp(t, 0.0, 1.0);
        }
        const double ddx = f.fx - (ax + t * ex);
        const double ddy = f.fy - (ay + t * ey);
        const double d2 = ddx * ddx + ddy * ddy;
        if (best < 0.0 || d2 < best) {
            best = d2;
            d_p = dist[i - 1] + t * std::sqrt(len2);
        }
    }

    // The schedule anchors: (along-route distance, absolute time).
    // wp0's depart IS the moment the flight sits at wp0; every
    // waypoint with an arrival is another. In order (dist increases
    // with the index; wp0 comes first).
    struct Anchor {
        double d;
        std::int64_t t;
    };
    std::vector<Anchor> anchors;
    if (first_depart > 0) anchors.push_back({0.0, first_depart});
    for (std::size_t i = 1; i < route.size(); ++i) {
        if (route[i].arrive > 0) anchors.push_back({dist[i], route[i].arrive});
    }
    if (anchors.size() < 2) return;   // no speed to extrapolate with

    // The schedule time at d_p: interpolate inside the anchors, use
    // the end pair's own speed at the ends (a fold past the last
    // waypoint, or before the first arrival).
    std::int64_t t_p = 0;
    const auto speed = [](const Anchor& a, const Anchor& b) {
        const double dd = std::max(1e-9, b.d - a.d);
        return static_cast<double>(b.t - a.t) / dd;
    };
    if (d_p <= anchors.front().d) {
        const double v = speed(anchors[0], anchors[1]);
        t_p = anchors[0].t -
              static_cast<std::int64_t>(
                  std::llround((anchors[0].d - d_p) * v));
    } else if (d_p >= anchors.back().d) {
        const double v = speed(anchors[anchors.size() - 2],
                               anchors.back());
        t_p = anchors.back().t +
              static_cast<std::int64_t>(
                  std::llround((d_p - anchors.back().d) * v));
    } else {
        for (std::size_t i = 1; i < anchors.size(); ++i) {
            if (d_p <= anchors[i].d) {
                const double k = (d_p - anchors[i - 1].d) /
                                 std::max(1e-9, anchors[i].d -
                                                    anchors[i - 1].d);
                t_p = anchors[i - 1].t +
                      static_cast<std::int64_t>(std::llround(
                          k * static_cast<double>(anchors[i].t -
                                                  anchors[i - 1].t)));
                break;
            }
        }
    }

    // One constant shift slides the whole schedule so it passes
    // through the folded position AT the fold time. Every leg duration
    // and dwell survives (the route shape and its speeds are the
    // save's own); only the clock they run on moves.
    const std::int64_t delta = t_p - now_abs;
    if (delta == 0) return;
    for (auto& w : route) {
        if (w.depart > 0) {
            w.depart = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                w.depart - delta, 1, 2147483647));
        }
        if (w.arrive > 0) {
            w.arrive = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                w.arrive - delta, 1, 2147483647));
        }
    }
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
    if (flights_[index].scrubbed) return -1;   // CAMP-CMD-2: no sortie
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
    if (flights_[index].scrubbed) return -1;   // CAMP-CMD-2: no sortie
    const std::int64_t over = flights_[index].mission_over_time;
    if (over <= 0) return -1;     // no mission-over time in the save
    const std::int64_t d = over - (epoch_ + clock_);
    return d > 2147483647 ? 2147483647
         : d < -2147483648 ? -2147483648
                           : static_cast<std::int32_t>(d);
}

std::int32_t FlightAggregateEngine::seconds_to_time_on_target(
    std::size_t index) const {
    if (index >= flights_.size()) return -1;
    if (flights_[index].scrubbed) return -1;   // CAMP-CMD-2: no sortie
    const std::int64_t tot = flights_[index].time_on_target;
    if (tot <= 0) return -1;      // no TOT in the save / on the intent
    const std::int64_t d = tot - (epoch_ + clock_);
    return d > 2147483647 ? 2147483647
         : d < -2147483648 ? -2147483648
                           : static_cast<std::int32_t>(d);
}

double FlightAggregateEngine::current_heading_rad(std::size_t index) const {
    if (index >= flights_.size() || routes_[index].empty()) return 0.0;
    const auto& route = routes_[index];
    const auto& f = flights_[index];
    std::size_t target = f.wp_index < route.size() ? f.wp_index : 0;
    double dx = static_cast<double>(route[target].x) - f.fx;
    double dy = static_cast<double>(route[target].y) - f.fy;
    // FID-5: a flight sitting ON its cursor waypoint (the pre-departure
    // hold, or a fold-back landing exactly on a waypoint) has a
    // degenerate zero leg — the bearing it will actually FLY is the
    // NEXT leg's. Without this the pose/velocity reads north (the
    // 0-radian default) while the flight is pointed down its route,
    // and the convergence trigger's predicted tracks never close.
    while (dx == 0.0 && dy == 0.0 && target + 1 < route.size()) {
        ++target;
        dx = static_cast<double>(route[target].x) - f.fx;
        dy = static_cast<double>(route[target].y) - f.fy;
    }
    if (dx == 0.0 && dy == 0.0) return 0.0;
    // Compass bearing (0 = north = +grid-y), radians — the same
    // convention enu_quat_from_compass consumes at the spawn.
    return std::atan2(dx, dy);
}

void FlightAggregateEngine::display_position(
        std::size_t index, std::int64_t now_abs, double& fx, double& fy,
        float& altitude_ft) const {
    if (index >= flights_.size()) return;
    const auto& f = flights_[index];
    fx = f.fx;
    fy = f.fy;
    altitude_ft = f.altitude_ft;

    // A row that is not flying its route as an aggregate reports its
    // stored position: suspended (the sim owns the truth — the session
    // overlays the lead's transform), arrived/destroyed/scrubbed
    // (terminal), route-less (parked).
    if (f.suspended || f.arrived || f.destroyed || f.scrubbed ||
        !f.has_route) {
        return;
    }
    const auto& route = routes_[index];
    if (route.empty()) return;

    // The takeoff gate (advance_flight_'s own rule): a flight holds at
    // its save position until its first waypoint's depart.
    const std::int64_t first_depart = route.front().depart;
    if (first_depart > 0 && now_abs < first_depart) return;

    if (time_mode_[index]) {
        // TIME mode — the wire's schedule is a pure function of now_abs:
        // re-derive the leg interpolation read-only (advance_flight_'s
        // walk, minus the state writes). Past legs override in order;
        // the first upcoming leg interpolates; before it, hold.
        for (std::size_t i = 1; i < route.size(); ++i) {
            const auto& from = route[i - 1];
            const auto& to = route[i];
            if (to.arrive <= 0) continue;   // leg without a schedule
            if (now_abs >= to.arrive) {
                fx = static_cast<double>(to.x);
                fy = static_cast<double>(to.y);
                altitude_ft = static_cast<float>(to.z);
                continue;
            }
            if (now_abs > from.depart) {
                const double t = frac(now_abs, from.depart, to.arrive);
                fx = static_cast<double>(from.x) +
                     t * (static_cast<double>(to.x) - from.x);
                fy = static_cast<double>(from.y) +
                     t * (static_cast<double>(to.y) - from.y);
                altitude_ft = static_cast<float>(
                    static_cast<double>(from.z) +
                    t * (static_cast<double>(to.z) - from.z));
            }
            break;   // mid-leg or still holding at route[i-1]
        }
        return;
    }

    // SPEED mode — walk forward from the last advance at the cruise.
    // The anchor is the LAST MOVE — an ABSOLUTE campaign stamp (every
    // mutation that touches position writes epoch_+clock_ into it: the
    // updates, the fold-back, the retask, the registration) — clamped
    // forward to the departure gate so a flight whose first update has
    // not fired yet does not extrapolate from a pre-departure stamp (it
    // would race a full hold's worth of distance ahead of the engine's
    // own first step). NOT epoch_+last_move: the stamp already carries
    // the epoch — doubling it pushes the anchor into the future and the
    // extrapolation never fires (elapsed ≤ 0 forever).
    std::int64_t anchor = f.last_move;
    if (first_depart > 0 && first_depart > anchor) anchor = first_depart;
    const double elapsed = static_cast<double>(now_abs - anchor);
    if (elapsed <= 0.0) return;
    double remaining = cfg_.cruise_grid_per_min * (elapsed / 60.0);
    double px = fx, py = fy;
    float pz = altitude_ft;
    std::size_t wp = f.wp_index < route.size() ? f.wp_index : route.size();
    while (remaining > 0.0 && wp < route.size()) {
        const auto& target = route[wp];
        const double dx = static_cast<double>(target.x) - px;
        const double dy = static_cast<double>(target.y) - py;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (dist <= remaining) {
            // Waypoint reached inside the extrapolation: land on it,
            // spend the distance, target the next leg (a degenerate
            // dist == 0 — the flight sits ON its cursor waypoint, the
            // pre-departure hold's shape — advances the cursor exactly
            // like the engine's own walk, then flies the NEXT leg).
            px = static_cast<double>(target.x);
            py = static_cast<double>(target.y);
            pz = static_cast<float>(target.z);
            remaining -= dist;
            ++wp;
        } else {
            const double k = dist > 0.0 ? remaining / dist : 0.0;
            px += k * dx;
            py += k * dy;
            pz = static_cast<float>(static_cast<double>(pz) +
                                    k * (static_cast<double>(target.z) -
                                         static_cast<double>(pz)));
            remaining = 0.0;
        }
    }
    fx = px;
    fy = py;
    altitude_ft = pz;
}

} // namespace f4::campaign

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
        // CAMP-SAVE-WIRE — the stock saves' waypoint times are the ATO
        // planner's multi-day horizon: computable legs cross Korea at
        // ~0.01 grid/min (weeks per leg). The wire cannot drive
        // motion — strict TIME-mode interpolation freezes the whole
        // war into imperceptible creep. A timed route whose
        // computable legs ALL imply a crawl below the floor (or that
        // has no computable leg at all) flies SPEED mode at the
        // campaign cruise instead — the DEAGG-RWY precedent (remove
        // the crawl for the population that suffered it). Sane wires
        // keep the TIME identity; mixed wires keep theirs (a slow
        // loiter leg is a schedule, not garbage — the walk honors
        // each leg's own times).
        if (timed && cfg_.min_leg_speed_grid_per_min > 0.0) {
            const auto& r = routes_.back();
            int computable = 0;
            bool all_crawl = true;
            for (std::size_t i = 1; i < r.size(); ++i) {
                const std::int64_t dep = r[i - 1].depart > 0
                                             ? r[i - 1].depart
                                             : r[i - 1].arrive;
                if (r[i].arrive <= 0 || dep <= 0 ||
                    r[i].arrive <= dep) {
                    continue;
                }
                const double len = std::sqrt(
                    std::pow(static_cast<double>(r[i].x) - r[i - 1].x,
                             2) +
                    std::pow(static_cast<double>(r[i].y) - r[i - 1].y,
                             2));
                if (len <= 0.0) continue;
                ++computable;
                if (len / (static_cast<double>(r[i].arrive - dep) /
                           60.0) >=
                    cfg_.min_leg_speed_grid_per_min) {
                    all_crawl = false;
                    break;
                }
            }
            if (computable == 0 || all_crawl) {
                timed = false;
                // The wire's times are the rejected garbage — scrub
                // them: a SPEED-mode route must not read them as TOT
                // appointments (the multi-day arrive values would hold
                // the walk forever).
                for (auto& w : routes_.back()) w.arrive = 0;
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

        // AGG-4 — the lazy row's books: schedule generation 0 (its
        // arrival events arm under it) and the fuel anchor at the
        // epoch (the save's own burn seeds the stored field; the
        // closed form counts eligible updates from here).
        arrival_gen_.push_back(0);
        fuel_anchor_.push_back(epoch_);

        flights_.push_back(f);
        ++taken;
    }

    // AGG-4: a mid-war save's TIME rows materialize their schedule
    // state at the epoch (the walk's own overrides — a loaded flight
    // past its first arrivals starts at the schedule's position with
    // the right cursor and terminal flag, exactly what the first
    // update's walk used to paste), then arm their first arrival.
    for (std::size_t i = 0; i < flights_.size(); ++i) {
        if (!time_mode_[i] || !flights_[i].has_route) continue;
        materialize_time_state_(i, epoch_);
        reschedule_arrivals_(i);
    }

    refresh_stats();
}

// ============================================================================
// tick — the update loop (the GroundWar accumulator shape)
// ============================================================================

void FlightAggregateEngine::tick(CampaignTime delta_sec) {
    if (delta_sec <= 0) return;
    clock_ += delta_sec;
    const std::int64_t now_abs = epoch_ + clock_;

    // AGG-4 — the TIME rows' discrete transitions first: the waypoint
    // arrivals fire at the schedule's own seconds, key-ordered by the
    // queue's (due, priority, seq) contract. Between events a TIME row
    // costs NOTHING — the query faces derive its state from (route,
    // now) on read.
    arrivals_.pop_due(now_abs, [this](ArrivalEvent&& ev) {
        fire_arrival_(ev, ev.due);
    });

    // The chunk walk: SPEED rows only. A TIME row's position is the
    // schedule — stepping it through 60-s quanta was the propagation
    // cost AGG-4 deletes (the plan's `MoveUnit`-shape residue).
    while (clock_ >= next_update_ + cfg_.update_sec) {
        next_update_ += cfg_.update_sec;

        const std::int64_t update_now = epoch_ + next_update_;
        for (std::size_t i = 0; i < flights_.size(); ++i) {
            FlightAggregateState& f = flights_[i];
            if (f.suspended || f.arrived || f.destroyed || f.scrubbed ||
                time_mode_[i]) {
                continue;
            }
            advance_flight_(f, i, routes_[i], update_now);
        }

        ++stats_.updates;
        refresh_stats();
    }
}

// ============================================================================
// AGG-4 — the arrival events (the TIME rows' discrete transitions)
// ============================================================================

void FlightAggregateEngine::fire_arrival_(const ArrivalEvent& ev,
                                          std::int64_t due_abs) {
    if (ev.index >= flights_.size()) return;
    if (flights_[ev.index].vu != ev.vu) return;
    if (arrival_gen_[ev.index] != ev.gen) return;   // re-armed by a mutation
    FlightAggregateState& f = flights_[ev.index];
    const auto& route = routes_[ev.index];
    if (!time_mode_[ev.index] || !f.has_route || route.empty() ||
        f.suspended || f.arrived || f.destroyed || f.scrubbed ||
        ev.wp >= route.size()) {
        return;   // stale shape — the generation check's belt and braces
    }

    // Materialize exactly what the update walk used to write when its
    // quanta crossed this arrival: the waypoint snap, the cursor, and
    // (at the schedule's end) the terminal flag. The dirty/last_move
    // stamps ride along (the writeback and the fold read them).
    f.fx = static_cast<double>(route[ev.wp].x);
    f.fy = static_cast<double>(route[ev.wp].y);
    f.altitude_ft = static_cast<float>(route[ev.wp].z);
    f.wp_index = ev.wp;
    f.dirty = true;
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(due_abs, 2147483647));
    if (ev.wp == schedule_end_index_[ev.index]) {
        f.arrived = true;   // the last scheduled waypoint reached
        refresh_stats();
        return;             // terminal — the row leaves the machinery
    }
    // The cursor-chase: arm the next scheduled arrival (strictly after
    // this one — the fired event is done; the row was not mutated, so
    // its generation stands).
    arm_next_arrival_(ev.index, ev.gen, due_abs);
}

void FlightAggregateEngine::arm_next_arrival_(std::size_t index,
                                              std::uint32_t gen,
                                              std::int64_t after_abs) {
    const auto& route = routes_[index];
    for (std::size_t j = 1; j < route.size(); ++j) {
        if (route[j].arrive > after_abs) {
            ArrivalEvent ev;
            ev.vu = flights_[index].vu;
            ev.gen = gen;
            ev.index = index;
            ev.wp = j;
            ev.due = route[j].arrive;
            arrivals_.schedule(route[j].arrive, 0, std::move(ev));
            return;
        }
    }
    // No future arrival — unscheduled tail already past the schedule's
    // end, or nothing scheduled ahead. The row flies (or sits) without
    // events until a mutation re-arms it.
}

void FlightAggregateEngine::reschedule_arrivals_(std::size_t index) {
    if (index >= flights_.size()) return;
    const std::uint32_t gen = ++arrival_gen_[index];
    const FlightAggregateState& f = flights_[index];
    if (!time_mode_[index] || !f.has_route || routes_[index].empty() ||
        f.suspended || f.arrived || f.destroyed || f.scrubbed) {
        return;   // the bump retired the row's stale events; none re-arm
    }
    // Inclusive from now: an arrival landing exactly on the mutation's
    // second still owes its stored-face materialization (the fire's own
    // re-arm is strict, so this cannot loop).
    arm_next_arrival_(index, gen, epoch_ + clock_ - 1);
}

void FlightAggregateEngine::materialize_time_state_(std::size_t index,
                                                    std::int64_t now_abs) {
    FlightAggregateState& f = flights_[index];
    const auto& route = routes_[index];
    if (route.empty()) return;

    // The advance walk's own gate and overrides, read-only face → the
    // row: a flight past its first arrivals starts where the schedule
    // says (a mid-war save), a pre-departure flight keeps its save
    // position.
    const std::int64_t first_depart = route.front().depart;
    if (first_depart > 0 && now_abs < first_depart) return;
    for (std::size_t i = 1; i < route.size(); ++i) {
        const auto& to = route[i];
        if (to.arrive <= 0) continue;   // leg without a schedule
        if (now_abs >= to.arrive) {
            f.fx = static_cast<double>(to.x);
            f.fy = static_cast<double>(to.y);
            f.altitude_ft = static_cast<float>(to.z);
            f.wp_index = i;
            if (i + 1 == route.size()) f.arrived = true;
            continue;
        }
        if (now_abs > route[i - 1].depart) {
            const double t = frac(now_abs, route[i - 1].depart, to.arrive);
            f.fx = static_cast<double>(route[i - 1].x) +
                   t * (static_cast<double>(to.x) - route[i - 1].x);
            f.fy = static_cast<double>(route[i - 1].y) +
                   t * (static_cast<double>(to.y) - route[i - 1].y);
            f.altitude_ft = static_cast<float>(
                static_cast<double>(route[i - 1].z) +
                t * (static_cast<double>(to.z) - route[i - 1].z));
            f.wp_index = i;
        }
        break;   // mid-leg or still holding at route[i-1]
    }
    const std::size_t end_i = schedule_end_index_[index];
    if (!f.arrived && end_i != static_cast<std::size_t>(-1) &&
        now_abs >= route[end_i].arrive) {
        f.arrived = true;   // the unscheduled-tail rule
    }
}

std::int64_t FlightAggregateEngine::count_burn_updates_(
    std::size_t index, std::int64_t from_abs, std::int64_t to_abs) const {
    if (to_abs <= from_abs) return 0;
    const auto& route = routes_[index];
    if (route.size() < 2) return 0;
    const std::size_t end_i = schedule_end_index_[index];
    if (end_i == static_cast<std::size_t>(-1)) return 0;
    const std::int64_t a_end = route[end_i].arrive;
    if (a_end <= 0) return 0;

    // The walk's moved() predicate as one integer threshold. An update
    // moves the row when the takeoff gate is open AND (a scheduled
    // arrival has passed OR the first upcoming scheduled leg is being
    // flown — `now > its from.depart`). Before the first scheduled
    // arrival the "first upcoming leg" is the route's first scheduled
    // one, so the threshold is exact for the whole pre-arrival span;
    // past it the arrival disjunct owns the answer.
    std::int64_t a_first = -1;
    std::int64_t d_first = 0;
    for (std::size_t i = 1; i < route.size(); ++i) {
        if (route[i].arrive > 0) {
            a_first = route[i].arrive;
            d_first = route[i - 1].depart;
            break;
        }
    }
    if (a_first <= 0) return 0;
    std::int64_t t_act = a_first < d_first + 1 ? a_first : d_first + 1;
    const std::int64_t gate = route.front().depart;
    if (gate > 0 && gate > t_act) t_act = gate;

    // Eligible grid updates: u = epoch_ + k·update_sec (k ≥ 1) with
    // u ≥ t_act, u > from_abs, u ≤ to_abs, u < a_end (never on the
    // arriving update — the walk's own rule).
    std::int64_t lo = t_act > from_abs + 1 ? t_act : from_abs + 1;
    std::int64_t hi = to_abs < a_end - 1 ? to_abs : a_end - 1;
    if (hi < lo) return 0;
    const std::int64_t step = cfg_.update_sec;
    const auto ceil_div = [](std::int64_t a, std::int64_t b) {
        return a >= 0 ? (a + b - 1) / b : -((-a) / b);
    };
    std::int64_t k_lo = ceil_div(lo - epoch_, step);
    if (k_lo < 1) k_lo = 1;
    std::int64_t k_hi = (hi - epoch_) / step;
    if (k_hi < k_lo) return 0;
    return k_hi - k_lo + 1;
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
// advance_flight_ — one SPEED row, one update (AGG-4: TIME rows are the
// schedule — the queue's arrivals and the query faces serve them; the
// chunk walk below never touches one)
// ============================================================================

void FlightAggregateEngine::advance_flight_(
        FlightAggregateState& f, std::size_t index,
        std::vector<f4::entities::WaypointState>& route,
        std::int64_t now_abs) {
    if (!f.has_route || route.empty()) return;   // nothing to fly

    // The takeoff gate: a flight holds at its save position until its
    // first waypoint's depart (the wire's own schedule).
    const std::int64_t first_depart = route.front().depart;
    if (first_depart > 0 && now_abs < first_depart) return;

    bool moved = false;
    {
        // SPEED mode — walk the legs at this row's cruise toward the
        // current cursor waypoint (the fold may have booked the lead's
        // real ground speed; 0 rides the config default). Start
        // position → wp[0] → wp[1] → … The cursor names the waypoint
        // being flown TOWARD; altitude lerps toward the target's z by
        // the same distance fraction.
        const double gpm = f.cruise_grid_per_min > 0.0
                               ? f.cruise_grid_per_min
                               : cfg_.cruise_grid_per_min;
        double remaining = gpm *
                           (static_cast<double>(
                                std::min<CampaignTime>(cfg_.update_sec, 3600)) /
                            60.0);
        while (remaining > 0.0 && f.wp_index < route.size() &&
               !f.arrived) {
            const auto& target = route[f.wp_index];
            // CAMP-TOT-PACE: an appointment time (the seed stamps the
            // delivery waypoint's arrive = TOT) holds the walk SHORT
            // of the waypoint until its time — a flight with slack
            // used to transit the target whenever it got there (the
            // early side of the ±30-min delivery scatter). TIME mode's
            // own semantics, borrowed for one appointment.
            if (target.arrive > 0 && now_abs < target.arrive) {
                break;
            }
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
    if (suspended) {
        // AGG-4: the row's modeled burn is closed-form while it flies —
        // paste it into the stored field BEFORE the sim takes the truth
        // (the query below must still see a flying row), and freeze the
        // fuel anchor so the suspended window's grid updates count
        // nothing.
        flights_[idx].fuel_burnt = fuel_burnt_now(idx);
        fuel_anchor_[idx] = epoch_ + clock_;
    }
    flights_[idx].suspended = suspended;
    // The row's arrival events retire while it is suspended (a stale
    // event would materialize a schedule point into a sim-owned row);
    // the unsuspend re-arms them.
    reschedule_arrivals_(idx);
    refresh_stats();
}

void FlightAggregateEngine::reaggregate(std::uint32_t vu, double fx,
                                        double fy, float altitude_ft,
                                        std::int32_t fuel_burnt,
                                        double cruise_grid_per_min,
                                        bool lead_airborne) {
    const std::size_t idx = index_of(vu);
    if (idx == static_cast<std::size_t>(-1)) return;
    FlightAggregateState& f = flights_[idx];
    f.fx = std::clamp(fx, -32768.0, 32767.0);
    f.fy = std::clamp(fy, -32768.0, 32767.0);
    f.altitude_ft = altitude_ft;
    // AGG-4: materialize the row's own modeled burn before the monotone
    // max (a direct fold on a flying TIME row keeps the accrual the
    // closed form booked; a suspended row's stored field already holds
    // it — the query returns it verbatim). Monotonic fuel: the sim can
    // only have burned more (the fold-back never resurrects fuel the
    // aggregate already booked).
    f.fuel_burnt = fuel_burnt_now(idx);
    f.fuel_burnt = std::max(f.fuel_burnt, fuel_burnt);
    // The fold's pace: the lead's actual ground speed when the caller
    // booked one (0 = back to the config default).
    f.cruise_grid_per_min = std::max(0.0, cruise_grid_per_min);
    // The schedule re-anchor runs BEFORE the cursor reset so the cursor
    // derives from the shifted wire (the fold's whole point: the
    // schedule now passes through the folded position — reset_cursor_'s
    // TIME-mode walk reads the shifted arrives).
    if (time_mode_[idx]) {
        reanchor_schedule_(f, routes_[idx], epoch_ + clock_,
                           lead_airborne);
    }
    reset_cursor_(f, routes_[idx], epoch_ + clock_);
    f.suspended = false;
    f.dirty = true;
    f.last_move = static_cast<std::int32_t>(
        std::min<std::int64_t>(epoch_ + clock_, 2147483647));
    // AGG-4: the re-anchored schedule is the row's new truth — the fuel
    // anchor restarts here (the folded burn is the base), and the
    // arrival events re-arm on the shifted wire.
    fuel_anchor_[idx] = epoch_ + clock_;
    reschedule_arrivals_(idx);
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
    reschedule_arrivals_(idx);   // AGG-4: the row's events retire
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

    // AGG-4: paste the closed-form burn into the stored field before
    // the mode flips (the SPEED walk resumes accruing on the stored
    // field; the TIME-mode modeled burn must not be lost), and restart
    // the fuel anchor here.
    f.fuel_burnt = fuel_burnt_now(idx);
    fuel_anchor_[idx] = epoch_ + clock_;

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
    reschedule_arrivals_(idx);   // AGG-4: a retasked row flies SPEED — no events
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
    reschedule_arrivals_(idx);   // AGG-4: the row's events retire
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
    // AGG-4: the lazy row's books (SPEED — no arrival events will arm;
    // the fuel anchor seeds at the registration, the stored burn is 0).
    arrival_gen_.push_back(0);
    fuel_anchor_.push_back(epoch_ + clock_);
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
        std::int64_t now_abs, bool lead_airborne) {
    if (route.size() < 2) return;
    // A GROUNDED pre-departure fold keeps the wire: the flight has not
    // started flying the schedule, so the takeoff gate still owns it.
    // An AIRBORNE lead re-anchors regardless — it already flew off the
    // wire (a bubble/ops deagg took it off the ramp early), and the
    // shift below lands the gate in the past, exactly where a flying
    // sortie's gate belongs.
    const std::int64_t first_depart = route.front().depart;
    if (!lead_airborne && first_depart > 0 && now_abs < first_depart) {
        return;
    }

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

std::size_t FlightAggregateEngine::time_cursor_(std::size_t index,
                                                std::int64_t now_abs) const {
    // The advance walk's own cursor rule, read-only: past arrivals
    // override in order; the first upcoming scheduled leg names its
    // target when the flight is mid-leg; a hold keeps the previous.
    const auto& route = routes_[index];
    std::size_t cursor = 0;
    for (std::size_t i = 1; i < route.size(); ++i) {
        const auto& to = route[i];
        if (to.arrive <= 0) continue;   // leg without a schedule
        if (now_abs >= to.arrive) {
            cursor = i;
            continue;
        }
        if (now_abs > route[i - 1].depart) cursor = i;
        break;
    }
    return cursor;
}

std::size_t FlightAggregateEngine::waypoint_cursor(std::size_t index) const {
    if (index >= flights_.size()) return 0;
    const FlightAggregateState& f = flights_[index];
    if (!time_mode_[index] || !f.has_route || routes_[index].empty()) {
        return f.wp_index < routes_[index].size() ? f.wp_index : 0;
    }
    return time_cursor_(index, epoch_ + clock_);
}

std::int32_t FlightAggregateEngine::fuel_burnt_now(std::size_t index) const {
    if (index >= flights_.size()) return 0;
    const FlightAggregateState& f = flights_[index];
    // The closed form serves the flying TIME aggregates only; every
    // other row's stored field IS its truth (the save's seed, the
    // sim-side tracking, the walk's own accrual, or a materialized
    // paste from a transition).
    if (!time_mode_[index] || f.suspended || f.destroyed || f.scrubbed ||
        !f.has_route || cfg_.fuel_burn_lbs_per_min <= 0) {
        return f.fuel_burnt;
    }
    const std::int64_t per_update = std::llround(
        static_cast<double>(cfg_.fuel_burn_lbs_per_min) *
        static_cast<double>(cfg_.update_sec) / 60.0);
    const std::int64_t burnt =
        static_cast<std::int64_t>(f.fuel_burnt) +
        count_burn_updates_(index, fuel_anchor_[index], epoch_ + clock_) *
            per_update;
    if (burnt < 0) return 0;   // clamp absurd saves (the walk's own rule)
    return burnt > 2147483647 ? 2147483647 : static_cast<std::int32_t>(burnt);
}

double FlightAggregateEngine::current_heading_rad(std::size_t index) const {
    if (index >= flights_.size() || routes_[index].empty()) return 0.0;
    const auto& route = routes_[index];
    const auto& f = flights_[index];
    // AGG-4: a TIME row's stored cursor materializes at the arrival
    // events; the leg it is actually FLYING comes from the schedule
    // (the same walk rule the walking engine's per-update cursor
    // wrote). SPEED rows keep the stored cursor.
    const std::size_t cursor = waypoint_cursor(index);
    std::size_t target = cursor < route.size() ? cursor : 0;
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

    // SPEED mode — walk forward from the last advance at this row's
    // cruise (the fold's booked ground speed, else the config default).
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
    const double gpm = f.cruise_grid_per_min > 0.0
                           ? f.cruise_grid_per_min
                           : cfg_.cruise_grid_per_min;
    double remaining = gpm * (elapsed / 60.0);
    double px = fx, py = fy;
    float pz = altitude_ft;
    std::size_t wp = f.wp_index < route.size() ? f.wp_index : route.size();
    while (remaining > 0.0 && wp < route.size()) {
        const auto& target = route[wp];
        // The appointment gate (advance_flight_'s own rule): the
        // serving face holds short of an appointed waypoint too — one
        // truth, no pause-then-jump at the hold point.
        if (target.arrive > 0 && now_abs < target.arrive) {
            break;
        }
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

// f4-ai/src/navigation_module.cpp
//
// NavigationModule implementation — waypoint following via AirSteering.

#include "f4/ai/modules/navigation_module.hpp"

#include <f4/ai/modules/strike_module.hpp>  // is_ag_delivery_action

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

namespace f4::ai::modules {

// ============================================================================
// Construction
// ============================================================================

NavigationModule::NavigationModule()
    : sm_(build_sm())
{
    // Cruise tune: cooler than the AirSteering defaults. With the default
    // gains the altitude channel phugoids (long-period pitch/speed
    // oscillation) against the FCS G-command lag and never settles on the
    // route altitude — which then hands the approach off far above the
    // beam. Same lesson as the landing tune.
    // STAB-E15: calm enroute tune. The previous values (attitude_gain 1.5,
    // path_gain 0.0001, vs_gain 5.0, max_vs 3000) overrode the STAB-E1
    // defaults and sustained a bang-bang limit cycle through the whole
    // route (digi_full_mission t=640-722: ptcmd saturating +2.0 G then
    // -0.6 G alternately, pitch +-25 deg at ~40 s period, VS +-9,000 fpm).
    // The cascade saturated because the gains demanded more authority than
    // the FCS G-lag (~2-3 s) could deliver without overshoot. Slower
    // authority + strong VS-error damping:
    air_steering.attitude_gain = 1.0;
    // NAV-E: raised from 0.0005. The VS-error gamma term is the phugoid
    // damper; at 0.0005 the square-route legs rang +-550 ft (9,570 to
    // 10,700) with the altitude loop phase-lagged through the FCS G-lag
    // (standard_rate_turn t=104-220). 0.0012 restores damping without
    // re-triggering the STAB-E1 bang-bang (that needed hot attitude_gain
    // AND path_gain together; attitude stays soft here).
    // PHUG-P4 retune (M3 linear-band rule): 0.0005 -> 0.00005. At 0.0005
    // the gamma-correction damper saturated its 0.10-rad limit for ANY vs
    // error beyond 200 fpm — across the enroute phugoid's ±2,000 fpm it
    // ran as a bang-bang RELAY (direction flips with the error sign).
    // The P4.1-corrected inner loop executes the relay faithfully instead
    // of filtering it through the old G-lag: measured (ground-avoid E2E),
    // the post-recovery release state decays through an honest phugoid
    // downswing (vs 0 -> -9,000 fpm) the railed damper cannot arrest and
    // the jet descends onto terrain. 0.00005 keeps the ±2,000 fpm
    // operating band proportional (~5.7 deg of correction at the swing
    // amplitude, the limit reached only beyond ±2,000 fpm).
    air_steering.vs_gain = 2.5;
    air_steering.max_vs_fpm = 1500.0;
    air_steering.roll_gain = 4.0;
    // INIT-2c: the cruise small-error bank drive. The steering's
    // sub-5-deg heading deadband (the landing beam's wings-level
    // decoupling, kept there) froze every enroute leg's residual
    // cross-track at the roll-out offset — the measured SAD attack run
    // flew a constant +0.8 deg right of its leg with a 2.7-deg intercept
    // commanded, the track passed 290 ft off the aim, and the bombs
    // inherited it (the pipper's cross term, zero features). The cap
    // 0.10 rad = 5.7 deg: a ~0.4 deg/s heading rate at 450 kts — gentle
    // enough to leave the altitude hold uncoupled, decisive enough to
    // converge a 250-ft offset inside a 10,000-ft window.
    air_steering.small_error_bank_cap_rad = 0.10;
    // NAV-E: disable the STAB-E46 anti-balloon energy damper enroute. It
    // was tuned for approach balloons (chop throttle + full board when
    // vs overshoots +1,200 fpm); on the 250-kt square legs the phugoid's
    // climb half-cycle legitimately exceeds that during altitude
    // recovery, and the guard then chops power mid-recovery — PUMPING the
    // oscillation it was meant to damp (t=176-192: throttle 0.08 + full
    // board while climbing at 245 kts, next cycle deeper than the last).
    // Enroute there is terrain clearance to let the VS loop do its job;
    // the landing tune keeps the guard.
    air_steering.balloon_guard_fpm = 1000000.0;
}

fsm::StateMachine<NavigationState, NavigationEvent>
NavigationModule::build_sm()
{
    return typename fsm::StateMachine<NavigationState, NavigationEvent>::Builder()
        .initial(NavigationState::ToWaypoint)
        .state(NavigationState::ToWaypoint, "ToWaypoint")
        .state(NavigationState::Done,       "Done")
        .event_name(NavigationEvent::WaypointCaptured, "WaypointCaptured")
        .on(NavigationState::ToWaypoint, NavigationState::Done,
            NavigationEvent::WaypointCaptured,
            nullptr, nullptr, "last_waypoint_reached")
        .build();
}

void NavigationModule::set_route(std::vector<Waypoint> route) {
    route_ = std::move(route);
    wp_index_ = 0;
    wp_timer_ = 0.0;
    // P7 — the station hold resets with the route (a re-tasked module
    // must not inherit the previous hold's state).
    holding_ = false;
    station_done_ = false;
    station_elapsed_ = 0.0;
    loop_start_ = loop_end_ = 0;
    // EMPL-1a: a re-tasked module must not inherit the previous route's
    // attack run (same rule as the station hold above).
    attack_engaged_ = false;
    // INIT-2h: every set_route prints under the probe — the measured
    // OCASTRIKE flight's hold re-armed twice after its release and the
    // flight cycled the anchor->delivery arc for 75 min; without this
    // print the re-router is invisible (no capture events, no phase
    // change, no disarm — the route replacement resets the hold state
    // and the cycle restarts silently).
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        std::fprintf(stderr,
                     "[setroute] n=%zu first (%.0f,%.0f) last (%.0f,%.0f)\n",
                     route_.size(),
                     route_.empty() ? -1.0 : route_.front().position.x,
                     route_.empty() ? -1.0 : route_.front().position.y,
                     route_.empty() ? -1.0 : route_.back().position.x,
                     route_.empty() ? -1.0 : route_.back().position.y);
    }
    // NAV-B: the first leg emanates from where the aircraft is when the
    // FIRST update() runs (see update() — set_route can be called before
    // any state has been cached, e.g. the Enroute start-phase handoff,
    // so the anchor cannot be resolved here).
    leg_initialized_ = false;
    // An empty route completes immediately: the aircraft has nowhere to go.
    if (route_.empty()) {
        sm_.process(NavigationEvent::WaypointCaptured);
    }
}
void NavigationModule::resume_from(std::size_t index) {
    if (route_.empty() || index == 0 || index >= route_.size()) return;
    wp_index_ = index;
    wp_timer_ = 0.0;
    holding_ = false;
    station_done_ = false;
    station_elapsed_ = 0.0;
    loop_start_ = loop_end_ = 0;
    attack_engaged_ = false;
    leg_from_ = geo::WorldPosition{route_[index - 1].position.x,
                                   route_[index - 1].position.y,
                                   route_[index - 1].position.z};
}


// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput NavigationModule::update(double dt, const flight::IAircraftState* state)
{
    cache_aircraft_state(state);
    wp_timer_ += dt;
    // P7 — the station hold's clock runs while the hold is armed (the
    // racetrack keeps flying; the hold is what the loop IS).
    // The stabilized hold does not burn its clock: the receiver owns
    // the tanker for the protocol's duration (the release path resumes
    // the racetrack with the remaining station time).
    if (holding_ && !contact_stabilized_) station_elapsed_ += dt;

    // NAV-B: resolve the first leg's anchor on the first cached update —
    // the leg emanates from where the aircraft actually is (an offset
    // spawn like course_intercept's 8k ft right offset is then corrected
    // by the cross-track law against the course THROUGH the aircraft,
    // rather than producing a degenerate origin-anchored line).
    //
    // Exception (spawn-on-leg consolidation, what a real FMS does on route
    // activation): if the aircraft is ALREADY past wp0 and within the
    // abeam window of the wp0->wp1 line, it is established on leg 1 —
    // anchor there and skip wp0. Otherwise the module would fly a course
    // through itself toward wp1 and the cross-track law would never
    // correct the offset (the exact "homing" behavior NAV-B removes).
    if (!leg_initialized_ && !route_.empty()) {
        leg_initialized_ = true;
        leg_from_ = current_position_;
        // T3: a splice resume (wp_index_ > 0 — the brain's ONE route
        // decision) is authoritative; the spawn-on-leg consolidation
        // below is for the un-spliced activation (the cursor still at
        // the route start) and must not override it. Without this guard
        // the consolidation dragged every resumed cursor back to wp1 —
        // a deaggregate materialized at the route's recovery end flew
        // the tail legs instead of completing the route.
        if (route_.size() >= 2 && wp_index_ == 0) {
            const auto& a = route_[0].position;
            const auto& b = route_[1].position;
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
            const double ux = dx / len, uy = dy / len;
            const double along = (current_position_.x - a.x) * ux
                               + (current_position_.y - a.y) * uy;
            const double xte0 = (current_position_.x - a.x) * uy
                              + (current_position_.y - a.y) * (-ux);
            if (along > 0.0 && std::abs(xte0) < abeam_capture_ft) {
                wp_index_ = 1;
                leg_from_ = a;
            }
        }
    }

    // ROUTE-HOLD telemetry — the F4_LAND_DEBUG pattern (1 Hz, gated on
    // the env var; the module carries no entity id, so the brain's
    // [splice] prints bracket the phases and the position correlates).
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_nav = 0;
        if (++dbg_nav % 60 == 1) {
            std::fprintf(stderr,
                         "[nav] wp %zu/%zu xte %.0f hdg %.1f"
                         " alt %.0f vcas %.0f\n",
                         wp_index_, route_.size(),
                         cross_track_ft(),
                         current_heading_rad_ * 57.29577951308232,
                         current_alt_msl_ft_, current_vcas_kts_);
        }
    }

    for (int iter = 0; iter < 8; ++iter) {
        const auto before = sm_.current();
        if (sm_.current() == NavigationState::ToWaypoint) {
            check_waypoint_capture();
        }
        if (sm_.current() == before) break;
    }

    // EMPL-1a: engage the attack run the tick its delivery waypoint
    // becomes current — the virtual leg anchors where the aircraft
    // ACTUALLY is (after capture/consolidation, so a spawn established
    // mid-leg engages from the consolidated position, the same rule as
    // leg_from_ above). Keyed by index: the same waypoint never
    // re-anchors (a re-armed stick on a re-flown leg keeps its line).
    if (sm_.current() == NavigationState::ToWaypoint &&
        wp_index_ < route_.size() &&
        is_ag_delivery_action(route_[wp_index_].action) &&
        (!attack_engaged_ || attack_from_wp_ != wp_index_)) {
        attack_from_ = current_position_;
        attack_from_wp_ = wp_index_;
        attack_engaged_ = true;
    }

    switch (sm_.current()) {
        case NavigationState::ToWaypoint:
            return controls_for_waypoint();
        case NavigationState::Done:
            return {};  // no control output — sequencer takes over
    }
    return {};
}

// ============================================================================
// State caching + transitions
// ============================================================================

void NavigationModule::cache_aircraft_state(const flight::IAircraftState* state)
{
    if (!state) return;
    current_position_ = geo::WorldPosition(
        state->position_east_ft(), state->position_north_ft(), state->altitude_msl_ft());
    current_alt_msl_ft_ = state->altitude_msl_ft();
    current_vcas_kts_ = state->vcas_kts();
    current_heading_rad_ = state->heading_rad();
    current_pitch_rad_ = state->pitch_angle_rad();
    current_roll_rad_ = state->roll_angle_rad();
    current_roll_rate_radps_ = state->roll_rate_radps();
    current_pitch_rate_radps_ = state->pitch_rate_radps();
    current_vs_fpm_ = state->vertical_speed_fpm();
}

void NavigationModule::check_waypoint_capture()
{
    // INIT-1d: the armed-stick capture hold (see hold_delivery_capture
    // in the header) — a delivery waypoint whose stick is armed and
    // unfallen is not sequenced by the speed-proportional radius; the
    // attack leg flies INTO the aim and the brain lifts the hold when
    // the stick completes.
    if (hold_delivery_capture_ && wp_index_ < route_.size() &&
        is_ag_delivery_action(route_[wp_index_].action)) {
        return;
    }
    if (wp_index_ >= route_.size()) return;  // nothing left

    const auto& target = route_[wp_index_].position;
    const double dx = target.x - current_position_.x;
    const double dy = target.y - current_position_.y;
    const double dist = std::sqrt(dx * dx + dy * dy);

    // --- NAV-B: turn-anticipation lead (primary capture rule) ---
    //
    // A bank-limited aircraft cannot fly a corner: at 300 kts and a
    // 30-deg bank the turn radius is ~13,800 ft. Sequencing AT the
    // waypoint guarantees the next leg starts with the full turn still
    // to fly (the pursuit overshoot the traces show). The textbook fix
    // is to switch early by the lead distance
    //     lead = R * tan(|dtheta|/2),  R = v^2 / (g * tan(bank_max))
    // where dtheta is the course change from the CURRENT leg onto the
    // next — the aircraft begins the turn such that the arc rolls out
    // tangent ON the next leg's centerline. Only applies when there IS
    // a next leg; the LAST waypoint keeps the plain capture/abeam rules
    // (nothing to establish on afterwards — the sequencer takes over).
    // A-G (M5): a DELIVERY-action waypoint (WP_STRIKE / BOMB / GNDSTRIKE /
    // NAVSTRIKE / SEAD) is a MUST-FLY point — the release trigger keys on
    // the path through it, and a corner cut that bends the path wide of
    // the strike point starves the envelope (the first TestCamp A-G QC
    // run: the turn lead put the closest approach 7,160 ft wide of the
    // target with a ~5,700 ft envelope — no release). Zero lead on
    // delivery waypoints: fly THROUGH the point, turn after.
    // EMPL-2: a CAMPAIGN WP_REFUEL waypoint is must-fly for the same
    // reason — it IS the declared rendezvous, the point the receiver's
    // pairing and join are keyed on (a 180° corner's clamped lead
    // sequenced the waypoint before the receiver ever flew its leg:
    // the e2e catch).
    // ROUTE-HOLD: the join-stack orbit this tranche synthesizes is a
    // twelve-point circle (see campaign_bridge) precisely so this
    // must-fly contract costs nothing — thirty-degree fly-through
    // corners leave ~700-ft bows, where a racetrack's ninety-degree
    // corners left 4,000-20,000 ft for the whole 45-minute station.
    if (wp_index_ + 1 < route_.size() &&
        !is_ag_delivery_action(route_[wp_index_].action) &&
        !is_campaign_refuel_action(route_[wp_index_].action)) {
        constexpr double GRAVITY_FPS2 = 32.174;
        // NAV-B: turn geometry needs TRUE airspeed — the aircraft turns at
        // TAS, but the interface only exposes CAS. At 10,000 ft the ISA
        // density ratio makes TAS ~16% above CAS, i.e. the turn radius
        // (and lead) are ~35% bigger than a CAS computation predicts; the
        // CAS-based lead left every 90-deg corner of standard_rate_turn
        // ~3,300 ft wide (the nominal arc assumes R = 13,907 ft at "300
        // kts"; the aircraft was actually flying R ~ 18,750 at TAS 348).
        // Estimate TAS from ISA troposphere density: sigma = (1-h/145442)^4.2561.
        const double alt_ft = std::max(0.0, std::min(36000.0, current_alt_msl_ft_));
        const double sigma = std::pow(1.0 - alt_ft / 145442.0, 4.2561);
        const double tas_fps =
            std::max(150.0, current_vcas_kts_ * 1.68781 / std::sqrt(std::max(0.3, sigma)));
        const double R = tas_fps * tas_fps
                       / (GRAVITY_FPS2 * std::tan(air_steering.max_bank_rad));
        const double crs_in = AirSteering::bearing_to(leg_from_, target);
        const double crs_out = AirSteering::bearing_to(target,
                                                route_[wp_index_ + 1].position);
        const double dtheta = std::abs(AirSteering::heading_error(crs_out,
                                                                  crs_in));
        turn_lead_ft_ = std::clamp(R * std::tan(dtheta / 2.0)
                                       + turn_lead_lag_s * tas_fps,
                                   0.0, turn_lead_max_ft);
    } else {
        turn_lead_ft_ = 0.0;
    }

    // Off-nose (abeam) capture. A fast jet with a bank limit cannot always
    // turn tightly enough to fly directly over a waypoint — pure-pursuit
    // radius capture would orbit it forever (turn radius at 370 kts and
    // 30 deg bank is ~21,000 ft). Once the waypoint is well off the nose
    // AND within the abeam window, sequence to the next one.
    //
    // The dwell timer guards the capture: right after sequencing to
    // waypoint N+1, it can legitimately be >90 deg off the nose and
    // inside the window — without the guard the module would insta-skip
    // it while still heading away. After min_wp_dwell_s the aircraft has
    // turned toward it (30 s at ~3 deg/s covers ~90 deg), so an
    // off-nose >80 deg then genuinely means "passed it". The timer also
    // guarantees no orbit deadlock: by 30 s into a pursuit orbit (which
    // holds the target near 90 deg off the nose) the rule fires.
    const double bearing = AirSteering::bearing_to(current_position_, target);
    const double off_nose = std::abs(AirSteering::heading_error(bearing,
                                                                current_heading_rad_));

    // NAV-B2: the lead capture requires being roughly ESTABLISHED on the
    // inbound leg. Turn anticipation assumes the aircraft arrives on the
    // leg; sequencing mid-correction hands the next corner an unsettled
    // aircraft (standard_rate_turn: the S1 lead fired while the E leg was
    // still 700 ft out and 5 deg off — the corner then chased the S course
    // from a bad position for the whole next leg). Not established? Keep
    // flying the leg; the plain capture radius and abeam rules still
    // sequence the waypoint (late beats inherited chaos).
    const double along_in = (current_position_.x - leg_from_.x) * 0.0;  // unused
    (void)along_in;
    const double xte_now = cross_track_ft();
    // Tranche 37: relaxed from 400 to 1500 ft. At 300 kts the cross-track
    // during a turn easily exceeds 400 ft (0.8 s of lateral velocity) —
    // the tight check blocked the lead capture mid-turn, forcing the
    // aircraft to fly past the waypoint without sequencing.
    // (ROUTE-HOLD lesson: widening this gate inside a station hold to
    // let the turn anticipation fire off a recovering leg sequenced
    // every corner UNSETTLED and measured worse everywhere — the
    // "late beats inherited chaos" rule stands.)
    const bool established_enough = std::abs(xte_now) < 1500.0;
    // Tranche 37: speed-proportional capture radius. At 300 kts the old
    // fixed 3000 ft is 3 s of flight — the aircraft flies through it in
    // one tick. Scale by 10× the CAS (300 kts → 3000 ft, 350 → 3500 ft),
    // floored at the config value.
    const double effective_capture = std::max(capture_radius_ft,
                                               10.0 * current_vcas_kts_);
    const bool captured =
        (dist < turn_lead_ft_ && established_enough) ||
        dist < effective_capture ||
        (wp_timer_ > min_wp_dwell_s && dist < abeam_capture_ft &&
         off_nose > abeam_bearing_rad);

    // EMPL-2 — contact stabilization gates the capture: a stabilized
    // tanker flies its leg STRAIGHT through the corner (see the header
    // note); the release path re-forms the racetrack.
    if (captured && !contact_stabilized_) {
        // NAV-B: the new leg emanates from the waypoint we just captured.
        leg_from_ = route_[wp_index_].position;

        // P7 — the station hold. Capturing a RACETRACK ANCHOR
        // (station_time_s > 0, loop_waypoints ≥ 2, span fits the
        // route) arms the one-shot hold: loop_start_ is the anchor,
        // loop_end_ the span's last corner. The flag pair guards the
        // re-captures every wrap produces (the anchor is re-flown each
        // lap — re-arming would restart the timer forever).
        const auto& captured_wp = route_[wp_index_];
        if (!holding_ && !station_done_ && captured_wp.station_time_s > 0.0 &&
            captured_wp.loop_waypoints >= 2 &&
            wp_index_ + captured_wp.loop_waypoints <= route_.size()) {
            holding_ = true;
            station_elapsed_ = 0.0;
            loop_start_ = wp_index_;
            loop_end_ = wp_index_ + captured_wp.loop_waypoints - 1;
        }

        ++wp_index_;
        wp_timer_ = 0.0;
        // P7 — the wrap check rides BEFORE the completion check: a hold
        // whose span ends at the route's LAST waypoint must wrap back to
        // the anchor while the timer runs (the circuit IS the hold), not
        // complete the route. The Step-15 support-brain E2E caught the
        // ordering: the old else-if let a span-that-is-the-whole-route
        // fall into the Done path on the first lap and the "station"
        // silently became a fly-through. The release path is unchanged:
        // an expired hold at the last corner completes the route (the
        // orbit-to-end shape — the flight ends in the stack), a hold
        // with a tail sequences out of the span.
        if (holding_ && wp_index_ - 1 == loop_end_) {
            if (station_elapsed_ < route_[loop_start_].station_time_s) {
                wp_index_ = loop_start_;
            } else {
                holding_ = false;
                station_done_ = true;
                if (wp_index_ >= route_.size()) {
                    sm_.process(NavigationEvent::WaypointCaptured);
                }
            }
        } else if (wp_index_ >= route_.size()) {
            sm_.process(NavigationEvent::WaypointCaptured);
        }
    }
}

// ============================================================================
// Contact stabilization (EMPL-2)
// ============================================================================
void NavigationModule::set_contact_stabilized(bool on) noexcept
{
    if (contact_stabilized_ == on) return;
    contact_stabilized_ = on;
    if (on || !holding_) return;
    // Release: the leg's corner may now be far behind — treat it
    // captured and turn to the next racetrack corner. This is the same
    // wrap rule the capture path runs: the circuit re-forms while the
    // station clock runs, or the hold releases and the route resumes.
    leg_from_ = route_[wp_index_].position;
    ++wp_index_;
    wp_timer_ = 0.0;
    if (wp_index_ >= route_.size()) {
        sm_.process(NavigationEvent::WaypointCaptured);
    } else if (wp_index_ - 1 == loop_end_) {
        if (station_elapsed_ < route_[loop_start_].station_time_s) {
            wp_index_ = loop_start_;
        } else {
            holding_ = false;
            station_done_ = true;
        }
    }
}

// ============================================================================
// Control logic
// ============================================================================

AIControlOutput NavigationModule::controls_for_waypoint() const
{
    if (wp_index_ >= route_.size()) return {};
    const auto& wp = route_[wp_index_];

    const double desired_hdg = nav_heading_rad();
    // Slow down for big course changes: turn radius scales with V^2, and
    // a slow turn is what lets the aircraft actually converge on the next
    // leg instead of orbiting the waypoint.
    // NAV-E: clean-airframe speed floor. Below the min-drag speed the
    // aircraft flies the backside of the power curve, where speed control
    // via throttle is unstable and the altitude/speed loops pump a
    // +-400-500 ft phugoid (standard_rate_turn at 250 kts: throttle pinned
    // at the floor, speed riding 254-268, altitude 9,580-10,550 forever;
    // the same aircraft at 300 kts holds +-26 ft — course_intercept).
    // Enroute legs never need slow flight: the landing module owns the
    // drag-curve backside with its own floors and flaps.
    // Tranche 37: lowered from 270 to 220. The old 270 floor prevented
    // deceleration for the approach transition — the aircraft arrived at
    // the approach entry fix at 270+ kts (way too fast for a 170-213 kt
    // approach). 220 stays above the clean-airframe power-curve backside
    // for fighters while allowing the waypoint speeds (200-350) to actually
    // command what they specify. The landing module owns the slow-flight
    // regime below this with its own floors + flaps.
    constexpr double ENROUTE_SPEED_FLOOR_KTS = 200.0;  // Tranche 38: lowered from 220 for approach deceleration
    double speed = std::max(wp.speed_kts, ENROUTE_SPEED_FLOOR_KTS);
    const double hdg_err = std::abs(AirSteering::heading_error(desired_hdg,
                                                               current_heading_rad_));
    if (hdg_err > turn_slow_hdg_rad) {
        // The turn slowdown is ALSO floored: slowing to the old 250-kt
        // turn_speed mid-corner put the aircraft back on the power-curve
        // backside exactly when the turn needs stable energy — the corner
        // geometry then wanders (S-leg residual ~970 ft vs ~250 when the
        // corner is flown on the front side).
        speed = std::min(speed, std::max(turn_speed_kts, ENROUTE_SPEED_FLOOR_KTS));
    }

    // INIT-1f: slow for the turn AT this waypoint while still approaching
    // it. The hdg_err gate above only fires once the turn has started,
    // and a jet at 400+ kts cannot decelerate inside the turn: the
    // measured BARCAP2 recovery turn was flown at 402 kts (R ~30,000 ft,
    // the excursion 30,700 ft, the window's final quarter caught the
    // recovery). The turn's size is knowable on approach — the course
    // change from the incoming leg (the anchor's bearing) to the outgoing
    // one — so command the corner speed within the deceleration distance
    // (60,000 ft; measured: the peaks fell 35-55% — AWACS 28,030 ->
    // 12,704, BARCAP2 29,072 -> 16,271). A 120k gate measured the same
    // or slightly worse (the aircraft slows below the deceleration need
    // and burns arc time). The remaining excursion is the GEOMETRIC
    // floor: a 120-deg course change at the slowest legal enroute speed
    // (250 kts, R ~12,000 ft) deviates ~R from the new line by
    // construction — the 2,000-ft band is unreachable mid-turn; the
    // turn-window semantics belong to the metric doctrine.
    if (wp_index_ + 1 < route_.size() && wp_index_ > 0) {
        const double in_crs = AirSteering::bearing_to(leg_from_, wp.position);
        const double out_crs =
            AirSteering::bearing_to(wp.position,
                                    route_[wp_index_ + 1].position);
        const double dtheta = std::abs(AirSteering::heading_error(out_crs,
                                                                  in_crs));
        if (dtheta > turn_slow_hdg_rad) {
            const double dx = wp.position.x - current_position_.x;
            const double dy = wp.position.y - current_position_.y;
            if (dx * dx + dy * dy < 60000.0 * 60000.0) {
                speed = std::min(speed,
                                 std::max(turn_speed_kts,
                                          ENROUTE_SPEED_FLOOR_KTS));
            }
        }
    }

    // Tranche 39: lowered from 3000 to 500. The old 3000 ft MSL floor
    // overrode the 1500 ft pattern altitude of the radar pattern's downwind
    // leg — the aircraft never descended below 3000 ft during enroute, arriving
    // at the approach entry fix at 3000+ ft instead of 1500 ft (too high for
    // a glideslope-from-below intercept). 500 ft MSL is above Korea's coastal
    // terrain (near sea level) and below the 1500 ft pattern altitude.
    constexpr double TERRAIN_CLEARANCE_FLOOR_MSL = 3000.0;  // Tranche 43: raised from 500 — Korea has mountains
    // The LAST waypoint (approach entry fix) is NOT floored — it needs its
    // specified altitude (1500 ft) for the glideslope-from-below intercept.
    // The landing module's altitude gate (check_fix_reached) handles the
    // descent to pattern altitude before the intercept begins.
    // EMPL-1a: a DELIVERY waypoint is not floored either — it flies its
    // own altitude. The campaign bridge floors delivery waypoints at
    // 1,500 ft MSL (kMinDeliveryWaypointAltFt — the release envelope's
    // design dz); the 3,000 ft terrain floor silently overrode it and
    // DOUBLED the throw (dz 3,000 -> R ~9,200 ft at 400 kts), which
    // stretched the stick's along-track spread and halved the angular
    // budget at the release boundary. The campaign world is flat (the
    // save convention — objectives at z=0, no terrain in the world), so
    // the 1,500 ft delivery altitude is safe there; a terrain-aware
    // world owns its own delivery floor (the M4.5 profile tranche).
    const bool is_last_wp = (wp_index_ >= route_.size() - 1);
    const bool is_delivery_wp = is_ag_delivery_action(wp.action);
    double target_alt =
        (is_last_wp || is_delivery_wp)
            ? wp.position.z
            : std::max(wp.position.z, TERRAIN_CLEARANCE_FLOOR_MSL);
    return air_steering.steer(desired_hdg, target_alt, speed,
                              steering_input());
}

// ============================================================================
// NAV-B: LNAV heading — leg course + cross-track correction
// ============================================================================

double NavigationModule::nav_heading_rad() const
{
    if (wp_index_ >= route_.size()) return current_heading_rad_;
    const auto& wp = route_[wp_index_];

    // EMPL-1a: a delivery waypoint's leg is an ATTACK RUN, not a
    // navigation leg — and it is flown as a VIRTUAL LNAV leg THROUGH
    // the aim, anchored at the engagement position (frozen in update()).
    //
    // History: NAV-B's original leg law left a ~400 ft corner residual
    // through the release point (the trigger starved — exit 4); EMPL-1's
    // interim fix was pure pursuit at the point, which restored the
    // release but EXPOSED the next defect: a pursuit curve of a
    // stationary point CONSERVES its entry lateral offset almost to the
    // target (bearing rate ~ offset/distance — the offset only kills in
    // the last few hundred feet). The TestCamp INTSTRIKE stick released
    // ~6 deg off bearing at ~6,700 ft: 675 of the 682-889 ft miss was
    // LATERAL, under 100 ft along-track, and every bomb missed the
    // feature grid wide (features_destroyed=0, EMPL-1a's gate).
    //
    // The virtual leg fixes the convergence: the NAV-B cross-track law
    // on the line attack_from_ -> aim drives the offset down
    // EXPONENTIALLY and holds the line, and the line ENDS at the aim —
    // so at the release boundary the track passes through the aim in
    // BOTH axes, which is exactly the geometry the bomb's straight-line
    // flyout assumes. Every non-delivery leg keeps the LNAV law
    // byte-identically.
    if (is_ag_delivery_action(wp.action)) {
        if (attack_engaged_) {
            const double adx = wp.position.x - attack_from_.x;
            const double ady = wp.position.y - attack_from_.y;
            const double alen = std::sqrt(adx * adx + ady * ady);
            if (alen >= attack_min_virtual_leg_ft) {
                const double course =
                    AirSteering::bearing_to(attack_from_, wp.position);
                const double leg_len = std::max(1.0, alen);
                // INIT-1d: past the aim the leg's extension runs AWAY
                // from the target, and the line law (course + cross-track
                // correction) has no along-track reversal — the armed
                // aircraft chased the extension for the rest of the run
                // with the stick unfallen (the measured SAD flight:
                // armed 120 min, closest approach 8,715 ft, inside the
                // release range the whole time). Command pursuit of the
                // aim instead — the turn back re-joins the leg inbound,
                // where the release gate fires.
                //
                // INIT-2h diagnosis (the [gate] probe): the immediate
                // pursuit swings the reversal around at the BANK-LIMITED
                // turn radius (262 kts / 25 deg = 1.94 deg/s, R ~13,000
                // ft) into a pursuit orbit tangent to the aim — the
                // measured flight crossed ABEAM 623 times in range with
                // the release cone refusing every pass (75 min, stick
                // unfallen). The bounded re-attack extension (fly the
                // extension outbound 10-20k, then pursue) fixed the
                // pass quality (the re-joins released at pipper 49-335
                // ft) but measured MAP-NEUTRAL (the matrix totals
                // identical at both 10k and 20k; the extra re-attack
                // time pushed the 20k variant's TOT/recovery late) and
                // was REVERTED — the orbit family's flights (the
                // delivery-first route shapes) fail for the
                // delivery-first reasons, not the pursuit. The [gate]
                // probe stays for the family's next tranche.
                const double along =
                    (current_position_.x - attack_from_.x) * (adx / alen)
                  + (current_position_.y - attack_from_.y) * (ady / alen);
                if (along > leg_len) {
                    return AirSteering::bearing_to(current_position_,
                                                   wp.position);
                }
                // Right unit vector of the virtual leg (ENU; compass
                // course convention) — the same construction the real
                // leg uses below.
                const double right_x = ady / leg_len;
                const double right_y = -adx / leg_len;
                const double xte =
                    (current_position_.x - attack_from_.x) * right_x
                  + (current_position_.y - attack_from_.y) * right_y;
                // NAV-B/B2 correction (mirrored from the leg law below;
                // the gains are the module's own, so a retune moves both).
                // INIT-2g: the attack run now uses the leg law's
                // DISTANCE-SCHEDULED intercept (the ROUTE-HOLD ramp to
                // max_intercept_far_rad, the damper faded by the same
                // schedule). The flat 0.35-rad clamp was the delivery-
                // first orbit: an air-spawned flight (the measured SAD
                // lead, 12,000 ft up heading away from its aim) drifted
                // 24,000-38,000 ft off the virtual leg during the initial
                // turn and recovered at the flat clamp's ~140 ft/s
                // fixed point — THREE full swings (~150 s each, the
                // 13,700-ft turn radius at the 262-kt slowdown) before
                // the stick fell at 15 min. The far-field 0.95-rad cut
                // converges the same excursion in one pass, and the
                // near-field law stays byte-identical (the ramp starts
                // at xte_gain_ft, where the releases live).
                const double axte_a = std::abs(xte);
                const double ta =
                    std::clamp((axte_a - xte_gain_ft) / (2.0 * xte_gain_ft),
                               0.0, 1.0);
                const double lim_a =
                    max_intercept_rad
                  + (max_intercept_far_rad - max_intercept_rad) * ta;
                const double corr_p =
                    std::clamp(std::atan2(-xte, xte_gain_ft),
                               -lim_a, lim_a);
                const double closing = AirSteering::heading_error(
                    current_heading_rad_, course);
                const double corr =
                    std::clamp(corr_p - (1.0 - ta) * xte_damp_gain
                                              * std::sin(closing),
                               -lim_a, lim_a);
                return course + corr;
            }
        }
        // Degenerate virtual leg (never engaged — e.g. a bare
        // nav_heading_rad() probe — or engaged too close to the aim for
        // a stable anchored course): pure pursuit at the point, exact
        // where the offset has already converged.
        return AirSteering::bearing_to(current_position_, wp.position);
    }

    // --- Desired heading from the LEG, with cross-track correction ---
    //
    // Old law: desired_hdg = bearing(me -> wp) — pure pursuit, homing.
    // New law: desired_hdg = leg_course + clamp(atan2(-xte, xte_gain_ft),
    //                                           +/-max_intercept_rad)
    // where leg_course is bearing(leg_from_ -> wp) and xte is the signed
    // cross-track distance from that course line (+ = right of course).
    // On the centerline the commanded heading IS the course — the aircraft
    // ESTABLISHES and flies the leg, which pursuit guidance never does.
    // Off to one side it cuts a stable intercept angle toward the course
    // (bounded by the distance-scheduled limit, matched to the bank
    // limit near the line), which converges without the bow-then-overshoot
    // of a pursuit curve.
    const double course = AirSteering::bearing_to(leg_from_, wp.position);
    const double leg_dx = wp.position.x - leg_from_.x;
    const double leg_dy = wp.position.y - leg_from_.y;
    const double leg_len = std::max(1.0, std::sqrt(leg_dx * leg_dx + leg_dy * leg_dy));
    // Right unit vector of the leg (ENU; compass course convention).
    const double right_x = leg_dy / leg_len;
    const double right_y = -leg_dx / leg_len;
    const double xte = (current_position_.x - leg_from_.x) * right_x
                     + (current_position_.y - leg_from_.y) * right_y;
    // ROUTE-HOLD: the intercept limit is distance-scheduled. The flat
    // 20-deg clamp converged a departure-scale excursion (the measured
    // 21,000-34,000 ft the runway-vs-route reversals and NAV-B's own
    // 22,000-ft turn-lead corner cuts produce) at the damp-fixed-point
    // ~140 ft/s — 3-4 minutes of chase that carried the transient into
    // the PATH clause's final quarter on 52 of 86 coverage-map flights.
    // t: 0 at or inside xte_gain_ft (the law below is byte-identical to
    // the flat clamp there), 1 at 3*xte_gain_ft — the ramp must complete
    // by the mid-field because that is where the REPEATED excursions
    // live (the loiter-stack leg changes re-anchor 4,000-8,000 ft off
    // and their windows are too short for a throttled recovery; the
    // first smoke run still failed three of four BARCAPs at
    // 4,160-11,047 ft with the gentler 4x ramp). The 2x ramp — the full
    // cut at 10,000 ft — measured the same matrix but chased the BVR
    // two-ship's rejoining wingman past its 4,000-ft station gate
    // (CombatIntegration.AiVersusAiTwoShipBvrFight); 3x is the
    // calibration point on both harnesses.
    const double axte = std::abs(xte);
    const double t = std::clamp((axte - xte_gain_ft) / (2.0 * xte_gain_ft),
                                0.0, 1.0);
    const double lim = max_intercept_rad
                     + (max_intercept_far_rad - max_intercept_rad) * t;
    // NAV-B2: track-rate damping. The bare atan2 correction is P-only on
    // cross-track; through the heading loop's lag it converges by
    // OVERSHOOTING (~600 ft past each zero-crossing on the square route —
    // the aircraft crosses the course with 13-17 deg of residual heading
    // error, then spends 10-15 s snaking back, and the next corner's turn
    // anticipation can fire mid-snake, inheriting an unsettled leg).
    // Subtracting a term proportional to the CURRENT closing rate
    // (sin of track offset) is phase LEAD: it eases off the intercept as
    // the aircraft converges, canceling the loop lag. Zero when settled.
    // ROUTE-HOLD: the damper fades out with the same schedule t. Its own
    // fixed point corr = corr_p - g*sin(corr) permanently throttles a
    // SATURATED intercept to ~12 deg (the AWACS departure chase: roll_cmd
    // 0.00 at xte 22,700 ft, closing exactly v*sin(12.9 deg) — the
    // measured 140 ft/s — for 224 s). Full strength near the line, where
    // the zero-crossing overshoot it was tuned for lives; zero far out,
    // where the steady intercept IS the maneuver.
    const double corr_p = std::clamp(std::atan2(-xte, xte_gain_ft),
                                     -lim, lim);
    const double closing = AirSteering::heading_error(current_heading_rad_,
                                                      course);
    const double corr = std::clamp(corr_p
                                     - (1.0 - t) * xte_damp_gain
                                                 * std::sin(closing),
                                   -lim, lim);
    return course + corr;
}

double NavigationModule::cross_track_ft() const
{
    if (wp_index_ >= route_.size()) return 0.0;
    const auto& wp = route_[wp_index_];
    const double leg_dx = wp.position.x - leg_from_.x;
    const double leg_dy = wp.position.y - leg_from_.y;
    const double leg_len = std::max(1.0, std::sqrt(leg_dx * leg_dx + leg_dy * leg_dy));
    const double right_x = leg_dy / leg_len;
    const double right_y = -leg_dx / leg_len;
    return (current_position_.x - leg_from_.x) * right_x
         + (current_position_.y - leg_from_.y) * right_y;
}

NavigationModule::AttackLegDebug NavigationModule::attack_leg_debug() const
{
    AttackLegDebug d;
    if (!attack_engaged_ || wp_index_ >= route_.size() ||
        !is_ag_delivery_action(route_[wp_index_].action)) {
        return d;
    }
    const auto& wp = route_[wp_index_];
    const double adx = wp.position.x - attack_from_.x;
    const double ady = wp.position.y - attack_from_.y;
    d.length_ft = std::sqrt(adx * adx + ady * ady);
    d.engaged = d.length_ft >= attack_min_virtual_leg_ft;
    d.course_rad = AirSteering::bearing_to(attack_from_, wp.position);
    d.wp_z_ft = wp.position.z;
    const auto& dbg = air_steering.last_debug();
    d.vs_target_fpm = dbg.vs_target_fpm;
    d.gamma_ff_deg = dbg.gamma_ff_rad * 57.2957795;
    d.alt_err_ft = dbg.alt_err_ft;
    d.theta_target_deg = dbg.theta_target_rad * 57.2957795;
    d.speed_err_kt = dbg.speed_err_kt;
    const double inv = 1.0 / std::max(1.0, d.length_ft);
    d.along_ft = ((current_position_.x - attack_from_.x) * adx
                + (current_position_.y - attack_from_.y) * ady) * inv;
    d.xte_ft = ((current_position_.x - attack_from_.x) * (ady * inv)
              + (current_position_.y - attack_from_.y) * (-adx * inv));
    return d;
}

AirSteering::Input NavigationModule::steering_input() const noexcept
{
    AirSteering::Input in;
    in.position = current_position_;
    in.heading_rad = current_heading_rad_;
    in.pitch_rad = current_pitch_rad_;
    in.roll_rad = current_roll_rad_;
    in.roll_rate_radps = current_roll_rate_radps_;
    in.pitch_rate_radps = current_pitch_rate_radps_;
    in.vs_fpm = current_vs_fpm_;
    in.vcas_kts = current_vcas_kts_;
    in.alt_msl_ft = current_alt_msl_ft_;
    return in;
}

// ============================================================================
// Human-readable state name
// ============================================================================

std::string NavigationModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

} // namespace f4::ai::modules

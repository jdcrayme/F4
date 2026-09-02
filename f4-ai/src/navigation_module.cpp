// f4-ai/src/navigation_module.cpp
//
// NavigationModule implementation — waypoint following via AirSteering.

#include "f4/ai/modules/navigation_module.hpp"

#include <f4/ai/modules/strike_module.hpp>  // is_ag_delivery_action

#include <cmath>

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
    air_steering.path_gain = 0.0005;
    air_steering.vs_gain = 2.5;
    air_steering.max_vs_fpm = 1500.0;
    air_steering.roll_gain = 4.0;
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

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput NavigationModule::update(double dt, const flight::IAircraftState* state)
{
    cache_aircraft_state(state);
    wp_timer_ += dt;

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
        if (route_.size() >= 2) {
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

    for (int iter = 0; iter < 8; ++iter) {
        const auto before = sm_.current();
        if (sm_.current() == NavigationState::ToWaypoint) {
            check_waypoint_capture();
        }
        if (sm_.current() == before) break;
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
    if (wp_index_ + 1 < route_.size() &&
        !is_ag_delivery_action(route_[wp_index_].action)) {
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
    const bool established_enough = std::abs(xte_now) < 400.0;
    const bool captured =
        (dist < turn_lead_ft_ && established_enough) ||
        dist < capture_radius_ft ||
        (wp_timer_ > min_wp_dwell_s && dist < abeam_capture_ft &&
         off_nose > abeam_bearing_rad);

    if (captured) {
        // NAV-B: the new leg emanates from the waypoint we just captured.
        leg_from_ = route_[wp_index_].position;
        ++wp_index_;
        wp_timer_ = 0.0;
        if (wp_index_ >= route_.size()) {
            sm_.process(NavigationEvent::WaypointCaptured);
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
    constexpr double ENROUTE_SPEED_FLOOR_KTS = 270.0;
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

    constexpr double TERRAIN_CLEARANCE_FLOOR_MSL = 3000.0;
    double target_alt = std::max(wp.position.z, TERRAIN_CLEARANCE_FLOOR_MSL);
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
    // (bounded by max_intercept_rad, matched to the bank limit), which
    // converges without the bow-then-overshoot of a pursuit curve.
    const double course = AirSteering::bearing_to(leg_from_, wp.position);
    const double leg_dx = wp.position.x - leg_from_.x;
    const double leg_dy = wp.position.y - leg_from_.y;
    const double leg_len = std::max(1.0, std::sqrt(leg_dx * leg_dx + leg_dy * leg_dy));
    // Right unit vector of the leg (ENU; compass course convention).
    const double right_x = leg_dy / leg_len;
    const double right_y = -leg_dx / leg_len;
    const double xte = (current_position_.x - leg_from_.x) * right_x
                     + (current_position_.y - leg_from_.y) * right_y;
    // NAV-B2: track-rate damping. The bare atan2 correction is P-only on
    // cross-track; through the heading loop's lag it converges by
    // OVERSHOOTING (~600 ft past each zero-crossing on the square route —
    // the aircraft crosses the course with 13-17 deg of residual heading
    // error, then spends 10-15 s snaking back, and the next corner's turn
    // anticipation can fire mid-snake, inheriting an unsettled leg).
    // Subtracting a term proportional to the CURRENT closing rate
    // (sin of track offset) is phase LEAD: it eases off the intercept as
    // the aircraft converges, canceling the loop lag. Zero when settled.
    const double corr_p = std::clamp(std::atan2(-xte, xte_gain_ft),
                                     -max_intercept_rad, max_intercept_rad);
    const double closing = AirSteering::heading_error(current_heading_rad_,
                                                      course);
    const double corr = std::clamp(corr_p
                                     - xte_damp_gain * std::sin(closing),
                                   -max_intercept_rad, max_intercept_rad);
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

// f4-ai/src/landing_module.cpp
//
// LandingModule implementation — straight-in approach, landing, rollout,
// taxi-in. See header for the geometry conventions.

#include "f4/ai/modules/landing_module.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai::modules {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;
} // namespace

// ============================================================================
// State machine construction
// ============================================================================

LandingModule::LandingModule()
    : sm_(build_sm())
{
    // Approach tune: gentle and SMOOTH by design. The beam is a 3-deg
    // path over a 60,000 ft final — there is no need for fast tracking,
    // and fast demands RING: a high vs_gain saturates the VS cap and the
    // demand flips sign at each beam crossing, sustaining a ±3000 fpm
    // limit cycle through the FCS lag. Low gain + small bounded gamma
    // corrections converge without ringing.
    //
    // speed_damp is deliberately tiny here: it is a phugoid damper, but
    // with a big value it acts as a constant nose-down bias whenever the
    // aircraft is faster than target — on final that cancelled the
    // climb-back-to-beam command and the aircraft settled BELOW the beam
    // all the way to a short touchdown.
    air_steering.bank_gain = 1.2;
    air_steering.max_bank_rad = 0.44;    // ~25 deg: enough to close lateral S-turns
    air_steering.roll_gain = 3.0;
    air_steering.vs_gain = 5.0;
    air_steering.max_vs_fpm = 1400.0;
    air_steering.path_gain = 0.00008;
    air_steering.gamma_corr_limit = 0.07;
    air_steering.attitude_gain = 1.2;
    air_steering.speed_damp_rad_per_kt = 0.0008;
    air_steering.throttle_min = 0.15;
}

fsm::StateMachine<LandingState, LandingEvent>
LandingModule::build_sm()
{
    return typename fsm::StateMachine<LandingState, LandingEvent>::Builder()
        .initial(LandingState::RequestApproach)
        .state(LandingState::RequestApproach, "RequestApproach")
        .state(LandingState::ProceedToFix,    "ProceedToFix")
        .state(LandingState::InterceptFinal,  "InterceptFinal")
        .state(LandingState::OnFinal,         "OnFinal")
        .state(LandingState::Flare,           "Flare")
        .state(LandingState::Rollout,         "Rollout")
        .state(LandingState::TaxiIn,          "TaxiIn")
        .state(LandingState::Parked,          "Parked")
        .state(LandingState::GoAround,        "GoAround")

        .event_name(LandingEvent::ApproachGranted, "ApproachGranted")
        .event_name(LandingEvent::FixReached,      "FixReached")
        .event_name(LandingEvent::Established,     "Established")
        .event_name(LandingEvent::Flare,           "Flare")
        .event_name(LandingEvent::Touchdown,       "Touchdown")
        .event_name(LandingEvent::RunwayVacated,   "RunwayVacated")
        .event_name(LandingEvent::ParkedComplete,  "ParkedComplete")
        .event_name(LandingEvent::GoAround,        "GoAround")
        .event_name(LandingEvent::Reintercept,     "Reintercept")

        .on(LandingState::RequestApproach, LandingState::ProceedToFix,
            LandingEvent::ApproachGranted,
            nullptr, nullptr, "landing_clearance_received")
        .on(LandingState::ProceedToFix, LandingState::InterceptFinal,
            LandingEvent::FixReached,
            nullptr, nullptr, "approach_fix_reached")
        .on(LandingState::InterceptFinal, LandingState::OnFinal,
            LandingEvent::Established,
            nullptr, nullptr, "established_on_final")
        .on(LandingState::OnFinal, LandingState::Flare,
            LandingEvent::Flare,
            nullptr, nullptr, "flare_height")
        .on(LandingState::OnFinal, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "missed_approach")
        .on(LandingState::GoAround, LandingState::ProceedToFix,
            LandingEvent::Reintercept,
            nullptr, nullptr, "climbed_to_pattern_altitude")
        .on(LandingState::Flare, LandingState::Rollout,
            LandingEvent::Touchdown,
            nullptr, nullptr, "wheels_down")
        .on(LandingState::Rollout, LandingState::TaxiIn,
            LandingEvent::RunwayVacated,
            nullptr, nullptr, "slowed_to_taxi_speed")
        .on(LandingState::TaxiIn, LandingState::Parked,
            LandingEvent::ParkedComplete,
            nullptr, nullptr, "parking_spot_reached")

        // Entry actions
        .on_enter(LandingState::RequestApproach, [this](const LandingEvent&) {
            // Publish LandingRequest. At construction bus_ is null; initialize()
            // re-fires this via sm_.reset().
            if (bus_) {
                atc::LandingRequest req;
                req.aircraft_id = ownship_id_;
                bus_->publish(req);
            }
        })
        .on_enter(LandingState::ProceedToFix, [this](const LandingEvent&) {
            // Restart the abeam-capture dwell timer (guards the fix
            // capture — see check_fix_reached).
            fix_timer_ = 0.0;
        })
        .on_enter(LandingState::OnFinal, [this](const LandingEvent&) {
            // Established inbound: request clearance to land.
            if (bus_) {
                atc::ApproachClearance req;
                req.aircraft_id = ownship_id_;
                req.runway_id = runway_id_;
                req.approach_type = "VISUAL";
                bus_->publish(req);
            }
        })
        .on_enter(LandingState::GoAround, [this](const LandingEvent&) {
            if (bus_) {
                atc::GoAroundMessage msg;
                msg.aircraft_id = ownship_id_;
                msg.runway_id = runway_id_;
                msg.reason = cleared_to_land_ ? "threshold_overflown" : "not_cleared";
                bus_->publish(msg);
            }
        })
        .build();
}

// ============================================================================
// Initialization
// ============================================================================

void LandingModule::configure(const geo::WorldPosition& entry_fix,
                               std::vector<geo::WorldPosition> taxi_in_route) {
    entry_fix_ = entry_fix;
    taxi_in_route_ = std::move(taxi_in_route);
    taxi_wp_index_ = 0;
}

void LandingModule::initialize(std::uint64_t ownship_id,
                                entities::EntityWorld& world,
                                messaging::MessageBus& bus)
{
    ownship_id_ = ownship_id;
    world_ = &world;
    bus_ = &bus;

    bus.subscribe<atc::LandingClearance>([this](const atc::LandingClearance& msg) {
        if (msg.aircraft_id != ownship_id_) return;
        runway_id_ = msg.runway_id;
        runway_heading_rad_ = msg.runway_heading_rad;
        threshold_position_ = msg.threshold_position;
        threshold_alt_ft_ = msg.threshold_altitude_ft;
        glide_slope_angle_rad_ = msg.glide_slope_angle_rad;
        pattern_altitude_ft_ = msg.pattern_altitude_ft;
        if (sm_.current() == LandingState::RequestApproach) {
            sm_.process(LandingEvent::ApproachGranted);
        }
    });

    bus.subscribe<atc::ClearedToLand>([this](const atc::ClearedToLand& msg) {
        if (msg.aircraft_id == ownship_id_) {
            cleared_to_land_ = true;
        }
    });

    // Re-fire the RequestApproach entry action now that bus_ is set.
    sm_.reset();
}

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput LandingModule::update(double dt, const flight::IAircraftState* state)
{
    cache_aircraft_state(state);
    fix_timer_ += dt;

    for (int iter = 0; iter < 8; ++iter) {
        const auto before = sm_.current();
        switch (sm_.current()) {
            case LandingState::ProceedToFix:
                check_fix_reached();
                break;
            case LandingState::InterceptFinal:
                check_established();
                break;
            case LandingState::OnFinal:
                check_flare_or_goaround();
                break;
            case LandingState::Flare:
                check_touchdown();
                break;
            case LandingState::Rollout:
                check_runway_vacated();
                break;
            case LandingState::TaxiIn:
                check_taxi_in_progress();
                break;
            case LandingState::GoAround:
                // Climbed back to pattern altitude: re-enter the intercept.
                if (current_alt_msl_ft_ > pattern_altitude_ft_ + 500.0) {
                    sm_.process(LandingEvent::Reintercept);
                }
                break;
            default:
                break;
        }
        if (sm_.current() == before) break;
    }

    switch (sm_.current()) {
        case LandingState::RequestApproach: return controls_for_request_approach();
        case LandingState::ProceedToFix:    return controls_for_proceed_to_fix();
        case LandingState::InterceptFinal:
            return track_final(std::min(pattern_altitude_ft_, glide_slope_alt_ft()),
                               approach_speed_kts + 40.0);
        case LandingState::OnFinal: {
            // Proportional undershoot of the beam: ride ~8% BELOW it so
            // the offset shrinks geometrically with distance-to-go and
            // the convergence point lands at the threshold. Riding the
            // beam exactly leaves a residual offset at the flare (the
            // loop closes too slowly over the final miles) — touchdown
            // ends up thousands of feet short. Same idea as a localizer
            // intercept lead angle.
            const double dist = std::max(0.0, -course_along_ft());
            const double beam = threshold_alt_ft_
                + dist * std::tan(glide_slope_angle_rad_);
            const double target = threshold_alt_ft_
                + 0.92 * (beam - threshold_alt_ft_);
            return track_final(target, approach_speed_kts);
        }
        case LandingState::Flare:           return controls_for_flare();
        case LandingState::Rollout:         return controls_for_rollout();
        case LandingState::TaxiIn:          return controls_for_taxi_in();
        case LandingState::Parked:          return controls_for_parked();
        case LandingState::GoAround:        return controls_for_go_around();
    }
    return {};
}

// ============================================================================
// State caching
// ============================================================================

void LandingModule::cache_aircraft_state(const flight::IAircraftState* state)
{
    if (!state) return;
    current_position_ = geo::WorldPosition(
        state->position_east_ft(), state->position_north_ft(), state->altitude_msl_ft());
    current_alt_msl_ft_ = state->altitude_msl_ft();
    current_alt_agl_ft_ = state->altitude_agl_ft();
    current_vcas_kts_ = state->vcas_kts();
    current_heading_rad_ = state->heading_rad();
    current_pitch_rad_ = state->pitch_angle_rad();
    current_roll_rad_ = state->roll_angle_rad();
    current_vs_fpm_ = state->vertical_speed_fpm();
    on_ground_ = state->on_ground();
}

// ============================================================================
// Final-course geometry
// ============================================================================
// course_dir points from the threshold down the runway (the landing
// direction); course_right is 90 deg right of it. On approach the aircraft
// is at along < 0 (before the threshold), lateral > 0 right of centerline.

double LandingModule::course_along_ft() const {
    const double fx = std::sin(runway_heading_rad_);
    const double fy = std::cos(runway_heading_rad_);
    return (current_position_.x - threshold_position_.x) * fx
         + (current_position_.y - threshold_position_.y) * fy;
}

double LandingModule::course_lateral_ft() const {
    const double rx = std::cos(runway_heading_rad_);
    const double ry = -std::sin(runway_heading_rad_);
    return (current_position_.x - threshold_position_.x) * rx
         + (current_position_.y - threshold_position_.y) * ry;
}

double LandingModule::glide_slope_alt_ft() const {
    // Distance to the beam's aim point: the threshold plus the aim offset
    // (the beam reaches the ground beam_aim_offset_ft PAST the threshold,
    // like a real ILS — crossing height over the threshold is then ~130 ft
    // at 3 deg instead of 0, and the flare touches down inside the runway).
    const double dist = std::max(0.0, -course_along_ft() + beam_aim_offset_ft);
    return threshold_alt_ft_ + dist * std::tan(glide_slope_angle_rad_);
}

double LandingModule::localizer_heading_rad() const {
    // Right of centerline (lateral > 0) -> steer left (subtract correction).
    const double corr = std::clamp(localizer_gain * course_lateral_ft(),
                                   -max_localizer_corr_rad, max_localizer_corr_rad);
    return runway_heading_rad_ - corr;
}

// ============================================================================
// Transition checks
// ============================================================================

void LandingModule::check_fix_reached() {
    const double dx = entry_fix_.x - current_position_.x;
    const double dy = entry_fix_.y - current_position_.y;
    const double dist = std::sqrt(dx * dx + dy * dy);

    // Off-nose (abeam) capture with a dwell timer guard (same rationale
    // and same pitfall as NavigationModule — see the long comment there:
    // no timer => possible insta-skip while heading away or an orbit
    // deadlock; the timer resolves both).
    const double bearing = AirSteering::bearing_to(current_position_, entry_fix_);
    const double off_nose = std::abs(AirSteering::heading_error(bearing,
                                                                current_heading_rad_));

    const bool captured =
        dist < fix_radius_ft ||
        (fix_timer_ > 30.0 && dist < fix_abeam_ft &&
         off_nose > fix_abeam_bearing_rad);

    if (captured) {
        sm_.process(LandingEvent::FixReached);
    }
}

void LandingModule::check_established() {
    const double hdg_err = std::abs(AirSteering::heading_error(
        localizer_heading_rad(), current_heading_rad_));
    if (hdg_err < establish_hdg_tol_rad &&
        std::abs(course_lateral_ft()) < establish_lateral_ft) {
        sm_.process(LandingEvent::Established);
    }
}

void LandingModule::check_flare_or_goaround() {
    // Missed approach: overflew the threshold airborne, or descended
    // through decision height without clearance to land.
    if (course_along_ft() > missed_along_ft) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    if (current_alt_agl_ft_ < dh_goaround_agl_ft && !cleared_to_land_) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    if (current_alt_agl_ft_ < flare_agl_ft) {
        sm_.process(LandingEvent::Flare);
    }
}

void LandingModule::check_touchdown() {
    if (on_ground_) {
        sm_.process(LandingEvent::Touchdown);
    }
}

void LandingModule::check_runway_vacated() {
    if (current_vcas_kts_ <= rollout_exit_speed_kts) {
        sm_.process(LandingEvent::RunwayVacated);
    }
}

void LandingModule::check_taxi_in_progress() {
    if (taxi_in_route_.empty() || taxi_wp_index_ >= taxi_in_route_.size()) {
        sm_.process(LandingEvent::ParkedComplete);
        return;
    }
    const auto& target = taxi_in_route_[taxi_wp_index_];
    const double dx = target.x - current_position_.x;
    const double dy = target.y - current_position_.y;
    if (std::sqrt(dx * dx + dy * dy) < taxi_wp_capture_radius_ft) {
        ++taxi_wp_index_;
        if (taxi_wp_index_ >= taxi_in_route_.size()) {
            sm_.process(LandingEvent::ParkedComplete);
        }
    }
}

// ============================================================================
// Per-state control logic
// ============================================================================

AIControlOutput LandingModule::controls_for_request_approach() const {
    // Hold current state wings-level while waiting for the clearance.
    AIControlOutput out = air_steering.steer(current_heading_rad_,
                                             current_alt_msl_ft_,
                                             current_vcas_kts_,
                                             air_input());
    return out;
}

AIControlOutput LandingModule::controls_for_proceed_to_fix() const {
    const double desired = AirSteering::bearing_to(current_position_, entry_fix_);
    // Return to the entry fix at PATTERN altitude (both after handoff and
    // after a go-around — the VS cascade descends from above or climbs
    // back up from below; do not pin the target to the current altitude,
    // a low aircraft could never climb away).
    return air_steering.steer(desired, pattern_altitude_ft_,
                              approach_speed_kts, air_input());
}

AIControlOutput LandingModule::track_final(double target_alt_ft,
                                           double target_speed_kts) const {
    // Final-track control: the shared AirSteering cascades with the cool
    // landing tune (see the constructor). Two iterations of note:
    //   1. The default hot cascade phugoided on final (FCS G-lag phase).
    //   2. A pure proportional beam law (feedforward trim + beam error)
    //      tracked but overshoot BELOW the beam with no damping — the
    //      aircraft touched down short of the runway.
    // The damped VS cascade with cool gains + a tight VS cap is the
    // steady hand: it flew the enroute descents cleanly.
    AIControlOutput out = air_steering.steer(localizer_heading_rad(),
                                             target_alt_ft,
                                             target_speed_kts,
                                             air_input());
    out.gear_handle_down = true;
    return out;
}

AIControlOutput LandingModule::controls_for_flare() const {
    // Idle power, hold the flare attitude, stop tracking the beam.
    AIControlOutput out;
    out.gear_handle_down = true;
    out.throttle_cmd = 0.0;
    const double target = flare_pitch_deg * D2R;
    out.pitch_cmd = std::clamp(flare_pitch_gain * (target - current_pitch_rad_),
                               -0.1, 0.5);
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);  // wings level
    return out;
}

AIControlOutput LandingModule::controls_for_rollout() const {
    // Brakes on, nose-wheel hold of the runway heading. Full pedal
    // authority is safe: the EOM fades steer rate with speed.
    AIControlOutput out = ground_steering.align_heading(
        runway_heading_rad_, ground_input(), 0.0, /*stop=*/false);
    out.throttle_cmd = 0.0;
    out.wheel_brakes = true;
    out.gear_handle_down = true;
    return out;
}

AIControlOutput LandingModule::controls_for_taxi_in() const {
    if (taxi_in_route_.empty() || taxi_wp_index_ >= taxi_in_route_.size()) {
        return ground_steering.hold();
    }
    const bool last_wp = (taxi_wp_index_ + 1 == taxi_in_route_.size());
    return ground_steering.steer_toward(taxi_in_route_[taxi_wp_index_],
                                        ground_input(),
                                        taxi_speed_kts,
                                        /*stop_at_target=*/last_wp);
}

AIControlOutput LandingModule::controls_for_parked() const {
    AIControlOutput out = ground_steering.hold();
    out.parking_brake = true;
    return out;
}

AIControlOutput LandingModule::controls_for_go_around() const {
    // Climb away wings-level at MIL. Phase 1 treats this as terminal (no
    // re-attempt); the sequencer keeps the module's output.
    AIControlOutput out;
    const double target = 12.0 * D2R;
    out.pitch_cmd = std::clamp(2.5 * (target - current_pitch_rad_), -0.3, 0.5);
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
    out.throttle_cmd = 1.0;
    out.gear_handle_down = (current_alt_agl_ft_ < 200.0);
    return out;
}

// ============================================================================
// Steering inputs
// ============================================================================

AIControlOutput LandingModule::hold_complete() const {
    return controls_for_parked();
}

AirSteering::Input LandingModule::air_input() const noexcept {
    AirSteering::Input in;
    in.position = current_position_;
    in.heading_rad = current_heading_rad_;
    in.pitch_rad = current_pitch_rad_;
    in.roll_rad = current_roll_rad_;
    in.vs_fpm = current_vs_fpm_;
    in.vcas_kts = current_vcas_kts_;
    in.alt_msl_ft = current_alt_msl_ft_;
    return in;
}

GroundSteering::Input LandingModule::ground_input() const noexcept {
    GroundSteering::Input in;
    in.position = current_position_;
    in.heading_rad = current_heading_rad_;
    in.speed_kts = current_vcas_kts_;
    return in;
}

// ============================================================================
// Human-readable state name
// ============================================================================

std::string LandingModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

} // namespace f4::ai::modules

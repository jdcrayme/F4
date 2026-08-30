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
    // speed_damp_rad_per_kt is kept at the default 0.002 now that the
    // speed channel has an integral term (AirSteering::throttle_integral_gain).
    // Previously it was reduced to 0.0008 to mask the persistent
    // "nose-down bias when fast" caused by steady-state speed error from
    // the P-only throttle law — but that reduction weakened the phugoid
    // damper and contributed to altitude oscillation on final.
    // See FLIGHT_CONTROL_STABILITY_PLAN.md §4.2 RC-3.
    air_steering.bank_gain = 1.2;
    air_steering.max_bank_rad = 0.44;    // ~25 deg: enough to close lateral S-turns
    air_steering.roll_gain = 3.0;
    // STAB-E43: vs_gain 4.0 -> 1.5 (the calm treatment that fixed the
    // pattern and enroute loops). The 4.0 tune was validated on
    // landing_only, which spawns SETTLED on the beam — the E2E final
    // hands over mid-oscillation and 4.0 (200 ft of beam error = 800
    // fpm of command through the ~10 s effective FCS+airframe lag)
    // GREW the cycle: ±3,500 fpm around the beam, threshold crossed
    // 1,540 ft high (fix15 t=1144-1203). With the STAB-E6 beam ff
    // carrying the descent rate, the loop only needs to trim residuals.
    air_steering.vs_gain = 1.5;
    // STAB-E18: 900 -> 1400. The beam feedforward alone is -980 fpm at
    // 185 kts on a 3-deg beam; with the VS cap at 900 the law could never
    // command even the beam's own rate, so a correctly-tracked final
    // drifted slowly high with no authority to correct (the ride floats
    // until the alt error grows the E10 window past the cap). The cap
    // must exceed |beam ff| + the correction window: -980 - 300 = -1280.
    air_steering.max_vs_fpm = 1400.0;
    // STAB-E10: base correction window ±300 fpm around the beam
    // feedforward, scaling up with altitude error (see air_steering.cpp).
    // Tight near the beam (smooth ride), full ±900 authority for a
    // from-below capture.
    air_steering.vs_corr_max_fpm = 300.0;
    // STAB-E1: path_gain raised from 0.00008 to 0.0004 (5x) — the VS-error
    // damping term is what arrests the phugoid. At 0.00008 a 2,000 fpm VS
    // error produced 0.016 rad (~0.9 deg) of correction: nothing. The
    // on_glideslope trace showed the aircraft crossing the beam with
    // +2,811 fpm and the law unable to flatten it — it sailed 466 ft
    // high, reversed, and dove at -6,200 fpm into the flare.
    air_steering.path_gain = 0.0006;
    air_steering.gamma_corr_limit = 0.10;
    // STAB-E1: attitude_gain lowered 1.2 -> 0.9 and pitch_rate_damp raised
    // 0.3 (class default) -> 0.5. The final tune's 1.2 with the FCS G-lag
    // injected more energy per cycle than the damping removed: the zoom
    // reached +10,441 fpm / 43 deg pitch at 150 kts (alpha 14-18, riding
    // the stall cliff). Slower stick + more rate damping settles the beam.
    // STAB-E12: raised 0.9 -> 1.3 after E10 tamed the VS commands — with
    // bounded corrections the loop can afford firmer pitch authority,
    // which it needs to arrest -2,400 fpm dive transients (at 0.9 a 13-deg
    // pitch error produced only 0.2 stick ≈ +0.3 G — too weak to flatten
    // a beam-crossing overshoot before the next one).
    // STAB-E39/E47: pitch-tracking authority. With the E29 slew, E34 beam
    // ff, E44 phase-lead damper and E46 energy damper all in place the
    // loop is stable in its linear band, but at attitude_gain 0.8 the
    // pitch TRACKS poorly: the beam ride floated 150-350 ft high because
    // an 11-deg pitch error produced only 0.16 stick — not enough push
    // to unwind the FCS pitch integrator (fix20 t=1204-1216: command
    // -1,400 fpm, actual -200, pitch stuck at +7.6 deg). 1.2 restores
            // tracking; the dampers (not this gain) carry the stability.
    air_steering.attitude_gain = 1.2;
    air_steering.pitch_rate_damp = 0.8;
    // STAB-E1: speed_damp restored from 0.0005 toward the class default.
    // The 0.0005 reduction was a workaround for the P-only throttle law's
    // steady-state error; with the throttle integral (Phase 2d) that error
    // is gone, and 0.0005 removed the phugoid damping this channel
    // provides. 0.0012 keeps a gentle trim while damping.
    // STAB-E44: 0.0012 -> 0.0030. In the phugoid the speed LEADS the
    // vertical speed by ~90 deg, so a pitch trim proportional to speed
    // error is a true phase-LEAD damper — the one actuator in this
    // architecture whose phugoid-frequency response is not consumed by
    // the ~10 s effective FCS+airframe delay that turns every VS-error
    // correction into a pump (fix16: the final rode a growing ±5,300 fpm
    // cycle at the natural ~42 s period and ballooned to 2,000 ft over
    // the threshold). At ±25 kt phugoid swings this gives ±5.6 deg of
    // anti-phase trim. The throttle PI holds the mean speed, so the
    // channel only ever sees the oscillation.
    air_steering.speed_damp_rad_per_kt = 0.0030;
    // STAB-E1: softer speed loop on final — the default P gain swung the
    // throttle 0.04 -> 1.00 rail-to-rail on the phugoid's ±40 kt speed
    // swings, pumping the oscillation. Softer P + tighter integral clamp.
    air_steering.throttle_gain = 0.004;
    air_steering.throttle_integral_max = 0.2;
    // STAB-E31: throttle_mid for the final tune: 0.55. The beam descent
    // at 185 kts in the drag bucket trims ~0.5; the earlier 0.78 (level
    // flight trim) plus the 0.2 integral range floored the throttle at
    // 0.58 — the jet stayed fast and high, and the E46 damper's chops
    // banged the engine 0.08 <-> 1.00 (fix19 t=1208-1240). 0.55 centers
    // the PI on the actual beam-ride trim.
    air_steering.throttle_mid = 0.55;
    air_steering.throttle_min = 0.0;   // per-call floors (STAB-E36) govern

    // Pattern tune: same cascade, steeper bank + more gamma authority.
    // At 200 kts a 35-deg bank turns with ~5,000 ft radius — the pattern
    // legs exist as straights between the corner arcs (with the final's
    // 25-deg cap the radius is ~7,000 ft and the crosswind leg
    // degenerates into one continuous spiral). The wider
    // gamma_corr_limit matters too: in a 35-deg bank only ~82% of the
    // lift holds the vertical — the 0.07 rad final-approach cap cannot
    // cover that and the turns sank 2,000+ ft.
    //
    // STAB-E19: calm pattern tune (the STAB-E15 treatment that fixed the
    // enroute phugoid). The previous overrides kept the final tune's
    // attitude_gain 1.3 + vs_gain 4.0 with max_vs 2000 and only
    // path_gain 0.0002 of VS-error damping — the exact gain combination
    // STAB-E15 identified as the bang-bang limit cycle driver. The
    // pattern trace showed it: downwind commanded ±2,000 fpm through a
    // ~2 s FCS lag with 0.0002 of damping, porpoising ±25 deg pitch /
    // ±7,000 fpm around a 1,500 ft target for the whole leg (t=730-1040),
    // arriving at the base turn 3,500 ft high — which then blew every
    // intercept. Same cure as enroute: slow the authority, strengthen
    // the damping.
    pattern_steering = air_steering;
    pattern_steering.max_bank_rad = 0.40;        // ~23 deg — STAB-E49:
                                        // the fix40-48 series proved this
                                        // airframe's slow alpha response
                                        // cannot hold gamma in sustained
                                        // 30+ deg banks (vertical nz =
                                        // nz*cos(phi) goes marginal, the
                                        // pitch loop cannot arrest the
                                        // sink through the FCS integrator
                                        // lag, and every deep-banked turn
                                        // ends in a −5,000..−11,000 fpm
                                        // spiral-dive half-cycle). 23 deg
                                        // needs only 1.09 G — inside
                                        // authority with margin, and the
                                        // pattern's PLANE-crossing captures
                                        // absorb the wider turn radius by
                                        // design (base_turn/capture/upwind
                                        // geometry widened to match).
    // STAB-E28: gamma_corr_limit 0.25 -> 0.10 (the final tune's value).
    // The 0.25 override turned the VS-error damper into a ±14-deg
    // gamma RELAY: it saturated at any VS error over ~420 fpm (which is
    // always, during any capture) and the bang-bang through the ~2-3 s
    // FCS G-lag sustained a ±4,000-8,000 fpm limit cycle through the
    // whole pattern (fix2 trace: base dive -8,448 then zoom +3,969;
    // downwind riding 1,200 ft above target). A saturating damper is
    // WORSE than a small linear one — the calm enroute tune never
    // widened this limit and it is the one loop that tracks cleanly.
    pattern_steering.gamma_corr_limit = 0.10;
    pattern_steering.attitude_gain = 0.8;
    pattern_steering.pitch_rate_damp = 0.8;
    pattern_steering.vs_gain = 2.0;
    pattern_steering.path_gain = 0.0006;
    pattern_steering.max_vs_fpm = 1500.0;        // calm jet pattern descents
    // STAB-E31: pattern mid covers the flap-1/2 downwind through the
    // gear+full-flap base/intercept; the integral covers the rest.
    pattern_steering.throttle_mid = 0.65;
    pattern_steering.throttle_integral_max = 0.25;
    pattern_steering.throttle_min = 0.0;  // per-call floors (STAB-E36) govern
    // STAB-E44: phase-lead phugoid damper (see the final-tune comment).
    pattern_steering.speed_damp_rad_per_kt = 0.0025;
    // STAB-E48: aggressive balloon guard for the pattern legs — chopping
    // their zooms early is what keeps the ±5,000-11,000 fpm swings down
    // (fix23's base leg after the guard was raised for the final).
    pattern_steering.balloon_guard_fpm = 200.0;
}

fsm::StateMachine<LandingState, LandingEvent>
LandingModule::build_sm()
{
    return typename fsm::StateMachine<LandingState, LandingEvent>::Builder()
        .initial(LandingState::RequestApproach)
        .state(LandingState::RequestApproach, "RequestApproach")
        .state(LandingState::ProceedToFix,    "ProceedToFix")
        .state(LandingState::PatternDownwind, "PatternDownwind")
        .state(LandingState::PatternBase,     "PatternBase")
        .state(LandingState::InterceptFinal,  "InterceptFinal")
        .state(LandingState::OnFinal,         "OnFinal")
        .state(LandingState::Flare,           "Flare")
        .state(LandingState::Rollout,         "Rollout")
        .state(LandingState::TaxiIn,          "TaxiIn")
        .state(LandingState::Parked,          "Parked")
        .state(LandingState::GoAround,        "GoAround")

        .event_name(LandingEvent::ApproachGranted,  "ApproachGranted")
        .event_name(LandingEvent::FixReached,       "FixReached")
        .event_name(LandingEvent::PatternEntry,     "PatternEntry")
        .event_name(LandingEvent::DownwindComplete, "DownwindComplete")
        .event_name(LandingEvent::BaseComplete,     "BaseComplete")
        .event_name(LandingEvent::Established,      "Established")
        .event_name(LandingEvent::Flare,            "Flare")
        .event_name(LandingEvent::Touchdown,        "Touchdown")
        .event_name(LandingEvent::RunwayVacated,    "RunwayVacated")
        .event_name(LandingEvent::ParkedComplete,   "ParkedComplete")
        .event_name(LandingEvent::GoAround,         "GoAround")
        .event_name(LandingEvent::Reintercept,      "Reintercept")

        .on(LandingState::RequestApproach, LandingState::ProceedToFix,
            LandingEvent::ApproachGranted,
            nullptr, nullptr, "landing_clearance_received")
        .on(LandingState::ProceedToFix, LandingState::InterceptFinal,
            LandingEvent::FixReached,
            nullptr, nullptr, "approach_fix_reached")
        .on(LandingState::ProceedToFix, LandingState::PatternDownwind,
            LandingEvent::PatternEntry,
            nullptr, nullptr, "pattern_entry_fix_reached")
        .on(LandingState::PatternDownwind, LandingState::PatternBase,
            LandingEvent::DownwindComplete,
            nullptr, nullptr, "downwind_leg_complete")
        .on(LandingState::PatternBase, LandingState::InterceptFinal,
            LandingEvent::BaseComplete,
            nullptr, nullptr, "base_leg_reached_final_course")
        .on(LandingState::PatternBase, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "base_leg_crossed_threshold")
        .on(LandingState::PatternDownwind, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "pattern_geometry_blown")
        .on(LandingState::InterceptFinal, LandingState::OnFinal,
            LandingEvent::Established,
            nullptr, nullptr, "established_on_final")
        // STAB-E21: the establish-floor safety valve. Without this
        // transition the GoAround event fired by check_established() had
        // no matching edge and the aircraft flew 53 miles away in
        // InterceptFinal (fix1 trace t>1110).
        .on(LandingState::InterceptFinal, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "intercept_not_established_in_time")
        .on(LandingState::OnFinal, LandingState::Flare,
            LandingEvent::Flare,
            nullptr, nullptr, "flare_height")
        .on(LandingState::OnFinal, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "missed_approach")
        // STAB-E3: the Flare state previously had NO exit except Touchdown
        // (on_ground_). The Phase C4 energy-managed flare law can command a
        // climb-away when the predicted touchdown is outside the runway —
        // but without this transition the aircraft climbed away FOREVER,
        // still in Flare, never touching down and never going around
        // (observed: 200k ticks stuck in Flare). A missed prediction during
        // the flare must re-fly the approach, not hover.
        .on(LandingState::Flare, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "flare_missed_prediction")
        // STAB-E24: after a go-around, a pattern-mode aircraft re-enters
        // the pattern LOCALLY (crosswind corner -> downwind -> base ->
        // final) instead of hauling 55,000 ft back to the far entry fix.
        // The old GoAround->ProceedToFix cycle burned 3+ minutes per
        // re-attempt (observed: 6+ cycles in the E2E trace, none of which
        // converged). Straight-in mode keeps the entry-fix re-fly.
        .on(LandingState::GoAround, LandingState::PatternDownwind,
            LandingEvent::Reintercept,
            nullptr,
            [this]() { return fly_traffic_pattern; },
            "climbed_to_pattern_altitude_reenter_downwind")
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
        .on_enter(LandingState::PatternDownwind, [this](const LandingEvent& ev) {
            pattern_timer_ = 0.0;
            // Fresh pattern entry (PatternEntry) starts the overhead join
            // at the upwind leg. A local RE-ENTRY after a go-around
            // (Reintercept) starts at the crosswind corner: the aircraft
            // is already low over the field having just gone around, and
            // re-flying the upwind overfly would drag it 14,000 ft past
            // the far end first. STAB-E24.
            pattern_leg_ = (ev == LandingEvent::Reintercept) ? 1 : 0;
        })
        .on_enter(LandingState::PatternBase, [this](const LandingEvent&) {
            pattern_timer_ = 0.0;
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
        .on_enter(LandingState::Flare, [this](const LandingEvent&) {
            // STAB-E3: start the flare timeout clock (see check_touchdown).
            flare_timer_ = 0.0;
        })
        .on_enter(LandingState::TaxiIn, [this](const LandingEvent&) {
            // STAB-E25: skip taxi-in waypoints that are already BEHIND the
            // aircraft. The derived taxi-in route starts at the threshold
            // (the takeoff position), but the rollout typically ends
            // 2,000-5,000 ft PAST it — following route[0] would command a
            // 180-degree turn on the runway and a back-taxi to the far
            // end before heading to parking. Instead, advance to the first
            // waypoint ahead of the nose so the aircraft exits forward.
            // (Always keeps at least the final waypoint: the parking spot.)
            const double hx = std::sin(current_heading_rad_);
            const double hy = std::cos(current_heading_rad_);
            while (taxi_wp_index_ + 1 < taxi_in_route_.size()) {
                const auto& wp = taxi_in_route_[taxi_wp_index_];
                const double ahead = (wp.x - current_position_.x) * hx
                                  + (wp.y - current_position_.y) * hy;
                if (ahead >= 0.0) break;
                ++taxi_wp_index_;
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
        // STAB-E9: latch instead of inline sm_.process() — the StubATC
        // answers synchronously inside publish(), which can originate
        // from our own sm_.reset() entry action (RequestApproach), making
        // an inline process() re-entrant (UB). Drained at update() top.
        if (sm_.current() == LandingState::RequestApproach) {
            deferred_event_ = LandingEvent::ApproachGranted;
        }
    });

    bus.subscribe<atc::ClearedToLand>([this](const atc::ClearedToLand& msg) {
        if (msg.aircraft_id == ownship_id_) {
            cleared_to_land_ = true;
        }
    });

    // Re-fire the RequestApproach entry action now that bus_ is set.
    sm_.reset();

    // STAB-E9: same deferred-clearance drain as TakeoffModule — the
    // StubATC answers inside the publish chain; process the latched
    // event now that reset() has returned (outside any SM frame).
    if (deferred_event_) {
        const auto ev = *deferred_event_;
        deferred_event_.reset();
        sm_.process(ev);
    }
}

// ============================================================================
// Per-tick update
// ============================================================================

AIControlOutput LandingModule::update(double dt, const flight::IAircraftState* state)
{
    cache_aircraft_state(state);
    // STAB-E9: drain any clearance event latched by a subscription handler
    // (see initialize). Safe here — outside any sm_ frame.
    if (deferred_event_) {
        const auto ev = *deferred_event_;
        deferred_event_.reset();
        sm_.process(ev);
    }
    fix_timer_ += dt;
    pattern_timer_ += dt;
    // STAB-E3: flare timeout clock (see check_touchdown).
    if (sm_.current() == LandingState::Flare) {
        flare_timer_ += dt;
    }

    for (int iter = 0; iter < 8; ++iter) {
        const auto before = sm_.current();
        switch (sm_.current()) {
            case LandingState::ProceedToFix:
                check_fix_reached();
                break;
            case LandingState::PatternDownwind:
                check_pattern_downwind();
                break;
            case LandingState::PatternBase:
                check_pattern_base();
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
        case LandingState::PatternDownwind: return controls_for_pattern_downwind();
        case LandingState::PatternBase:     return controls_for_pattern_base();
        case LandingState::InterceptFinal:
            // Pattern mode delivers the aircraft close in and ~90 deg
            // off; the intercept turn is short and tight, so fly it at
            // approach speed (the +40 was for the long straight-in
            // intercept at pattern altitude, where speed helps).
            // The descent is floored: laterally far off course the beam
            // altitude is meaningless, and chasing it down there put the
            // aircraft on the ground short of the runway.
            return track_final(
                std::min(pattern_altitude_ft_,
                         std::max(glide_slope_alt_ft(),
                                  threshold_alt_ft_ + intercept_floor_agl_ft)),
                approach_speed_kts +
                    (fly_traffic_pattern ? 0.0 : 40.0),
                /*pattern_turn=*/true);
        case LandingState::OnFinal: {
            // Phase B3 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase B3): ride the
            // beam EXACTLY. Previously this used an 8% undershoot bias
            // (0.92 * (beam - threshold_alt)) as a workaround for slow
            // localizer convergence.
            //
            // STAB-E6b: use glide_slope_alt_ft() (which zeros at the aim
            // point beam_aim_offset_ft PAST the threshold), NOT the inline
            // threshold-referenced beam. The inline version put 0 ft AGL at
            // the threshold itself, steering the flare INTO the pavement
            // edge — a direct contributor to "lands short". The aim-point
            // beam puts ~130 ft of crossing height over the threshold,
            // like a real ILS.
            return track_final(glide_slope_alt_ft(), approach_speed_kts);
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
    current_roll_rate_radps_ = state->roll_rate_radps();
    current_pitch_rate_radps_ = state->pitch_rate_radps();
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
    // Phase B2 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase B2): when far from the
    // centerline (|xtrack| > intercept_offset_ft), command heading directly
    // toward a point ahead on the centerline. Standard ILS intercept
    // geometry — at large offset the proportional localizer law saturates
    // and can't close the gap.
    //
    // STAB-E20: the lead distance now SCALES with the cross-track
    // (lead = max(intercept_lead_ft, intercept_lead_ratio * |xtrack|)),
    // bounding the intercept cut at ~27 deg for any offset. The previous
    // fixed 1,500 ft lead commanded atan2(3279/1500) = 65 deg cuts — the
    // base->final handoff at 3,279 ft dove across the localizer at 65
    // deg, overshot to +5,000 ft, and S-turned the whole final away
    // (digi_full_mission t=1087-1150). A 27-deg cut closes the same
    // offset in ~6,700 ft of track and rolls out NEAR the course instead
    // of across it. The floor keeps small offsets from commanding
    // near-perpendicular cuts (at 400 ft: atan2(400/1500) = 15 deg,
    // continuous with the proportional law just below).
    const double xtrack = course_lateral_ft();
    if (std::abs(xtrack) > intercept_offset_ft) {
        // bearing from the aircraft's current position to the aim point
        // (a point ahead on the centerline, relative to the aircraft's
        // projection onto the centerline).
        // -xtrack = toward centerline (positive lateral offset => steer left)
        // lead = forward along the course
        const double lead = std::max(intercept_lead_ft,
                                     intercept_lead_ratio * std::abs(xtrack));
        const double bearing_to_aim = std::atan2(-xtrack, lead);
        return runway_heading_rad_ + bearing_to_aim;
    }
    // Right of centerline (lateral > 0) -> steer left (subtract correction).
    // STAB-E20: gain softened 0.0015 -> 0.0009 so the near-course law's
    // command at the intercept_offset boundary (400 ft: 0.36 rad = 21 deg)
    // is continuous with the scaled-lead law's cut there (~15 deg) — the
    // old 0.0015 gain produced a 34-deg command at the boundary, a bank
    // step UP right where the intercept should be relaxing.
    const double corr = std::clamp(localizer_gain * xtrack,
                                   -max_localizer_corr_rad, max_localizer_corr_rad);
    return runway_heading_rad_ - corr;
}

// ============================================================================
// Traffic-pattern geometry
// ============================================================================

geo::WorldPosition LandingModule::pattern_point(double along_ft,
                                                double lateral_ft) const {
    // Forward (landing direction) and right-of-course unit vectors from
    // the threshold — same convention as course_along_ft/course_lateral_ft.
    const double fx = std::sin(runway_heading_rad_);
    const double fy = std::cos(runway_heading_rad_);
    const double rx = std::cos(runway_heading_rad_);
    const double ry = -std::sin(runway_heading_rad_);
    return geo::WorldPosition(
        threshold_position_.x + fx * along_ft + rx * lateral_ft,
        threshold_position_.y + fy * along_ft + ry * lateral_ft,
        threshold_position_.z);
}

geo::WorldPosition LandingModule::pattern_leg_target() const {
    switch (pattern_leg_) {
        case 0:  // Upwind overfly: far corner, slight pattern-side offset.
            return pattern_point(upwind_along_ft,
                                 pattern_lateral_sign() * pattern_join_offset_ft);
        case 1:  // Crosswind: same corner, widened to the pattern offset.
            return pattern_point(upwind_along_ft,
                                 pattern_lateral_sign() * pattern_offset_ft);
        default: // Downwind leg: back to the base-turn point.
            return pattern_point(-base_turn_along_ft,
                                 pattern_lateral_sign() * pattern_offset_ft);
    }
}

geo::WorldPosition LandingModule::base_aim_point() const {
    return pattern_point(-base_aim_along_ft, 0.0);
}

double LandingModule::base_target_alt_ft() const {
    // Descend on base toward ~base_alt_agl_ft over the field, but never
    // above the pattern altitude (the VS cascade handles either side).
    return std::min(pattern_altitude_ft_,
                    threshold_alt_ft_ + base_alt_agl_ft);
}

bool LandingModule::waypoint_captured(const geo::WorldPosition& target,
                                      double dwell_s,
                                      double radius_ft,
                                      double abeam_window_ft) const {
    const double dx = target.x - current_position_.x;
    const double dy = target.y - current_position_.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist < radius_ft) return true;

    const double bearing = AirSteering::bearing_to(current_position_, target);
    const double off_nose = std::abs(AirSteering::heading_error(bearing,
                                                                current_heading_rad_));
    return dwell_s > 30.0 && dist < abeam_window_ft &&
           off_nose > fix_abeam_bearing_rad;
}

// ============================================================================
// Transition checks
// ============================================================================

void LandingModule::check_fix_reached() {
    // Off-nose (abeam) capture with a dwell timer guard (same rationale
    // and same pitfall as NavigationModule — see the long comment there:
    // no timer => possible insta-skip while heading away or an orbit
    // deadlock; the timer resolves both).
    if (waypoint_captured(entry_fix_, fix_timer_, fix_radius_ft, fix_abeam_ft)) {
        sm_.process(fly_traffic_pattern ? LandingEvent::PatternEntry
                                        : LandingEvent::FixReached);
    }
}

void LandingModule::check_pattern_downwind() {
    // STAB-E33: ground-contact recovery — if the aircraft is DRIVING on
    // the ground at speed (a botched turn sank it onto the deck and the
    // EOM ground clamp can hold a low-alpha jet pinned at 200+ kts),
    // transition to GoAround: its fixed climb-out law commands rotation
    // like a takeoff and flies it off.
    if (on_ground_ && current_vcas_kts_ > 80.0) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // Three-leg walker with PLANE-CROSSING captures on the runway
    // along/lateral axes. A fast jet at 250 kts turns with a ~13,000 ft
    // radius, so the corners are arcs that bulge well past the corner
    // points — point-radius captures fire at arbitrary phases of the arc
    // (the first attempt used them and the base turn started mid-turn,
    // 2,600 ft from its point, heading the wrong way). Plane captures
    // are monotone along each leg and immune to the bulge:
    //   leg 0: past the far-corner plane  (along > upwind_along - lead)
    //   leg 1: widened past the offset    (|lateral| > offset - lead)
    //   leg 2: back before the base plane (along < -base_turn_along + lead)
    constexpr double LEAD_FT = 1500.0;  // start each turn this early
    const double side = pattern_lateral_sign();
    switch (pattern_leg_) {
        case 0:
            if (course_along_ft() > upwind_along_ft - LEAD_FT) {
                pattern_leg_ = 1;
                pattern_timer_ = 0.0;
            }
            break;
        case 1:
            if (course_lateral_ft() * side > pattern_offset_ft - LEAD_FT) {
                pattern_leg_ = 2;
                pattern_timer_ = 0.0;
            }
            break;
        default:
            if (course_along_ft() < -(base_turn_along_ft - LEAD_FT)) {
                sm_.process(LandingEvent::DownwindComplete);
            }
            break;
    }
}

void LandingModule::check_pattern_base() {
    // Safety valve: the base leg should never cross the threshold — if it
    // does, the turn geometry was blown and the safest out is a go-around.
    if (course_along_ft() > 0.0) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E33: same ground-contact recovery as the downwind walker.
    if (on_ground_ && current_vcas_kts_ > 80.0) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E42: base->final HEALTH gate — only start the final turn when
    // the aircraft is energy-stable. The capture fired mid-recovery in
    // fix14 (lat -12,016 crossed while sinking -7,400 at 820 ft after a
    // guardian zoom) and handed the intercept an 820 ft/282 kt mess it
    // could not save. Unstable = keep flying the base leg (safe, it
    // points away from the threshold) until settled; the along > 0
    // valve above still bounds the leg.
    if (std::abs(current_vs_fpm_) > 2500.0 || current_alt_agl_ft_ < 700.0) {
        return;
    }
    // Base -> final turn when close enough to the extended centerline.
    if (std::abs(course_lateral_ft()) < base_capture_lateral_ft) {
        sm_.process(LandingEvent::BaseComplete);
    }
}

void LandingModule::check_established() {
    // Grounded short of the runway while still intercepting (a botched
    // pattern turn dove it into the dirt): the beam-chase would keep it
    // sliding at 250 kts forever — nothing else transitions. Go around
    // and re-fly the approach instead. (Established-on-final ground
    // contact near the threshold is the normal flare path, and along >
    // -1500 ft excludes the overrun zone.)
    if (on_ground_ && course_along_ft() < -1500.0) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E21: not established by this close-in floor = the intercept is
    // not converging — go around cleanly instead of dragging a 90-deg
    // crosser through the missed-approach plane (observed: OnFinal entry
    // at 82 deg heading 15.8k out, threshold overflown at 1,905 ft AGL,
    // 1,285 ft off centerline).
    if (course_along_ft() > -establish_floor_ft) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E22: the heading gate now references the RUNWAY heading, not
    // localizer_heading_rad(). The old check compared the aircraft's
    // heading to the INTERCEPT heading — which itself contains up to 65
    // deg of lead — so an aircraft still 3,279 ft off-course diving at
    // the localizer at a 65-deg cut was "established" (65-deg cut vs
    // 65-deg command = 0 error). Established must mean ROLLED OUT on the
    // course: heading within tolerance of the runway heading.
    const double hdg_err = std::abs(AirSteering::heading_error(
        runway_heading_rad_, current_heading_rad_));
    // Pattern mode still gets a wider lateral tolerance (the base->final
    // turn hands over on a converging cut, not a centered track), but the
    // heading gate is the same: aligned is aligned.
    const double lat_tol = fly_traffic_pattern
                               ? std::max(establish_lateral_ft, 1000.0)
                               : establish_lateral_ft;
    // STAB-E23: vertical gate — established means on the BEAM too, and
    // (STAB-E45) SETTLED: |vs| < 900. The old gate handed OnFinal a
    // +280 ft / +1,800 fpm climbing transient at 15k ft out (fix17);
    // the calm final needs ~30 s to damp such a transient and 15,000 ft
    // of track is only ~45 s — the balloon peaked +1,500 ft above the
    // beam and the threshold was crossed at 1,300 ft. Refusing unsettled
    // handoffs keeps the ride in its linear band from the first second.
    const double beam_err = std::abs(current_alt_msl_ft_ - glide_slope_alt_ft());
    const double settle_err = std::abs(current_vs_fpm_ + 0.0);
    if (hdg_err < establish_hdg_tol_rad &&
        std::abs(course_lateral_ft()) < lat_tol &&
        beam_err < 300.0 &&
        settle_err < 900.0) {
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
    // STAB-E11: stable-approach gate. Flare entry requires being within
    // the near-runway environment: reaching flare HEIGHT this far out
    // means the approach is unstable (observed: crossing 60 ft AGL at
    // 9,500 ft short while still 200 ft below the beam — the flare law
    // cannot salvage that and touchdown would be far short of the
    // pavement). Go around and re-fly instead of flaring at the grass.
    if (current_alt_agl_ft_ < flare_agl_ft) {
        if (course_along_ft() > -missed_along_ft) {
            sm_.process(LandingEvent::Flare);
        } else {
            sm_.process(LandingEvent::GoAround);
        }
    }
}

void LandingModule::check_touchdown() {
    // STAB-E3: safety valve — if we somehow gained altitude back during the
    // flare (balloon) or the sink is unrecoverable and the predicted
    // touchdown has left the runway, go around and re-fly. Without this the
    // Flare state had no exit except wheels-on (see the SM transition note).
    if (current_alt_agl_ft_ > flare_agl_ft * 2.0 && current_vs_fpm_ > 0.0) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // Overflew the threshold airborne during the flare (long-float
    // case): go around rather than touching down halfway down the runway.
    // STAB-E55: the flare-state bound is missed_along + flare_overrun_ft
    // (NOT the bare missed plane): a legitimate flare can begin as late
    // as ~+2,300 along (60 ft AGL happens near the aim point when riding
    // slightly high) and still touch down with thousands of feet of
    // pavement remaining — the bare +2,500 plane insta-aborted those
    // 0.6 s after flare entry (straight-in fix30). OnFinal's own
    // threshold-overflight check (check_flare_or_goaround) still uses
    // the bare missed plane: airborne at +2,500 ABOVE flare height is a
    // genuine missed approach.
    if (course_along_ft() > missed_along_ft + flare_overrun_ft) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // Flare timeout: still airborne well below flare height for >15 s means
    // the flare law is holding the aircraft off — go around.
    if (flare_timer_ > 15.0) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
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
    // STAB-E19: flown with the CALM pattern tune, not the final tune —
    // the final's attitude_gain 1.3 + the ±300 beam-ride correction
    // window is tuned for riding a 3-deg beam, not for a 10,000+ ft
    // re-positioning leg; with it this state still rang ±3,000 fpm
    // (t=1860-1990 of the baseline trace).
    return pattern_steering.steer(desired, pattern_altitude_ft_,
                                  approach_speed_kts, air_input());
}

AIControlOutput LandingModule::controls_for_pattern_downwind() const {
    // Fly the current pattern leg. Gear stays up until the base turn.
    //
    // STAB-E32: the descent is one CONTINUOUS beam-parallel slope, not
    // stepped altitudes. The old law stepped the target 1,500 (pattern)
    // -> 900 (base, fixed AGL) -> beam (intercept): each step was a VS
    // transient the phugoid-prone cascade amplified, and the base->beam
    // step arrived while already low and turning (the deck dives).
    // Riding beam+offset from the downwind on, every leg's target moves
    // at the SAME -3 deg slope the final will fly — the loop stays near
    // trim the whole way down.
    //
    // STAB-E26: MANEUVERING FLAPS from the downwind leg (leg 2) onward.
    // The pattern turns at 200-230 kts CLEAN ride the model's clean
    // stall boundary in a 35-deg bank (~200 kts effective) — half TEF
    // + some LEF drops the stall ~25 kts and makes the turns
    // comfortable; full landing flaps come on the base leg.
    // STAB-E32/E38: the pattern holds the PATTERN ALTITUDE from the join
    // through the base leg; the DESCENT to the beam happens entirely on
    // the intercept (its target already follows the beam down from the
    // pattern altitude — at 28,000 ft out the 3-deg beam IS ~1,500 ft,
    // so the handoff is seamless). The earlier beam-parallel downwind
    // (beam+600) dragged the target to ~600 ft AGL over the field —
    // 19,000 ft from the threshold on the wrong side of the aim point,
    // where the beam reference is meaningless — and the residual phugoid
    // flew it to 7 ft AGL (fix11 trace t=1004-1016).
    //
    // STAB-E26/E33: maneuvering flaps from the CROSSWIND turn (leg 1) on.
    const double desired = AirSteering::bearing_to(current_position_,
                                                   pattern_leg_target());
    // STAB-E36 throttle floors (recalibrated E41 after reading the engine
    // model: throttle maps 0..1 = idle..MIL, so the drag-bucket trim at
    // approach speed is ~0.15-0.25, NOT the 0.35-0.5 first guessed —
    // those floors held the aircraft at 250-280 kts through the whole
    // pattern and final): 0.10 through the join (bleed the arrival
    // speed), 0.15 downwind, 0.20 in the landing configuration (mild
    // energy insurance; the sink guardian catches the terminal case).
    const double thr_floor = (pattern_leg_ == 0) ? 0.10
                           : (pattern_leg_ == 1) ? 0.15 : 0.15;
    AIControlOutput out = pattern_steering.steer(desired, pattern_altitude_ft_,
                                  pattern_speed_kts, air_input(),
                                  thr_floor);
    // The crosswind corner enters a 34-deg bank at 215+ kts CLEAN —
    // nz available at the trim alpha is ~0.85 vs the 1.2 the turn needs,
    // and the aircraft fell out of the turn at -8,900 fpm onto the deck
    // (fix6 trace t=916-935: tef 0.00, nzcgs 0.79, phi -34). Half flaps
    // + LEF restores the margin for EVERY pattern turn after the join.
    if (pattern_leg_ >= 1) {
        out.tef_cmd = 0.5;   // maneuvering flaps
        out.lef_cmd = 0.3;
    }
    // STAB-E33: GPWS-style sink guardian — the turn-entry sink developed
    // -8,900 fpm from 1,700 ft and no state logic noticed. Low + sinking
    // hard = unconditional max climb (wings level, MIL) until the sink
    // breaks. A target-altitude law cannot arrest a dynamic sink; this
    // can. (fix9: -2,257 fpm at 600 ft slipped past a -2,500 threshold
    // and rode it to 39 ft — tightened to -1,800.)
    if (current_alt_agl_ft_ < 1400.0 && current_vs_fpm_ < -1800.0) {
        out.pitch_cmd = std::clamp(2.5 * (0.21 - current_pitch_rad_), -0.1, 0.5);
        out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
        out.throttle_cmd = 1.0;
    }
    return out;
}

AIControlOutput LandingModule::controls_for_pattern_base() const {
    // Base leg: steer toward the extended-centerline aim point, gear +
    // full flaps down for the landing.
    // STAB-E32: base rides BEAM+200 (the same continuous slope the
    // downwind started), not a fixed AGL — no target step into the
    // base->final turn (see the downwind comment).
    // STAB-E31: base flies at APPROACH speed, not pattern-25: with gear +
    // full flaps the drag bucket needs the speed for pull authority.
    const double desired = AirSteering::bearing_to(current_position_,
                                                   base_aim_point());
    // STAB-E38: base holds the PATTERN ALTITUDE (min with beam+100 for
    // the rare shallow-beam case) — the descent to the beam belongs to
    // the intercept alone (see the downwind comment). No beam ff here:
    // the target is level.
    AIControlOutput out = pattern_steering.steer(desired,
                                                 std::min(pattern_altitude_ft_,
                                                          glide_slope_alt_ft() + 100.0),
                                                 approach_speed_kts,
                                                 air_input(),
                                                 /*throttle_floor=*/0.20);
    out.gear_handle_down = true;
    out.tef_cmd = landing_tef_cmd;
    out.lef_cmd = landing_lef_cmd;
    // STAB-E33: same sink guardian as the downwind legs.
    if (current_alt_agl_ft_ < 1400.0 && current_vs_fpm_ < -1800.0) {
        out.pitch_cmd = std::clamp(2.5 * (0.21 - current_pitch_rad_), -0.1, 0.5);
        out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
        out.throttle_cmd = 1.0;
    }
    return out;
}

AIControlOutput LandingModule::track_final(double target_alt_ft,
                                           double target_speed_kts,
                                           bool pattern_turn) const {
    // Final-track control: the shared AirSteering cascades with the cool
    // landing tune (see the constructor).
    //
    // STAB-E6: when tracking the BEAM (OnFinal), the altitude feedforward
    // is the beam's own descent rate at the current groundspeed. Without
    // it, zero altitude error commanded LEVEL flight while the beam kept
    // descending ~1,000 fpm — the aircraft floated above, then dove to
    // re-catch, arriving at flare height with -3,000+ fpm (the
    // on_glideslope trace). Intercept turns (pattern_turn) chase a FLOOR
    // or pattern altitude, not the beam — feedforward 0 there.
    AirSteering::Input in = air_input();
    if (!pattern_turn) {
        const double v_fps = std::max(100.0, current_vcas_kts_ * 1.68781);
        in.vs_ff_fpm = -std::tan(glide_slope_angle_rad_) * v_fps * 60.0;
    } else {
        // STAB-E34: the pattern intercept rides the beam-floored target —
        // feed the beam rate forward whenever the target is on the beam
        // SEGMENT (not when clamped at the pattern altitude or the
        // intercept floor, whose slope is zero).
        const double floor_alt = threshold_alt_ft_ + intercept_floor_agl_ft;
        const bool beam_limited = glide_slope_alt_ft() > floor_alt &&
                                  glide_slope_alt_ft() < pattern_altitude_ft_;
        if (beam_limited) {
            const double v_fps = std::max(100.0, current_vcas_kts_ * 1.68781);
            in.vs_ff_fpm = -std::tan(glide_slope_angle_rad_) * v_fps * 60.0;
        }
    }
    const AirSteering& steer = (pattern_turn && fly_traffic_pattern)
                                   ? pattern_steering
                                   : air_steering;
    // STAB-E35/E36: landing-configuration floor — 0.35: enough energy to
    // hold the beam at approach speed without diving, while still
    // allowing the drag bucket to BLEED the pattern's arrival speed
    // (a 0.5 floor held the aircraft at 250+ kts the whole final and it
    // crossed the threshold 650 ft hot — fix12).
    AIControlOutput out = steer.steer(localizer_heading_rad(),
                                      target_alt_ft,
                                      target_speed_kts,
                                      in,
                                      /*throttle_floor=*/0.20);
    out.gear_handle_down = true;
    // STAB-E33/E35/E40: the sink guardian, SOFTER on the final track —
    // no MIL (the max-climb/MIL version pumps the beam oscillation:
    // fix12's guardian zoom carried the aircraft over the threshold 650
    // ft high), but a REAL arrest: WINGS LEVEL (in a bank the pitch loop
    // cannot raise the nose — the lift vector points sideways; fix13's
    // spiral dive sank −5,200 with 1.4 G on), pitch to 12 deg, 0.7 power.
    // (fix14: the previous +4-deg arrest was too weak for −5,000 fpm.)
    if (current_alt_agl_ft_ < 1400.0 && current_vs_fpm_ < -2500.0) {
        out.pitch_cmd = std::clamp(2.5 * (0.21 - current_pitch_rad_), -0.1, 0.5);
        out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
        out.throttle_cmd = std::max(out.throttle_cmd, 0.7);
    }
    // Phase C2: extend flaps on final. The commands are held steady from
    // OnFinal entry through touchdown; the FM actuates the actual surfaces
    // at TEF_RATE/LEF_RATE (flight_model.cpp:453-454). With flaps extended
    // the stall speed drops ~30 kts, which is what allows Phase C3's
    // approach_speed_kts reduction from 210 to 160.
    // STAB-E26: also on the INTERCEPT turn (pattern_turn) — the final turn
    // at approach speed is on the clean stall boundary (see the downwind
    // comment); the surfaces are already scheduled from base anyway.
    out.tef_cmd = landing_tef_cmd;
    out.lef_cmd = landing_lef_cmd;
    return out;
}

AIControlOutput LandingModule::controls_for_flare() const {
    // Phase C4 (FLIGHT_CONTROL_NEXT_STEPS.md §4 Phase C4): energy-managed
    // flare. The previous law held a fixed 8-deg pitch attitude at idle
    // throttle — it had no concept of energy. If the approach was high/fast
    // (which it often was before Phase C3), the aircraft carried extra
    // kinetic energy into the flare and floated long, landing 1000-3000 ft
    // past the threshold. If low/slow, the flare was late and the aircraft
    // touched down short.
    //
    // The new law predicts the touchdown point from the current state and
    // modulates flare pitch by the predicted-vs-aim error:
    //   td_distance = (alt_agl / max(|vs_fpm|, 50)) * vcas_kts * 1.68781 / 60
    //   td_along = course_along + td_distance
    // If td_along is past missed_along_ft or before -500 ft, go around.
    // Otherwise, modulate flare pitch by td_err = td_along - aim_along.
    //
    // This makes the flare law actively manage touchdown point instead of
    // passively holding 8 deg.
    AIControlOutput out;
    out.gear_handle_down = true;
    out.throttle_cmd = 0.0;  // idle

    // Phase C2: keep flaps extended through the flare (the surfaces stay
    // put until the aircraft slows on rollout).
    out.tef_cmd = landing_tef_cmd;
    out.lef_cmd = landing_lef_cmd;

    // Predicted touchdown point (linearized around the current state).
    //
    // STAB-E4: use the SINK RATE (-vs_fpm), not |vs_fpm|. The previous
    // |vs| formulation treated a CLIMB as a descent: at +50 fpm (floored)
    // from 59 ft AGL it predicted a touchdown 19,000 ft downrange and
    // commanded a climb-away — from a flare the aircraft had already
    // begun. Only sink closes the distance to the ground; a climbing
    // aircraft is not about to touch down anywhere.
    const double sink_fpm = std::max(-current_vs_fpm_, 50.0);
    const double time_to_ground_s = current_alt_agl_ft_ / sink_fpm * 60.0;
    const double v_fps = current_vcas_kts_ * 1.68781;
    const double td_distance_ft = time_to_ground_s * v_fps;
    const double td_along = course_along_ft() + td_distance_ft;

    // If the predicted touchdown is outside the runway bounds, transition
    // to GoAround. The state transition itself happens in
    // check_flare_or_goaround() (which fires independently of this output),
    // but we set a climbing pitch + MIL throttle here so the aircraft
    // doesn't sink further while the transition is processed.
    // STAB-E54: 3-second GRACE at flare entry — the prediction at entry
    // still reflects the beam ride's sink (-1,500 or worse) and can insta-
    // abort a recoverable flare (straight-in fix30 run: Flare -> GoAround
    // 0.6 s after entry with a marginally-long prediction). The flare law
    // needs a moment to establish its own sink before the arbiter speaks.
    // The balloon/overflight/timeout valves in check_touchdown() remain
    // active from the first tick.
    if (flare_timer_ > 3.0 &&
        (td_along > missed_along_ft || td_along < -500.0)) {
        out.pitch_cmd = 0.3;    // climb away
        out.throttle_cmd = 1.0;  // MIL
        out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
        return out;
    }

    // Modulate the flare by SINK RATE (STAB-E8), not by the touchdown
    // prediction. The prediction's time-to-ground diverges at small sink
    // (floored at 50 fpm it predicted touchdowns 18,000 ft downrange from
    // 59 ft AGL, commanding a MIL climb-away from a flare already begun —
    // the balloon + go-around in the on_glideslope trace). A real flare
    // targets a touchdown SINK RATE (~250 fpm) and modulates pitch to
    // achieve it; the touchdown-point prediction above remains the
    // go-around ARBITER only.
    //
    //   sink_err = target_sink - current_sink   (positive = sinking too fast)
    //   pitch_target = flare_pitch_deg + clamp(sink_err / 300, -3, +5) deg
    const double target_sink_fpm = -400.0;
    // Positive sink_err = sinking faster than the target → pitch up more.
    const double sink_err = target_sink_fpm - current_vs_fpm_;
    double flare_pitch_adj = std::clamp(sink_err / 300.0, -3.0, 5.0);  // deg
    // STAB-E13: small bounded trim from the touchdown-point prediction
    // (the C4 energy idea, demoted from driver to trim). Keeps the float
    // near the aim point: landing long → pitch up a touch more; short →
    // relax. Bounded to ±2 deg so it can never dominate the sink law.
    flare_pitch_adj += std::clamp((td_along - beam_aim_offset_ft) / 1000.0,
                                  -2.0, 2.0);
    const double target = (flare_pitch_deg + flare_pitch_adj) * D2R;
    out.pitch_cmd = std::clamp(flare_pitch_gain * (target - current_pitch_rad_),
                               -0.1, 0.5);
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);  // wings level
    return out;
}

AIControlOutput LandingModule::controls_for_rollout() const {
    AIControlOutput out = ground_steering.align_heading(
        runway_heading_rad_, ground_input(), 0.0, /*stop=*/false);
    out.throttle_cmd = 0.0;
    out.wheel_brakes = true;
    out.gear_handle_down = true;
    out.pitch_cmd = -0.3;  // nose-down: unwind the flare's pitch integrator
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
    // Go-around: climb straight ahead on the RUNWAY heading to just above
    // the pattern altitude; Reintercept then hands the geometry to
    // PatternDownwind (pattern mode, STAB-E24) or ProceedToFix.
    //
    // STAB-E28: the previous version (a) pinned MIL throttle while the
    // pitch law trimmed against the resulting speed error — the aircraft
    // accelerated to 440 kts and the speed-damp term (-16 deg of nose-down
    // trim at 200 kts fast) dove it ±6,700 fpm at 200-500 ft AGL — and
    // (b) steered by pure pursuit of the crosswind corner point, which
    // at 330+ kts orbits (turn radius ≈ distance to the corner).
    // Straight-ahead heading-hold + the speed PI is stable at any speed,
    // and the pattern legs' PLANE-crossing captures handle the turn back.
    //
    // STAB-E30: below 400 ft AGL fly a FIXED max-climb law — no cascade.
    // A go-around initiated low (deck-scrape intercept, fix3 trace: 13 ft
    // AGL, -2,100 fpm) needs unconditional pitch-up + MIL; the cascade's
    // damping terms (nose-down when fast / when VS exceeds command)
    // actively fought the climb-out from 50 ft and settled the aircraft
    // back onto the ground at 250 kts, ground-clamped forever.
    if (current_alt_agl_ft_ < 400.0) {
        AIControlOutput out;
        const double target = 12.0 * D2R;
        out.pitch_cmd = std::clamp(2.5 * (target - current_pitch_rad_), -0.3, 0.5);
        out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
        out.throttle_cmd = 1.0;
        out.gear_handle_down = (current_alt_agl_ft_ < 100.0);
        return out;
    }
    AIControlOutput out = pattern_steering.steer(
        runway_heading_rad_, pattern_altitude_ft_ + 800.0,
        pattern_speed_kts, air_input());
    // Zoom-climb aid while slow only: MIL below 250 kts helps the
    // climb-out without run-away above it.
    if (current_vcas_kts_ < 250.0) out.throttle_cmd = 1.0;
    out.gear_handle_down = false;
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
    in.roll_rate_radps = current_roll_rate_radps_;
    in.pitch_rate_radps = current_pitch_rate_radps_;
    in.vs_fpm = current_vs_fpm_;
    in.vcas_kts = current_vcas_kts_;
    in.alt_msl_ft = current_alt_msl_ft_;
    return in;
}

AirSteering::Input LandingModule::beam_input() const noexcept {
    // STAB-E34: air_input() + the glide beam's own descent rate as the
    // VS feedforward. The beam-parallel pattern legs (downwind leg 2,
    // base, intercept) ride targets that descend at the beam's own rate
    // (~-1,000 fpm at 200 kts) — without the feedforward the loop sees a
    // perpetually fresh altitude error, commands VS in steps, and the
    // delay through the FCS turns every step into a phugoid half-cycle
    // (fix7 trace: intercept VS +3,400 -> -5,400 around a beam+0 target,
    // ending at 21 ft AGL). With the ff the command is the beam rate by
    // construction and the loop only trims residuals — the same fix
    // STAB-E6 applied to OnFinal.
    AirSteering::Input in = air_input();
    const double v_fps = std::max(100.0, current_vcas_kts_ * 1.68781);
    in.vs_ff_fpm = -std::tan(glide_slope_angle_rad_) * v_fps * 60.0;
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

// f4-ai/src/landing_module.cpp
//
// LandingModule implementation — straight-in approach, landing, rollout,
// taxi-in. See header for the geometry conventions.

#include "f4/ai/modules/landing_module.hpp"
#include <f4/math/constants.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace f4::ai::modules {

namespace {
using f4::math::PI;
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
    // PHUG-P4 retune (STAB-E43 rescale): vs_gain 1.5 -> 3.0. The E43
    // reduction (4.0 -> 1.5) tamed a beam cycle sustained by the OLD inner
    // loop's ~10 s effective lag — 200 ft of beam error became 800 fpm of
    // command the airframe could not follow. The P4.1-corrected loop
    // delivers gamma within ~1 s, so the outer gain is no longer hot; at
    // 1.5 the law commands only ~-280 fpm against the spawn-transient
    // phugoid (+1,100 fpm at 188 ft above the target, measured) and the
    // aircraft floats 188 ft over the ProceedToFix target before the
    // reversal dives back through the beam (the establish-delay chain).
    // 3.0 halves the transient overshoot while staying inside the
    // corrected loop's bandwidth.
    air_steering.vs_gain = 3.0;
    // STAB-E18: 900 -> 1400. The beam feedforward alone is -980 fpm at
    // 185 kts on a 3-deg beam; with the VS cap at 900 the law could never
    // command even the beam's own rate, so a correctly-tracked final
    // drifted slowly high with no authority to correct (the ride floats
    // until the alt error grows the E10 window past the cap). The cap
    // must exceed |beam ff| + the correction window: -980 - 300 = -1280.
    // STAB-E65: 1,400 -> 1,800. The pattern-mode intercept establishes
    // ~350 ft above the beam (the catch-down equilibrium) and the ride
    // must null it before the deck: at 1,400 the commanded catch ran
    // -1,400 against the beam's own -1,080 — a ~70 fpm NET after the
    // FCS delivery gap — the catch was still 200 ft high at the threshold
    // and the firm arrival touched 2,639 ft out (the band ends at 2,500).
    // 1,800 doubles the net to ~470 fpm; the beam ride itself is
    // unaffected (it commands the beam rate +- residuals, far below the
    // clamp).
    air_steering.max_vs_fpm = 1800.0;
    air_steering.window_excludes_integral = true;  // STAB-E53
    // PHUG-P4 retune (findings §3.2 — M3 one level up): the altitude
    // integral's clamp is sized to the TRIM NEED, not the capture
    // authority. The STAB-E7 integral exists to null the P-only
    // steady-state beam offset (~40 ft -> ~60 fpm); the old 500 fpm
    // clamp let it saturate on any sustained 40-ft error in ~3 s and
    // unwind through a 10 s leak — at the InterceptFinal->OnFinal
    // handoff it carried a ~500 fpm climb bias that held the aircraft
    // 300-380 ft above the beam while the lateral capture peak was
    // measured (1500ftOffset: max_final_lateral 392 -> 417 ft with the
    // corrected P4.1 inner loop, which tracks the demand the broken
    // loop used to sag under). 150 fpm keeps the nulling authority
    // with 3x margin and bounds the handoff transient.
    air_steering.alt_integral_max = 150.0;
    // PHUG-P4 retune (STAB-E29 rescale): the 400 fpm/s slew limit was
    // tuned to the P3 G-loop's ~0.07 rad/s response — "a full-authority
    // change ramps over ~4 s, comparable to the FCS G-lag". P4.1 corrected
    // the inner loop to omega_sp ~ 0.8 rad/s (10x faster); the limiter is
    // now the bottleneck: ProceedToFix hands InterceptFinal a -1,300 fpm
    // demand that takes 3+ s to reverse, the aircraft dips ~150 ft below
    // the hold altitude, the dip-recovery climb blocks the SETTLED
    // establish gate until 232 ft ACROSS the course, and the lateral
    // window's overshoot grows past the 400-ft gate. 800 fpm/s halves the
    // handoff ramp (2 s) to match the corrected G-loop; the limiter keeps
    // its anti-ring role.
    air_steering.vs_slew_fpm_per_s = 800.0;
    // STAB-E10: base correction window ±300 fpm around the beam
    // feedforward, scaling up with altitude error (see air_steering.cpp).
    // Tight near the beam (smooth ride), full ±900 authority for a
    // from-below capture.
    air_steering.vs_corr_max_fpm = 300.0;
    // PHUG-P4 retune (M3 linear-band rule): path_gain rescaled
    // 0.0006 -> 0.00006. At 0.0006 the gamma-correction damper
    // saturated its 0.10-rad (5.7 deg) limit for ANY vs error beyond
    // 167 fpm — across the whole ±1,300 fpm handover phugoid it ran
    // as a bang-bang RELAY, and the P4.1-corrected inner loop
    // (omega_sp ~0.8 rad/s) faithfully executes the relay instead of
    // filtering it: the ProceedToFix handover dive-recover ring that
    // the broken loop could not drive is now fully excited (this is
    // the P2-measured L3 16.7-s relay cycle). 0.00006 puts the ring
    // amplitude (±1,300 fpm) at ~80% of the limit — a proportional
    // damper across the operating band that rails only beyond
    // ±1,670 fpm. STAB-E1's "unable to flatten" symptom does not
    // return: the corrected inner loop actually delivers the commanded
    // gamma within ~1 s, which is what the 0.00008-era tune lacked.
    air_steering.path_gain = 0.00006;
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
    // PHUG-P4 retune (M3 linear-band rule): 0.0006 -> 0.00006 — the same
    // rescale as the straight-in tune above. STAB-E28's gamma RELAY
    // history (saturated beyond ~420 fpm of vs error, bang-bang through
    // the FCS G-lag, ±4,000-8,000 fpm pattern limit cycle) is the same
    // mechanism: the P4.1-corrected inner loop now executes the relay
    // faithfully, so the damper must live in its linear band. At 0.00006
    // the ±1,500 fpm pattern-descent band maps to ~80% of the 0.10-rad
    // limit, proportional end to end.
    pattern_steering.path_gain = 0.00006;
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
        // REPAIR-T4b: the grounded short-touchdown strand — the guard in
        // check_fix_reached fires GoAround from the deck; without this
        // edge the event had no transition from ProceedToFix (no path
        // ever fired it before) and the strand persisted.
        .on(LandingState::ProceedToFix, LandingState::GoAround,
            LandingEvent::GoAround,
            nullptr, nullptr, "grounded_on_iap")
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
                req.airbase_id = airbase_id;
                bus_->publish(req);
            }
        
            // STAB-E48: arm the past-the-fix geometric capture for THIS
            // approach handoff only (start_in_approach spawns established
            // past the entry fix). Any GoAround disarms it — the climb-out
            // is itself past the fix, and re-capturing mid-missed-approach
            // ping-pongs the state machine.
            past_fix_capture_armed_ = true;})
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
        .on_enter(LandingState::InterceptFinal, [this](const LandingEvent&) {
            // PHUG-P4 retune (findings §3.6): latch the arrival altitude
            // for the straight-in do-not-climb hold (see the altitude
            // target selection in controls_for_state).
            intercept_entry_alt_ft_ = current_alt_msl_ft_;
            // The floor's convergence spare measures from THIS entry.
            prev_establish_lateral_ft_ = 1.0e9;
        })
        .on_enter(LandingState::OnFinal, [this](const LandingEvent&) {
            // Established inbound: request clearance to land.
            if (bus_) {
                atc::ApproachClearance req;
                req.aircraft_id = ownship_id_;
                req.runway_id = runway_id_;
                req.approach_type = "VISUAL";
                req.airbase_id = airbase_id;
                bus_->publish(req);
            }
        })
        .on_enter(LandingState::Flare, [this](const LandingEvent&) {
            // STAB-E3: start the flare timeout clock (see check_touchdown).
            flare_timer_ = 0.0;
        })
        .on_enter(LandingState::TaxiIn, [this](const LandingEvent&) {
            // The rollout ended: report the runway vacated. A sequencing
            // tower (TowerATC) releases this aircraft's runway claim on the
            // report and clears the next waiter; the StubATC ignores it.
            if (bus_) {
                atc::RunwayVacatedReport report;
                report.aircraft_id = ownship_id_;
                report.airbase_id = airbase_id;
                report.runway_id = runway_id_;
                bus_->publish(report);
            }

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
            if (std::getenv("F4_LAND_DEBUG") != nullptr) {
                std::fprintf(stderr,
                             "[ga-r] id %llu reason %s along %.0f agl %.0f "
                             "hdg %.2f lat %.0f beam %.0f\n",
                             (unsigned long long)ownship_id_,
                             ga_reason_.empty() ? "(cleared)" : ga_reason_.c_str(),
                             course_along_ft(), current_alt_agl_ft_,
                             AirSteering::heading_error(runway_heading_rad_,
                                                        current_heading_rad_)
                                 / D2R,
                             std::fabs(course_lateral_ft()),
                             std::fabs(current_alt_msl_ft_
                                       - glide_slope_alt_ft()));
            }
            if (bus_) {
                atc::GoAroundMessage msg;
                msg.aircraft_id = ownship_id_;
                msg.runway_id = runway_id_;
                msg.reason = !ga_reason_.empty()
                                 ? ga_reason_.c_str()
                                 : (cleared_to_land_ ? "threshold_overflown"
                                                     : "not_cleared");
                ga_reason_.clear();
                msg.airbase_id = airbase_id;
                bus_->publish(msg);
            }
        
            past_fix_capture_armed_ = false;  // STAB-E48: re-fly the procedure
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

    // FID-OPT-1: RAII subscription bundle (see TakeoffModule::initialize —
    // the leaked LandingClearance/ClearedToLand handlers outlived the
    // module and read freed memory on every later publish).
    subscriptions_.unsubscribe_all();
    subscriptions_.bind(bus);

    subscriptions_.subscribe<atc::LandingClearance>([this](const atc::LandingClearance& msg) {
        if (msg.aircraft_id != ownship_id_) return;
        runway_id_ = msg.runway_id;
        runway_heading_rad_ = msg.runway_heading_rad;
        threshold_position_ = msg.threshold_position;
        threshold_alt_ft_ = msg.threshold_altitude_ft;
        glide_slope_angle_rad_ = msg.glide_slope_angle_rad;
        pattern_altitude_ft_ = msg.pattern_altitude_ft;
        runway_width_ft_ = msg.runway_width_ft;
        runway_length_ft_ = msg.runway_length_ft;
        // Tranche 33: compute the intercept lead from the TURN RADIUS.
        // R (ft) = V² / (11.25 × tan(θ))  [V in knots, θ in degrees]
        // The aircraft must start the turn R feet before the course to
        // roll out on course (user guidance: "for a 90 degree intercept
        // the aircraft should start turning at R feet prior to the course").
        // The old fixed 1500 ft lead was < 1/4 of R at 185 kts / 25 deg
        // (R = 6525 ft) — the aircraft started too late and overshot.
        {
            const double V = approach_speed_kts;
            const double tan_theta = std::tan(air_steering.max_bank_rad);
            if (tan_theta > 0.01 && V > 10.0) {
                const double R_ft = (V * V) / (11.25 * tan_theta);
                // Set the lead floor to the turn radius (the minimum
                // distance the aircraft needs to complete the turn).
                // Keep the ratio form for larger offsets; the floor
                // ensures small offsets use at least R.
                intercept_lead_ft = std::max(intercept_lead_ft, R_ft);
            }
        }
        // CAMP-FAF — anchor a REAL final approach fix when the configured
        // entry fix is not one. The campaign's approach hands off at the
        // route's last waypoint — the landing waypoint at the AIRBASE
        // CENTER: along ≈ 0..+3500 ft, i.e., ON the runway. ProceedToFix
        // from there captures the fix over the field, fails the establish
        // floor immediately (the aircraft is past it), and go-arounds —
        // forever (the user's "overflying the runway, always going
        // around"). The scenario path's authors place the entry fix out on
        // the approach; the campaign path gets a FAF synthesized from the
        // clearance: 5 nm out on the extended centerline, where the 3-deg
        // beam sits at ~1,570 ft — the intercept's tuned pattern-altitude
        // catch-down envelope.
        //
        // REPAIR-T4b — the acceptance WINDOW adds a lateral gate to the
        // old along-only guard. The old guard (`along_fix > -15,000`)
        // kept the configured fix whenever it sat more than 15,000 ft
        // before the threshold — but the campaign's route-end waypoint is
        // the FIELD CENTER, and on a long runway (or a complex wide of
        // the runway) that point projects 15,000+ ft down the field from
        // the landing threshold AND up to tens of thousands of feet off
        // the extended centerline. Measured on the stock-save run: 7 of
        // the 13 fields with approach traffic kept such raw fixes; the
        // biggest drew 15,247 ProceedToFix telemetry rows — aircraft
        // orbiting a point 37,000-63,000 ft off the course line, where
        // the T4 fix-7 centerline capture gate (|course_lateral| < 8,000
        // ft) can never pass. The rule now keeps a configured fix only
        // when it IS an approach fix: the old along bound (15,000+ ft out
        // — close-in fixes still re-anchor to the 5-nm FAF, which leaves
        // the intercept room the establish floor needs) AND on the course
        // (within 2,000 ft of the extended centerline). Everything else
        // synthesizes the standard FAF.
        {
            const double cx = std::sin(runway_heading_rad_);
            const double cy = std::cos(runway_heading_rad_);
            const double along_fix =
                (entry_fix_.x - threshold_position_.x) * cx +
                (entry_fix_.y - threshold_position_.y) * cy;
            const double lat_fix =
                (entry_fix_.x - threshold_position_.x)
                    * std::cos(runway_heading_rad_)
              - (entry_fix_.y - threshold_position_.y)
                    * std::sin(runway_heading_rad_);
            constexpr double kFafOutFt = 30000.0;   // 5 nm
            constexpr double kFafLatTolFt = 2000.0;
            constexpr double kFafMinOutFt = 15000.0;
            const bool hand_authored_faf =
                std::abs(lat_fix) < kFafLatTolFt &&
                along_fix <= -kFafMinOutFt;
            if (!hand_authored_faf) {
                entry_fix_.x = threshold_position_.x - cx * kFafOutFt;
                entry_fix_.y = threshold_position_.y - cy * kFafOutFt;
                entry_fix_.z = threshold_alt_ft_ + 1500.0;
            }
        }
        // STAB-E9: latch instead of inline sm_.process() — the StubATC
        // answers synchronously inside publish(), which can originate
        // from our own sm_.reset() entry action (RequestApproach), making
        // an inline process() re-entrant (UB). Drained at update() top.
        if (sm_.current() == LandingState::RequestApproach) {
            deferred_event_ = LandingEvent::ApproachGranted;
        }
    });

    subscriptions_.subscribe<atc::ClearedToLand>([this](const atc::ClearedToLand& msg) {
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
                // REPAIR-T4: the gate sat at pattern+500 while the climb
                // cascade asymptotes ~350-480 ft above pattern at the
                // pattern-speed level trim (measured: two aircraft stalled
                // at pattern+356 and pattern+478, leveled 20-150 ft below
                // the gate forever). The capture gate and the altitude
                // gate both use pattern+300 — the re-intercept gate now
                // matches them (the reference's own shape: climb to
                // pattern altitude, re-enter).
                if (current_alt_msl_ft_ > pattern_altitude_ft_ + 300.0) {
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
        case LandingState::InterceptFinal: {
            // Pattern mode delivers the aircraft close in and ~90 deg
            // off; the intercept turn is short and tight, so fly it at
            // approach speed (the +40 was for the long straight-in
            // intercept at pattern altitude, where speed helps).
            // The descent is floored: laterally far off course the beam
            // altitude is meaningless, and chasing it down there put the
            // aircraft on the ground short of the runway.
            // NAV-F: never CLIMB to the beam on the intercept. When the
            // beam is above us (base leg handed off at 900 AGL while the
            // 3-deg beam at 30k out is ~1,700), the old law commanded a
            // +1,000+ fpm climb to it; through the FCS G-lag the aircraft
            // ballooned 900 ft OVER the beam and arrived at the establish
            // gates diving at -2,500 fpm — GA every cycle (digi pattern
            // E2E: 5 consecutive not_cleared go-arounds). Standard
            // procedure flies the intercept LEVEL at a safe altitude and
            // lets the descending beam arrive from above: hold (do-not-
            // climb) until the beam is within 200 ft, then ride it down.
            // From-above intercepts (straight-in at pattern altitude) are
            // unaffected: the beam is already below.
            // Tranche 38: intercept from below (user guidance: "intercept
            // from below while stable and established on course"). The
            // InterceptFinal HOLDS AT PATTERN ALTITUDE — it does NOT track
            // the beam down. The beam descends toward the aircraft; when it
            // reaches pattern altitude (~27000 ft out at 3 deg), the
            // aircraft is established on the beam (intercept from below).
            // OnFinal owns the beam tracking; InterceptFinal owns the
            // lateral convergence + level flight.
            //
            // The old law targeted the beam altitude directly — when the
            // aircraft arrived at 4000 ft (from enroute) and the beam was
            // at 1500 ft, it dove 2500 ft through the beam while in a 25-deg
            // bank turn, sinking to 604 ft (below the beam) and arriving at
            // OnFinal too low and too fast (GoAround every time).
            const double beam_now = glide_slope_alt_ft();
            const double floor_alt = threshold_alt_ft_ + intercept_floor_agl_ft;
            // Hold at pattern altitude (or the floor, whichever is higher).
            // The beam is tracked in OnFinal, not here.
            double intercept_alt = std::max(pattern_altitude_ft_, floor_alt);
            // Once the beam is within 200 ft of pattern altitude (close-in),
            // start tracking it — this is the intercept-from-below capture.
            if (beam_now < intercept_alt + 200.0) {
                intercept_alt = std::max(beam_now, floor_alt);
            }
            // PHUG-P4 retune (findings §3.6): straight-in arrivals must NOT
            // climb on the intercept. Hold the arrival altitude (latched at
            // state entry) and let the descending beam arrive from above —
            // the NAV-F principle above, applied to the pattern-altitude
            // hold as well. MEASURED (P4.1 inner loop, 1500ftOffset): the
            // corrected G-loop faithfully tracks the climb-to-pattern
            // target the broken loop used to sag under, the aircraft
            // balloons ~150 ft through the roll-out, the SETTLED
            // establish gate delays the handoff, and the lateral window's
            // overshoot grows 392 -> 417 ft (threshold 400). A level
            // intercept removes the balloon at the source.
            if (!fly_traffic_pattern) {
                // REPAIR-T4: the straight-in catch-down. The old law
                // pinned the hold at the ARRIVAL altitude for the whole
                // intercept — the beam-capture above only fires from
                // BELOW (beam within 200 of the hold), so any from-above
                // arrival (every real campaign RTB: the route's home leg
                // comes down from cruise) held ~650+ ft above the beam
                // while the establish gate's 400-ft beam check closed
                // out — the aircraft never established, overflew the
                // field at pattern altitude, and cycled. The ILS
                // straight-in shape: once the LOCALIZER is captured
                // (lateral inside the proportional band) the aircraft
                // tracks the descending beam down. NAV-F stands: the
                // target only moves DOWN (beam_now < hold), never a
                // climb above the hold.
                if (std::abs(course_lateral_ft()) < intercept_offset_ft &&
                    beam_now < intercept_alt) {
                    intercept_alt = std::max(beam_now, floor_alt);
                } else {
                    intercept_alt = std::min(
                        intercept_alt,
                        std::max(intercept_entry_alt_ft_, floor_alt));
                }
            }
            return track_final(intercept_alt,
                approach_speed_kts,
                /*pattern_turn=*/true);
        }
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
        //
        // REPAIR-T5 — the one-sided FAF clamp: ONLY an aircraft past the
        // THRESHOLD (the wrong side of the field — a go-around climb-out,
        // an RTB from beyond) aims back (a backward bearing at the fix —
        // the T4 fix-5 turn-around, now turn-anticipated). The earlier
        // "past the fix" form also bit the NORMAL final positions inside
        // the FAF — the pattern mode's base->final intercept hands over
        // there, and its backward aim never established (the digi
        // TrafficPattern red: zero touchdowns). Inside the FAF the
        // unclamped ILS law is correct.
        const double fx = std::sin(runway_heading_rad_);
        const double fy = std::cos(runway_heading_rad_);
        const double proj_along =
            (current_position_.x - threshold_position_.x) * fx +
            (current_position_.y - threshold_position_.y) * fy;
        const double along_fix =
            (entry_fix_.x - threshold_position_.x) * fx +
            (entry_fix_.y - threshold_position_.y) * fy;
        double lead = std::max(intercept_lead_ft,
                               intercept_lead_ratio * std::abs(xtrack));
        if (proj_along > 0.0) {
            lead = along_fix - proj_along;  // negative: aim BACK at the fix
        }
        const double bearing_to_aim = std::atan2(-xtrack, lead);
        return runway_heading_rad_ + bearing_to_aim;
    }
    // Right of centerline (lateral > 0) -> steer left (subtract correction).
    // STAB-E20: gain softened 0.0015 -> 0.0009 so the near-course law's
    // command at the intercept_offset boundary (400 ft: 0.36 rad = 21 deg)
    // is continuous with the scaled-lead law's cut there (~15 deg) — the
    // old 0.0015 gain produced a 34-deg command at the boundary, a bank
    // step UP right where the intercept should be relaxing.
    //
    // STAB-E47 (P4.3 lateral damping, measured): the P-only localizer
    // chases with the bank cascade's response lag and WEAVES — the
    // digi_full_mission final crosses the centerline every ~13 s with a
    // ~27 s period and NO decay (±250 ft limit cycle, phi ±8, psi ±5;
    // touchdown cross 180 ft, max tracking lateral 257 ft — both gates
    // missed by exactly this weave). The closing rate is the aircraft's
    // own cross-course velocity: feeding it back (k2 = 0.008 rad per
    // ft/s, design zeta ~= 1.0 nominal against omega_n = sqrt(g·k1) =
    // 0.127 rad/s, the heading-loop lag eats the rest) turns the tracker
    // into a damped 2nd-order loop. The contribution is clamped to
    // +-0.15 rad (8.6 deg) so a wide intercept cut (heading 25+ deg off
    // course inside the 600-ft band) cannot saturate the total command.
    const double v_fps = std::max(100.0, current_vcas_kts_ * 1.68781);
    const double v_cross_fps = v_fps * std::sin(current_heading_rad_
                                                - runway_heading_rad_);
    const double damp_corr = std::clamp(localizer_damp_gain * v_cross_fps,
                                        -localizer_damp_max_rad,
                                        localizer_damp_max_rad);
    const double corr = std::clamp(localizer_gain * xtrack + damp_corr,
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
    // REPAIR-T4b: the short-touchdown strand. An aircraft that meets the
    // deck during the IAP-leg catch-down is forgiven by the T1 ground
    // sweep (the Approach phase is a landing-owned context — no crash is
    // booked) but ProceedToFix had NO ground recovery: it drove the leg
    // at approach power forever, the sibling of the OnFinal deadfall
    // (fix 2). On the deck mid-approach the only honest way out is to fly
    // again — the go-around's low-altitude law is a rotation attempt
    // (MIL + pitch-up), which from the deck is a touch-and-go.
    if (on_ground_) {
        ga_reason_ = "grounded_on_iap";
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E48: past-the-fix capture (the start_in_approach handoff). An
    // aircraft ALREADY established inbound past the entry fix — projection
    // onto the course beyond the fix, inside the lateral corridor, rolling
    // out on the course heading, not climbing away — has effectively
    // arrived: flying 8,000 ft BACK to the fix is not a procedure. The
    // landing_only scenario spawns exactly there (30k ft out, on the
    // centerline, at the approach fix altitude), and the old law held
    // ProceedToFix forever (the abeam capture needs a 30 s dwell AND the
    // fix behind the nose; the orbit guard made both unreachable).
    //
    // The Tranche-38 pattern-altitude gate below does NOT apply here: that
    // gate protects the ENROUTE arrival that still has 2,500 ft to
    // descend; this arm admits only a non-climbing aircraft already on the
    // approach (the InterceptFinal do-not-climb latch owns the altitude
    // from there, and the go-around climb-out (vs > 500) is excluded so a
    // Reintercept cannot shortcut through this arm mid-missed-approach).
    {
        const double fx = std::sin(runway_heading_rad_);
        const double fy = std::cos(runway_heading_rad_);
        const double past_fix = (current_position_.x - entry_fix_.x) * fx
                              + (current_position_.y - entry_fix_.y) * fy;
        const double lat_fix = (current_position_.x - entry_fix_.x)
                                   * std::cos(runway_heading_rad_)
                             - (current_position_.y - entry_fix_.y)
                                   * std::sin(runway_heading_rad_);
        const double hdg_off = std::abs(AirSteering::heading_error(
            runway_heading_rad_, current_heading_rad_));
        if (past_fix_capture_armed_ &&
            past_fix > 0.0 &&
            std::abs(lat_fix) < std::max(establish_lateral_ft, 1000.0) &&
            hdg_off < 0.35 &&
            current_vs_fpm_ < 500.0) {
            past_fix_capture_armed_ = false;
            sm_.process(fly_traffic_pattern ? LandingEvent::PatternEntry
                                            : LandingEvent::FixReached);
            return;
        }
    }
    // CAMP-FAF: capture the fix APPROACHING — within the turn distance
    // ahead of the nose (FF CheckVector's `relx < turnDist &&
    // |rely| < turnDist*3`). The turn must START before the fix so the
    // roll-out is on the final course right AT it: the old abeam
    // capture fired when the fix passed BEHIND the nose, so the
    // aircraft overflew the FAF, turned 180° in place (displacing 2R
    // laterally), and spent minutes re-converging onto the lateral
    // gate — or never passed it.
    {
        const double dx = entry_fix_.x - current_position_.x;
        const double dy = entry_fix_.y - current_position_.y;
        const double nx = std::sin(current_heading_rad_);
        const double ny = std::cos(current_heading_rad_);
        const double rel_along = dx * nx + dy * ny;   // + = the fix ahead
        const double rel_lat = dx * std::cos(current_heading_rad_) -
                               dy * std::sin(current_heading_rad_);
        const double lead_base = std::max(intercept_lead_ft, fix_radius_ft);
        // REPAIR-T4b: the lead is also bounded below by the turn radius at
        // the CURRENT speed — the capture must start the intercept turn
        // early enough to roll out on the course. The approach-speed
        // radius (intercept_lead_ft, ~6,500 ft) is unreachable for a fast
        // handoff: at 250 kts / 23 deg bank the radius is ~13,100 ft and
        // the old 6,462-ft window let the aircraft close on the fix
        // without ever sequencing (the turn radius exceeded the capture
        // window — the orbit never tightened). The ProceedToFix leg is
        // flown with the pattern tune's bank cap, so that is the bank the
        // turn will actually use.
        double lead = lead_base;
        if (current_vcas_kts_ > 60.0) {
            const double turn_radius_ft =
                (current_vcas_kts_ * current_vcas_kts_)
                / (11.25 * std::tan(pattern_steering.max_bank_rad));
            lead = std::max(lead, turn_radius_ft);
        }
        // REPAIR-T4: the capture requires the LANDING direction — a
        // wrong-side arrival (heading against the runway) must not
        // sequence into the intercept (it would flip outbound; see the
        // aim-point note in controls_for_proceed_to_fix). Within 100 deg
        // of the runway heading = the landing hemisphere.
        const double hdg_to_runway = std::abs(AirSteering::heading_error(
            runway_heading_rad_, current_heading_rad_));
        // REPAIR-T4: the capture ALSO requires proximity to the
        // LOCALIZER (the centerline, not the fix). The nose-relative
        // window alone let an aircraft 11 NM off the course sequence
        // into the intercept the moment the fix crossed its nose —
        // the establish gate then rightly refused a 57,000-ft lateral
        // forever (measured: 12 approach cycles, zero OnFinal). The
        // ProceedToFix aim-point steers the aircraft onto the course
        // first; the intercept sequences only near it.
        if (fix_timer_ > 1.0 && hdg_to_runway < 1.75 &&
            std::abs(course_lateral_ft()) < 8000.0 &&
            rel_along < lead &&
            rel_along > -fix_abeam_ft &&
            std::abs(rel_lat) < lead * 3.0) {
            // The Tranche-38 altitude gate still owns the descent.
            if (current_alt_msl_ft_ > pattern_altitude_ft_ + 300.0) {
                return;
            }
            sm_.process(fly_traffic_pattern ? LandingEvent::PatternEntry
                                            : LandingEvent::FixReached);
            return;
        }
    }
    // Off-nose (abeam) capture with a dwell timer guard (same rationale
    // and same pitfall as NavigationModule — see the long comment there:
    // no timer => possible insta-skip while heading away or an orbit
    // deadlock; the timer resolves both). REPAIR-T4: the same
    // landing-direction requirement as the lead-window capture above —
    // a wrong-side arrival never sequences.
    if (waypoint_captured(entry_fix_, fix_timer_, fix_radius_ft, fix_abeam_ft) &&
        std::abs(course_lateral_ft()) < 8000.0 &&
        std::abs(AirSteering::heading_error(runway_heading_rad_,
                                            current_heading_rad_)) < 1.75) {
        // Tranche 38: altitude gate. Don't transition to InterceptFinal
        // until within 300 ft of pattern altitude. The old 2D-only capture
        // sequenced the aircraft into the intercept while still at 4000 ft
        // (from the enroute phase) — the dive to the beam from 4000 ft
        // while in a 25-deg bank turn arrived at OnFinal too low and too
        // fast (GoAround every time). The ProceedToFix state targets
        // pattern altitude; this gate ensures it actually gets there
        // before the intercept begins.
        // Only gate when ABOVE pattern altitude (the enroute arrival case).
        // Below pattern altitude (unit tests, go-around re-entry) proceeds
        // normally — the aircraft will climb to pattern altitude.
        if (current_alt_msl_ft_ > pattern_altitude_ft_ + 300.0) {
            return;  // Still descending to pattern altitude — hold in ProceedToFix
        }
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
    // REPAIR-T4: the missed-approach bound for the INTERCEPT. The
    // OnFinal state carries one (along > missed_along -> GA); the
    // intercept had none — an aircraft that captured the localizer
    // heading OUTBOUND (a wrong-side arrival that slipped the capture
    // check) flew the extended centerline away from the field forever,
    // "stable" (aligned + centered) at pattern altitude, establishing
    // nothing. Past the missed plane outbound = a busted approach.
    if (course_along_ft() > missed_along_ft) {
        ga_reason_ = "intercept_outbound";
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E21: not established by this close-in floor = the intercept is
    // not converging — go around cleanly instead of dragging a 90-deg
    // crosser through the missed-approach plane (observed: OnFinal entry
    // at 82 deg heading 15.8k out, threshold overflown at 1,905 ft AGL,
    // 1,285 ft off centerline).
    // The gate geometry first: the floor is a NOT-CONVERGING guard,
    // not a distance deadline. The CAMP-FAF QC measured a stable
    // approach (on course, on speed, descending) at the floor still
    // ~300-500 ft above the beam — the old floor fired on DISTANCE a
    // few seconds before the beam gate could pass, and the aircraft
    // go-arounded at 7,000 ft, forever.
    const double hdg_err_floor = std::abs(AirSteering::heading_error(
        runway_heading_rad_, current_heading_rad_));
    const double lat_now = std::abs(course_lateral_ft());
    const double hdg_now = hdg_err_floor;
    // Converging on ANY axis: the lateral closing, or the nose still
    // sweeping onto the course (a turn-back at the floor is a working
    // intercept, not a missed approach). Per-tick thresholds: the
    // lateral needs ~0.5 ft of closure, the heading ~0.3 deg of
    // sweep.
    const bool converging =
        lat_now < prev_establish_lateral_ft_ - 0.5 ||
        hdg_now < prev_establish_hdg_err_rad_ - 0.005;
    prev_establish_lateral_ft_ = lat_now;
    prev_establish_hdg_err_rad_ = hdg_now;
    const bool stable_shape =
        hdg_now < establish_hdg_tol_rad &&
        (lat_now < establish_lateral_ft || converging);
    if (course_along_ft() > -establish_floor_ft &&
        !stable_shape && !converging) {
        ga_reason_ = "intercept_not_established";
        if (std::getenv("F4_LAND_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "[land-dbg] floor: along %.0f vcas %.0f alt %.0f agl %.0f vs %.0f"
                         " on_ground %d hdg %.2f lat %.0f\n",
                         course_along_ft(), current_vcas_kts_, current_alt_msl_ft_,
                         current_alt_agl_ft_, current_vs_fpm_,
                         on_ground_ ? 1 : 0,
                         hdg_now, lat_now);
        }
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
    // (STAB-E45/E55) SETTLED. The E45 form (|vs| < 900) measured a
    // level-hold context; it conflates "settled" with "level" and can
    // never pass on an honest beam-riding catch-down (the pattern-mode
    // intercept rides the beam at -1,080..-1,150 fpm — a settled loop
    // equilibrium the old gate reads as a -1,150 fpm transient and
    // refuses forever). SETTLED means IN EQUILIBRIUM WITH THE COMMANDED
    // PATH: the aircraft's VS within 900 fpm of the VS the control law
    // itself commanded last frame (AirSteerDebug::vs_target_fpm — the
    // law's own demand). The E45 transient refusal is preserved: the
    // fix17 case (a +280 ft / +1,800 fpm climbing balloon against a
    // level/beam command) still fails by ~900+ fpm of command tracking
    // error, and the calm final still receives a loop in its linear band.
    const double beam_err = std::abs(current_alt_msl_ft_ - glide_slope_alt_ft());
    // T5 fix: the settle reference is the steering the intercept ACTUALLY
    // flies — track_final picks pattern_steering in pattern mode and
    // air_steering in straight-in mode, but this gate read
    // pattern_steering unconditionally. On a straight-in approach the
    // stale ProceedToFix target (~0 fpm, a level leg) sat in
    // pattern_steering while the live catch-down descended at −900 fpm
    // through air_steering: settle_err never dropped under 900 and the
    // approach could never establish — every intercept flew to the
    // missed plane and GA'd (measured: the stock-save observed flight,
    // intercept_outbound at along +2,503 with lat 11-28 ft and hdg
    // within 5 deg — a perfect lateral track refused forever). The
    // scenario path masked it: its short level ProceedToFix leg left the
    // stale value ≈ the real one.
    const AirSteering& settle_source = fly_traffic_pattern
                                           ? pattern_steering
                                           : air_steering;
    const double vs_commanded = settle_source.last_debug().vs_target_fpm;
    const double settle_err = std::abs(current_vs_fpm_ - vs_commanded);
    if (hdg_err < establish_hdg_tol_rad &&
        std::abs(course_lateral_ft()) < lat_tol &&
        // STAB-E54: establish_beam_tol_ft IS the configured gate — the
        // hardcoded 300.0 here was a silent re-tighten of the E23 knob
        // (the header default 400 never took effect). The pattern-mode
        // intercept rides the beam-catch-down equilibrium ~340 ft above
        // the beam at the floor: inside the configured 400, outside the
        // dead-code 300 — GoAround every cycle on a gate that was never
        // the documented one.
        beam_err < establish_beam_tol_ft &&
        settle_err < 900.0) {
        if (std::getenv("F4_LAND_DEBUG") != nullptr) {
            static int dbg_gate = 0;
            if (++dbg_gate % 120 == 1) {
                std::fprintf(stderr,
                             "[land-dbg] gates: hdg %.2f lat %.0f beam %.0f"
                             " settle %.0f along %.0f vs %.0f\n",
                             hdg_err, course_lateral_ft(), beam_err,
                             settle_err, course_along_ft(),
                             current_vs_fpm_);
            }
        }
        sm_.process(LandingEvent::Established);
    } else if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_gate2 = 0;
        if (++dbg_gate2 % 120 == 1) {
            std::fprintf(stderr,
                         "[land-dbg] id %llu gate FAIL: hdg %.2f/%.2f lat %.0f/%.0f"
                         " beam %.0f/%.0f settle %.0f/900 along %.0f\n",
                         (unsigned long long)ownship_id_,
                         hdg_err, establish_hdg_tol_rad,
                         std::abs(course_lateral_ft()),
                         fly_traffic_pattern
                             ? std::max(establish_lateral_ft, 1000.0)
                             : establish_lateral_ft,
                         beam_err, establish_beam_tol_ft, settle_err,
                         course_along_ft());
        }
    }
}

void LandingModule::check_flare_or_goaround() {
    // CAMP-FAF descent telemetry: the final-approach profile, 1 Hz.
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_fin = 0;
        if (++dbg_fin % 60 == 1) {
            std::fprintf(stderr,
                         "[final] along %+.0f agl %.0f beam %.0f err %+.0f vs %+.0f vcas %.0f cleared %d\n",
                         course_along_ft(), current_alt_agl_ft_,
                         glide_slope_alt_ft(),
                         current_alt_msl_ft_ - glide_slope_alt_ft(),
                         current_vs_fpm_, current_vcas_kts_,
                         cleared_to_land_ ? 1 : 0);
        }
    }
    // Tranche A2/39: the OnFinal lateral bounds guard is REMOVED. The
    // guard was too aggressive — it fired GoAround at 200-300 ft when the
    // aircraft was 100-200 ft off centerline (the normal localizer tracking
    // residual for a fast jet), preventing the aircraft from ever reaching
    // the flare. The Flare-state guard (check_touchdown) with the low-
    // altitude commit gate handles the lateral — below flare height the
    // aircraft commits to the landing. The OnFinal guard served no purpose
    // the Flare guard doesn't already cover.
    // Missed approach: overflew the threshold airborne, or descended
    // through decision height without clearance to land.
    if (course_along_ft() > missed_along_ft) {
        sm_.process(LandingEvent::GoAround);
        return;
    }
    if (current_alt_agl_ft_ < dh_goaround_agl_ft && !cleared_to_land_) {
        if (std::getenv("F4_LAND_DEBUG") != nullptr) {
            std::fprintf(stderr,
                         "[land-dbg] DH go-around: agl %.0f dh %.0f "
                         "cleared %d along %.0f ownship %llu\n",
                         current_alt_agl_ft_, dh_goaround_agl_ft,
                         cleared_to_land_ ? 1 : 0, course_along_ft(),
                         static_cast<unsigned long long>(ownship_id_));
        }
        sm_.process(LandingEvent::GoAround);
        return;
    }
    // STAB-E11: stable-approach gate. Flare entry requires being within
    // the near-runway environment: reaching flare HEIGHT this far out
    // means the approach is unstable (observed: crossing 60 ft AGL at
    // 9,500 ft short while still 200 ft below the beam — the flare law
    // cannot salvage that and touchdown would be far short of the
    // pavement). Go around and re-fly instead of flaring at the grass.
    // STAB-E64: the flare fires only when the sink is ARRESTABLE — the
    // E57/E58/E60 probe chain measured that an entry beyond ~-1,250 fpm
    // cannot be rounded out inside the FCS alpha lag (~2.5 s): the stored
    // G arrives at the deck and the aircraft bounces (the pattern-mode
    // catch-down rode into the flare still converging at -1,450..-1,850).
    // A firm arrival (the ride continues through the gate; the sink
    // guardian bounds the dive) beats a bounced flare that never lands.
    if (current_alt_agl_ft_ < flare_agl_ft) {
        // STAB-E11 (T5-widened): the low-side bound is the missed plane
        // PLUS the E55 flare-overrun allowance — the same shape as the
        // flare state's high-side bound. Measured: a slightly-low beam
        // ride (80 ft under) crossed flare height at along −2,508 — 8 ft
        // past the bare −2,500 — and insta-GA'd with an empty reason on
        // every attempt, three cycles running; the deck under a short
        // flare is flat ground (the sim's ground plane), not a cliff.
        if (course_along_ft() > -(missed_along_ft + flare_overrun_ft)) {
            // STAB-E64, RE-TIGHTENED by measurement (1,250 -> 900): the
            // stock-save flare crossed the height at −978 — inside the
            // old band — and the arrest STILL bounced (the sink doubled
            // through the FCS lag, the arrest developed at 16-65 ft, the
            // stored alpha ballooned the airframe +3,500 fpm, the balloon
            // valve GA'd; three knob generations — the direct servo, the
            // rate-damped servo, the +50-ft budget — reproduced the same
            // bounce bit-for-bit). The E64 doctrine stands: beyond the
            // arrestable band the RIDE is the landing — a firm arrival
            // the gear strut absorbs (the T4 fix-2 wheels-are-the-truth
            // path fires Touchdown in OnFinal).
            if (std::fabs(current_vs_fpm_) < 900.0) {
                sm_.process(LandingEvent::Flare);
            }
            // else: hold the ride — a firm touchdown follows.
            // REPAIR-T4: the ride's touchdown was never OBSERVED:
            // LandingEvent::Touchdown fired only in the Flare state,
            // so an aircraft that met the deck in OnFinal (the E64
            // firm arrival, sink beyond the arrestable band) stayed
            // OnFinal forever — rolling down the runway at approach
            // power with the SM convinced it was still airborne (the
            // digi-mission suite's "RunwayVacatedReport must fire"
            // reds rode exactly this deadfall). The wheels are the
            // truth: fire the Touchdown event wherever they touch,
            // inside the flare window.
            if (on_ground_) {
                sm_.process(LandingEvent::Touchdown);
            }
        } else {
            ga_reason_ = "flare_height_short_of_the_field";
            sm_.process(LandingEvent::GoAround);
        }
    }
}

void LandingModule::check_touchdown() {
    // Tranche A2/39: the flare lateral bounds guard is REMOVED. Once the
    // aircraft is in the flare (below 60 ft AGL) it has committed to the
    // landing — the lateral offset is the localizer tracking residual
    // (100-200 ft for a fast jet) and cannot be recovered by going around
    // this low. The flare + rollout handle the lateral alignment. The
    // OnFinal guard was already removed for the same reason.
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
    // REPAIR-T4b telemetry: the IAP leg's geometry AND its capture-window
    // state, 1 Hz — the orbit diagnosis needs to see which of the fix-
    // capture gates is closed at each pass (T4b: the aircraft orbited the
    // fix for 100+ minutes without the capture ever firing).
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_ptf = 0;
        if (++dbg_ptf % 60 == 1) {
            const double dx = entry_fix_.x - current_position_.x;
            const double dy = entry_fix_.y - current_position_.y;
            const double nx = std::sin(current_heading_rad_);
            const double ny = std::cos(current_heading_rad_);
            const double rel_along = dx * nx + dy * ny;
            const double rel_lat = dx * std::cos(current_heading_rad_)
                                 - dy * std::sin(current_heading_rad_);
            const double hdg_rw = AirSteering::heading_error(
                runway_heading_rad_, current_heading_rad_) / D2R;
            std::fprintf(stderr,
                         "[ptf] id %llu pos %.0f,%.0f fix %.0f,%.0f alt %.0f "
                         "pattern %.0f beam %.0f vcas %.0f hdgrw %+.0f "
                         "rAlng %+.0f rLat %+.0f cLat %+.0f\n",
                         (unsigned long long)ownship_id_,
                         current_position_.x, current_position_.y,
                         entry_fix_.x, entry_fix_.y, current_alt_msl_ft_,
                         pattern_altitude_ft_, glide_slope_alt_ft(),
                         current_vcas_kts_, hdg_rw, rel_along, rel_lat,
                         course_lateral_ft());
            {
                static int dbg_geom = 0;
                if (dbg_geom++ < 3) {
                    std::fprintf(stderr,
                                 "[ptf-geom] id %llu thr %.0f,%.0f rwyhdg %.1f deg "
                                 "fix %.0f,%.0f\n",
                                 (unsigned long long)ownship_id_,
                                 threshold_position_.x, threshold_position_.y,
                                 runway_heading_rad_ / D2R,
                                 entry_fix_.x, entry_fix_.y);
                }
            }
        }
    }
    // REPAIR-T5 — the IAP leg is a two-shape law on the projected along:
    //
    // WRONG SIDE (the projection past the fix — a go-around climb-out,
    // or an RTB arriving from beyond the field): pursue the FIX itself
    // (a real turn-around bearing) and hold the pattern altitude. The
    // FAF-clamped intercept law degenerates here — its lead floors at
    // 500 ft and the aircraft chases the line NORTHBOUND forever
    // (measured: 2,400 s of straight outbound line chase at 1,500 ft,
    // out of every capture window), and the beam extrapolated on the
    // far side is meaningless as an altitude target (the old target sat
    // below the re-intercept gate, stranding the aircraft at 1,500 ft
    // outside every window). The pursuit turns the aircraft around at
    // pattern altitude; as the projection crosses back before the fix,
    // the intercept law takes over smoothly.
    //
    // APPROACH SIDE: the course-line intercept law; the FAF clamp lives
    // inside localizer_heading_rad() (shared with the intercept).
    const double proj_along = course_along_ft();
    const double fx = std::sin(runway_heading_rad_);
    const double fy = std::cos(runway_heading_rad_);
    const double along_fix =
        (entry_fix_.x - threshold_position_.x) * fx +
        (entry_fix_.y - threshold_position_.y) * fy;
    constexpr double kWrongSideMarginFt = 1000.0;
    const bool wrong_side = proj_along > along_fix + kWrongSideMarginFt;
    double desired = wrong_side
        ? AirSteering::bearing_to(current_position_, entry_fix_)
        : localizer_heading_rad();
    // The reciprocal-heading deadlock (either branch): an aircraft whose
    // heading sits EXACTLY opposite the command — the overshoot ON the
    // course line — reads a wrapped error of +-180 deg whose sign flips
    // on every drift wobble. The bank target alternated +-0.40 tick by
    // tick (measured: phi weaving +-7 deg while the roll command
    // saturated +-1.000), the turn never committed, and the aircraft
    // flew the line outbound for hundreds of miles. Bias the command
    // toward the course line whenever the error is in the ambiguous
    // band: the error drops to ~150 deg with a deterministic sign and
    // the turn commits.
    if (std::fabs(AirSteering::heading_error(desired,
                                             current_heading_rad_)) > 2.62) {
        const double side = (course_lateral_ft() >= 0.0) ? -1.0 : 1.0;
        desired += side * 0.52;  // ~30 deg — well inside one wrap side
    }
    // CAMP-FAF rev 2 — the FF IAP ladder (atcbrain.cpp GetAltitude):
    // the entry/holding altitude is the 3-deg profile extended out from
    // the field, not a flat pattern leg. The old flat pattern-altitude
    // target handed the intercept an aircraft 1,000+ ft ABOVE the beam
    // inside the FAF (the descent then happened inside 2 nm, arriving
    // at the threshold high — the user's "overflying the runway").
    // Targeting max(pattern, the beam at this position) flies the
    // aircraft DOWN the extended profile to the FAF: at the FAF it is
    // on-slope, at approach speed, configured — exactly what the
    // intercept and OnFinal assume. The pattern floor keeps a
    // go-around's climb-out from diving (the field beam is below).
    // STAB-E19: still flown with the CALM pattern tune (the final's
    // attitude_gain + narrow correction window rings on long legs).
    // REPAIR-T4: the IAP leg's altitude CEILING is the FAF's own
    // crossing altitude. The old target rode the beam AT THE CURRENT
    // POSITION — on any outbound excursion (an orbit around the fix)
    // the 3-deg beam extrapolates HIGH and the target rises with it:
    // the aircraft chased the beam around the orbit, 500+ ft above the
    // capture gate's pattern+300 ceiling, and never captured (the
    // stock-save observed flight: 90 minutes in ProceedToFix, altitude
    // 2,186-2,461 against a 1,800 gate). The fix altitude is the
    // highest the IAP leg may command; the slope ride below it is the
    // intercept's and OnFinal's job.
    const double faf_ceiling_ft = entry_fix_.z + 300.0;
    const double profile_alt =
        wrong_side
            ? pattern_altitude_ft_  // the far-side beam is meaningless
            : std::max(pattern_altitude_ft_,
                       std::min(glide_slope_alt_ft(), faf_ceiling_ft));
    // FF's config schedule (landme.cpp): the gear drops at 3 nm at
    // lOnFinal entry and the pattern legs fly MinVcas — our
    // track_final (the intercept + OnFinal) already does exactly that.
    // The ProceedToFix/IAP leg stays clean.
    AIControlOutput out = pattern_steering.steer(desired, profile_alt,
                                                 approach_speed_kts,
                                                 air_input());
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_out = 0;
        if (++dbg_out % 60 == 1) {
            std::fprintf(stderr,
                         "[ptf-c] id %llu roll %+.3f pitch %+.3f thr %+.3f "
                         "hdg_err %+.2f bank_t %+.2f wrong %d\n",
                         (unsigned long long)ownship_id_,
                         out.roll_cmd, out.pitch_cmd, out.throttle_cmd,
                         AirSteering::heading_error(desired,
                                                    current_heading_rad_)
                             / D2R,
                         pattern_steering.last_debug().bank_target_rad,
                         wrong_side ? 1 : 0);
        }
    }
    return out;
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
    // REPAIR-T4 telemetry: the flare's own state, 1 Hz (the [final] rows
    // stop at flare entry — this row is what the touchdown-gate work
    // drives against).
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_flare = 0;
        if (++dbg_flare % 60 == 1) {
            std::fprintf(stderr,
                         "[flare] id %llu timer %.1f agl %.0f vs %+.0f vcas %.0f "
                         "pitch %.1f on_ground %d\n",
                         (unsigned long long)ownship_id_,
                         flare_timer_, current_alt_agl_ft_, current_vs_fpm_,
                         current_vcas_kts_, current_pitch_rad_ * 57.3,
                         on_ground_ ? 1 : 0);
        }
    }
    AIControlOutput out;
    out.gear_handle_down = true;
    out.throttle_cmd = 0.0;  // idle

    // Phase C2: keep flaps extended through the flare (the surfaces stay
    // put until the aircraft slows on rollout).
    out.tef_cmd = landing_tef_cmd;
    out.lef_cmd = landing_lef_cmd;

    // Predicted touchdown point — Tranche A3 fix: the go-around arbiter
    // now uses the NON-DIVERGENT beam-distance predictor (the same one the
    // pitch driver uses), NOT the sink-rate-based td_distance that
    // diverges at small sink. The old law: time_to_ground = alt / sink_fpm,
    // floored at 50 fpm — from 65 ft AGL ballooning at +930 fpm it computed
    // time_to_ground = 65/50*60 = 78 s, td_distance = 78 * 375 = 29,250 ft,
    // and fired GoAround from a normal flare balloon (the exact STAB-E4
    // bug the pitch driver fixed, but the arbiter still had it).
    //
    // The beam-distance predictor: td_distance = alt_agl / sin(glideslope).
    // Bounded, non-divergent, geometry-correct. From 65 ft at 3 deg:
    // 65 / 0.0524 = 1,241 ft — a real, bounded number.
    //
    // Tranche A3 balloon-tolerance: the arbiter ONLY fires when the
    // aircraft is SINKING (vs < 0). A balloon (vs > 0) is the flare law
    // working — the aircraft pitched up, bled energy, and is settling.
    // The check_touchdown() balloon valve (alt > 2*flare_height && vs > 0)
    // handles unrecoverable balloons; the arbiter must not fire on
    // transient balloons or it aborts every flare that isn't a perfect
    // sink-to-touchdown.
    const double v_fps = current_vcas_kts_ * 1.68781;
    const double sin_gs = std::sin(glide_slope_angle_rad_);
    const double td_distance_ft = (sin_gs > 0.01)
        ? (current_alt_agl_ft_ / sin_gs) : 0.0;
    const double td_along = course_along_ft() + td_distance_ft;

    // Go-around arbiter: fires only when (a) past the 3-second grace,
    // (b) the aircraft is SINKING (not ballooning), and (c) the predicted
    // touchdown is outside the runway. The balloon/overflight/timeout
    // valves in check_touchdown() remain active from the first tick.
    if (flare_timer_ > 3.0 && current_vs_fpm_ < 0.0 &&
        (td_along > missed_along_ft || td_along < -500.0)) {
        out.pitch_cmd = 0.3;    // climb away
        out.throttle_cmd = 1.0;  // MIL
        out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
        return out;
    }

    // Tranche A3: energy-height flare point-precision. STAB-E8 demoted the
    // touchdown predictor to a ±2 deg trim because the predictor's
    // time_to_ground (alt / sink_fpm) diverged at small sink — floored at
    // 50 fpm it predicted touchdowns 18,000 ft downrange. The cure is a
    // NON-DIVERGENT predictor that stays speed-dependent (a faster aircraft
    // carries more kinetic energy, floats longer, lands longer — the STAB-E8
    // sink-rate-only law threw away that information).
    //
    // Energy height (specific energy):  h_e = alt_agl + v^2 / (2g)
    //   — the altitude the aircraft would reach if it traded all KE for PE,
    //     or equivalently the airspeed it would have at sea level from PE.
    //   — bounded (alt and v are both bounded), non-divergent (no division
    //     by sink), and speed-dependent (v^2 term).
    //
    // The aim point on the beam has its own energy height:
    //   h_aim = beam_aim_offset_ft * sin(glideslope)
    //   (the altitude of the beam at the aim distance — for a 1500 ft aim
    //   on a 3-deg beam, ~78 ft. The aircraft should arrive at the aim
    //   point at ~0 AGL with approach speed, so its target h_e ≈ h_aim.)
    //
    // Energy excess = h_e - h_aim. Positive = too much energy (will land
    // long / float) → pitch up to bleed. Negative = too little (will land
    // short) → relax. The sink-rate floor (STAB-E8) prevents diving when
    // the energy is low but the sink is hard.
    const double g_fps2 = 32.177;   // gravity, ft/s^2
    // v_fps and sin_gs are already in scope (declared by the go-around arbiter above).
    const double ke_height_ft = 0.5 * v_fps * v_fps / g_fps2;  // specific KE
    const double h_e_ft = current_alt_agl_ft_ + ke_height_ft;
    const double h_aim_ft = beam_aim_offset_ft * (sin_gs > 0.01 ? sin_gs : 0.0524);
    const double energy_excess_ft = h_e_ft - h_aim_ft;
    // Energy driver: the scale (/2000) is tuned so a 250-kt jet at 50 ft
    // AGL (~2800 ft energy height, ~2740 ft excess) commands ~1.4 deg of
    // extra flare pitch, while a 160-kt baseline (~1100 ft excess) commands
    // ~0.55 deg. Both stay below the ±0.5 pitch_cmd clamp so the speed-
    // differentiation is observable (the old STAB-E8 trim was ±2 deg; this
    // is the same range but as the driver, not the trim). Negative excess
    // (slow + low) relaxes toward the sink floor.
    //
    // REPAIR-T4: inside the touchdown gate the aim-point management is
    // OVER — its extra pull at 17 ft / -1,386 fpm is a float command,
    // and the measured flare held ~6 ft AGL for the full 15 s timeout
    // (a hover). Below the gate the symmetric sink-rate servo alone
    // owns the pitch: it arrests hard sinks (the entry arrest) and
    // pushes to develop the touchdown sink; the strut absorbs it.
    // REPAIR-T5 — the flare law is the DIRECT VS SERVO FROM ENTRY (the
    // T4 touchdown servo, now the whole flare): the pitch stick IS the
    // sink error against the −700-fpm touchdown target. The T4 shape —
    // an attitude/energy phase above a 60-ft gate, the servo below —
    // only ever worked in the degenerate landing_only case (measured
    // entry: 6 ft / vs −0, a deck hover). The REAL flare (stock-save
    // measured: 130 ft entry at −978) exposed the attitude phase: the
    // energy driver's +0.9-deg trim approached the target at ~0.157
    // stick while the sink GREW −978 → −2,060 (the E4 ground-effect
    // equilibrium again), the late servo arrest at 16 ft ballooned the
    // airframe to 300 ft, and the 15-s timeout GA'd every attempt. The
    // servo owns the loop from entry: full-push arrest at a hard sink,
    // zero stick at the target, the bounded push at a balloon; the
    // ground bounds it.
    const double target_sink_fpm = -700.0;  // STAB-E61: the touchdown sink
    const double vs_err = target_sink_fpm - current_vs_fpm_;
    // REPAIR-T5: pitch-RATE damping on the servo — without it the arrest
    // overshoots through the FCS lag exactly as the attitude loop's
    // history measured: the stick saturated while the rotation developed
    // unchecked (pitch 8 -> 27 deg, the sink −2,166 arrested to +3,760 —
    // the balloon valve GA'd every attempt). The damp term grows exactly
    // when the rotation develops and caps the round-out attitude.
    out.pitch_cmd = std::clamp(vs_err * flare_touchdown_vs_gain
                                   - 0.8 * current_pitch_rate_radps_,
                               -flare_touchdown_push_clamp, 0.5);
    // (The attitude/energy flare machinery — the energy driver, the
    // sink-rate floor, the 8-deg attitude target and the 60-ft touchdown
    // gate — is RETIRED by REPAIR-T5: the direct VS servo above owns the
    // flare from entry. See the servo block's note for the measured
    // history.)
    // STAB-E63: the flare keeps flying the LOCALIZER with a bounded-bank
    // heading chase (wings-level let the flare-start residual plus ~8 s of
    // drift ride to the touchdown — 75 ft measured vs the 50 ft gate). The
    // PD heading hold nulls the residual heading and the cross-track drift
    // with banks bounded ~4 deg: the stick saturates at 0.12 (roll rate
    // ~21 deg/s) only for a large error, and the FCS roll loop's own rate
    // damping carries the rest.
    const double lat_hdg_err = AirSteering::heading_error(localizer_heading_rad(),
                                                          current_heading_rad_);
    out.roll_cmd = std::clamp(1.2 * lat_hdg_err
                                  - 0.6 * current_roll_rate_radps_,
                              -0.12, 0.12);
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
    // REPAIR-T4 telemetry: the climb-out's own state, 1 Hz.
    if (std::getenv("F4_LAND_DEBUG") != nullptr) {
        static int dbg_ga = 0;
        if (++dbg_ga % 60 == 1) {
            std::fprintf(stderr,
                         "[ga] id %llu agl %.0f alt %.0f pattern %.0f pitch %.1f "
                         "vcas %.0f\n",
                         (unsigned long long)ownship_id_,
                         current_alt_agl_ft_, current_alt_msl_ft_,
                         pattern_altitude_ft_, current_pitch_rad_ * 57.3,
                         current_vcas_kts_);
        }
    }
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

// f4-ai/src/wvr_module.cpp
//
// WVRModule implementation — see modules/wvr_module.hpp for design notes.
//
// FreeFalcon reference: wvrengage.cpp (WvrChooseTactic state pick),
// merge.cpp (closure + sorting geometry), gunsjink.cpp (defensive break
// turns), mengage.cpp (IR fire control — via the embedded MissileModule).

#include "f4/ai/modules/wvr_module.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace f4::ai::modules {

namespace {

constexpr double FEET_PER_NM = 6076.11548;
constexpr double PI = 3.14159265358979323846;

/// Any one detection source = visible (SensorFusion::can_see's rule,
/// inlined to keep the module layer on TargetInfo snapshots only).
[[nodiscard]] inline bool can_see(const TargetInfo& t) noexcept {
    return t.detected_by_radar || t.detected_by_rwr ||
           t.detected_by_visual || t.detected_by_gci;
}

/// Can this module fight this target? Hostile, visible, a fighter-class
/// contact (not an incoming missile — that is MissileModule's job).
[[nodiscard]] inline bool engageable(const TargetInfo& t) noexcept {
    return t.is_hostile && !t.is_missile && can_see(t);
}

/// Wrap to [0, 2*pi).
[[nodiscard]] inline double wrap_2pi(double a) noexcept {
    while (a < 0.0) a += 2.0 * PI;
    while (a >= 2.0 * PI) a -= 2.0 * PI;
    return a;
}

} // anonymous namespace

// ============================================================================
// Construction + FSM
// ============================================================================

WVRModule::WVRModule()
    : sm_(build_sm())
{
    // Combat tune, BVR's philosophy pushed one notch harder: the merge
    // banks near the aerodynamic limit (70 deg), runs the throttle rail
    // to AB, and the IR fire control is tuned for heaters (shorter
    // cooldown — a heater off the rail barely disturbs the shooter —
    // and a lower Pk floor: IR shots are opportunity shots).
    air_steering_.max_bank_rad = 1.22;       // ~70 deg
    air_steering_.balloon_guard_fpm = 1000000.0;
    air_steering_.throttle_max = 1.5;        // AB available in the fight
    // The vertical authority too. The nav-comfort VS tune (STAB-E1's
    // 2,500 fpm cap + STAB-E29's 400 fpm/s slew) cannot hold a fight
    // plane against a spawn-energy balloon: the guns-merge eagle
    // balloons at ~4,900 fpm while the merge's descent command slews
    // toward ~2,500 — the pair diverges ~850 ft vertically through the
    // merge and the gun cone (1.5 deg at 1,500 ft) never closes. A
    // fight lives seconds; the CA break already flies this authority
    // (12,000 fpm, slew off) for exactly this reason — the comfort
    // limiters are the wrong tune for it. The E46 balloon guard stays
    // off (set above): the merge's own altitude command IS the
    // anti-balloon loop.
    air_steering_.max_vs_fpm = 12000.0;
    air_steering_.vs_slew_fpm_per_s = -1.0;  // STAB-E29: OFF in the fight
    fire_.config().fire_cooldown_sec = 3.0;
    fire_.config().pk_base = 0.9;            // heater-class reliability
    fire_.config().shoot_shoot_threshold = 0.35;
    fire_.set_envelope_nm(0.5, 8.0);         // AIM-9M doctrine default
}

fsm::StateMachine<WVRState, WVREvent>
WVRModule::build_sm()
{
    return typename fsm::StateMachine<WVRState, WVREvent>::Builder()
        .initial(WVRState::None)
        .state(WVRState::None,       "None")
        .state(WVRState::Merge,      "Merge")
        .state(WVRState::Offensive,  "Offensive")
        .state(WVRState::Defensive,  "Defensive")
        .state(WVRState::BugOut,     "BugOut")
        .event_name(WVREvent::TargetNear,         "TargetNear")
        .event_name(WVREvent::Advantage,          "Advantage")
        .event_name(WVREvent::Threat,             "Threat")
        .event_name(WVREvent::Neutralized,        "Neutralized")
        .event_name(WVREvent::Separate,           "Separate")
        .event_name(WVREvent::SeparationComplete, "SeparationComplete")
        .event_name(WVREvent::LostTarget,         "LostTarget")
        .on(WVRState::None, WVRState::Merge, WVREvent::TargetNear,
            nullptr, nullptr, "hostile_inside_wvr_band")
        .on(WVRState::Merge, WVRState::Offensive, WVREvent::Advantage,
            nullptr, nullptr, "we_hold_the_angle")
        .on(WVRState::Merge, WVRState::Defensive, WVREvent::Threat,
            nullptr, nullptr, "target_holds_the_angle")
        .on(WVRState::Offensive, WVRState::Defensive, WVREvent::Threat,
            nullptr, nullptr, "angle_flipped_to_target")
        .on(WVRState::Offensive, WVRState::Merge, WVREvent::Neutralized,
            nullptr, nullptr, "angle_washed_out")
        .on(WVRState::Defensive, WVRState::Offensive, WVREvent::Advantage,
            nullptr, nullptr, "countered_back_to_angle")
        .on(WVRState::Defensive, WVRState::Merge, WVREvent::Neutralized,
            nullptr, nullptr, "threat_angle_washed_out")
        .on(WVRState::Merge, WVRState::BugOut, WVREvent::Separate,
            nullptr, nullptr, "doctrine_separate")
        .on(WVRState::Offensive, WVRState::BugOut, WVREvent::Separate,
            nullptr, nullptr, "doctrine_separate")
        .on(WVRState::Defensive, WVRState::BugOut, WVREvent::Separate,
            nullptr, nullptr, "shots_spent_defense_sustained")
        .on(WVRState::BugOut, WVRState::None, WVREvent::SeparationComplete,
            nullptr, nullptr, "range_reopened_past_exit_ring")
        .on(WVRState::Merge, WVRState::None, WVREvent::LostTarget,
            nullptr, nullptr, "target_gone")
        .on(WVRState::Offensive, WVRState::None, WVREvent::LostTarget,
            nullptr, nullptr, "target_gone")
        .on(WVRState::Defensive, WVRState::None, WVREvent::LostTarget,
            nullptr, nullptr, "target_gone")
        .on(WVRState::BugOut, WVRState::None, WVREvent::LostTarget,
            nullptr, nullptr, "target_gone_while_separating")
        .build();
}

// ============================================================================
// Public API
// ============================================================================

void WVRModule::reset() {
    clear_engagement();
    sm_ = build_sm();
    tactic_ = WVRTactic::None;
    dwell_timer_ = 0.0;
    defensive_timer_ = 0.0;
    jink_timer_ = 0.0;
    jink_side_ = +1;
    desired_heading_rad_ = 0.0;
    desired_alt_ft_ = 0.0;
    // The guns reset WITH the fight (trigger state), but the velocity
    // history is the ownship's, not the fight's — it survives so the
    // next engagement has a boresight estimate from its first tick.
}

bool WVRModule::own_advantage(const TargetInfo& t) noexcept {
    // WE hold the angle when the target sits in our forward cone and is
    // pointed away from us: we can chase, it cannot shoot back.
    // Hysteresis margins (75/100 deg) keep the two classes from touching
    // at exactly 90 deg — the merge geometry sits between them.
    return t.ata_from_rad < (75.0 * PI / 180.0) &&
           t.ata_rad > (100.0 * PI / 180.0);
}

bool WVRModule::target_advantage(const TargetInfo& t) noexcept {
    // The TARGET holds the angle when it is outside our forward cone and
    // pointed at us: we cannot employ, it can (guns/IR zone).
    return t.ata_from_rad > (105.0 * PI / 180.0) &&
           t.ata_rad < (80.0 * PI / 180.0);
}

AIControlOutput WVRModule::update(double dt,
                                  const flight::IAircraftState* state,
                                  const TargetInfo* target) {
    AIControlOutput out{};

    // Intents are single-tick pulses by contract.
    release_pulse_ = false;
    gun_pulse_ = false;
    // The committed-merge flag starts each tick clean; the
    // Merge/Offensive steering cases set it (a fight inside the gun
    // band — either side, trigger armed or not).
    merge_committed_ = false;

    // The IR fire-control cooldown + the gun trigger cycle burn every
    // tick, fight or not (physical time, not doctrine time).
    fire_.tick_cooldown(dt);
    guns_.tick(dt);

    if (!state) {
        target_ = nullptr;
        wants_lock_ = false;
        return out;
    }

    // Cache the ownship picture for the steering helpers.
    current_heading_rad_ = state->heading_rad();
    current_pitch_rad_ = state->pitch_angle_rad();
    current_roll_rad_ = state->roll_angle_rad();
    current_roll_rate_radps_ = state->roll_rate_radps();
    current_pitch_rate_radps_ = state->pitch_rate_radps();
    current_vs_fpm_ = state->vertical_speed_fpm();
    current_vcas_kts_ = state->vcas_kts();
    current_alt_msl_ft_ = state->altitude_msl_ft();
    current_position_ = geo::WorldPosition(state->position_east_ft(),
                                           state->position_north_ft(),
                                           state->altitude_msl_ft());

    // Ownship velocity estimate (the guns boresight): consecutive
    // positions / dt. The first update has no history -> zero -> the
    // guns hold fire that tick (a solution you cannot measure is not
    // one you can fire on).
    velocity_estimate_ = f4::math::Vec3<double>{0.0, 0.0, 0.0};
    if (has_prev_position_ && dt > 0.0) {
        velocity_estimate_ = f4::math::Vec3<double>{
            (current_position_.x - prev_position_.x) / dt,
            (current_position_.y - prev_position_.y) / dt,
            (current_position_.z - prev_position_.z) / dt};
    }
    prev_position_ = current_position_;
    has_prev_position_ = true;

    // Target validity for THIS tick. Out-of-band (the fight reopened past
    // the exit ring) reads as no fight — the brain is the rung authority
    // and hands the band back to BVRModule; this guard keeps a stale
    // target pointer from resurrecting the module on its own.
    const bool fightable = target != nullptr && engageable(*target) &&
                           target->range_nm <= cfg_.wvr_exit_range_nm;
    target_ = fightable ? target : nullptr;

    dwell_timer_ += dt;
    jink_timer_ += dt;

    // ------------------------------------------------------------------
    // Transition logic (geometry + doctrine, dwell-guarded).
    // ------------------------------------------------------------------
    switch (sm_.current()) {
        case WVRState::None:
            if (fightable) {
                engage(*target);
                sm_.process(WVREvent::TargetNear);
            }
            break;

        case WVRState::Merge:
        case WVRState::Offensive:
        case WVRState::Defensive: {
            if (!fightable) {
                // Target dead, no longer visible, or the range reopened
                // past the exit ring — fight over for this module.
                sm_.process(WVREvent::LostTarget);
                clear_engagement();
                break;
            }
            if (engagement_target_id_ != target->entity_id) {
                // Re-target: reset the bookkeeping, re-engage in Merge.
                clear_engagement();
                engage(*target);
                sm_.process(WVREvent::LostTarget);
                sm_.process(WVREvent::TargetNear);
                break;
            }

            // Geometry classification, dwell-guarded: a flip is only
            // accepted once the current state has been held at least
            // tactic_dwell_sec (anti-chatter — the angles swing fast).
            const bool dwell = dwell_timer_ >= cfg_.tactic_dwell_sec;
            if (sm_.current() == WVRState::Merge) {
                if (dwell && own_advantage(*target)) {
                    sm_.process(WVREvent::Advantage);
                } else if (dwell && target_advantage(*target)) {
                    sm_.process(WVREvent::Threat);
                }
            } else if (sm_.current() == WVRState::Offensive) {
                if (dwell && target_advantage(*target)) {
                    sm_.process(WVREvent::Threat);
                } else if (dwell && !own_advantage(*target)) {
                    sm_.process(WVREvent::Neutralized);
                }
            } else {  // Defensive
                if (dwell && own_advantage(*target)) {
                    sm_.process(WVREvent::Advantage);
                } else if (dwell && !target_advantage(*target)) {
                    sm_.process(WVREvent::Neutralized);
                }
            }

            // Doctrine: the bug-out is only available once the IR
            // allotment is spent AND the defense has been sustained
            // (defensive_grace_sec). While heaters remain there is
            // always a reason to stay.
            if (sm_.current() == WVRState::Defensive) {
                defensive_timer_ += dt;
                if (fire_.shots_fired() >=
                        fire_.config().shoot_shoot_max_shots &&
                    defensive_timer_ >= cfg_.defensive_grace_sec) {
                    sm_.process(WVREvent::Separate);
                    break;
                }
            } else {
                defensive_timer_ = 0.0;
            }
            break;
        }

        case WVRState::BugOut:
            // The separation is complete once the range reopens past the
            // exit ring (or the target is gone — nothing to separate
            // from). BVRModule re-owns the reopened fight.
            if (!fightable || target->range_nm > cfg_.wvr_exit_range_nm) {
                sm_.process(WVREvent::SeparationComplete);
                clear_engagement();
            }
            break;
    }

    // ------------------------------------------------------------------
    // Tactic selection + steering + intents per state.
    // ------------------------------------------------------------------
    wants_lock_ = false;

    switch (sm_.current()) {
        case WVRState::None:
            tactic_ = WVRTactic::None;
            return out;  // empty: the brain flies its mission module

        case WVRState::Merge: {
            tactic_ = WVRTactic::RandP;
            wants_lock_ = true;   // keep the STT hot through the merge
            desired_heading_rad_ = pursuit_heading_rad();
            // The fight plane: the altitude captured at engage() — the
            // engage() contract's own words ("the altitude the vertical
            // game weaves around"). Chasing the target's LIVE altitude
            // here builds a two-brain positive feedback loop: both sides
            // of a merge run this same doctrine, each chasing the
            // other's lagged climb response, and the pair diverges
            // vertically through the merge (measured: 846 ft apart by
            // t+10 s of the guns fight — eagle at 16,653 ft climbing
            // +81 ft/s while its bandit chased it at +195 ft/s from
            // 15,807 ft). The gun fire control pays for that divergence
            // first: the lead point rides hundreds of feet off the
            // boresight plane and the hit-quality cone (1.5 deg at
            // 1,500 ft) can never close through the transient. The
            // Defensive weave and the BugOut already reference this
            // captured plane; the Merge now holds it too, and the gun
            // branch's lead-point tracking (below) owns the remaining
            // vertical aiming.
            desired_alt_ft_ = clamp_alt_ft(engage_alt_ft_);
            // GUNS (Steps 11-12) — the merge SNAPSHOT. While the gun is
            // armed and the target is inside its envelope, the steering
            // tracks the GUN solution (aiming IS steering there); the
            // trigger goes down when the nose sits inside the hit-quality
            // cone. Guns tight: the pursuit flies the merge exactly as
            // before (a heater fight, untouched).
            const bool gun_work =
                fightable && !guns_.config().hold_fire &&
                guns_.in_envelope(*target, current_position_);
            // The committed merge (the cavoid exemption): INSIDE the
            // gun COMMIT BAND (the outer edge only — see GunModule::
            // in_commit_band; the trigger envelope's minimum bound ends
            // the commit exactly at the pass's most lethal second) the
            // pass owns the geometry for BOTH jets — this one whether
            // its own trigger is armed (gun_work) or the opponent holds
            // the angle on us (a drone defending the pass holds its
            // line; a 0.7-s break spoils nothing but the pass). FRESH
            // track-file range — the fusion's range_nm is seconds stale
            // at merge closure.
            merge_committed_ =
                fightable &&
                guns_.in_commit_band(*target, current_position_);
            if (gun_work && !gun_steering_active_) {
                // The steering reference CHANGES here (level merge ->
                // the gun's climbing lead line). Integrators wound on
                // the old reference are windup on the new one (the
                // measured ~0.2-deg vertical overshoot that held the
                // trigger closed through the whole window) — reset, the
                // same reference-change rule every rung handoff follows.
                air_steering_.reset_integrators();
            }
            gun_steering_active_ = gun_work;
            if (gun_work) {
                desired_heading_rad_ = guns_.lead_heading_rad(
                    *target_, current_position_);
                desired_alt_ft_ = clamp_alt_ft(
                    guns_.lead_point(*target_, current_position_).z);
            }
            // Fire control: the merge head-on IS the IR opportunity shot
            // (all-aspect heater) — but only into the forward cone: a
            // heater at a target on our six has nothing to track.
            if (fightable && target->ata_from_rad < cfg_.fire_cone_rad &&
                fire_.should_fire(*target)) {
                fire_.note_fired();
                release_pulse_ = true;
            }
            // The guns' own gate (envelope + hit-quality cone + trigger
            // state + ROE, all inside should_fire).
            if (fightable &&
                guns_.should_fire(*target, current_position_,
                                  velocity_estimate_)) {
                guns_.note_burst();
                gun_pulse_ = true;
            }
            break;
        }

        case WVRState::Offensive: {
            wants_lock_ = true;
            desired_heading_rad_ = pursuit_heading_rad();
            // GUNS (Steps 11-12) — inside the gun envelope (armed) the
            // steering tracks the GUN solution, not the missile-grade
            // pursuit: the bullet's lead point is far short of the
            // pursuit lead at these ranges, and the gun fires where the
            // nose points. Steering there IS aiming. (Envelope + lead
            // both use the track-file PREDICTION — the gun's window is
            // seconds deep, stale snapshot geometry would miss it
            // entirely.) Guns tight: the pursuit, as before.
            const bool gun_work =
                fightable && !guns_.config().hold_fire &&
                guns_.in_envelope(*target, current_position_);
            // The commit band (see the Merge case): the geometry stays
            // owned through the Offensive pass too — the overshoot
            // offset turn below is a WEAPONS maneuver, and the min-bound
            // race measured on the guns merge (predicted range crossing
            // below 0.08 NM mid-pass) lives here as well.
            merge_committed_ =
                fightable &&
                guns_.in_commit_band(*target, current_position_);
            gun_steering_active_ = gun_work;
            if (gun_work) {
                desired_heading_rad_ = guns_.lead_heading_rad(
                    *target_, current_position_);
            }
            // Overshoot control (FreeFalcon OverB): inside the overshoot
            // guard with hard closure, offset the pursuit so the pass
            // leaves the target in front, not behind. Overrides the gun
            // steering too — flying THROUGH the target ends the fight
            // for both sides; the guns pause during the overshoot (the
            // 45-deg offset fails the solution cone on its own).
            if (fightable && target_->range_nm < cfg_.overshoot_range_nm &&
                target_->rangedot > 300.0) {
                tactic_ = WVRTactic::OverB;
                desired_heading_rad_ = wrap_2pi(
                    desired_heading_rad_ +
                    cfg_.overshoot_offset_rad * jink_side_);
            } else {
                tactic_ = WVRTactic::RandP;
            }
            // The fight plane (see the Merge case): the captured engage
            // altitude in the pursuit branch — chasing the target's LIVE
            // altitude here re-opens the two-brain vertical loop through
            // the Offensive rung (the opponent's Merge/Offensive chases
            // US; a live-reference chase on both sides diverges). The
            // gun-work branch keeps the lead-point tracking: with both
            // jets holding stable fight planes the lead point's z is
            // nearly static and the VS cascade settles onto it — the
            // remaining vertical error is the predictor's drop term,
            // inside the hit-quality cone.
            desired_alt_ft_ = clamp_alt_ft(
                gun_work ? guns_.lead_point(*target_,
                                            current_position_).z
                         : engage_alt_ft_);
            if (fightable && target->ata_from_rad < cfg_.fire_cone_rad &&
                fire_.should_fire(*target)) {
                fire_.note_fired();
                release_pulse_ = true;
            }
            // GUNS — the sustained solution: the trigger goes down when
            // the nose (velocity) sits inside the predictor's cone.
            if (fightable &&
                guns_.should_fire(*target, current_position_,
                                  velocity_estimate_)) {
                guns_.note_burst();
                gun_pulse_ = true;
            }
            break;
        }

        case WVRState::Defensive: {
            tactic_ = WVRTactic::GunJink;
            gun_steering_active_ = false;  // jinking, not aiming
            wants_lock_ = true;   // keep the picture: re-counter needs it
            // The commit band holds here too (the band is symmetric —
            // see gun_pass_target_id's header): a target inside the gun
            // band while WE are defensive is the same committed pass —
            // the jink IS the defensive maneuver, and a cavoid break
            // hijacking it mid-pass forfeits both the jink's geometry
            // and the re-counter. Outside the band the exemption drops
            // and full collision protection returns.
            merge_committed_ =
                fightable &&
                guns_.in_commit_band(*target, current_position_);
            // Break turn: offset off the THREAT bearing, reversing every
            // jink_period_sec. The reversal is the point — a constant
            // turn settles into a predictable rate the shooter can lead;
            // the reversal spoils the solution.
            if (jink_timer_ >= cfg_.jink_period_sec) {
                jink_timer_ = 0.0;
                jink_side_ = -jink_side_;
            }
            desired_heading_rad_ = wrap_2pi(
                target_bearing_rad() + cfg_.jink_offset_rad * jink_side_);
            // Altitude weave: the vertical jink on top of the horizontal.
            const double weave =
                std::sin(2.0 * PI * jink_timer_ / cfg_.jink_period_sec) *
                cfg_.jink_alt_swing_ft;
            desired_alt_ft_ = clamp_alt_ft(engage_alt_ft_ + weave);
            break;
        }

        case WVRState::BugOut:
            tactic_ = WVRTactic::BugOut;
            gun_steering_active_ = false;  // separating, not aiming
            wants_lock_ = false;  // cold: no lock while separating
            if (fightable) {
                desired_heading_rad_ = wrap_2pi(target_bearing_rad() + PI);
            }
            desired_alt_ft_ = clamp_alt_ft(engage_alt_ft_);
            break;
    }

    // Steering through the shared cascade. Defensive runs the rail.
    air_steering_.throttle_min =
        (sm_.current() == WVRState::Defensive ||
         sm_.current() == WVRState::BugOut)
            ? 0.9 : 0.4;
    // The offensive states fight at MIL. AB buys closure the head-on
    // geometry already has (two ~400-kt jets close at ~1,350 fps — the
    // gun envelope opens and closes in ~2 s regardless), and the FCS's
    // G-hold converts the excess thrust into climb: measured on the
    // guns merge, the bandit's speed loop saturated at 1.5 chasing the
    // 450-kt engage command from its spawn CAS and the aircraft
    // ballooned +2,400 fpm AGAINST its own descent command — the whole
    // gun solution (a 1.1-1.7 deg hit-quality cone) pays for every foot
    // of that vertical excursion. Defensive/BugOut keep AB: the break
    // and the separation are energy fights.
    air_steering_.throttle_max =
        (sm_.current() == WVRState::Defensive ||
         sm_.current() == WVRState::BugOut)
            ? 1.5 : 1.0;
    const double speed_kts = (sm_.current() == WVRState::Defensive ||
                              sm_.current() == WVRState::BugOut)
                                 ? cfg_.defensive_speed_kts
                                 : engage_cas_kts_;
    out = air_steering_.steer(desired_heading_rad_, desired_alt_ft_,
                              speed_kts, steering_input(*state));
    out.weapon_release = release_pulse_;
    out.trigger_down = guns_.trigger_down();
    return out;
}

// ============================================================================
// Steering helpers
// ============================================================================

double WVRModule::target_bearing_rad() const {
    if (!target_) return current_heading_rad_;
    // NOTE: the snapshot position (not the track-file prediction). The
    // pursuit/bearing laws were TUNED on this data (the two_ship rejoin
    // calibration keyed on the ghost-chase's lag); the GUN branch's
    // steering predicts independently (GunModule::lead_heading_rad) —
    // in a co-axial merge the stale along-track error does not rotate
    // the bearing, so the pursuit does not need it.
    const double dx = target_->position.x - current_position_.x;  // east
    const double dy = target_->position.y - current_position_.y;  // north
    if (dx == 0.0 && dy == 0.0) return current_heading_rad_;
    return std::atan2(dx, dy);
}

double WVRModule::pursuit_heading_rad() const {
    if (!target_) return current_heading_rad_;
    const double dx = target_->position.x - current_position_.x;  // east
    const double dy = target_->position.y - current_position_.y;  // north
    const double range_ft = target_->range_nm * FEET_PER_NM;
    if (dx == 0.0 && dy == 0.0) return current_heading_rad_;

    const double spd = std::sqrt(
        target_->velocity.x * target_->velocity.x +
        target_->velocity.y * target_->velocity.y +
        target_->velocity.z * target_->velocity.z);

    // Time-to-go estimate: range / closing speed (EWMA rangedot,
    // positive = closing), floored so a stationary picture never divides
    // by ~0. Same estimator as BVRModule's pursuit.
    const double closing = std::max(target_->rangedot, 300.0);
    const double t_go = std::clamp(range_ft / closing, 0.0, 30.0);

    // Lead point clamped to 40% of range (gimbal sanity at the merge is
    // even tighter than BVR: the IR cone is 20 deg half-angle).
    const double lead_ft = std::min(t_go * spd, 0.4 * range_ft);
    const double scale = (spd > 1.0 && lead_ft > 0.0) ? lead_ft / spd : 0.0;

    const double lx = dx + target_->velocity.x * scale;
    const double ly = dy + target_->velocity.y * scale;
    if (lx == 0.0 && ly == 0.0) return current_heading_rad_;
    return std::atan2(lx, ly);
}

AirSteering::Input WVRModule::steering_input(
    const flight::IAircraftState& s) const noexcept {
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

double WVRModule::clamp_alt_ft(double alt_ft) const noexcept {
    return std::clamp(alt_ft, cfg_.min_alt_ft, cfg_.max_alt_ft);
}

// ============================================================================
// Engagement bookkeeping
// ============================================================================

void WVRModule::engage(const TargetInfo& target) {
    engagement_target_id_ = target.entity_id;
    // Capture the altitude the vertical game weaves around.
    engage_alt_ft_ = current_alt_msl_ft_;
    // Capture the speed the merge holds: the CAS it arrived at, capped
    // at the doctrine maximum (the CAP doctrine — see Config::
    // engage_speed_kts; an acceleration command balloons the plant).
    engage_cas_kts_ = std::min(current_vcas_kts_, cfg_.engage_speed_kts);
    // Drop the velocity history: it may be stale from before this fight
    // (the module only runs while the WVR rung is live), and a stale
    // delta over a fresh dt is a garbage boresight. The first tick of
    // the engagement has no estimate -> the guns hold fire; from the
    // second tick the estimate is exact.
    has_prev_position_ = false;
    // Reset the IR shot count for the new engagement; the cooldown
    // survives (the shooter's rail cadence).
    fire_.reset_engagement();
    guns_.reset_engagement();
    dwell_timer_ = 0.0;
    defensive_timer_ = 0.0;
}

void WVRModule::clear_engagement() {
    engagement_target_id_ = 0;
    wants_lock_ = false;
    release_pulse_ = false;
    gun_pulse_ = false;
    gun_steering_active_ = false;
    fire_.reset_engagement();
    guns_.reset_engagement();
}

// ============================================================================
// Names
// ============================================================================

std::string WVRModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

std::string WVRModule::tactic_name() const {
    switch (tactic_) {
        case WVRTactic::None:       return "None";
        case WVRTactic::RandP:      return "RandP";
        case WVRTactic::OverB:      return "OverB";
        case WVRTactic::Roop:       return "Roop";
        case WVRTactic::GunJink:    return "GunJink";
        case WVRTactic::Straight:   return "Straight";
        case WVRTactic::BugOut:     return "BugOut";
        case WVRTactic::Avoid:      return "Avoid";
        case WVRTactic::Beam:       return "Beam";
        case WVRTactic::BeamReturn: return "BeamReturn";
        case WVRTactic::RunAway:    return "RunAway";
    }
    return "Unknown";
}

} // namespace f4::ai::modules

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

    // The IR fire-control cooldown burns every tick, fight or not.
    fire_.tick_cooldown(dt);

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
            desired_alt_ft_ = clamp_alt_ft(target_ ? target_->position.z
                                                   : engage_alt_ft_);
            // Fire control: the merge head-on IS the IR opportunity shot
            // (all-aspect heater) — but only into the forward cone: a
            // heater at a target on our six has nothing to track.
            if (fightable && target->ata_from_rad < cfg_.fire_cone_rad &&
                fire_.should_fire(*target)) {
                fire_.note_fired();
                release_pulse_ = true;
            }
            break;
        }

        case WVRState::Offensive: {
            wants_lock_ = true;
            desired_heading_rad_ = pursuit_heading_rad();
            // Overshoot control (FreeFalcon OverB): inside the overshoot
            // guard with hard closure, offset the pursuit so the pass
            // leaves the target in front, not behind.
            if (fightable && target_->range_nm < cfg_.overshoot_range_nm &&
                target_->rangedot > 300.0) {
                tactic_ = WVRTactic::OverB;
                desired_heading_rad_ = wrap_2pi(
                    desired_heading_rad_ +
                    cfg_.overshoot_offset_rad * jink_side_);
            } else {
                tactic_ = WVRTactic::RandP;
            }
            desired_alt_ft_ = clamp_alt_ft(target_ ? target_->position.z
                                                   : engage_alt_ft_);
            if (fightable && target->ata_from_rad < cfg_.fire_cone_rad &&
                fire_.should_fire(*target)) {
                fire_.note_fired();
                release_pulse_ = true;
            }
            break;
        }

        case WVRState::Defensive: {
            tactic_ = WVRTactic::GunJink;
            wants_lock_ = true;   // keep the picture: re-counter needs it
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
    const double speed_kts = (sm_.current() == WVRState::Defensive ||
                              sm_.current() == WVRState::BugOut)
                                 ? cfg_.defensive_speed_kts
                                 : cfg_.engage_speed_kts;
    out = air_steering_.steer(desired_heading_rad_, desired_alt_ft_,
                              speed_kts, steering_input(*state));
    out.weapon_release = release_pulse_;
    return out;
}

// ============================================================================
// Steering helpers
// ============================================================================

double WVRModule::target_bearing_rad() const {
    if (!target_) return current_heading_rad_;
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
    // Reset the IR shot count for the new engagement; the cooldown
    // survives (the shooter's rail cadence).
    fire_.reset_engagement();
    dwell_timer_ = 0.0;
    defensive_timer_ = 0.0;
}

void WVRModule::clear_engagement() {
    engagement_target_id_ = 0;
    wants_lock_ = false;
    release_pulse_ = false;
    fire_.reset_engagement();
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

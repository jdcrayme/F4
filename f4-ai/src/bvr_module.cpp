// f4-ai/src/bvr_module.cpp
//
// BVRModule implementation — see modules/bvr_module.hpp for design notes.
//
// FreeFalcon reference: bvrengage.cpp (BvrChooseTactic / BvrCrank /
// BvrNotch), dlogic.cpp (BVR range-band decisions, bugout), mengage.cpp
// (fire control — via the embedded MissileModule).

#include "f4/ai/modules/bvr_module.hpp"

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

} // anonymous namespace

// ============================================================================
// Construction + FSM
// ============================================================================

BVRModule::BVRModule()
    : sm_(build_sm())
{
    // Combat tune: the nav cascade with the energy guards relaxed. A
    // BVR fight banks harder (45 deg crank turns) and runs the throttle
    // rail-to-rail; the anti-balloon guard tuned for approaches would
    // chop power mid-support-window the same way it pumped the enroute
    // phugoid (see NavigationModule's NAV-E note).
    air_steering_.max_bank_rad = 0.87;       // ~50 deg
    air_steering_.balloon_guard_fpm = 1000000.0;
    air_steering_.throttle_max = 1.5;        // AB available in a fight
}

fsm::StateMachine<BVRState, BVREvent>
BVRModule::build_sm()
{
    return typename fsm::StateMachine<BVRState, BVREvent>::Builder()
        .initial(BVRState::None)
        .state(BVRState::None,       "None")
        .state(BVRState::Entering,   "Entering")
        .state(BVRState::Employing,  "Employing")
        .state(BVRState::Separating, "Separating")
        .event_name(BVREvent::TargetDetected,     "TargetDetected")
        .event_name(BVREvent::InRange,            "InRange")
        .event_name(BVREvent::WeaponFired,        "WeaponFired")
        .event_name(BVREvent::ThreatDetected,     "ThreatDetected")
        .event_name(BVREvent::BugOut,             "BugOut")
        .event_name(BVREvent::SeparationComplete, "SeparationComplete")
        .event_name(BVREvent::LostTarget,         "LostTarget")
        .on(BVRState::None, BVRState::Entering, BVREvent::TargetDetected,
            nullptr, nullptr, "hostile_inside_entry_ring")
        .on(BVRState::Entering, BVRState::Employing, BVREvent::InRange,
            nullptr, nullptr, "inside_employment_envelope")
        .on(BVRState::Entering, BVRState::None, BVREvent::LostTarget,
            nullptr, nullptr, "target_gone_before_envelope")
        .on(BVRState::Employing, BVRState::Employing, BVREvent::WeaponFired,
            nullptr, nullptr, "shot_away_support_window_starts")
        .on(BVRState::Employing, BVRState::Separating, BVREvent::BugOut,
            nullptr, nullptr, "merge_or_doctrine")
        .on(BVRState::Employing, BVRState::None, BVREvent::LostTarget,
            nullptr, nullptr, "target_gone")
        .on(BVRState::Separating, BVRState::None,
            BVREvent::SeparationComplete,
            nullptr, nullptr, "range_reopened")
        .on(BVRState::Separating, BVRState::None, BVREvent::LostTarget,
            nullptr, nullptr, "target_gone_while_separating")
        .build();
}

// ============================================================================
// Public API
// ============================================================================

void BVRModule::reset() {
    clear_engagement();
    sm_ = build_sm();
    tactic_ = BVRTactic::None;
    re_eval_timer_ = 0.0;
    crank_timer_ = 0.0;
    separation_timer_ = 0.0;
    desired_heading_rad_ = 0.0;
}

BVRRangeBand BVRModule::band_for(double range_nm) const {
    const double entry = entry_range_nm();
    if (range_nm > entry) return BVRRangeBand::OutOfEnvelope;
    if (range_nm > fire_.config().max_pk_range_nm) return BVRRangeBand::BVR;
    if (range_nm > cfg_.wvr_entry_range_nm) return BVRRangeBand::Employ;
    if (range_nm > cfg_.separate_range_nm) return BVRRangeBand::WVR;
    return BVRRangeBand::Merge;
}

AIControlOutput BVRModule::update(double dt,
                                  const flight::IAircraftState* state,
                                  const TargetInfo* target) {
    AIControlOutput out{};

    // Intents are single-tick pulses by contract.
    release_pulse_ = false;

    // The offensive fire-control cooldown burns every tick, fight or not.
    fire_.tick_cooldown(dt);

    if (!state) {
        // No airframe state (should not happen in flight): treat as no
        // fight, keep whatever trace state exists.
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

    // Target validity for THIS tick.
    const bool fightable = target != nullptr && engageable(*target);
    target_ = fightable ? target : nullptr;

    const double range_nm = fightable ? target->range_nm : 1.0e9;
    const BVRRangeBand band = band_for(range_nm);

    // ------------------------------------------------------------------
    // Transition logic (guarded by the target picture + range bands).
    // ------------------------------------------------------------------
    switch (sm_.current()) {
        case BVRState::None:
            if (fightable && band != BVRRangeBand::OutOfEnvelope) {
                engage(*target);
                sm_.process(BVREvent::TargetDetected);
            }
            break;

        case BVRState::Entering:
            if (!fightable) {
                sm_.process(BVREvent::LostTarget);
                clear_engagement();
            } else if (engagement_target_id_ != target->entity_id) {
                // Re-target: reset the engagement bookkeeping, stay in
                // Entering with the new target.
                clear_engagement();
                engage(*target);
            } else if (band == BVRRangeBand::Employ ||
                       band == BVRRangeBand::WVR ||
                       band == BVRRangeBand::Merge) {
                sm_.process(BVREvent::InRange);
            }
            break;

        case BVRState::Employing: {
            if (!fightable) {
                // Target dead or no longer visible — the M3 host policy
                // stops painting corpses, so "lost" usually means "won".
                sm_.process(BVREvent::LostTarget);
                clear_engagement();
                break;
            }
            if (engagement_target_id_ != target->entity_id) {
                // Re-target mid-fight: run the SM through Lost -> Detected
                // -> InRange so the trace records the swap.
                clear_engagement();
                engage(*target);
                sm_.process(BVREvent::LostTarget);
                sm_.process(BVREvent::TargetDetected);
                sm_.process(BVREvent::InRange);
                break;
            }
            // Doctrine transitions out of Employing.
            if (band == BVRRangeBand::Merge ||
                (fire_.shots_fired() >=
                     fire_.config().shoot_shoot_max_shots &&
                 crank_timer_ <= 0.0)) {
                sm_.process(BVREvent::BugOut);
                separation_timer_ = 0.0;
                break;
            }
            break;
        }

        case BVRState::Separating: {
            separation_timer_ += dt;
            const double reopen =
                cfg_.separation_complete_mult * entry_range_nm();
            const bool reopened =
                !fightable ? true : (target->range_nm > reopen);
            // Minimum separation time even against a lost target: the
            // turn needs a few seconds before the brain can hand the
            // route back (avoids a 1-tick Separating blip in traces).
            if (reopened && separation_timer_ > 5.0) {
                sm_.process(BVREvent::SeparationComplete);
                clear_engagement();
            }
            break;
        }
    }

    // ------------------------------------------------------------------
    // Tactic selection + steering + intents per state.
    // ------------------------------------------------------------------
    wants_lock_ = false;

    switch (sm_.current()) {
        case BVRState::None:
            tactic_ = BVRTactic::FollowWaypoints;
            return out;  // empty: the brain flies its mission module

        case BVRState::Entering:
            tactic_ = BVRTactic::Pursuit;
            wants_lock_ = true;  // STT early: the track must be hot
            desired_heading_rad_ = pursuit_heading_rad(*state);
            break;

        case BVRState::Employing: {
            wants_lock_ = true;
            if (crank_timer_ > 0.0) {
                // Support window after a shot: crank off the bearing.
                tactic_ = BVRTactic::Crank;
                desired_heading_rad_ = target_bearing_rad(*state) +
                                       cfg_.crank_offset_rad;
            } else {
                tactic_ = BVRTactic::Pursuit;
                desired_heading_rad_ = pursuit_heading_rad(*state);
                // Fire control: pulse the release intent for exactly one
                // tick when the shot is legal.
                if (fightable && fire_.should_fire(*target)) {
                    fire_.note_fired();
                    release_pulse_ = true;
                    sm_.process(BVREvent::WeaponFired);
                    crank_timer_ = cfg_.crank_hold_sec;
                    re_eval_timer_ = cfg_.re_eval_interval_sec;
                }
            }
            break;
        }

        case BVRState::Separating:
            tactic_ = BVRTactic::BugOut;
            wants_lock_ = false;  // cold: do not hold a lock while bugging
            if (fightable) {
                desired_heading_rad_ = target_bearing_rad(*state) + PI;
            }
            break;
    }

    // Timers.
    if (crank_timer_ > 0.0) crank_timer_ = std::max(0.0, crank_timer_ - dt);
    if (re_eval_timer_ > 0.0) {
        re_eval_timer_ = std::max(0.0, re_eval_timer_ - dt);
    }

    // Steering through the shared cascade. In Separating we keep the AB
    // rail; in Entering/Employing the engage tune applies.
    air_steering_.throttle_min = 0.4;
    out = air_steering_.steer(desired_heading_rad_, engage_alt_ft_,
                              cfg_.engage_speed_kts,
                              steering_input(*state));
    out.weapon_release = release_pulse_;
    return out;
}

// ============================================================================
// Steering helpers
// ============================================================================

double BVRModule::target_bearing_rad(
    const flight::IAircraftState& own) const {
    if (!target_) return current_heading_rad_;
    const double dx = target_->position.x - current_position_.x;  // east
    const double dy = target_->position.y - current_position_.y;  // north
    if (dx == 0.0 && dy == 0.0) return own.heading_rad();
    return std::atan2(dx, dy);
}

double BVRModule::pursuit_heading_rad(
    const flight::IAircraftState& own) const {
    if (!target_) return current_heading_rad_;
    const double dx = target_->position.x - current_position_.x;  // east
    const double dy = target_->position.y - current_position_.y;  // north
    const double range_ft = target_->range_nm * FEET_PER_NM;
    if (dx == 0.0 && dy == 0.0) return own.heading_rad();

    const double spd = std::sqrt(
        target_->velocity.x * target_->velocity.x +
        target_->velocity.y * target_->velocity.y +
        target_->velocity.z * target_->velocity.z);

    // Time-to-go estimate: range / closing speed. Closing speed from the
    // EWMA-smoothed rangedot (positive = closing) with a floor so a
    // stationary target picture never divides by ~0.
    const double closing = std::max(target_->rangedot, 300.0);
    const double t_go = std::clamp(range_ft / closing, 0.0, 60.0);

    // Lead point: where the target will be. Clamp the lead distance to
    // 40% of the range so a fast crossing target never points the nose
    // more than ~23 deg past it (keeps the radar gimbal sane).
    const double lead_ft =
        std::min(t_go * spd, 0.4 * range_ft);
    const double scale = (spd > 1.0 && lead_ft > 0.0) ? lead_ft / spd : 0.0;

    const double lx = dx + target_->velocity.x * scale;
    const double ly = dy + target_->velocity.y * scale;
    if (lx == 0.0 && ly == 0.0) return own.heading_rad();
    return std::atan2(lx, ly);
}

AirSteering::Input BVRModule::steering_input(
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

// ============================================================================
// Engagement bookkeeping
// ============================================================================

void BVRModule::engage(const TargetInfo& target) {
    engagement_target_id_ = target.entity_id;
    // Capture the altitude to hold for the fight on entry (level BVR
    // fights; the vertical game arrives with WVRModule).
    engage_alt_ft_ = current_alt_msl_ft_;
    // Reset the fire-control shot count for the new engagement; the
    // launch cooldown survives (it is the shooter's rail cadence).
    fire_.reset_engagement();
}

void BVRModule::clear_engagement() {
    engagement_target_id_ = 0;
    wants_lock_ = false;
    release_pulse_ = false;
    fire_.reset_engagement();
}

// ============================================================================
// Names
// ============================================================================

std::string BVRModule::state_name() const {
    auto name = sm_.name_of(sm_.current());
    return name.empty() ? std::to_string(static_cast<int>(sm_.current()))
                        : std::string(name);
}

std::string BVRModule::tactic_name() const {
    switch (tactic_) {
        case BVRTactic::None:           return "None";
        case BVRTactic::FollowWaypoints:return "FollowWaypoints";
        case BVRTactic::Pursuit:        return "Pursuit";
        case BVRTactic::Crank:          return "Crank";
        case BVRTactic::Notch:          return "Notch";
        case BVRTactic::BugOut:         return "BugOut";
    }
    return "Unknown";
}

} // namespace f4::ai::modules

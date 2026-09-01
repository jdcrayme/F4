// f4-ai/src/gun_module.cpp
//
// GunModule implementation — see modules/gun_module.hpp for design notes.
//
// FreeFalcon reference: the guns predictor inside GunsEngageMode (the
// lead-point solution the digi steers to before the trigger moves) and
// the burst cadence of guneval — short bursts, never a hose.

#include "f4/ai/modules/gun_module.hpp"

#include <algorithm>
#include <cmath>

namespace f4::ai::modules {

namespace {

constexpr double FEET_PER_NM = 6076.11548;

/// Any one detection source = visible (SensorFusion::can_see's rule —
/// inlined so the module layer stays on TargetInfo snapshots).
[[nodiscard]] inline bool can_see(const TargetInfo& t) noexcept {
    return t.detected_by_radar || t.detected_by_rwr ||
           t.detected_by_visual || t.detected_by_gci;
}

[[nodiscard]] inline bool engageable(const TargetInfo& t) noexcept {
    return t.is_hostile && !t.is_missile && can_see(t);
}

} // anonymous namespace

// ============================================================================
// Role 1 — the predictor
// ============================================================================

geo::WorldPosition GunModule::predicted_position(
    const TargetInfo& t) noexcept {
    return geo::WorldPosition{
        t.position.x + t.velocity.x * t.age_s,
        t.position.y + t.velocity.y * t.age_s,
        t.position.z + t.velocity.z * t.age_s};
}

double GunModule::range_now_ft(const TargetInfo& t,
                               const geo::WorldPosition& ownship_pos)
    noexcept {
    const auto now = predicted_position(t);
    const double dx = now.x - ownship_pos.x;
    const double dy = now.y - ownship_pos.y;
    const double dz = now.z - ownship_pos.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double GunModule::bullet_tof_s(const TargetInfo& t,
                               const geo::WorldPosition& ownship_pos)
    const noexcept {
    const double range_ft = range_now_ft(t, ownship_pos);
    if (range_ft <= 0.0) return 0.0;
    // Bullet ground speed = muzzle + closure (a closing fight shortens
    // the flight; the target is running INTO the rounds). Floor the
    // closure at zero so an opening target never yields a negative (and
    // thus absurdly short) time of flight.
    const double closure = std::max(t.rangedot, 0.0);
    const double speed = cfg_.muzzle_velocity_fps + closure;
    if (speed <= 1.0) return 2.5;
    return std::clamp(range_ft / speed, 0.0, 2.5);
}

geo::WorldPosition GunModule::lead_point(
    const TargetInfo& t, const geo::WorldPosition& ownship_pos)
    const noexcept {
    const double tof = bullet_tof_s(t, ownship_pos);
    const auto now = predicted_position(t);
    // SUPERELEVATION: bullets drop under gravity over the flight time
    // (0.5*g*t^2 — 16 ft at 1 s, 39 ft at 1.5 s), so the aim point sits
    // ABOVE the kinematic lead by exactly the drop. Every real gun fire
    // computer does this; without it the rounds land short-low by the
    // drop distance.
    constexpr double GRAVITY_FPS2 = 32.174;
    const double drop_ft = 0.5 * GRAVITY_FPS2 * tof * tof;
    return geo::WorldPosition{
        now.x + t.velocity.x * tof,
        now.y + t.velocity.y * tof,
        now.z + t.velocity.z * tof + drop_ft};
}

double GunModule::lead_heading_rad(
    const TargetInfo& t, const geo::WorldPosition& ownship_pos)
    const noexcept {
    const auto lead = lead_point(t, ownship_pos);
    const double dx = lead.x - ownship_pos.x;  // east
    const double dy = lead.y - ownship_pos.y;  // north
    if (dx == 0.0 && dy == 0.0) return 0.0;
    return std::atan2(dx, dy);
}

bool GunModule::in_envelope(const TargetInfo& t,
                            const geo::WorldPosition& ownship_pos)
    const noexcept {
    const double range_nm = range_now_ft(t, ownship_pos) / FEET_PER_NM;
    return range_nm >= cfg_.min_range_nm && range_nm <= cfg_.max_range_nm;
}

double GunModule::solution_error_rad(
    const TargetInfo& t, const geo::WorldPosition& ownship_pos,
    const math::Vec3<double>& ownship_velocity) const noexcept {
    const auto lead = lead_point(t, ownship_pos);
    math::Vec3<double> to_lead{lead.x - ownship_pos.x,
                               lead.y - ownship_pos.y,
                               lead.z - ownship_pos.z};
    const double dist = to_lead.length();
    const double spd = ownship_velocity.length();
    // Degenerate geometry (lead point on top of us, or a stationary
    // ownship with no boresight to measure against): report MAXIMUM
    // error — a solution you cannot measure is not a solution you can
    // fire on. (The stationary case is real: a gun aims along the
    // velocity; no velocity, no aim.)
    if (dist <= 1.0 || spd <= 1.0) return 3.14159265358979323846;
    const double cos_err = std::clamp(
        to_lead.dot(ownship_velocity) / (dist * spd), -1.0, 1.0);
    return std::acos(cos_err);
}

double GunModule::solution_tolerance_rad(
    const TargetInfo& t, const geo::WorldPosition& ownship_pos)
    const noexcept {
    const double range_ft = range_now_ft(t, ownship_pos);
    if (range_ft <= 1.0) return cfg_.max_solution_rad;
    return std::min(
        std::atan2(cfg_.hit_radius_ft, range_ft),
        cfg_.max_solution_rad);
}

bool GunModule::should_fire(const TargetInfo& t,
                            const geo::WorldPosition& ownship_pos,
                            const math::Vec3<double>& ownship_velocity)
    const noexcept {
    if (cfg_.hold_fire) return false;
    if (!engageable(t)) return false;
    if (!in_envelope(t, ownship_pos)) return false;
    if (state_ != TriggerState::Idle) return false;
    if (rounds_ <= 0) return false;
    return solution_error_rad(t, ownship_pos, ownship_velocity) <=
           solution_tolerance_rad(t, ownship_pos);
}

// ============================================================================
// Role 2 — the trigger state machine
// ============================================================================

void GunModule::note_burst() {
    if (state_ != TriggerState::Idle) return;   // cannot double-fire
    if (rounds_ <= 0 || cfg_.burst_rounds <= 0) return;

    const int burst = std::min(cfg_.burst_rounds, rounds_);
    rounds_ -= burst;
    ++bursts_;
    pulse_ = true;

    // Bursting for the trigger's physical duration (rounds at the cyclic
    // rate), then the cooldown before the next pull.
    timer_ = std::max(
        static_cast<double>(burst) / std::max(cfg_.rounds_per_minute, 1.0)
            * 60.0,
        1.0 / 60.0);
    state_ = TriggerState::Bursting;
}

void GunModule::tick(double dt) noexcept {
    // Single-tick pulse contract.
    pulse_ = false;

    if (state_ == TriggerState::Idle) return;
    timer_ -= dt;
    if (timer_ > 0.0) return;

    if (state_ == TriggerState::Bursting) {
        state_ = TriggerState::Cooldown;
        timer_ = cfg_.burst_cooldown_sec;
        if (timer_ <= 0.0) {
            state_ = TriggerState::Idle;   // no cooldown configured
            timer_ = 0.0;
        }
    } else {  // Cooldown
        state_ = TriggerState::Idle;
        timer_ = 0.0;
    }
}

void GunModule::reset_engagement() noexcept {
    pulse_ = false;
    if (state_ == TriggerState::Bursting) {
        // Trigger released mid-burst (fight over / re-target): the
        // rounds already in the stream keep flying on their own, but
        // the gun still owes its cycle — drop straight to cooldown.
        state_ = TriggerState::Cooldown;
        timer_ = cfg_.burst_cooldown_sec;
        if (timer_ <= 0.0) {
            state_ = TriggerState::Idle;
            timer_ = 0.0;
        }
    }
    // Cooldown: untouched (the cycle is the shooter's, not the target's).
    // Idle: untouched. bursts_/rounds_ survive: ammo and the doctrine
    // counter are the shooter's too.
}

} // namespace f4::ai::modules

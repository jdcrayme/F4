// f4-weapons/include/f4/weapons/missile.hpp
//
// Missile — 3-DOF point-mass guided-munition flyout.
//
// This is the pure (ECS-free) missile model. The ECS binding lives in
// missile_battery.hpp (MissileComponent + MissileSimComponent); tests use
// this class directly for determinism.
//
// Motion model (per tick, in sim-local ENU feet):
//   1. Guidance (while locked): true proportional navigation in vector form,
//          a_cmd = N' * Vc * (omega x v_hat)
//      where omega = (r x v_rel)/|r|^2 is the LOS-rate vector, Vc the
//      closing speed (positive when closing), v_hat the missile velocity
//      unit vector. a_cmd is intrinsically perpendicular to v_hat; it is
//      clamped to max_g and steers the velocity vector:
//          vel += a_cmd * dt
//   2. Thrust (while burning):  vel += v_hat * (thrust / mass) * dt
//      mass depletes linearly from launch_mass to burnout_mass over
//      burn_time, then stays at burnout_mass.
//   3. Drag:    vel -= v_hat * (0.5 * rho(alt) * V^2 * cd * A / mass) * dt
//      with an exponential atmosphere rho = 0.0023769 * exp(-alt/24000)
//      slugs/ft^3 (standard sea-level density, ~24k ft scale height).
//      Deliberately a local 10-line helper: f4-flight-model's atmosphere is
//      not linked from here (see COMBAT_CHAIN_PLAN.md §2 M1 design notes).
//   4. Gravity: vel.z -= 32.174 * dt
//   5. Speed cap at max_speed_fts (models the missile's structural/aero
//      envelope better than drag alone at low altitude).
//   6. pos += vel * dt
//
// Seeker: while guided, the target must stay inside the seeker cone
// (angle between LOS and the missile's velocity axis <= seeker_half_angle)
// and within seeker_max_range. Losing either switches the missile to
// Ballistic (it keeps flying straight under thrust/drag/gravity).
//
// Fuze: detonates when the range to the (valid) target drops to
// fuze_radius, or on closest approach if the range starts growing inside
// 8 * lethal_radius (proximity fuze), or self-destructs at tof_limit.
// Detonation is terminal: status becomes Detonated/Expired and tick() turns
// into a no-op.
//
// Deterministic: no RNG anywhere in the flyout.

#pragma once

#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>
#include <f4/weapons/weapon_types.hpp>

namespace f4::weapons {

// ============================================================================
// MissileConfig — flyout parameters. Built from a WeaponClassRecord (all
// the flyout-relevant fields have the same names); tests may also build one
// by hand for tight control over the math.
// ============================================================================
struct MissileConfig {
    double launch_mass_lb  = 0.0;
    double burnout_mass_lb = 0.0;
    double thrust_lbf      = 0.0;
    double burn_time_s     = 0.0;

    double ref_area_ft2    = 0.0;
    double cd              = 0.0;
    double max_speed_fts   = 0.0;
    double max_g           = 0.0;
    double guidance_gain   = 4.0;

    double seeker_half_angle_rad = 0.0;  // radians (record stores degrees!)
    double seeker_max_range_ft   = 0.0;
    double fuze_radius_ft        = 0.0;
    double lethal_radius_ft      = 0.0;
    double tof_limit_s           = 0.0;

    /// Build from a weapon class record (converts deg -> rad for the seeker).
    [[nodiscard]] static MissileConfig from_record(const WeaponClassRecord& rec);
};

// ============================================================================
// TargetSnapshot — what the seeker sees this tick. The ECS layer fills this
// from the target's TransformComponent; the pure class takes it by value so
// guidance is testable without a world.
// ============================================================================
struct TargetSnapshot {
    bool   valid = false;                 // false = no track this tick
    f4::geo::WorldPosition position{};    // ENU feet
    f4::math::Vec3<double> velocity{};    // ENU ft/s
};

// ============================================================================
// MissileStatus
//   Guided   — seeker has the target, PN is steering
//   Ballistic— seeker lost the target (cone/range); straight-line coast
//   Detonated— fuze fired against the target (terminal)
//   Expired  — self-destructed at time-of-flight limit (terminal)
// ============================================================================
enum class MissileStatus : std::uint8_t {
    Guided    = 0,
    Ballistic = 1,
    Detonated = 2,
    Expired   = 3,
};

[[nodiscard]] inline const char* missile_status_name(MissileStatus s) noexcept {
    switch (s) {
        case MissileStatus::Guided:    return "guided";
        case MissileStatus::Ballistic: return "ballistic";
        case MissileStatus::Detonated: return "detonated";
        case MissileStatus::Expired:   return "expired";
    }
    return "unknown";
}

// ============================================================================
// Missile
// ============================================================================
class Missile {
public:
    /// Configure + launch. pos/vel are the SHOOTER's at release (typically
    /// nose position + shooter velocity); the caller may offset forward.
    void launch(const MissileConfig& config,
                const f4::geo::WorldPosition& pos,
                const f4::math::Vec3<double>& vel);

    /// Advance one tick. Terminal states are no-ops.
    /// `target` may be invalid (no track): a Guided missile goes Ballistic
    /// on the first invalid tick (it cannot home on nothing).
    void tick(double dt, const TargetSnapshot& target);

    // --- State accessors ---
    [[nodiscard]] MissileStatus status() const noexcept { return status_; }
    [[nodiscard]] bool terminal() const noexcept {
        return status_ == MissileStatus::Detonated || status_ == MissileStatus::Expired;
    }
    [[nodiscard]] const f4::geo::WorldPosition& position() const noexcept { return pos_; }
    [[nodiscard]] const f4::math::Vec3<double>& velocity() const noexcept { return vel_; }
    [[nodiscard]] double mass_lb() const noexcept { return mass_; }
    [[nodiscard]] double flight_time_s() const noexcept { return t_; }
    [[nodiscard]] bool motor_burning() const noexcept { return t_ < cfg_.burn_time_s; }

    /// Range to the target at the last tick (before any detonation).
    [[nodiscard]] double last_target_range_ft() const noexcept { return last_range_; }
    /// Closest range ever achieved to a target (the miss distance when the
    /// fuze fires on closest approach).
    [[nodiscard]] double min_range_ft() const noexcept { return min_range_; }
    /// True once the seeker has lost lock (status Ballistic or later).
    [[nodiscard]] bool seeker_lost() const noexcept {
        return status_ != MissileStatus::Guided;
    }

    /// Range the target would take damage at (lethal_radius from config).
    [[nodiscard]] double lethal_radius_ft() const noexcept { return cfg_.lethal_radius_ft; }
    /// Fuze trigger radius (the detonation distance against a valid target).
    [[nodiscard]] double fuze_radius_ft() const noexcept { return cfg_.fuze_radius_ft; }

private:
    /// Seeker visibility test (cone + range) against the current velocity.
    [[nodiscard]] bool seeker_sees(const TargetSnapshot& target) const;

    /// True when the missile is not closing on the target (range growing or
    /// static) — used by the closest-approach fuze logic.
    [[nodiscard]] bool closing_velocity_nonpositive(const TargetSnapshot& target) const;

    MissileConfig cfg_{};
    f4::geo::WorldPosition pos_{};
    f4::math::Vec3<double> vel_{};
    double mass_  = 0.0;
    double t_     = 0.0;
    MissileStatus status_ = MissileStatus::Guided;

    double last_range_   = -1.0;  // <0 = no valid target seen yet
    double min_range_    = -1.0;
    int    grow_ticks_   = 0;     // consecutive ticks the range has grown
};

/// Exponential-atmosphere density (slugs/ft^3) at altitude `alt_ft` (MSL).
/// Documented local duplicate of the flight model's atmosphere — see the
/// file comment. Kept public for tests.
[[nodiscard]] double atmosphere_density(double alt_ft);

/// Gravitational acceleration used across the sim (ft/s^2).
inline constexpr double kGravityFps2 = 32.174;

} // namespace f4::weapons

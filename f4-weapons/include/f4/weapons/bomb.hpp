// f4-weapons/include/f4/weapons/bomb.hpp
//
// Bomb — 3-DOF point-mass ballistic munition flyout.
//
// This is the pure (ECS-free) gravity-weapon model, the mirror of Missile
// for the air-to-ground employment slice (COMBAT_CHAIN_PLAN.md M5). The ECS
// binding lives in bomb_battery.hpp (BombComponent + BombSimComponent);
// tests use this class directly for determinism.
//
// Motion model (per tick, in sim-local ENU feet):
//   1. Drag:    vel -= v_hat * (0.5 * rho(alt) * V^2 * cd * A / mass) * dt
//      (same exponential atmosphere as missile.hpp — see atmosphere_density)
//   2. Gravity: vel.z -= 32.174 * dt
//   3. pos += vel * dt
// No thrust, no guidance, no seeker: a released bomb steers nothing. The
// release SOLUTION (where to drop so the impact lands on the aim point)
// belongs to the shooter — see bomb_release_range_ft().
//
// Terminal states:
//   Impact  — pos.z dropped to the release-time impact plane (the ground
//             elevation at the aim point, captured at release). Level-plane
//             approximation: exact for flat QC worlds, conservative within
//             a few feet for sloped terrain over a bomb's fall time.
//   Expired — time-of-flight limit (safety; a bomb that somehow never
//             reaches the plane, e.g. released above the plane climbing).
//
// Deterministic: no RNG anywhere in the flyout.

#pragma once

#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>
#include <f4/weapons/weapon_types.hpp>

namespace f4::weapons {

// ============================================================================
// BombConfig — flyout parameters, built from a WeaponClassRecord.
// ============================================================================
struct BombConfig {
    double mass_lb     = 0.0;
    double ref_area_ft2 = 0.0;
    double cd          = 0.0;
    double lethal_radius_ft = 0.0;
    double tof_limit_s = 0.0;

    /// Build from a weapon class record (only the ballistic fields — a
    /// bomb's seeker/fuze/guidance fields are ignored by design).
    [[nodiscard]] static BombConfig from_record(const WeaponClassRecord& rec);
};

// ============================================================================
// BombStatus
//   Released — falling, not yet at the impact plane
//   Impact   — reached the impact plane (terminal)
//   Expired  — time-of-flight limit reached while still above the plane
// ============================================================================
enum class BombStatus : std::uint8_t {
    Released = 0,
    Impact   = 1,
    Expired  = 2,
};

[[nodiscard]] inline const char* bomb_status_name(BombStatus s) noexcept {
    switch (s) {
        case BombStatus::Released: return "released";
        case BombStatus::Impact:   return "impact";
        case BombStatus::Expired:  return "expired";
    }
    return "unknown";
}

// ============================================================================
// Bomb
// ============================================================================
class Bomb {
public:
    /// Release. pos/vel are the SHOOTER's at release (typically the
    /// aircraft's center, with the aircraft's velocity — the bomb
    /// inherits it exactly, matching a free-fall release from a rail).
    /// `impact_plane_z` is the ground elevation at the AIM POINT (MSL ft),
    /// captured once at release; the terminal check is pos.z <= plane.
    void release(const BombConfig& config,
                 const f4::geo::WorldPosition& pos,
                 const f4::math::Vec3<double>& vel,
                 double impact_plane_z);

    /// Advance one tick. Terminal states are no-ops.
    void tick(double dt);

    // --- State accessors ---
    [[nodiscard]] BombStatus status() const noexcept { return status_; }
    [[nodiscard]] bool terminal() const noexcept {
        return status_ == BombStatus::Impact || status_ == BombStatus::Expired;
    }
    [[nodiscard]] const f4::geo::WorldPosition& position() const noexcept { return pos_; }
    [[nodiscard]] const f4::math::Vec3<double>& velocity() const noexcept { return vel_; }
    [[nodiscard]] double mass_lb() const noexcept { return cfg_.mass_lb; }
    [[nodiscard]] double flight_time_s() const noexcept { return t_; }
    [[nodiscard]] double impact_plane_z() const noexcept { return impact_plane_z_; }

    /// Horizontal distance traveled at terminal (ft) — the impact point's
    /// ground range from the release point.
    [[nodiscard]] double ground_range_ft() const noexcept {
        const double dx = pos_.x - release_.x;
        const double dy = pos_.y - release_.y;
        return std::sqrt(dx * dx + dy * dy);
    }

private:
    BombConfig cfg_{};
    f4::geo::WorldPosition release_{};
    f4::geo::WorldPosition pos_{};
    f4::math::Vec3<double> vel_{};
    double impact_plane_z_ = 0.0;
    double t_ = 0.0;
    BombStatus status_ = BombStatus::Released;
};

// ============================================================================
// Release solution helpers (pure functions, shared by the AI trigger and
// tests). Vacuum ballistics + a drag correction:
//
//   fall_time = sqrt(2 * dz / g)                    (dz = release alt - plane)
//   vacuum_range = v * fall_time
//   range = vacuum_range * drag_factor              (drag_factor <= 1)
//
// The drag_factor models the mean deceleration over the fall for a given
// class of bomb at a given speed; the shooter's fire control (the host,
// which owns the weapon table) computes it once and configures the AI's
// strike envelope with it. The BOMB itself still flies the full drag ODE —
// these formulas only decide WHERE to pull the release.
// ============================================================================

/// Time for a level release at `alt_ft` to fall `dz_ft` to the impact
/// plane, in seconds (vacuum).
[[nodiscard]] double bomb_fall_time_s(double dz_ft) noexcept;

/// Horizontal distance (ft) a bomb travels in vacuum when released at
/// `speed_ftps` with `dz_ft` of altitude to give up.
[[nodiscard]] double bomb_vacuum_range_ft(double dz_ft, double speed_ftps) noexcept;

} // namespace f4::weapons

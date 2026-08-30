// f4-weapons/include/f4/weapons/damage.hpp
//
// The damage model: what happens when a warhead detonates near a target.
//
// Model (documented approximation of FreeFalcon's FalconEntity::ApplyDamage):
//
//   FreeFalcon compares the weapon's `Power` against the entity's
//   `Strength` with a distance falloff. We keep that shape:
//
//     falloff(range)  = (1 - range/lethal_radius) clamped to [0, 1]
//     damage          = warhead_power * falloff * spread
//     spread          = 0.75 + 0.5 * roll      (roll in [0,1]; 0.5 = nominal)
//     killed          = hit_points_after <= 0
//
//   - At the fuze point (range ~ 0) damage == warhead_power * spread.
//   - At the lethal radius damage is exactly zero (LINEAR in between — a
//     detonation at the fuze radius still delivers most of the warhead's
//     effect; marginal launches damage, clean launches kill).
//   - The spread factor models hit variability; pass a seeded roll from the
//     caller (f4-weapons never rolls its own dice inside the math — the
//     RNG lives at the call site, keeping the pure function deterministic).
//
// `strength` is expressed in the same lb-scale as warhead_power: the
// target's hit points. FreeFalcon's VCD hit_points map onto this directly
// (f4-world populates DamageStateComponent from them later).

#pragma once

#include <cstdint>

namespace f4::weapons {

/// Result of one damage application against one target.
struct DamageOutcome {
    double damage_applied   = 0.0;  // lb-scale damage actually deducted
    double hit_points_after = 0.0;  // clamped at >= 0
    bool   killed           = false;
};

/// Blast-effectiveness factor at `range_ft` from the burst point for a
/// weapon with `lethal_radius_ft`. Pure, deterministic, monotone:
///   range <= 0          -> 1.0
///   range >= lethal     -> 0.0
[[nodiscard]] double blast_falloff(double range_ft, double lethal_radius_ft);

/// Apply one warhead to one target.
///
///   current_hp        target's hit points BEFORE this hit (> 0)
///   max_hp            target's nominal hit points (damage cap bookkeeping)
///   warhead_power_lb  the weapon's power (WeaponClassRecord::warhead_power_lb)
///   burst_range_ft    distance from burst point to target center
///   lethal_radius_ft  the weapon's damage radius
///   roll01            variance draw in [0,1]; 0.5 = nominal (deterministic)
///
/// Pure: no RNG, no globals, no side effects.
[[nodiscard]] DamageOutcome apply_damage(double current_hp,
                                         double max_hp,
                                         double warhead_power_lb,
                                         double burst_range_ft,
                                         double lethal_radius_ft,
                                         double roll01 = 0.5);

} // namespace f4::weapons

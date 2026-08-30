// f4-weapons/src/damage.cpp — see damage.hpp for the model documentation.

#include <f4/weapons/damage.hpp>

#include <algorithm>
#include <cmath>

namespace f4::weapons {

double blast_falloff(double range_ft, double lethal_radius_ft) {
    if (lethal_radius_ft <= 0.0) {
        // Degenerate weapon: no damage radius means no damage anywhere.
        return 0.0;
    }
    if (range_ft <= 0.0) {
        return 1.0;
    }
    if (range_ft >= lethal_radius_ft) {
        return 0.0;
    }
    const double fraction = 1.0 - (range_ft / lethal_radius_ft);
    return fraction;
}

DamageOutcome apply_damage(double current_hp,
                           double max_hp,
                           double warhead_power_lb,
                           double burst_range_ft,
                           double lethal_radius_ft,
                           double roll01) {
    DamageOutcome out{};

    // Guard degenerate inputs: a target already at/under zero HP cannot take
    // further damage (and cannot be "killed again").
    if (current_hp <= 0.0) {
        out.hit_points_after = std::max(0.0, current_hp);
        out.killed = current_hp <= 0.0;
        return out;
    }

    const double roll = std::clamp(roll01, 0.0, 1.0);
    const double spread = 0.75 + 0.5 * roll;  // 0.75 .. 1.25, nominal at 0.5

    const double falloff = blast_falloff(burst_range_ft, lethal_radius_ft);
    const double damage = std::max(0.0, warhead_power_lb) * falloff * spread;

    out.damage_applied = damage;
    out.hit_points_after = std::max(0.0, current_hp - damage);
    // A hit that fully exhausts HP kills. Guard the "power==0 at edge"
    // case: exactly-zero damage does not kill a live target.
    out.killed = out.hit_points_after <= 0.0 && damage > 0.0;

    // max_hp is bookkeeping (repair/refit math); it never amplifies damage.
    (void)max_hp;
    return out;
}

} // namespace f4::weapons

// f4-weapons/src/weapon_class_table.cpp — registry + built-in placeholder set.
//
// Built-in values are order-of-magnitude placeholders from public data
// (NOT the game's FALCON4.WST values — see the header). They exist so the
// flyout, store, and tests have realistic shapes to run against.

#include <f4/weapons/weapon_class_table.hpp>

namespace f4::weapons {

WeaponClassTable WeaponClassTable::with_builtins() {
    WeaponClassTable t;

    // --- 0: M61A1 Vulcan (gun) --------------------------------------------
    // 20 mm, 6,000 rpm, muzzle ~3,400 ft/s. HEI round ~0.22 lb; power modeled
    // per-round. Lethal radius is the burst-proximity scale, not gun range.
    {
        WeaponClassRecord g;
        g.name = "M61A1";
        g.category = WeaponCategory::Gun;
        g.guidance = GuidanceKind::None;
        g.max_range_ft = 6000.0;          // effective AA range (~1 NM)
        g.min_range_ft = 0.0;
        g.max_speed_fts = 3400.0;         // muzzle velocity
        g.max_g = 0.0;                    // bullets do not maneuver
        g.warhead_power_lb = 0.22;
        g.lethal_radius_ft = 40.0;
        t.add(g);
    }

    // --- 1: AIM-9M Sidewinder (IR) ----------------------------------------
    {
        WeaponClassRecord m;
        m.name = "AIM-9M";
        m.category = WeaponCategory::AirToAirMissile;
        m.guidance = GuidanceKind::Ir;
        m.max_range_ft = 10.0 * 6076.11548;   // ~10 NM engagement boundary
        m.min_range_ft = 1000.0;              // arming distance
        m.max_speed_fts = 2500.0;             // ~Mach 2.2-2.5
        m.max_g = 30.0;
        m.launch_mass_lb = 190.0;
        m.burnout_mass_lb = 145.0;
        m.thrust_lbf = 4200.0;
        m.burn_time_s = 5.0;
        m.ref_area_ft2 = 0.196;               // 5 in diameter
        m.cd = 0.30;
        m.guidance_gain = 4.0;
        m.seeker_half_angle_deg = 20.0;       // tight IR gimbal
        m.seeker_max_range_ft = 12.0 * 6076.11548;
        m.fuze_radius_ft = 25.0;              // active optical fuze
        m.lethal_radius_ft = 60.0;
        m.tof_limit_s = 60.0;
        m.warhead_power_lb = 24.0;            // 9.4 kg WDU-17/B annular blast-frag
        t.add(m);
    }

    // --- 2: AIM-7M Sparrow (semi-active radar) -----------------------------
    {
        WeaponClassRecord m;
        m.name = "AIM-7M";
        m.category = WeaponCategory::AirToAirMissile;
        m.guidance = GuidanceKind::SemiActiveRadar;
        m.max_range_ft = 30.0 * 6076.11548;
        m.min_range_ft = 2000.0;
        m.max_speed_fts = 3400.0;             // ~Mach 3-4
        m.max_g = 30.0;
        m.launch_mass_lb = 506.0;
        m.burnout_mass_lb = 330.0;
        m.thrust_lbf = 10000.0;
        m.burn_time_s = 6.5;
        m.ref_area_ft2 = 0.380;               // 8 in diameter
        m.cd = 0.30;
        m.guidance_gain = 4.0;
        m.seeker_half_angle_deg = 40.0;
        m.seeker_max_range_ft = 35.0 * 6076.11548;
        m.fuze_radius_ft = 40.0;
        m.lethal_radius_ft = 80.0;
        m.tof_limit_s = 90.0;
        m.warhead_power_lb = 88.0;            // 88 lb continuous-rod
        t.add(m);
    }

    // --- 3: AIM-120C AMRAAM (active radar) ---------------------------------
    {
        WeaponClassRecord m;
        m.name = "AIM-120C";
        m.category = WeaponCategory::AirToAirMissile;
        m.guidance = GuidanceKind::ActiveRadar;
        m.max_range_ft = 40.0 * 6076.11548;   // boundary vs cooperating target
        m.min_range_ft = 3000.0;
        m.max_speed_fts = 4400.0;             // ~Mach 4
        m.max_g = 40.0;
        m.launch_mass_lb = 335.0;
        m.burnout_mass_lb = 210.0;
        m.thrust_lbf = 8000.0;
        m.burn_time_s = 8.0;
        m.ref_area_ft2 = 0.267;               // 7 in diameter
        m.cd = 0.28;
        m.guidance_gain = 4.0;
        m.seeker_half_angle_deg = 60.0;
        m.seeker_max_range_ft = 15.0 * 6076.11548;  // onboard seeker acquisition
        m.fuze_radius_ft = 45.0;
        m.lethal_radius_ft = 120.0;
        m.tof_limit_s = 120.0;
        m.warhead_power_lb = 48.0;            // 44 lb WDU-41/B
        t.add(m);
    }

    // --- 4: Mk-82 (bomb, unguided — store bookkeeping only in M1) ----------
    {
        WeaponClassRecord b;
        b.name = "MK-82";
        b.category = WeaponCategory::Bomb;
        b.guidance = GuidanceKind::None;
        b.max_range_ft = 0.0;                 // release ballistics: M2+
        b.min_range_ft = 0.0;
        b.max_speed_fts = 0.0;
        b.max_g = 0.0;
        b.launch_mass_lb = 500.0;
        b.burnout_mass_lb = 500.0;
        b.warhead_power_lb = 192.0;           // 192 lb Tritonal
        b.lethal_radius_ft = 300.0;
        t.add(b);
    }

    return t;
}

std::uint32_t WeaponClassTable::add(WeaponClassRecord record) {
    record.id = static_cast<std::uint32_t>(records_.size());
    records_.push_back(std::move(record));
    return record.id;
}

const WeaponClassRecord* WeaponClassTable::get(std::uint32_t handle) const noexcept {
    if (handle == kInvalidWeapon || handle >= records_.size()) {
        return nullptr;
    }
    return &records_[handle];
}

std::uint32_t WeaponClassTable::find_by_name(const std::string& name) const noexcept {
    for (std::size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].name == name) {
            return records_[i].id;
        }
    }
    return kInvalidWeapon;
}

std::uint32_t WeaponClassTable::find_by_category(WeaponCategory category) const noexcept {
    for (std::size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].category == category) {
            return records_[i].id;
        }
    }
    return kInvalidWeapon;
}

} // namespace f4::weapons

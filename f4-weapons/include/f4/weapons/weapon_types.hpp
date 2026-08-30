// f4-weapons/include/f4/weapons/weapon_types.hpp
//
// Weapon domain types shared by the whole f4-weapons library.
//
// Scope: air-to-air weapons for M1 (see Docs/COMBAT_CHAIN_PLAN.md). The
// category/guidance enums are domain-neutral from day one so air-to-ground
// and SAM flyouts can be added without breaking call sites.
//
// Units: Imperial throughout the sim (feet, ft/s, lb mass, lbf force,
// seconds) — matches f4-flight-model and the MessageBus convention.
//
// FreeFalcon reference: WeaponClassData (falclib/class_tbl.cpp, loaded from
// FALCON4.WST). The struct below consolidates the fields the flyout and
// damage models actually need; the real WST parser (f4-convert) will fill
// this record from game data. Field names deliberately avoid the cryptic
// original abbreviations until those subsystems are implemented.

#pragma once

#include <cstdint>
#include <string>

namespace f4::weapons {

// ============================================================================
// WeaponCategory — what the weapon is FOR (store bookkeeping + AI employ
// logic). FreeFalcon's full WeaponFamily enum has dozens of values that
// differ mainly in UI presentation; we keep the employ-relevant subset.
// ============================================================================
enum class WeaponCategory : std::uint8_t {
    Gun               = 0,
    AirToAirMissile   = 1,
    AirToGroundMissile = 2,
    Bomb              = 3,
    Countermeasure    = 4,
    Other             = 5,
};

// ============================================================================
// GuidanceKind — how the weapon finds its target after launch. Drives the
// flyout's seeker behavior (see missile.hpp):
//   None        — ballistic after release (bombs, rockets)
//   Ir          — seeker locks on heat; limited gimbal, no datalink
//   SemiActiveRadar — needs illuminator; simulated as ActiveRadar in M1
//                 (documented approximation — the illuminator logic belongs
//                 to the sensor model, M2)
//   ActiveRadar — onboard radar seeker (AIM-120); loses lock if target
//                 leaves the seeker cone/range, then goes ballistic
// ============================================================================
enum class GuidanceKind : std::uint8_t {
    None            = 0,
    Ir              = 1,
    SemiActiveRadar = 2,
    ActiveRadar     = 3,
};

// ============================================================================
// WeaponClassRecord — the per-weapon-type data card.
//
// A note on "power" and "strength": FreeFalcon's damage model compares a
// weapon's `Power` against an entity's `Strength` (both unitless game
// weights, applied in FalconEntity::ApplyDamage). We keep the same shape —
// `warhead_power_lb` vs the target's hit points — so the eventual WST/VCD
// import maps 1:1. The interpretation used by damage.hpp is documented
// there; the field NAMES carry no unit claim beyond the lb convention.
// ============================================================================
struct WeaponClassRecord {
    std::uint32_t id = 0;              // stable handle == index into WeaponClassTable
    std::string   name;                // "AIM-120C", "M61A1", ...

    WeaponCategory category      = WeaponCategory::Other;
    GuidanceKind   guidance      = GuidanceKind::None;

    // --- Employment envelope (AI fire-control reads these) ---
    double max_range_ft   = 0.0;       // aerodynamic/Rmax launch boundary
    double min_range_ft   = 0.0;       // minimum employment (arming/footprint)
    double max_speed_fts  = 0.0;       // terminal speed cap
    double max_g          = 0.0;       // lateral maneuver limit

    // --- Flyout (point-mass) parameters ---
    double launch_mass_lb   = 0.0;     // mass at launch
    double burnout_mass_lb  = 0.0;     // mass after motor burnout
    double thrust_lbf       = 0.0;     // motor thrust (constant while burning)
    double burn_time_s      = 0.0;     // motor burn duration
    double ref_area_ft2     = 0.0;     // drag reference area
    double cd               = 0.0;     // drag coefficient (supersonic-ish mean)
    double guidance_gain    = 4.0;     // PN navigation constant N'

    // --- Seeker / fuze ---
    double seeker_half_angle_deg = 0.0; // gimbal/off-boresight limit (half-cone)
    double seeker_max_range_ft   = 0.0; // max acquisition/tracking range
    double fuze_radius_ft        = 0.0; // detonation trigger distance
    double lethal_radius_ft      = 0.0; // damage effectiveness radius
    double tof_limit_s           = 0.0; // self-destruct time of flight

    // --- Damage ---
    double warhead_power_lb = 0.0;     // vs target strength (see damage.hpp)
};

} // namespace f4::weapons

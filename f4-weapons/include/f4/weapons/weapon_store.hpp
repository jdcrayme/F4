// f4-weapons/include/f4/weapons/weapon_store.hpp
//
// WeaponStoreComponent — what a loaded entity is carrying.
//
// Passive ECS component (priority 0): pure inventory bookkeeping. The
// firing logic (launch_missile, future GunComponent/BVRModule) queries the
// store, then calls expend() after a successful launch — never before.
//
// Stations model hardpoints. Each station holds ONE weapon class with a
// round count (1 per missile, hundreds for a gun). A station whose rounds
// hit 0 is empty but keeps its slot (rearm restores it).
//
// Loadout helpers build the common shapes (pure A/A fighter loadout here;
// the campaign bridge will build per-mission loadouts from Flight
// loadout data later).

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/weapons/weapon_class_table.hpp>

#include <string>
#include <vector>

namespace f4::weapons {

struct WeaponStation {
    std::uint32_t weapon_handle = kInvalidWeapon;
    std::string   label;          // "station 3", "gun", "left wing" ...
    int           rounds = 0;
    int           initial_rounds = 0;
};

class WeaponStoreComponent : public entities::Component<WeaponStoreComponent> {
public:
    // --- Loadout construction -------------------------------------------------
    /// Add a station carrying `rounds` of `weapon_handle`. Returns its index.
    std::size_t add_station(std::uint32_t weapon_handle, int rounds,
                            std::string label = "");

    /// Convenience: 1x AIM-9M (wingtip L/R) + 4x AIM-120 + 511x M61A1 gun,
    /// using the handles found by name in `table`. Stations missing a weapon
    /// class are skipped (so custom tables still work).
    static WeaponStoreComponent standard_fighter(const WeaponClassTable& table);

    // --- Queries ---------------------------------------------------------------
    [[nodiscard]] std::size_t station_count() const noexcept { return stations_.size(); }
    [[nodiscard]] const WeaponStation* station(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t selected_index() const noexcept { return selected_; }

    /// Total remaining rounds of one weapon class across all stations.
    [[nodiscard]] int count_for(std::uint32_t weapon_handle) const noexcept;

    /// First station index carrying a weapon of `category` with rounds > 0,
    /// or npos.
    [[nodiscard]] std::size_t find_with_category(const WeaponClassTable& table,
                                                 WeaponCategory category) const noexcept;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // --- Selection ---------------------------------------------------------------
    /// Select by index (clamped to valid range). Returns the effective index.
    std::size_t select(std::size_t index);
    /// Cycle to the next station with rounds > 0 (wraps). Returns the new
    /// index, or npos if NOTHING is left anywhere.
    std::size_t select_next_loaded();

    // --- Expenditure ---------------------------------------------------------------
    /// Remove `n` rounds from station `index`. Returns rounds actually
    /// removed (never goes negative). Empty stations keep their slot.
    int expend(std::size_t index, int n = 1);

    /// True if station `index` exists, has a valid weapon handle, and has
    /// rounds remaining — i.e. `fire from here` would succeed.
    [[nodiscard]] bool can_fire(std::size_t index) const noexcept;

private:
    std::vector<WeaponStation> stations_;
    std::size_t selected_ = 0;
};

} // namespace f4::weapons

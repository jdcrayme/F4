// f4-weapons/include/f4/weapons/weapon_class_table.hpp
//
// WeaponClassTable — the registry of weapon types (data cards).
//
// M1 ships a documented BUILT-IN placeholder set (AIM-9M, AIM-7M, AIM-120C,
// M61A1, Mk-82). The numbers are order-of-magnitude correct from public
// data; they are NOT the game's WST values. When f4-convert grows the
// FALCON4.WST loader (see COMBAT_CHAIN_PLAN.md §5), load_wst() will replace
// the built-ins without touching call sites: the table is keyed by stable
// uint32 handles == indices, and every consumer reads fields, never
// constants.
//
// Zero third-party dependencies. Deterministic: the built-in set is
// registered in a fixed order (id 0..N-1).

#pragma once

#include <f4/weapons/weapon_types.hpp>

#include <optional>
#include <string>
#include <vector>

namespace f4::weapons {

/// Sentinel handle: "no weapon". Valid handles are 0..size()-1.
inline constexpr std::uint32_t kInvalidWeapon = 0xFFFFFFFFu;

class WeaponClassTable {
public:
    /// Empty table (no built-ins — for tests that register their own).
    WeaponClassTable() = default;

    /// The built-in placeholder set (see file comment). Order:
    /// 0=M61A1 gun, 1=AIM-9M, 2=AIM-7M, 3=AIM-120C, 4=Mk-82.
    [[nodiscard]] static WeaponClassTable with_builtins();

    /// Register a custom class; returns its stable handle.
    std::uint32_t add(WeaponClassRecord record);

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    /// Lookup by handle. Returns nullptr for kInvalidWeapon / out of range.
    [[nodiscard]] const WeaponClassRecord* get(std::uint32_t handle) const noexcept;

    /// First handle whose name matches exactly ("" never matches).
    [[nodiscard]] std::uint32_t find_by_name(const std::string& name) const noexcept;

    /// First handle matching a category (kInvalidWeapon if none).
    [[nodiscard]] std::uint32_t find_by_category(WeaponCategory category) const noexcept;

    [[nodiscard]] const std::vector<WeaponClassRecord>& records() const noexcept {
        return records_;
    }

private:
    std::vector<WeaponClassRecord> records_;
};

} // namespace f4::weapons

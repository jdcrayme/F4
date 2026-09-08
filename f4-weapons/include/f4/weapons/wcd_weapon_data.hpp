// f4-weapons/include/f4/weapons/wcd_weapon_data.hpp
//
// WcdWeaponData — the runtime-side reader for the Falcon4.WCD JSON export
// (wcd2json's "f4-weapon-class-table" schema) plus the overlay that folds
// the real employment/damage envelope into the engine's WeaponClassTable.
//
// Real-data tier (COMBAT_CHAIN_PLAN.md §5 / Task 64): the vanilla install's
// campaign weapon table is Falcon4.WCD — the table ClassTableEntry
// .data_ptr_index points at for data_type == DTYPE_WEAPON. The binary
// decoder lives importer-side (f4-world-convert); this header is the
// runtime consumer of its JSON form, mirroring how f4-world-types consumes
// ct2json's output. f4-weapons gains f4-io + f4-json as PRIVATE deps for
// this — the same dependency set f4-world-types uses.
//
// Deliberately RAW records: the FreeFalcon enum vocabularies (damage_type,
// the WEAP_ capability bits, guidance_flags) are carried, NOT interpreted.
// Mapping them into WeaponCategory/GuidanceKind is a later tranche gated on
// establishing the camplib.h bit definitions against a real export —
// fabricating the vocabulary here would be a fidelity lie. The overlay
// below therefore only touches the fields whose units are documented and
// unambiguous (range km, weight lb, blast radius ft, strength).
//
// Units: WCD carries range in km, weight in lb, blast radius in ft — the
// sim's imperial convention, converted at the overlay boundary.

#pragma once

#include <f4/weapons/weapon_class_table.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace f4::weapons {

// ============================================================================
// WcdWeaponRecord — one Falcon4.WCD entry, verbatim (wcd2json schema).
// Field semantics per the byte-exact decode in f4-world-convert's
// theater_data (WeaponClassData, 60-byte records).
// ============================================================================
struct WcdWeaponRecord {
    int index = 0;              // the WCD entry index (campaign wire id domain)
    int strength = 0;           // damage amount (unitless game weight)
    int damage_type = 0;        // DamType enum — RAW (vocabulary deferred)
    int range_km = 0;           // employment range, km
    int flags = 0;              // WEAP_ capability bits — RAW
    std::string name;           // e.g. "AIM-120 AMRAAM" (<= 20 chars in WCD)
    std::array<std::uint8_t, 8> hit_chance{};  // per movement-type, %
    int fire_rate = 0;          // shots per barrage (ground employment)
    int rarity = 0;             // % of full supply provided
    int guidance_flags = 0;     // guidance bitmask — RAW (vocabulary deferred)
    int collective = 0;
    int simweap_index = 0;      // index into the (FreeFalcon) SimWeaponDataTable
    int weight = 0;             // lb
    int drag_index = 0;
    int blast_radius = 0;       // ft
    int radar_type = 0;         // index into Falcon4.RCD
    int sim_data_idx = 0;       // index into Falcon4.SWD
    int max_alt = 0;            // max altitude, 1000s of ft
};

// The parsed export. find_by_name is case-insensitive on the trimmed name
// (WCD names are fixed-width arrays — padded/trimmed at the decode).
struct WcdWeaponData {
    std::string source;              // provenance (usually "FALCON4.WCD")
    std::string source_fingerprint;  // fnv1a-64 of the source binary
    std::vector<WcdWeaponRecord> entries;

    [[nodiscard]] const WcdWeaponRecord* find_by_name(
        std::string_view name) const noexcept;
};

struct WcdLoadResult {
    WcdWeaponData data;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    bool ok = false;
};

/// Parse a wcd2json output file.
[[nodiscard]] WcdLoadResult load_wcd_weapon_json(const std::filesystem::path& path);

/// Parse from memory (tests).
[[nodiscard]] WcdLoadResult load_wcd_weapon_json_string(std::string_view json);

// ============================================================================
// overlay_wcd_weapon_data — fold the real envelope into the engine table.
//
// For each (engine_name, wcd_name) alias that resolves in BOTH tables, the
// base record's REAL-data fields are overwritten:
//   max_range_ft     <- range_km * 3280.83989501312  (km -> ft)
//   launch_mass_lb   <- weight                        (lb)
//   warhead_power_lb <- strength                      (game weight)
//   lethal_radius_ft <- blast_radius                  (ft)
// Everything else — category, guidance, min_range, and the whole
// flyout/seeker/fuze card — stays exactly as the built-in card authored it:
// the flyout invariants hold by construction and no call site changes.
// (WCD has no minimum-range vocabulary; the AI envelope keeps the card's.)
//
// Aliases whose engine name is not in `base`, or whose WCD name does not
// resolve in `wcd`, append a warning and leave the base record untouched —
// a stale/default alias map must degrade to the placeholder, never throw.
//
// Returns the number of base records updated.
// ============================================================================
[[nodiscard]] std::size_t overlay_wcd_weapon_data(
    WeaponClassTable& base,
    const WcdWeaponData& wcd,
    const std::vector<std::pair<std::string, std::string>>& engine_to_wcd_name,
    std::vector<std::string>* warnings = nullptr);

} // namespace f4::weapons

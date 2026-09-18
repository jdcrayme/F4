// f4-world/include/f4/world/theater_tables.hpp
//
// CAMP-SCALE-1 — the runtime reader for the converted theater tables
// (CAMP_HOST_PLAN.md §8, the Tier-3 full-data pass).
//
// The importer (f4-world-convert, `cam2json --emit-tables`) emits the
// COMPLETE Falcon4.UCD/.VCD/.WCD tables as one JSON document
// ("f4.theater.tables/1" — see theater_data.hpp). This header is the
// runtime-side counterpart: it loads that document into plain structs the
// sim can consume, WITHOUT linking the importer (the F4_SIDE boundary —
// the contract between the two sides is the JSON, exactly like the world
// JSON itself).
//
// What consumes it (and why the tranche exists):
//
//   * VCD countermeasure counts — the SENSORS_COUNTERMEASURES_PLAN
//     documented constants (chaff 30 / flare 15) stand in until the
//     vehicle's own hardpoint supply converts. `resolve_countermeasures`
//     walks entity_type → class-table DTYPE_VEHICLE row → VCD hardpoints
//     → WCD rows and sums the shots on the weapons the WCD names
//     "chaff"/"flare" (case-insensitive — the WCD has no weapon-type
//     enum; the name IS the identity, the same vocabulary the reference's
//     weapon table ships). No chaff/flare hardpoints (or no tables at
//     all) → the resolver declines and the dispenser keeps its defaults:
//     absent data never re-prices a pinned fight (the golden-identity
//     rule).
//
//   * The full UCD rows also complete the threat-model/rating picture the
//     world JSON carries only per-unit (the sample-fixture limitation the
//     C3 notes document); the per-unit enrichment stays the wire face,
//     the tables are the whole-book reference.
//
// The loader is STRICT-lite: structural errors and unknown format
// versions throw; unknown per-row keys are skipped (the tables are a
// conversion target, not a wire protocol — additive exporter fields must
// not break older runtimes). Field names match the emitter exactly.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <f4/world_types/class_table.hpp>

namespace f4::world {

/// One UnitClass (Falcon4.UCD) row — every field the emitter writes.
struct TheaterUnitRow {
    int16_t index = 0;
    std::string name;
    uint32_t flags = 0;
    int32_t movement_type = 0;
    int16_t movement_speed = 0;
    int16_t max_range = 0;
    int32_t fuel = 0;
    int16_t rate = 0;
    int16_t pt_data_index = 0;
    std::vector<int32_t> num_elements;   // 16
    std::vector<int16_t> vehicle_type;   // 16 — 0-based VCD entries[] index
    std::vector<uint8_t> scores;         // 16 — per-mission-role
    uint8_t role = 0;
    std::vector<uint8_t> hit_chance;     // 8
    std::vector<uint8_t> strength;       // 8
    std::vector<uint8_t> range;          // 8
    std::vector<uint8_t> detection;      // 8
    std::vector<uint8_t> damage_mod;     // 11
    uint8_t radar_vehicle = 0;
    int16_t special_index = 0;
    int16_t icon_index = 0;
};

/// One VehicleClass (Falcon4.VCD) row.
struct TheaterVehicleRow {
    int16_t index = 0;
    std::string name;
    std::string nctr;
    int16_t hit_points = 0;
    uint32_t flags = 0;
    float rcs_factor = 0.0f;
    int32_t max_wt = 0;
    int32_t empty_wt = 0;
    int32_t fuel_wt = 0;
    int16_t fuel_econ = 0;
    int16_t engine_sound = 0;
    int16_t high_alt = 0;
    int16_t low_alt = 0;
    int16_t cruise_alt = 0;
    int16_t max_speed = 0;
    int16_t radar_type = 0;
    int16_t number_of_pilots = 0;
    uint16_t rack_flags = 0;
    uint16_t visible_flags = 0;
    uint8_t callsign_index = 0;
    uint8_t callsign_slots = 0;
    std::vector<uint8_t> hit_chance;     // 8
    std::vector<uint8_t> strength;       // 8
    std::vector<uint8_t> range;          // 8
    std::vector<uint8_t> detection;      // 8
    std::vector<int16_t> weapon;         // 16 — WCD weapon ID per hardpoint
    std::vector<uint8_t> weapons;        // 16 — shots per hardpoint
    std::vector<uint8_t> damage_mod;     // 11
};

/// One WeaponClass (Falcon4.WCD) row.
struct TheaterWeaponRow {
    int16_t index = 0;
    std::string name;
    uint16_t strength = 0;
    int32_t damage_type = 0;
    int16_t range_km = 0;
    uint16_t flags = 0;
    uint8_t fire_rate = 0;
    uint8_t rarity = 0;
    uint16_t guidance_flags = 0;
    uint8_t collective = 0;
    int16_t simweap_index = 0;
    uint16_t weight = 0;
    int16_t drag_index = 0;
    uint16_t blast_radius = 0;
    int16_t radar_type = 0;
    int16_t sim_data_idx = 0;
    int8_t max_alt = 0;
};

/// The converted theater tables (f4.theater.tables/1).
struct TheaterTables {
    std::vector<TheaterUnitRow> units;
    std::vector<TheaterVehicleRow> vehicles;
    std::vector<TheaterWeaponRow> weapons;

    [[nodiscard]] bool loaded() const noexcept {
        return !units.empty() || !vehicles.empty() || !weapons.empty();
    }

    /// Parse a tables JSON document (the emitter's exact vocabulary).
    /// Throws std::runtime_error on I/O or structural faults.
    [[nodiscard]] static TheaterTables load(const std::filesystem::path& path);

    /// Parse from a string (tests). Same contract as load().
    [[nodiscard]] static TheaterTables parse(const std::string& json);

    [[nodiscard]] const TheaterUnitRow* unit_at(std::size_t i) const noexcept {
        return i < units.size() ? &units[i] : nullptr;
    }
    [[nodiscard]] const TheaterVehicleRow* vehicle_at(std::size_t i) const noexcept {
        return i < vehicles.size() ? &vehicles[i] : nullptr;
    }
    [[nodiscard]] const TheaterWeaponRow* weapon_at(std::size_t i) const noexcept {
        return i < weapons.size() ? &weapons[i] : nullptr;
    }
};

/// The per-vehicle countermeasure supply the VCD carries (the SENSORS_
/// COUNTERMEASURES_PLAN's "the VCD's per-unit counts" — what replaces the
/// documented 30/15 defaults when the tables resolve a vehicle).
struct CountermeasureCounts {
    int chaff_rounds = 0;
    int flare_rounds = 0;
};

/// Resolve a vehicle's chaff/flare supply from the converted tables.
///
/// `vehicle_entity_type` is the VEHICLE's class-table entity type (the
/// value the world loader puts in VehicleCompositionComponent::groups —
/// e.g. F-16C = 273). The chain: class-table row (DTYPE_VEHICLE) →
/// VCD row → hardpoint weapon IDs → WCD rows named "chaff"/"flare"
/// (case-insensitive) → the summed shot counts.
///
/// Returns std::nullopt — leave the defaults standing — when the tables
/// are absent, the entity type does not resolve to a VCD row, or the row
/// carries no chaff/flare hardpoints. Never throws.
[[nodiscard]] std::optional<CountermeasureCounts>
resolve_countermeasures(const TheaterTables& tables,
                        const f4::world_types::ClassTable& ct,
                        std::uint16_t vehicle_entity_type) noexcept;

} // namespace f4::world

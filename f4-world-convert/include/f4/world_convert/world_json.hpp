// f4-world-convert/include/f4/convert/world_json.hpp
//
// Emits the combined world JSON from a parsed .cam archive. Mirrors the
// f4-convert dat2json pattern: the converter owns the binary-to-JSON step
// so downstream libraries (f4-world) never see binary formats.

#pragma once

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/theater_data.hpp>
#include <string>

namespace f4::world_convert {

/// Optional metadata for the world JSON emitter.
struct WorldJsonOptions {
    /// Theater identifier (e.g. "korea"). Emitted as the top-level "theater"
    /// field so consumers can look up the corresponding terrain file.
    std::string theater = "korea";
    /// Filename of the terrain JSON to reference (NOT embed). The world JSON
    /// records the path so f4-world can load the terrain side-by-side. The
    /// path is interpreted relative to the world JSON's location.
    std::string terrain_file = "korea.terrain.json";
    /// Optional class table (Falcon4.ct). When loaded, each objective's
    /// entity_type is resolved to an ObjectiveType enum value (1-39) and
    /// emitted as the "objective_type" field. Without it, objectives only
    /// carry the raw entity_type and the viewer can't pick icons.
    const ClassTable* class_table = nullptr;
    /// Optional theater object database (Falcon4.OCD/PHD/PD/UCD/VCD/FED/FCD).
    /// When loaded, enriches the world JSON with:
    ///   - Objective class names ("Airbase A-3", "Bridge B-12", ...) — pulled
    ///     from Falcon4.OCD.Name using the class-table-resolved objective
    ///     type as the index.
    ///   - Airbase ground layout — runway/taxiway/parking point lists pulled
    ///     from Falcon4.PHD/PD using ObjClassData.pt_data_index. Lets the
    ///     viewer draw runways and parking spots at real positions.
    ///   - Unit class names ("Armor", "Infantry", ...) — pulled from
    ///     Falcon4.UCD.Name using the class-table-resolved unit subclass
    ///     index. Without this, units only carry their (domain, subtype)
    ///     pair (e.g. "Armor") but not their full class name (e.g. "M1A2
    ///     Abrams Tank Platoon").
    ///   - Vehicle class composition (via Falcon4.VCD) — gives per-vehicle
    ///     hit points, fuel, weapons, RCS, etc. Combined with UnitClassData's
    ///     num_elements[] and vehicle_type[], this fully resolves a unit's
    ///     vehicle roster.
    const TheaterObjectDatabase* theater_db = nullptr;
};

/// Build the world JSON document from a loaded .cam archive.
/// Includes: the container manifest (all sub-files), the decoded .ver
/// version, the decoded .cmp campaign header (CurrentTime, TE block, all
/// 8 team name/motto slots), and raw sub-file bytes (base64) for types
/// not yet decoded.
[[nodiscard]] std::string to_world_json(const CamArchive& cam,
                                         const WorldJsonOptions& opts = {});

} // namespace f4::world_convert

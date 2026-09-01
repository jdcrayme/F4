// f4-import/include/f4/import/gltf_emitter.hpp
//
// KoreaObj → glTF 2.0 converter. Stage 3 of the asset pipeline
// (ASSET_PIPELINE_SPEC.md §3.2, §6).
//
// Takes the parsed geometry from f4-models (ModelDatabase::extract_model_geometry)
// and emits a .gltf JSON + .bin binary file pair that the runtime f4-gltf
// loader can read back.
//
// The emitter handles:
//   - Mesh tessellation: BSP geometry → flat triangle lists (the
//     extraction is done by f4-models; the emitter just writes the
//     flat arrays).
//   - Node tagging: DOF/switch/slot nodes are named per the §6 grammar
//     (dof:unknown.N for unmapped indices — the spec says "Untagged DOFs
//     are not lost").
//   - LOD chains: each LOD level becomes a sibling mesh node tagged
//     lod:N.
//   - Coordinate conversion: Falcon model space is feet, +Z up; glTF
//     is meters, +Y up. The transform is baked at export.
//
// Dependencies: f4-models (for the parsed geometry), f4-json (for the
// JSON writer), f4-assets (for AssetId). This is the importer side —
// the runtime never links this module.

#pragma once

#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace f4::import {

/// Options for the glTF emitter.
struct GltfEmitOptions {
    /// When true, convert Falcon model space (feet, +Z up) to glTF
    /// (meters, +Y up) at export. The spec says: "Units and axes convert
    /// at export. glTF is meters, +Y up; Falcon model space is feet,
    /// +Z up. The exporter bakes the transform so the runtime never
    /// needs Falcon-specific coordinate conventions." Default true.
    bool convert_to_gltf_coords = true;

    /// When true, emit one mesh per LOD level as sibling nodes tagged
    /// lod:0, lod:1, .... When false, emit only the highest-detail LOD.
    bool emit_all_lods = true;

    /// When true, tag DOF/switch/slot nodes with their original KoreaObj
    /// indices using the §6 grammar (dof:unknown.N, sw:unknown.N,
    /// slot:unknown.N). When a rosetta map is available (future work),
    /// the unknown.N tags are replaced with semantic names.
    bool tag_dof_switch_slot = true;
};

/// Result of a glTF emission.
struct GltfEmitResult {
    std::filesystem::path gltf_path;   // the .gltf file
    std::filesystem::path bin_path;    // the .bin file (external buffer)
    std::size_t total_vertices = 0;    // across all LODs
    std::size_t total_triangles = 0;  // across all LODs
    std::size_t lod_count = 0;        // number of LOD levels emitted
    std::size_t dof_count = 0;        // number of DOF nodes tagged
    std::size_t switch_count = 0;     // number of switch nodes tagged
    std::size_t slot_count = 0;       // number of slot nodes tagged
};

/// Emit a single KoreaObj model as a .gltf + .bin file pair.
///
/// @param db         Loaded ModelDatabase (must have called parse_model
///                   for the requested parent_index).
/// @param parent_index  Which model in the database to export.
/// @param out_dir    Directory to write the files into.
/// @param asset_id_string  The asset ID string (e.g. "koreaobj:00002")
///                   used for the filename (00002.gltf + 00002.bin).
/// @param opts       Emission options.
/// @return           Result with paths and counts.
GltfEmitResult emit_model_as_gltf(
    const f4::models::ModelDatabase& db,
    int parent_index,
    const std::filesystem::path& out_dir,
    const std::string& asset_id_string,
    const GltfEmitOptions& opts = {});

} // namespace f4::import

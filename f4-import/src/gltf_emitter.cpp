// f4-import/src/gltf_emitter.cpp
//
// KoreaObj → glTF 2.0 emitter. Takes the flat triangle lists from
// f4-models' geometry extractor and writes a .gltf JSON + .bin binary
// pair that the runtime f4-gltf loader can read back.
//
// The emitter is deliberately minimal: one mesh per LOD, one material
// per texture (no texture extraction yet — that's a follow-up), and
// DOF/switch/slot nodes tagged with the §6 grammar using dof:unknown.N
// for unmapped indices (the spec's "Untagged DOFs are not lost" rule).

#include <f4/import/gltf_emitter.hpp>
#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>
#include <f4/models/bsp_node.hpp>
#include <f4/models/geometry.hpp>
#include <f4/json/writer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace f4::import {

namespace {

// ── Coordinate conversion ──────────────────────────────────────────────────
//
// Falcon model space: feet, +Z up, +X forward, +Y right (left-handed
// from glTF's perspective). glTF: meters, +Y up, -Z forward, +X right
// (right-handed).
//
// The transform baked at export:
//   glTF_x =  falcon_y  * FEET_TO_METERS
//   glTF_y =  falcon_z  * FEET_TO_METERS
//   glTF_z = -falcon_x  * FEET_TO_METERS
//
// This is the standard "Z-up to Y-up" rotation combined with a unit
// conversion. The runtime never needs to know about Falcon's coordinate
// conventions.

constexpr float kFeetToMeters = 0.3048f;

struct Vec3f {
    float x, y, z;
};

Vec3f to_gltf(float fx, float fy, float fz) {
    return {
        fy * kFeetToMeters,   // glTF x = falcon y
        fz * kFeetToMeters,   // glTF y = falcon z
        -fx * kFeetToMeters   // glTF z = -falcon x (handedness flip)
    };
}

// ── Binary buffer builder ─────────────────────────────────────────────────
//
// The .bin file contains all vertex data (positions, normals) and index
// data, concatenated. Buffer views describe the slices; accessors describe
// the typed views into the buffer views.

struct BufferBuilder {
    std::vector<uint8_t> data;

    template <typename T>
    std::size_t push(const std::vector<T>& items) {
        // Align to the size of T (glTF requires 4-byte alignment for
        // accessor byteOffset; we align all buffer views to 4 bytes).
        while (data.size() % alignof(T) != 0) data.push_back(0);
        std::size_t offset = data.size();
        const auto* base = reinterpret_cast<const uint8_t*>(items.data());
        data.insert(data.end(), base, base + items.size() * sizeof(T));
        return offset;
    }

    std::size_t size() const noexcept { return data.size(); }
};

// Write a float as little-endian bytes.
void push_float(std::vector<uint8_t>& buf, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
}

// Write a uint32 as little-endian bytes.
void push_u32(std::vector<uint8_t>& buf, uint32_t u) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
}

// ── Node name generators (§6 grammar) ─────────────────────────────────────

std::string dof_node_name(int dof_index) {
    return "dof:unknown." + std::to_string(dof_index);
}

std::string switch_node_name(int sw_index) {
    return "sw:unknown." + std::to_string(sw_index);
}

std::string slot_node_name(int slot_index) {
    return "slot:unknown." + std::to_string(slot_index);
}

std::string lod_node_name(int level) {
    return "lod:" + std::to_string(level);
}

} // namespace

// ── Main emitter ───────────────────────────────────────────────────────────

GltfEmitResult emit_model_as_gltf(
    const f4::models::ModelDatabase& db,
    int parent_index,
    const std::filesystem::path& out_dir,
    const std::string& asset_id_string,
    const GltfEmitOptions& opts) {

    const f4::models::ModelRecord* rec = db.model(parent_index);
    if (!rec) {
        throw std::runtime_error("gltf_emitter: model index " +
            std::to_string(parent_index) + " not in database");
    }

    // Determine how many LODs to emit.
    int n_lods = opts.emit_all_lods ? rec->n_lods : 1;
    if (n_lods < 1) n_lods = 1;

    // ── Extract geometry for each LOD ──────────────────────────────────
    struct LodGeometry {
        f4::models::ModelGeometry geom;
        int level;
    };
    std::vector<LodGeometry> lods;
    lods.reserve(static_cast<std::size_t>(n_lods));
    for (int lod = 0; lod < n_lods; ++lod) {
        f4::models::ModelState state;
        state.lod_level = lod;
        auto geom = db.extract_model_geometry(parent_index, lod, state);
        if (geom.total_vertices() == 0) continue;
        lods.push_back({std::move(geom), lod});
    }

    if (lods.empty()) {
        throw std::runtime_error("gltf_emitter: model " +
            std::to_string(parent_index) +
            " has no extractable geometry (all LODs empty)");
    }

    // ── Build the binary buffer ─────────────────────────────────────────
    //
    // Layout: for each LOD, positions (VEC3 FLOAT) + normals (VEC3 FLOAT)
    // + indices (SCALAR UINT). Buffer views point at the slices.
    BufferBuilder builder;

    struct AccessorInfo {
        std::size_t buffer_view;
        std::size_t byte_offset;   // within buffer view
        int component_type;        // 5126 = FLOAT, 5125 = UINT
        std::size_t count;
        std::string type;          // "VEC3" or "SCALAR"
    };

    struct BufferViewInfo {
        std::size_t byte_offset;
        std::size_t byte_length;
        int target;                // 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER
    };

    std::vector<BufferViewInfo> buffer_views;
    std::vector<AccessorInfo> accessors;

    struct LodMeshInfo {
        std::size_t positions_accessor;
        std::size_t normals_accessor;
        std::size_t indices_accessor;
        std::size_t vertex_count;
        std::size_t triangle_count;
        int level;
    };

    std::vector<LodMeshInfo> lod_meshes;
    lod_meshes.reserve(lods.size());

    for (const auto& lod : lods) {
        // Merge all meshes in this LOD into one (the emitter doesn't
        // do per-texture materials yet — that's a follow-up).
        f4::models::Mesh merged = lod.geom.merged();
        if (merged.vertices.empty() || merged.triangles.empty()) continue;

        // Positions
        std::vector<float> positions;
        positions.reserve(merged.vertices.size() * 3);
        for (const auto& v : merged.vertices) {
            Vec3f p = opts.convert_to_gltf_coords
                ? to_gltf(v.position.x, v.position.y, v.position.z)
                : Vec3f{v.position.x, v.position.y, v.position.z};
            positions.push_back(p.x);
            positions.push_back(p.y);
            positions.push_back(p.z);
        }
        std::size_t pos_offset = builder.push<float>({});
        // Actually push the positions.
        builder.data.resize(pos_offset);
        for (float f : positions) push_float(builder.data, f);

        std::size_t pos_bv = buffer_views.size();
        buffer_views.push_back({pos_offset, positions.size() * 4, 34962});
        std::size_t pos_acc = accessors.size();
        accessors.push_back({pos_bv, 0, 5126, merged.vertices.size(), "VEC3"});

        // Normals
        std::vector<float> normals;
        normals.reserve(merged.vertices.size() * 3);
        for (const auto& v : merged.vertices) {
            Vec3f n = opts.convert_to_gltf_coords
                ? to_gltf(v.normal.x, v.normal.y, v.normal.z)
                : Vec3f{v.normal.x, v.normal.y, v.normal.z};
            normals.push_back(n.x);
            normals.push_back(n.y);
            normals.push_back(n.z);
        }
        std::size_t norm_offset = builder.push<float>({});
        builder.data.resize(norm_offset);
        for (float f : normals) push_float(builder.data, f);

        std::size_t norm_bv = buffer_views.size();
        buffer_views.push_back({norm_offset, normals.size() * 4, 34962});
        std::size_t norm_acc = accessors.size();
        accessors.push_back({norm_bv, 0, 5126, merged.vertices.size(), "VEC3"});

        // Indices (uint32)
        std::vector<uint32_t> indices;
        indices.reserve(merged.triangles.size() * 3);
        for (const auto& t : merged.triangles) {
            indices.push_back(t.v0);
            indices.push_back(t.v1);
            indices.push_back(t.v2);
        }
        std::size_t idx_offset = builder.push<uint32_t>({});
        builder.data.resize(idx_offset);
        for (uint32_t i : indices) push_u32(builder.data, i);

        std::size_t idx_bv = buffer_views.size();
        buffer_views.push_back({idx_offset, indices.size() * 4, 34963});
        std::size_t idx_acc = accessors.size();
        accessors.push_back({idx_bv, 0, 5125, indices.size(), "SCALAR"});

        lod_meshes.push_back({pos_acc, norm_acc, idx_acc,
                              merged.vertices.size(), merged.triangles.size(),
                              lod.level});
    }

    if (lod_meshes.empty()) {
        throw std::runtime_error("gltf_emitter: no meshes with geometry");
    }

    // ── Write the .bin file ────────────────────────────────────────────
    std::string filename_base = asset_id_string;
    // If the asset_id contains a colon (e.g. "koreaobj:00002"), use the
    // local-id part for the filename.
    auto colon_pos = filename_base.find(':');
    if (colon_pos != std::string::npos) {
        filename_base = filename_base.substr(colon_pos + 1);
    }

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    std::string bin_filename = filename_base + ".bin";
    std::filesystem::path bin_path = out_dir / bin_filename;
    {
        std::ofstream f(bin_path, std::ios::binary);
        if (!f) throw std::runtime_error("gltf_emitter: cannot write " + bin_path.string());
        f.write(reinterpret_cast<const char*>(builder.data.data()),
                static_cast<std::streamsize>(builder.data.size()));
    }

    // ── Build the .gltf JSON ───────────────────────────────────────────
    f4::json::Writer w;

    w.raw("{\n");
    w.raw("  \"asset\": { \"version\": \"2.0\", \"generator\": \"f4import models 0.4.0\" },\n");
    w.raw("  \"scene\": 0,\n");

    // Scene
    w.raw("  \"scenes\": [\n");
    w.raw("    { \"nodes\": [");
    // Root node is index 0. DOF/switch/slot nodes follow.
    w.number(0);
    w.raw("] }\n");
    w.raw("  ],\n");

    // ── Nodes ───────────────────────────────────────────────────────
    // Node layout:
    //   0: root (the model root, carries the LOD children)
    //   1..N: LOD mesh nodes (lod:0, lod:1, ...)
    //   N+1..: DOF nodes (dof:unknown.0, dof:unknown.1, ...)
    //   ...: switch nodes
    //   ...: slot nodes
    //
    // For Stage 3 minimal: root + LOD mesh nodes + DOF nodes (from the
    // model record's n_dofs). Switch and slot nodes are added when the
    // model has them.

    std::size_t n_lod_nodes = lod_meshes.size();
    int n_dofs = rec->effective_dofs();
    int n_switches = rec->effective_switches();
    int n_slots = static_cast<int>(rec->slots.size());

    std::size_t total_nodes = 1 + n_lod_nodes;
    if (opts.tag_dof_switch_slot) {
        total_nodes += static_cast<std::size_t>(n_dofs + n_switches + n_slots);
    }

    w.raw("  \"nodes\": [\n");

    // Node 0: root
    w.raw("    { \"name\": ");
    w.string("root");
    w.raw(", \"children\": [");
    for (std::size_t i = 0; i < n_lod_nodes; ++i) {
        if (i) w.raw(",");
        w.number(static_cast<unsigned long>(1 + i));
    }
    w.raw("] }");

    // LOD mesh nodes
    for (std::size_t i = 0; i < lod_meshes.size(); ++i) {
        w.raw(",\n    { \"name\": ");
        w.string(lod_node_name(lod_meshes[i].level));
        w.raw(", \"mesh\": ");
        w.number(static_cast<unsigned long>(i));
        w.raw(", \"extras\": { \"f4\": { \"v\": 1, \"kind\": \"lod\", \"id\": \"");
        w.raw(std::to_string(lod_meshes[i].level));
        w.raw("\", \"level\": ");
        w.number(static_cast<unsigned long>(lod_meshes[i].level));
        w.raw(" } } }");
    }

    // DOF nodes (dof:unknown.N)
    std::size_t node_idx = 1 + n_lod_nodes;
    if (opts.tag_dof_switch_slot) {
        for (int d = 0; d < n_dofs; ++d) {
            w.raw(",\n    { \"name\": ");
            w.string(dof_node_name(d));
            w.raw(", \"extras\": { \"f4\": { \"v\": 1, \"kind\": \"dof\", \"id\": \"unknown.");
            w.raw(std::to_string(d));
            w.raw("\", \"index\": ");
            w.number(static_cast<unsigned long>(d));
            w.raw(", \"min\": 0.0, \"max\": 0.0, \"mult\": 1.0, \"flags\": 0 } } }");
            ++node_idx;
        }

        // Switch nodes (sw:unknown.N)
        for (int s = 0; s < n_switches; ++s) {
            w.raw(",\n    { \"name\": ");
            w.string(switch_node_name(s));
            w.raw(", \"extras\": { \"f4\": { \"v\": 1, \"kind\": \"sw\", \"id\": \"unknown.");
            w.raw(std::to_string(s));
            w.raw("\", \"index\": ");
            w.number(static_cast<unsigned long>(s));
            w.raw(" } } }");
            ++node_idx;
        }

        // Slot nodes (slot:unknown.N)
        for (int s = 0; s < n_slots; ++s) {
            w.raw(",\n    { \"name\": ");
            w.string(slot_node_name(s));
            w.raw(", \"extras\": { \"f4\": { \"v\": 1, \"kind\": \"slot\", \"id\": \"unknown.");
            w.raw(std::to_string(s));
            w.raw("\", \"index\": ");
            w.number(static_cast<unsigned long>(s));
            // Bake the slot position as a translation (converted to glTF coords).
            const auto& slot = rec->slots[static_cast<std::size_t>(s)];
            Vec3f p = opts.convert_to_gltf_coords
                ? to_gltf(slot.position.x, slot.position.y, slot.position.z)
                : Vec3f{slot.position.x, slot.position.y, slot.position.z};
            w.raw(", \"translation\": [");
            // Use raw number formatting — w.number(double) would use %.17g
            char tmp[64];
            std::snprintf(tmp, sizeof(tmp), "%.6g, %.6g, %.6g", p.x, p.y, p.z);
            w.raw(tmp);
            w.raw("] } } }");
            ++node_idx;
        }
    }

    w.raw("\n  ],\n");

    // ── Meshes ───────────────────────────────────────────────────────
    w.raw("  \"meshes\": [\n");
    for (std::size_t i = 0; i < lod_meshes.size(); ++i) {
        if (i) w.raw(",\n");
        const auto& lm = lod_meshes[i];
        w.raw("    { \"name\": ");
        w.string("LOD_" + std::to_string(lm.level));
        w.raw(", \"primitives\": [ { \"POSITION\": ");
        w.number(static_cast<unsigned long>(lm.positions_accessor));
        w.raw(", \"NORMAL\": ");
        w.number(static_cast<unsigned long>(lm.normals_accessor));
        w.raw(", \"indices\": ");
        w.number(static_cast<unsigned long>(lm.indices_accessor));
        w.raw(", \"mode\": 4 } ] }");
    }
    w.raw("\n  ],\n");

    // ── Accessors ───────────────────────────────────────────────────
    w.raw("  \"accessors\": [\n");
    for (std::size_t i = 0; i < accessors.size(); ++i) {
        if (i) w.raw(",\n");
        const auto& a = accessors[i];
        w.raw("    { \"bufferView\": ");
        w.number(static_cast<unsigned long>(a.buffer_view));
        w.raw(", \"componentType\": ");
        w.number(static_cast<unsigned long>(a.component_type));
        w.raw(", \"count\": ");
        w.number(static_cast<unsigned long>(a.count));
        w.raw(", \"type\": ");
        w.string(a.type);
        w.raw(" }");
    }
    w.raw("\n  ],\n");

    // ── Buffer views ────────────────────────────────────────────────
    w.raw("  \"bufferViews\": [\n");
    for (std::size_t i = 0; i < buffer_views.size(); ++i) {
        if (i) w.raw(",\n");
        const auto& bv = buffer_views[i];
        w.raw("    { \"buffer\": 0, \"byteOffset\": ");
        w.number(static_cast<unsigned long>(bv.byte_offset));
        w.raw(", \"byteLength\": ");
        w.number(static_cast<unsigned long>(bv.byte_length));
        if (bv.target != 0) {
            w.raw(", \"target\": ");
            w.number(static_cast<unsigned long>(bv.target));
        }
        w.raw(" }");
    }
    w.raw("\n  ],\n");

    // ── Buffers ─────────────────────────────────────────────────────
    w.raw("  \"buffers\": [\n");
    w.raw("    { \"byteLength\": ");
    w.number(static_cast<unsigned long>(builder.size()));
    w.raw(", \"uri\": ");
    w.string(bin_filename);
    w.raw(" }\n");
    w.raw("  ]\n");

    w.raw("}\n");

    // Write the .gltf file
    std::string gltf_filename = filename_base + ".gltf";
    std::filesystem::path gltf_path = out_dir / gltf_filename;
    {
        std::ofstream f(gltf_path);
        if (!f) throw std::runtime_error("gltf_emitter: cannot write " + gltf_path.string());
        f << w.str();
    }

    // ── Build the result ──────────────────────────────────────────────
    GltfEmitResult result;
    result.gltf_path = gltf_path;
    result.bin_path = bin_path;
    result.lod_count = lod_meshes.size();
    result.dof_count = static_cast<std::size_t>(n_dofs);
    result.switch_count = static_cast<std::size_t>(n_switches);
    result.slot_count = static_cast<std::size_t>(n_slots);
    for (const auto& lm : lod_meshes) {
        result.total_vertices += lm.vertex_count;
        result.total_triangles += lm.triangle_count;
    }
    return result;
}

} // namespace f4::import

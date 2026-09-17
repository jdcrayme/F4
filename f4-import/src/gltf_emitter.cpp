// f4-import/src/gltf_emitter.cpp
//
// KoreaObj → glTF 2.0 emitter. Takes the flat primitive lists from
// f4-models' geometry extractor and writes a .gltf JSON + .bin binary
// pair that any glTF 2.0 loader can read back.
//
// Per Tranche 0c of NO_BINARY_RUNTIME_PLAN.md the emitter is
// spec-compliant and textured:
//   - One glTF mesh per LOD; one primitive per source Mesh. Source
//     meshes are already grouped per (texture, primitive kind), so each
//     glTF primitive maps 1:1 to a material — no merging, nothing lost
//     (triangle, line, and point meshes all survive).
//   - TEXCOORD_0 / COLOR_0 accessors are emitted when the source mesh
//     carries UVs / vertex colors. Vertex colors are resolved through
//     the HDR ColorBank exactly like f4-renderer's resolve_vertex_color
//     (index < 4096 → bank lookup; larger values are direct packed RGBA).
//   - Materials reference "textures/NNNNN.png" written by the
//     `f4import textures` step (Tranche 0c.1). Chroma-keyed textures get
//     alphaMode MASK so their keyed color stays transparent.
//   - DOF/switch/slot nodes tagged with the §6 grammar (dof:unknown.N
//     for unmapped indices — the spec's "Untagged DOFs are not lost").
//
// Coordinate conversion: Falcon model space is feet, +Z up; glTF is
// meters, +Y up. The transform is baked at export.

#include <f4/import/gltf_emitter.hpp>
#include <f4/import/vocab.hpp>
#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>
#include <f4/models/bsp_node.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/geometry_grouped.hpp>
#include <f4/json/writer.hpp>

#include <array>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
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

// ── Vertex color resolution ───────────────────────────────────────────────
//
// Prim.rgba is an int index into the HDR ColorBank, NOT packed ARGB.
// Mirrors f4-renderer/src/mesh_builder.cpp resolve_vertex_color so the
// exported colors match what the runtime renders:
//   0            → no color (white when textured, gray otherwise)
//   1..4095      → ColorBank lookup (0xRRGGBBAA)
//   >= 4096      → direct packed RGBA, R in the low byte (legacy path)

struct Rgba4 {
    float r, g, b, a;
};

Rgba4 resolve_color(uint32_t color_index,
                    const f4::models::ColorBank& bank,
                    bool mesh_is_textured) {
    if (color_index == 0) {
        if (mesh_is_textured) return {1.0f, 1.0f, 1.0f, 1.0f};
        return {180.0f / 255.0f, 180.0f / 255.0f, 180.0f / 255.0f, 1.0f};
    }
    if (color_index < 4096) {
        const uint32_t rgba = bank.rgba_at(static_cast<int>(color_index));
        if (rgba != 0) {
            return {
                static_cast<float>((rgba >> 24) & 0xFF) / 255.0f,
                static_cast<float>((rgba >> 16) & 0xFF) / 255.0f,
                static_cast<float>((rgba >> 8) & 0xFF) / 255.0f,
                static_cast<float>(rgba & 0xFF) / 255.0f};
        }
    }
    return {
        static_cast<float>(color_index & 0xFF) / 255.0f,
        static_cast<float>((color_index >> 8) & 0xFF) / 255.0f,
        static_cast<float>((color_index >> 16) & 0xFF) / 255.0f,
        static_cast<float>((color_index >> 24) & 0xFF) / 255.0f};
}

// ── Binary buffer builder ─────────────────────────────────────────────────
//
// The .bin file contains all vertex data (positions, normals, uvs,
// colors) and index data, concatenated. Buffer views describe the
// slices; accessors describe the typed views into the buffer views.

struct BufferBuilder {
    std::vector<uint8_t> data;

    // Align to 4 bytes (glTF requires accessor byteOffset to be aligned
    // to the component size; all our components are 4-byte floats/uints).
    void align4() {
        while (data.size() % 4 != 0) data.push_back(0);
    }
    std::size_t size() const noexcept { return data.size(); }
};

void push_float(std::vector<uint8_t>& buf, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
}

void push_u32(std::vector<uint8_t>& buf, uint32_t u) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
}

struct AccessorInfo {
    std::size_t buffer_view;
    int component_type;        // 5126 = FLOAT, 5125 = UINT
    std::size_t count;
    std::string type;          // "SCALAR", "VEC2", "VEC3", "VEC4"
};

struct BufferViewInfo {
    std::size_t byte_offset;
    std::size_t byte_length;
    int target;                // 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER
};

// Append float data + buffer view + accessor; returns the accessor index.
// element_count is the number of vecN elements (values.size() / components).
std::size_t append_float_accessor(
    BufferBuilder& builder,
    std::vector<BufferViewInfo>& buffer_views,
    std::vector<AccessorInfo>& accessors,
    const std::vector<float>& values,
    std::size_t element_count,
    const char* type,
    int target) {
    builder.align4();
    const std::size_t offset = builder.size();
    for (float f : values) push_float(builder.data, f);

    const std::size_t bv = buffer_views.size();
    buffer_views.push_back({offset, values.size() * 4, target});
    const std::size_t acc = accessors.size();
    accessors.push_back({bv, 5126, element_count, type});
    return acc;
}

// Append uint32 index data; returns the accessor index (SCALAR, UINT).
std::size_t append_index_accessor(
    BufferBuilder& builder,
    std::vector<BufferViewInfo>& buffer_views,
    std::vector<AccessorInfo>& accessors,
    const std::vector<uint32_t>& indices) {
    builder.align4();
    const std::size_t offset = builder.size();
    for (uint32_t i : indices) push_u32(builder.data, i);

    const std::size_t bv = buffer_views.size();
    buffer_views.push_back({offset, indices.size() * 4, 34963});
    const std::size_t acc = accessors.size();
    accessors.push_back({bv, 5125, indices.size(), "SCALAR"});
    return acc;
}

// ── Per-mesh accessor emission (shared flat/hierarchy) ───────────────────

struct MeshAccessors {
    std::size_t positions;
    std::size_t normals;
    std::size_t uv;          // SIZE_MAX = not emitted
    std::size_t color;       // SIZE_MAX = not emitted
    std::size_t indices;
    std::size_t vertex_count;
};

constexpr std::size_t kNoAccessor = static_cast<std::size_t>(-1);

/// Emit one f4::models::Mesh's vertex/index data into the binary buffer
/// and record its accessors. Shared by the flat and hierarchy emitters
/// so both paths bake identical vertex data (same UV/color presence
/// detection, same ColorBank resolution, same coordinate conversion).
MeshAccessors append_mesh_accessors(
    BufferBuilder& builder,
    std::vector<BufferViewInfo>& buffer_views,
    std::vector<AccessorInfo>& accessors,
    const f4::models::Mesh& m,
    bool convert_to_gltf_coords,
    const f4::models::ColorBank& color_bank) {

    const bool mesh_is_textured = (m.tex_id >= 0);

    // UV / color presence. Unset UVs stay (0,0); unset colors are 0.
    bool has_uv = false, has_color = false;
    for (const auto& v : m.vertices) {
        if (v.uv.u != 0.0f || v.uv.v != 0.0f) has_uv = true;
        if (v.color != 0) has_color = true;
    }

    // Positions
    std::vector<float> positions;
    positions.reserve(m.vertices.size() * 3);
    for (const auto& v : m.vertices) {
        Vec3f p = convert_to_gltf_coords
            ? to_gltf(v.position.x, v.position.y, v.position.z)
            : Vec3f{v.position.x, v.position.y, v.position.z};
        positions.push_back(p.x);
        positions.push_back(p.y);
        positions.push_back(p.z);
    }
    const std::size_t pos_acc = append_float_accessor(
        builder, buffer_views, accessors,
        positions, m.vertices.size(), "VEC3", 34962);

    // Normals
    std::vector<float> normals;
    normals.reserve(m.vertices.size() * 3);
    for (const auto& v : m.vertices) {
        Vec3f n = convert_to_gltf_coords
            ? to_gltf(v.normal.x, v.normal.y, v.normal.z)
            : Vec3f{v.normal.x, v.normal.y, v.normal.z};
        normals.push_back(n.x);
        normals.push_back(n.y);
        normals.push_back(n.z);
    }
    const std::size_t norm_acc = append_float_accessor(
        builder, buffer_views, accessors,
        normals, m.vertices.size(), "VEC3", 34962);

    // UVs
    std::size_t uv_acc = kNoAccessor;
    if (has_uv) {
        std::vector<float> uvs;
        uvs.reserve(m.vertices.size() * 2);
        for (const auto& v : m.vertices) {
            uvs.push_back(v.uv.u);
            uvs.push_back(v.uv.v);
        }
        uv_acc = append_float_accessor(
            builder, buffer_views, accessors,
            uvs, m.vertices.size(), "VEC2", 34962);
    }

    // Vertex colors (resolved through the ColorBank)
    std::size_t color_acc = kNoAccessor;
    if (has_color) {
        std::vector<float> colors;
        colors.reserve(m.vertices.size() * 4);
        for (const auto& v : m.vertices) {
            const Rgba4 c = resolve_color(v.color, color_bank, mesh_is_textured);
            colors.push_back(c.r);
            colors.push_back(c.g);
            colors.push_back(c.b);
            colors.push_back(c.a);
        }
        color_acc = append_float_accessor(
            builder, buffer_views, accessors,
            colors, m.vertices.size(), "VEC4", 34962);
    }

    // Indices per primitive kind
    std::vector<uint32_t> indices;
    indices.reserve(m.triangles.size() * 3);
    switch (m.kind) {
        case f4::models::PrimitiveKind::Triangles:
            for (const auto& t : m.triangles) {
                indices.push_back(t.v0);
                indices.push_back(t.v1);
                indices.push_back(t.v2);
            }
            break;
        case f4::models::PrimitiveKind::Lines:
            indices.reserve(m.lines.size() * 2);
            for (const auto& l : m.lines) {
                indices.push_back(l.v0);
                indices.push_back(l.v1);
            }
            break;
        case f4::models::PrimitiveKind::Points:
            indices.reserve(m.points.size());
            for (uint32_t i = 0; i < m.points.size(); ++i) {
                indices.push_back(i);
            }
            break;
    }
    const std::size_t idx_acc = append_index_accessor(
        builder, buffer_views, accessors, indices);

    return {pos_acc, norm_acc, uv_acc, color_acc, idx_acc,
            m.vertices.size()};
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

GltfEmitResult emit_model_hierarchy(
    const f4::models::ModelDatabase& db,
    int parent_index,
    const std::filesystem::path& out_dir,
    const std::string& asset_id_string,
    const GltfEmitOptions& opts);

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

    // Animated hierarchy path (AIRCRAFT_ANIMATION_PLAN.md §4).
    if (opts.emit_hierarchy) {
        return emit_model_hierarchy(db, parent_index, out_dir,
                                    asset_id_string, opts);
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

    // ── Plan materials ──────────────────────────────────────────────────
    //
    // One material per referenced texture (sorted by texture id for
    // deterministic output) plus one shared "vertexcolor" material for
    // untextured meshes. Chroma-keyed textures get alphaMode MASK.
    struct MaterialInfo {
        int32_t tex_id;        // -1 = vertex-color/untextured
        bool chroma_keyed;
        std::string name;
    };
    std::vector<MaterialInfo> materials;

    std::set<int32_t> used_tex_ids;
    for (const auto& lod : lods) {
        for (const auto& m : lod.geom.meshes) {
            if (m.tex_id >= 0) used_tex_ids.insert(m.tex_id);
        }
    }
    const auto& tex_entries = db.tex_entries();
    for (int32_t tex_id : used_tex_ids) {
        bool chroma = false;
        if (static_cast<std::size_t>(tex_id) < tex_entries.size()) {
            chroma = tex_entries[static_cast<std::size_t>(tex_id)].chroma_key != 0;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "tex:%05d", tex_id);
        materials.push_back({tex_id, chroma, name});
    }
    // The shared vertex-color material always sits at the last index;
    // untextured meshes reference it.
    const int vertexcolor_material = static_cast<int>(materials.size());
    materials.push_back({-1, false, "vertexcolor"});

    auto material_index_for = [&](const f4::models::Mesh& m) -> int {
        if (m.tex_id >= 0) {
            auto it = std::find_if(materials.begin(), materials.end(),
                [&](const MaterialInfo& mi) { return mi.tex_id == m.tex_id; });
            return static_cast<int>(std::distance(materials.begin(), it));
        }
        return vertexcolor_material;
    };

    // ── Build the binary buffer ─────────────────────────────────────────
    //
    // Per source mesh: positions (VEC3 FLOAT) + normals (VEC3 FLOAT) +
    // optional uvs (VEC2 FLOAT) + optional colors (VEC4 FLOAT) + indices
    // (SCALAR UINT). One glTF primitive per source mesh.

    BufferBuilder builder;
    std::vector<BufferViewInfo> buffer_views;
    std::vector<AccessorInfo> accessors;

    struct PrimInfo {
        int level;
        std::size_t positions_accessor;
        std::size_t normals_accessor;
        std::size_t uv_accessor;       // SIZE_MAX = not emitted
        std::size_t color_accessor;    // SIZE_MAX = not emitted
        std::size_t indices_accessor;
        std::size_t vertex_count;
        std::size_t prim_count;        // tris / lines / points
        int mode;                      // 4 = TRIANGLES, 1 = LINES, 0 = POINTS
        int material;
    };
    constexpr std::size_t kNoAccessor = static_cast<std::size_t>(-1);

    std::vector<PrimInfo> prims;

    for (const auto& lod : lods) {
        for (const auto& m : lod.geom.meshes) {
            // Mirror the renderer's skip logic (mesh_builder.cpp).
            if (m.vertices.empty()) continue;
            bool has_data = false;
            switch (m.kind) {
                case f4::models::PrimitiveKind::Triangles: has_data = !m.triangles.empty(); break;
                case f4::models::PrimitiveKind::Lines:     has_data = !m.lines.empty();     break;
                case f4::models::PrimitiveKind::Points:    has_data = !m.points.empty();    break;
            }
            if (!has_data) continue;

            // Vertex/index accessor emission is shared with the
            // hierarchy path (append_mesh_accessors).
            const auto acc = append_mesh_accessors(
                builder, buffer_views, accessors, m,
                opts.convert_to_gltf_coords, db.color_bank());

            int mode = 4;
            switch (m.kind) {
                case f4::models::PrimitiveKind::Lines:  mode = 1; break;
                case f4::models::PrimitiveKind::Points: mode = 0; break;
                default: mode = 4; break;
            }
            prims.push_back({lod.level, acc.positions, acc.normals, acc.uv,
                             acc.color, acc.indices, acc.vertex_count,
                             m.primitive_count(), mode,
                             material_index_for(m)});
        }
    }

    if (prims.empty()) {
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

    // ── Group primitives by LOD (one glTF mesh per LOD) ─────────────────
    std::map<int, std::vector<std::size_t>> prims_by_lod;
    for (std::size_t i = 0; i < prims.size(); ++i) {
        prims_by_lod[prims[i].level].push_back(i);
    }

    // ── Build the .gltf JSON ───────────────────────────────────────────
    f4::json::Writer w;

    w.raw("{\n");
    w.raw("  \"asset\": { \"version\": \"2.0\", \"generator\": \"f4import models 0.5.0\" },\n");
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

    std::size_t n_lod_nodes = prims_by_lod.size();
    int n_dofs = rec->effective_dofs();
    int n_switches = rec->effective_switches();
    int n_slots = static_cast<int>(rec->slots.size());

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

    // LOD mesh nodes (mesh index = position within prims_by_lod)
    {
        std::size_t mesh_index = 0;
        for (const auto& [level, prim_indices] : prims_by_lod) {
            w.raw(",\n    { \"name\": ");
            w.string(lod_node_name(level));
            w.raw(", \"mesh\": ");
            w.number(static_cast<unsigned long>(mesh_index));
            w.raw(", \"extras\": { \"f4\": { \"v\": 1, \"kind\": \"lod\", \"id\": \"");
            w.raw(std::to_string(level));
            w.raw("\", \"level\": ");
            w.number(static_cast<unsigned long>(level));
            w.raw(" } } }");
            ++mesh_index;
        }
    }

    // DOF nodes (dof:unknown.N)
    if (opts.tag_dof_switch_slot) {
        for (int d = 0; d < n_dofs; ++d) {
            w.raw(",\n    { \"name\": ");
            w.string(dof_node_name(d));
            w.raw(", \"extras\": { \"f4\": { \"v\": 1, \"kind\": \"dof\", \"id\": \"unknown.");
            w.raw(std::to_string(d));
            w.raw("\", \"index\": ");
            w.number(static_cast<unsigned long>(d));
            w.raw(", \"min\": 0.0, \"max\": 0.0, \"mult\": 1.0, \"flags\": 0 } } }");
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
        }
    }

    w.raw("\n  ],\n");

    // ── Meshes ───────────────────────────────────────────────────────
    w.raw("  \"meshes\": [\n");
    {
        std::size_t mesh_index = 0;
        for (const auto& [level, prim_indices] : prims_by_lod) {
            if (mesh_index) w.raw(",\n");
            w.raw("    { \"name\": ");
            w.string("LOD_" + std::to_string(level));
            w.raw(", \"primitives\": [");
            bool first_prim = true;
            for (std::size_t pi : prim_indices) {
                const auto& p = prims[pi];
                if (!first_prim) w.raw(",");
                first_prim = false;
                w.raw("\n      { \"attributes\": { \"POSITION\": ");
                w.number(static_cast<unsigned long>(p.positions_accessor));
                w.raw(", \"NORMAL\": ");
                w.number(static_cast<unsigned long>(p.normals_accessor));
                if (p.uv_accessor != kNoAccessor) {
                    w.raw(", \"TEXCOORD_0\": ");
                    w.number(static_cast<unsigned long>(p.uv_accessor));
                }
                if (p.color_accessor != kNoAccessor) {
                    w.raw(", \"COLOR_0\": ");
                    w.number(static_cast<unsigned long>(p.color_accessor));
                }
                w.raw(" }, \"indices\": ");
                w.number(static_cast<unsigned long>(p.indices_accessor));
                w.raw(", \"material\": ");
                w.number(static_cast<unsigned long>(p.material));
                w.raw(", \"mode\": ");
                w.number(static_cast<unsigned long>(p.mode));
                w.raw(" }");
            }
            w.raw("\n    ] }");
            ++mesh_index;
        }
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

    // ── Samplers / images / textures / materials ────────────────────
    //
    // Image URIs are relative to the .gltf file: write_texture_png puts
    // them in <data>/Models/koreaobj/textures/NNNNN.png and this emitter
    // writes .gltf files into <data>/Models/koreaobj/, so the relative
    // path is always "textures/NNNNN.png".
    if (!materials.empty()) {
        w.raw("  \"samplers\": [\n");
        w.raw("    { \"magFilter\": 9729, \"minFilter\": 9729, \"wrapS\": 10497, \"wrapT\": 10497 }\n");
        w.raw("  ],\n");

        w.raw("  \"images\": [\n");
        bool first_img = true;
        for (const auto& mat : materials) {
            if (mat.tex_id < 0) continue;
            if (!first_img) w.raw(",\n");
            first_img = false;
            char uri[32];
            std::snprintf(uri, sizeof(uri), "textures/%05d.png", mat.tex_id);
            w.raw("    { \"uri\": ");
            w.string(uri);
            w.raw(" }");
        }
        w.raw("\n  ],\n");

        w.raw("  \"textures\": [\n");
        {
            int tex_idx = 0;
            bool first_tex = true;
            for (const auto& mat : materials) {
                if (mat.tex_id < 0) continue;
                if (!first_tex) w.raw(",\n");
                first_tex = false;
                w.raw("    { \"sampler\": 0, \"source\": ");
                w.number(static_cast<unsigned long>(tex_idx));
                w.raw(" }");
                ++tex_idx;
            }
        }
        w.raw("\n  ],\n");

        w.raw("  \"materials\": [\n");
        for (std::size_t i = 0; i < materials.size(); ++i) {
            if (i) w.raw(",\n");
            const auto& mat = materials[i];
            w.raw("    { \"name\": ");
            w.string(mat.name);
            w.raw(", \"pbrMetallicRoughness\": { ");
            if (mat.tex_id >= 0) {
                // Textured: texture index = position among textured materials.
                int tex_idx = 0;
                for (std::size_t j = 0; j < i; ++j) {
                    if (materials[j].tex_id >= 0) ++tex_idx;
                }
                w.raw("\"baseColorTexture\": { \"index\": ");
                w.number(static_cast<unsigned long>(tex_idx));
                w.raw(" }");
            } else {
                w.raw("\"baseColorFactor\": [1.0, 1.0, 1.0, 1.0]");
            }
            w.raw(", \"metallicFactor\": 0.0, \"roughnessFactor\": 0.9 }");
            if (mat.chroma_keyed) {
                // Chroma-keyed pixels were exported with alpha = 0 by the
                // TEX decoder; MASK keeps them cut out in glTF viewers.
                w.raw(", \"alphaMode\": \"MASK\", \"alphaCutoff\": 0.5");
            }
            w.raw(", \"doubleSided\": true }");
        }
        w.raw("\n  ],\n");
    }

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
    result.lod_count = prims_by_lod.size();
    result.dof_count = static_cast<std::size_t>(n_dofs);
    result.switch_count = static_cast<std::size_t>(n_switches);
    result.slot_count = static_cast<std::size_t>(n_slots);
    result.material_count = materials.size();
    for (const auto& mat : materials) {
        if (mat.tex_id >= 0) ++result.texture_count;
    }
    for (const auto& p : prims) {
        result.total_vertices += p.vertex_count;
        result.primitive_count += p.prim_count;
        if (p.mode == 4) result.total_triangles += p.prim_count;
    }
    return result;
}


// ── Hierarchy emitter (AIRCRAFT_ANIMATION_PLAN.md §4) ──────────────────────

namespace {

// Falcon→glTF basis rows: glTF x = falcon y, glTF y = falcon z,
// glTF z = -falcon x (handedness flip). For a falcon-space linear map
// R, the glTF-space equivalent is M = B·R·Bᵀ; for a translation t,
// t' = 0.3048 · B·t (the unit scale applies to lengths only).
constexpr double kB[3][3] = {
    {0, 1, 0},
    {0, 0, 1},
    {-1, 0, 0},
};

std::array<double, 3> basis_translate(const f4::models::Vec3& t) {
    return {t.y * kFeetToMeters, t.z * kFeetToMeters, -t.x * kFeetToMeters};
}

/// B·R·Bᵀ followed by quaternion extraction (Shepperd). The rotation
/// maps directions, so no unit scale involved.
std::array<double, 4> conjugated_quaternion(
    const f4::math::Mat3<float>& R) {
    double M[3][3] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double sum = 0;
            for (int k = 0; k < 3; ++k) {
                for (int l = 0; l < 3; ++l) {
                    sum += kB[i][k] * R.m[k][l] * kB[j][l];
                }
            }
            M[i][j] = sum;
        }
    }

    std::array<double, 4> q{};
    const double trace = M[0][0] + M[1][1] + M[2][2];
    if (trace > 0.0) {
        const double S = std::sqrt(trace + 1.0) * 2.0;
        q[3] = 0.25 * S;
        q[0] = (M[2][1] - M[1][2]) / S;
        q[1] = (M[0][2] - M[2][0]) / S;
        q[2] = (M[1][0] - M[0][1]) / S;
    } else if (M[0][0] > M[1][1] && M[0][0] > M[2][2]) {
        const double S = std::sqrt(1.0 + M[0][0] - M[1][1] - M[2][2]) * 2.0;
        q[3] = (M[2][1] - M[1][2]) / S;
        q[0] = 0.25 * S;
        q[1] = (M[0][1] + M[1][0]) / S;
        q[2] = (M[0][2] + M[2][0]) / S;
    } else if (M[1][1] > M[2][2]) {
        const double S = std::sqrt(1.0 + M[1][1] - M[0][0] - M[2][2]) * 2.0;
        q[3] = (M[0][2] - M[2][0]) / S;
        q[0] = (M[0][1] + M[1][0]) / S;
        q[1] = 0.25 * S;
        q[2] = (M[1][2] + M[2][1]) / S;
    } else {
        const double S = std::sqrt(1.0 + M[2][2] - M[0][0] - M[1][1]) * 2.0;
        q[3] = (M[1][0] - M[0][1]) / S;
        q[0] = (M[0][2] + M[2][0]) / S;
        q[1] = (M[1][2] + M[2][1]) / S;
        q[2] = 0.25 * S;
    }
    const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                               q[3] * q[3]);
    if (n > 1e-12) {
        for (auto& v : q) v /= n;
    } else {
        q = {0, 0, 0, 1};
    }
    return q;
}

/// A planned glTF node — everything needed to serialize it in one pass
/// once the children indices are known.
struct PlanNode {
    std::string name;
    std::string extras;   // raw JSON VALUE for "extras" (e.g. {"f4": ...}); empty = none
    bool has_mesh = false;
    std::size_t mesh = 0;
    bool has_t = false;
    std::array<double, 3> t{};
    bool has_q = false;
    std::array<double, 4> q{};
    std::vector<std::size_t> children;
};

std::string num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

std::string make_dof_extras(const f4::models::TaggedAncestor& a,
                            const std::string& id,
                            const std::string& channel) {
    f4::json::Writer w;
    w.raw("{ \"f4\": { \"v\": 1, \"kind\": \"dof\", \"id\": ");
    w.string(id);
    w.raw(", \"index\": ");
    w.number(static_cast<unsigned long>(a.number));
    w.raw(", \"min\": ");
    w.raw(num(a.dof_min));
    w.raw(", \"max\": ");
    w.raw(num(a.dof_max));
    w.raw(", \"mult\": ");
    w.raw(num(a.dof_multiplier));
    w.raw(", \"flags\": ");
    w.number(static_cast<unsigned long>(a.dof_flags));
    if (!channel.empty()) {
        w.raw(", \"channel\": ");
        w.string(channel);
    }
    switch (a.type) {
        case f4::models::BspNodeType::BTransNode: {
            // Translator: the vector scales by the processed value.
            // glTF-space vector at value 1.
            const auto tv = basis_translate(a.frame_translation);
            w.raw(", \"op\": \"trans\", \"trans\": [");
            w.raw(num(tv[0])); w.raw(", ");
            w.raw(num(tv[1])); w.raw(", ");
            w.raw(num(tv[2]));
            w.raw("]");
            break;
        }
        case f4::models::BspNodeType::BScaleNode: {
            // Scale: target scale at value 1. Scale factors are
            // unitless, so the basis conjugation only permutes the
            // components (glTF x = falcon y, glTF y = falcon z,
            // glTF z = -falcon x — the minus drops out of a diagonal).
            std::array<double, 3> st2 = {a.scale_target.y,
                                         a.scale_target.z,
                                         a.scale_target.x};
            w.raw(", \"op\": \"scale\", \"scale_target\": [");
            w.raw(num(st2[0])); w.raw(", ");
            w.raw(num(st2[1])); w.raw(", ");
            w.raw(num(st2[2]));
            w.raw("]");
            break;
        }
        default:
            // Rotational DOF: FreeFalcon rotates the subtree about the
            // LOCAL X axis after the frame (R = dof_rotation·Rx(v)).
            // Conjugating through the falcon→glTF basis B (glTF =
            // B·falcon): B·Rx(θ)·Bᵀ = Rot(det(B)·B·x̂, θ). B is
            // IMPROPER (det = −1 — the handedness flip), so the axis
            // flips: B·x̂ = (0,0,−1), det·B·x̂ = (0,0,+1). The sign
            // matters — with B·x̂ every DOF deflects the inverse way
            // (invisible at rest, wrong as soon as anything moves).
            w.raw(", \"op\": \"rot\", \"axis\": [0, 0, 1]");
            break;
    }
    w.raw(" } }");
    return w.str();
}

std::string make_sw_extras(const f4::models::TaggedAncestor& a,
                           const std::string& id, const std::string& channel,
                           bool branch, int child) {
    f4::json::Writer w;
    w.raw("{ \"f4\": { \"v\": 1, \"kind\": \"sw\", \"id\": ");
    w.string(branch ? id + "." + std::to_string(child) : id);
    w.raw(", \"index\": ");
    w.number(static_cast<unsigned long>(a.number));
    if (branch) {
        w.raw(", \"child\": ");
        w.number(static_cast<unsigned long>(child));
    }
    if (!channel.empty()) {
        w.raw(", \"channel\": ");
        w.string(channel);
    }
    if (a.switch_flags & 1) {  // XSWT_REVERSED_EFFECT
        w.raw(", \"reversed\": true");
    }
    w.raw(" } }");
    return w.str();
}

} // namespace

GltfEmitResult emit_model_hierarchy(
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
    const f4::import::FamilyTable* family = opts.family_table;

    int n_lods = opts.emit_all_lods ? rec->n_lods : 1;
    if (n_lods < 1) n_lods = 1;

    // ── Grouped extraction per LOD ──────────────────────────────────────
    struct LodGrouped {
        f4::models::GroupedGeometry geom;
        int level;
    };
    std::vector<LodGrouped> lods;
    lods.reserve(static_cast<std::size_t>(n_lods));
    for (int lod = 0; lod < n_lods; ++lod) {
        auto g = db.extract_model_geometry_grouped(parent_index, lod, {});
        if (g.total_vertices() == 0) continue;
        lods.push_back({std::move(g), lod});
    }
    if (lods.empty()) {
        throw std::runtime_error("gltf_emitter: model " +
            std::to_string(parent_index) +
            " has no extractable geometry (all LODs empty)");
    }

    // ── Materials (same planning as the flat path) ─────────────────────
    struct MaterialInfo {
        int32_t tex_id;
        bool chroma_keyed;
        std::string name;
    };
    std::vector<MaterialInfo> materials;
    std::set<int32_t> used_tex_ids;
    for (const auto& lod : lods) {
        for (const auto& gm : lod.geom.meshes) {
            if (gm.mesh.tex_id >= 0) used_tex_ids.insert(gm.mesh.tex_id);
        }
    }
    const auto& tex_entries = db.tex_entries();
    for (int32_t tex_id : used_tex_ids) {
        bool chroma = false;
        if (static_cast<std::size_t>(tex_id) < tex_entries.size()) {
            chroma = tex_entries[static_cast<std::size_t>(tex_id)].chroma_key != 0;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "tex:%05d", tex_id);
        materials.push_back({tex_id, chroma, name});
    }
    const int vertexcolor_material = static_cast<int>(materials.size());
    materials.push_back({-1, false, "vertexcolor"});
    auto material_index_for = [&](const f4::models::Mesh& m) -> int {
        if (m.tex_id >= 0) {
            auto it = std::find_if(materials.begin(), materials.end(),
                [&](const MaterialInfo& mi) { return mi.tex_id == m.tex_id; });
            return static_cast<int>(std::distance(materials.begin(), it));
        }
        return vertexcolor_material;
    };

    // ── Binary buffer: one glTF mesh per grouped mesh ───────────────────
    BufferBuilder builder;
    std::vector<BufferViewInfo> buffer_views;
    std::vector<AccessorInfo> accessors;

    struct PartMesh {
        int level;
        std::size_t accessor_primitive;  // placeholder for clarity
        std::size_t positions, normals, uv, color, indices;
        std::size_t vertex_count;
        std::size_t prim_count;
        int mode;
        int material;
    };
    std::vector<PartMesh> part_meshes;

    for (const auto& lod : lods) {
        for (const auto& gm : lod.geom.meshes) {
            bool has_data = false;
            switch (gm.mesh.kind) {
                case f4::models::PrimitiveKind::Triangles: has_data = !gm.mesh.triangles.empty(); break;
                case f4::models::PrimitiveKind::Lines:     has_data = !gm.mesh.lines.empty();     break;
                case f4::models::PrimitiveKind::Points:    has_data = !gm.mesh.points.empty();    break;
            }
            if (!has_data) continue;

            const auto acc = append_mesh_accessors(
                builder, buffer_views, accessors, gm.mesh,
                opts.convert_to_gltf_coords, db.color_bank());
            int mode = 4;
            switch (gm.mesh.kind) {
                case f4::models::PrimitiveKind::Lines:  mode = 1; break;
                case f4::models::PrimitiveKind::Points: mode = 0; break;
                default: break;
            }
            part_meshes.push_back({lod.level, 0, acc.positions, acc.normals,
                                   acc.uv, acc.color, acc.indices,
                                   acc.vertex_count,
                                   gm.mesh.primitive_count(), mode,
                                   material_index_for(gm.mesh)});
        }
    }
    if (part_meshes.empty()) {
        throw std::runtime_error("gltf_emitter: no meshes with geometry");
    }

    // glTF mesh indices = order of part_meshes (one primitive each).

    // ── Node planning ───────────────────────────────────────────────────
    std::vector<PlanNode> nodes;
    nodes.push_back(PlanNode{});          // 0: root
    nodes.front().name = "root";

    std::size_t part_mesh_cursor = 0;
    std::vector<std::size_t> lod_node_indices;
    std::vector<std::string> bound_channels;  // first-seen order

    for (const auto& lod : lods) {
        // lod:N node
        PlanNode lod_node;
        lod_node.name = lod_node_name(lod.level);
        {
            f4::json::Writer w;
            w.raw("{ \"f4\": { \"v\": 1, \"kind\": \"lod\", \"id\": \"");
            w.raw(std::to_string(lod.level));
            w.raw("\", \"level\": ");
            w.number(static_cast<unsigned long>(lod.level));
            w.raw(" } }");
            lod_node.extras = w.str();
        }
        const std::size_t lod_idx = nodes.size();
        nodes.push_back(std::move(lod_node));
        lod_node_indices.push_back(lod_idx);

        // The trie maps chain keys → plan node indices. Chain keys are
        // the extractor's (node_index << 4 | switch_child+1) elements;
        // group nodes (the "sw:<id>" parent) use child component 0.
        std::map<std::vector<int64_t>, std::size_t> trie;

        for (const auto& gm : lod.geom.meshes) {
            // Walk the chain, creating nodes as needed.
            std::vector<int64_t> key;
            std::size_t parent = lod_idx;
            std::size_t mesh_parent = lod_idx;

            for (const auto& a : gm.chain) {
                const bool is_switch =
                    (a.type == f4::models::BspNodeType::BSwitchNode ||
                     a.type == f4::models::BspNodeType::BXSwitchNode);
                const int64_t child_component =
                    (a.switch_child >= 0)
                        ? (static_cast<int64_t>(a.switch_child) + 1)
                        : 0;
                key.push_back((static_cast<int64_t>(a.node_index) << 4) |
                              child_component);

                if (is_switch) {
                    // Group node (keyed by child component 0).
                    std::vector<int64_t> group_key = key;
                    group_key.back() =
                        static_cast<int64_t>(a.node_index) << 4;
                    std::size_t group_idx;
                    auto git = trie.find(group_key);
                    if (git != trie.end()) {
                        group_idx = git->second;
                    } else {
                        std::string id = "unknown." + std::to_string(a.number);
                        std::string channel;
                        if (family) {
                            if (const auto* b = family->sw_at(a.number)) {
                                id = b->id;
                                channel = b->channel;
                                if (std::find(bound_channels.begin(),
                                              bound_channels.end(),
                                              channel) == bound_channels.end()) {
                                    bound_channels.push_back(channel);
                                }
                            }
                        }
                        PlanNode gn;
                        gn.name = "sw:" + id;
                        gn.extras = make_sw_extras(a, id, channel, false, -1);
                        group_idx = nodes.size();
                        nodes.push_back(std::move(gn));
                        nodes[parent].children.push_back(group_idx);
                        trie.emplace(group_key, group_idx);
                    }
                    parent = group_idx;

                    // Branch node (keyed by switch_child+1).
                    auto bit = trie.find(key);
                    if (bit != trie.end()) {
                        mesh_parent = bit->second;
                        parent = bit->second;
                        continue;
                    }
                    std::string id = "unknown." + std::to_string(a.number);
                    std::string channel;
                    if (family) {
                        if (const auto* b = family->sw_at(a.number)) {
                            id = b->id;
                            channel = b->channel;
                        }
                    }
                    PlanNode bn;
                    bn.name = "sw:" + id + "." + std::to_string(a.switch_child);
                    bn.extras = make_sw_extras(a, id, channel, true,
                                               a.switch_child);
                    const std::size_t branch_idx = nodes.size();
                    nodes.push_back(std::move(bn));
                    nodes[group_idx].children.push_back(branch_idx);
                    trie.emplace(key, branch_idx);
                    mesh_parent = branch_idx;
                    parent = branch_idx;
                } else {
                    // DOF / translator / scale node.
                    auto it = trie.find(key);
                    if (it != trie.end()) {
                        mesh_parent = it->second;
                        parent = it->second;
                        continue;
                    }
                    std::string id = "unknown." + std::to_string(a.number);
                    std::string channel;
                    if (family) {
                        if (const auto* b = family->dof_at(a.number)) {
                            id = b->id;
                            channel = b->channel;
                            if (std::find(bound_channels.begin(),
                                          bound_channels.end(),
                                          channel) == bound_channels.end()) {
                                bound_channels.push_back(channel);
                            }
                        }
                    }
                    PlanNode dn;
                    dn.name = "dof:" + id;
                    dn.extras = make_dof_extras(a, id, channel);
                    if (a.type == f4::models::BspNodeType::BTransNode) {
                        // Translator: at value 0 the subtree sits at the
                        // origin — authored transform is identity; the
                        // vector lives in the extras.
                    } else if (a.type == f4::models::BspNodeType::BScaleNode) {
                        dn.t = basis_translate(a.frame_translation);
                        dn.has_t = true;
                    } else {
                        // Rotational DOF: authored frame = dof_rotation
                        // with dof_translation as the pivot.
                        dn.q = conjugated_quaternion(a.frame_rotation);
                        dn.has_q = true;
                        dn.t = basis_translate(a.frame_translation);
                        dn.has_t = true;
                    }
                    const std::size_t dof_idx = nodes.size();
                    nodes.push_back(std::move(dn));
                    nodes[parent].children.push_back(dof_idx);
                    trie.emplace(key, dof_idx);
                    mesh_parent = dof_idx;
                    parent = dof_idx;
                }
            }

            // Mesh part node under the deepest chain node (or the lod
            // node for static geometry).
            const auto& pm = part_meshes[part_mesh_cursor];
            PlanNode part;
            part.name = "part_" + std::to_string(lod.level) + "_" +
                        std::to_string(part_mesh_cursor);
            part.has_mesh = true;
            part.mesh = part_mesh_cursor;
            const std::size_t part_idx = nodes.size();
            nodes.push_back(std::move(part));
            nodes[mesh_parent].children.push_back(part_idx);
            (void)pm;
            ++part_mesh_cursor;
        }
    }

    // Slot placeholder nodes (children of the root) — same as the flat
    // path; slot semantics are class-data driven, not tree-driven.
    std::vector<std::size_t> slot_node_indices;
    for (int s = 0; s < static_cast<int>(rec->slots.size()); ++s) {
        PlanNode sn;
        sn.name = slot_node_name(s);
        {
            f4::json::Writer w;
            w.raw("{ \"f4\": { \"v\": 1, \"kind\": \"slot\", \"id\": \"unknown.");
            w.raw(std::to_string(s));
            w.raw("\", \"index\": ");
            w.number(static_cast<unsigned long>(s));
            w.raw(" } }");
            sn.extras = w.str();
        }
        const auto& slot = rec->slots[static_cast<std::size_t>(s)];
        sn.t = opts.convert_to_gltf_coords
            ? basis_translate(slot.position)
            : std::array<double, 3>{slot.position.x, slot.position.y,
                                    slot.position.z};
        sn.has_t = true;
        const std::size_t idx = nodes.size();
        nodes.push_back(std::move(sn));
        slot_node_indices.push_back(idx);
    }
    for (auto s : slot_node_indices) nodes[0].children.push_back(s);
    for (auto l : lod_node_indices) nodes[0].children.push_back(l);

    // ── Write the .bin ──────────────────────────────────────────────────
    std::string filename_base = asset_id_string;
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
        if (!f) throw std::runtime_error("gltf_emitter: cannot write " +
                                         bin_path.string());
        f.write(reinterpret_cast<const char*>(builder.data.data()),
                static_cast<std::streamsize>(builder.data.size()));
    }

    // ── Serialize the .gltf ─────────────────────────────────────────────
    f4::json::Writer w;
    w.raw("{\n");
    w.raw("  \"asset\": { \"version\": \"2.0\", \"generator\": \"f4import models 0.5.0 hierarchy\" },\n");
    w.raw("  \"scene\": 0,\n");
    w.raw("  \"scenes\": [\n    { \"nodes\": [0] }\n  ],\n");

    w.raw("  \"nodes\": [\n");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        if (i) w.raw(",\n");
        w.raw("    { \"name\": ");
        w.string(n.name);
        if (n.has_mesh) {
            w.raw(", \"mesh\": ");
            w.number(static_cast<unsigned long>(n.mesh));
        }
        if (n.has_t) {
            w.raw(", \"translation\": [");
            w.raw(num(n.t[0])); w.raw(", ");
            w.raw(num(n.t[1])); w.raw(", ");
            w.raw(num(n.t[2]));
            w.raw("]");
        }
        if (n.has_q) {
            w.raw(", \"rotation\": [");
            w.raw(num(n.q[0])); w.raw(", ");
            w.raw(num(n.q[1])); w.raw(", ");
            w.raw(num(n.q[2])); w.raw(", ");
            w.raw(num(n.q[3]));
            w.raw("]");
        }
        if (!n.children.empty()) {
            w.raw(", \"children\": [");
            for (std::size_t c = 0; c < n.children.size(); ++c) {
                if (c) w.raw(",");
                w.number(static_cast<unsigned long>(n.children[c]));
            }
            w.raw("]");
        }
        if (!n.extras.empty()) {
            w.raw(", \"extras\": ");
            w.raw(n.extras);
        }
        w.raw(" }");
    }
    w.raw("\n  ],\n");

    // Meshes — one per part, one primitive each.
    w.raw("  \"meshes\": [\n");
    for (std::size_t i = 0; i < part_meshes.size(); ++i) {
        const auto& p = part_meshes[i];
        if (i) w.raw(",\n");
        w.raw("    { \"name\": ");
        w.string("part_" + std::to_string(p.level) + "_" + std::to_string(i));
        w.raw(", \"primitives\": [ { \"attributes\": { \"POSITION\": ");
        w.number(static_cast<unsigned long>(p.positions));
        w.raw(", \"NORMAL\": ");
        w.number(static_cast<unsigned long>(p.normals));
        if (p.uv != kNoAccessor) {
            w.raw(", \"TEXCOORD_0\": ");
            w.number(static_cast<unsigned long>(p.uv));
        }
        if (p.color != kNoAccessor) {
            w.raw(", \"COLOR_0\": ");
            w.number(static_cast<unsigned long>(p.color));
        }
        w.raw(" }, \"indices\": ");
        w.number(static_cast<unsigned long>(p.indices));
        w.raw(", \"material\": ");
        w.number(static_cast<unsigned long>(p.material));
        w.raw(", \"mode\": ");
        w.number(static_cast<unsigned long>(p.mode));
        w.raw(" } ] }");
    }
    w.raw("\n  ],\n");

    // Accessors
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

    // Buffer views
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

    // Samplers / images / textures / materials
    if (!materials.empty()) {
        w.raw("  \"samplers\": [\n");
        w.raw("    { \"magFilter\": 9729, \"minFilter\": 9729, \"wrapS\": 10497, \"wrapT\": 10497 }\n");
        w.raw("  ],\n");
        w.raw("  \"images\": [\n");
        bool first_img = true;
        for (const auto& mat : materials) {
            if (mat.tex_id < 0) continue;
            if (!first_img) w.raw(",\n");
            first_img = false;
            char uri[32];
            std::snprintf(uri, sizeof(uri), "textures/%05d.png", mat.tex_id);
            w.raw("    { \"uri\": ");
            w.string(uri);
            w.raw(" }");
        }
        w.raw("\n  ],\n");
        w.raw("  \"textures\": [\n");
        {
            int tex_idx = 0;
            bool first_tex = true;
            for (const auto& mat : materials) {
                if (mat.tex_id < 0) continue;
                if (!first_tex) w.raw(",\n");
                first_tex = false;
                w.raw("    { \"sampler\": 0, \"source\": ");
                w.number(static_cast<unsigned long>(tex_idx));
                w.raw(" }");
                ++tex_idx;
            }
        }
        w.raw("\n  ],\n");
        w.raw("  \"materials\": [\n");
        for (std::size_t i = 0; i < materials.size(); ++i) {
            if (i) w.raw(",\n");
            const auto& mat = materials[i];
            w.raw("    { \"name\": ");
            w.string(mat.name);
            w.raw(", \"pbrMetallicRoughness\": { ");
            if (mat.tex_id >= 0) {
                int tex_idx = 0;
                for (std::size_t j = 0; j < i; ++j) {
                    if (materials[j].tex_id >= 0) ++tex_idx;
                }
                w.raw("\"baseColorTexture\": { \"index\": ");
                w.number(static_cast<unsigned long>(tex_idx));
                w.raw(" }");
            } else {
                w.raw("\"baseColorFactor\": [1.0, 1.0, 1.0, 1.0]");
            }
            w.raw(", \"metallicFactor\": 0.0, \"roughnessFactor\": 0.9 }");
            if (mat.chroma_keyed) {
                w.raw(", \"alphaMode\": \"MASK\", \"alphaCutoff\": 0.5");
            }
            w.raw(", \"doubleSided\": true }");
        }
        w.raw("\n  ],\n");
    }

    w.raw("  \"buffers\": [\n");
    w.raw("    { \"byteLength\": ");
    w.number(static_cast<unsigned long>(builder.size()));
    w.raw(", \"uri\": ");
    w.string(bin_filename);
    w.raw(" }\n");
    w.raw("  ]\n");
    w.raw("}\n");

    std::string gltf_filename = filename_base + ".gltf";
    std::filesystem::path gltf_path = out_dir / gltf_filename;
    {
        std::ofstream f(gltf_path);
        if (!f) throw std::runtime_error("gltf_emitter: cannot write " +
                                         gltf_path.string());
        f << w.str();
    }

    GltfEmitResult result;
    result.gltf_path = gltf_path;
    result.bin_path = bin_path;
    result.lod_count = lods.size();
    result.dof_count = static_cast<std::size_t>(rec->effective_dofs());
    result.switch_count = static_cast<std::size_t>(rec->effective_switches());
    result.slot_count = rec->slots.size();
    result.material_count = materials.size();
    result.channels = bound_channels;
    for (const auto& mat : materials) {
        if (mat.tex_id >= 0) ++result.texture_count;
    }
    for (const auto& p : part_meshes) {
        result.total_vertices += p.vertex_count;
        result.primitive_count += p.prim_count;
        if (p.mode == 4) result.total_triangles += p.prim_count;
    }
    return result;
}

} // namespace f4::import

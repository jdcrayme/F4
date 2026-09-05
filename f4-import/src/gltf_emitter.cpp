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
#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>
#include <f4/models/bsp_node.hpp>
#include <f4/models/geometry.hpp>
#include <f4/json/writer.hpp>

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
                Vec3f p = opts.convert_to_gltf_coords
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
                Vec3f n = opts.convert_to_gltf_coords
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
                    const Rgba4 c = resolve_color(v.color, db.color_bank(), mesh_is_textured);
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
            int mode = 4;
            switch (m.kind) {
                case f4::models::PrimitiveKind::Triangles:
                    mode = 4;
                    indices.reserve(m.triangles.size() * 3);
                    for (const auto& t : m.triangles) {
                        indices.push_back(t.v0);
                        indices.push_back(t.v1);
                        indices.push_back(t.v2);
                    }
                    break;
                case f4::models::PrimitiveKind::Lines:
                    mode = 1;
                    indices.reserve(m.lines.size() * 2);
                    for (const auto& l : m.lines) {
                        indices.push_back(l.v0);
                        indices.push_back(l.v1);
                    }
                    break;
                case f4::models::PrimitiveKind::Points:
                    mode = 0;
                    indices.reserve(m.points.size());
                    for (uint32_t i = 0; i < m.points.size(); ++i) {
                        indices.push_back(i);
                    }
                    break;
            }
            const std::size_t idx_acc = append_index_accessor(
                builder, buffer_views, accessors, indices);

            prims.push_back({lod.level, pos_acc, norm_acc, uv_acc, color_acc,
                             idx_acc, m.vertices.size(),
                             m.primitive_count(), mode, material_index_for(m)});
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

} // namespace f4::import

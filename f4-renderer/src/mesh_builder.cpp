// f4-renderer/src/mesh_builder.cpp
//
// Converts f4::models::ModelGeometry -> Raylib ::Mesh array.

#include <f4/renderer/mesh_builder.hpp>

#include <f4/models/geometry.hpp>
#include <f4/models/model_database.hpp>

#include <raylib.h>

#include <cstring>
#include <vector>

namespace f4::renderer {

// ── resolve_vertex_color ──────────────────────────────────────────────────────

Color resolve_vertex_color(uint32_t color_index,
                           const f4::models::ColorBank& color_bank,
                           bool mesh_is_textured) noexcept {
    if (color_index == 0) {
        return mesh_is_textured ? Color{255, 255, 255, 255}
                                : Color{180, 180, 180, 255};
    }
    if (color_index < 4096) {
        const int idx = static_cast<int>(color_index);
        const uint32_t rgba = color_bank.rgba_at(idx);
        if (rgba != 0) {
            return Color{
                static_cast<unsigned char>((rgba >> 24) & 0xFF),  // R
                static_cast<unsigned char>((rgba >> 16) & 0xFF),  // G
                static_cast<unsigned char>((rgba >> 8)  & 0xFF),  // B
                static_cast<unsigned char>(rgba & 0xFF)           // A
            };
        }
    }
    // Large value — assume packed RGBA (backward compat)
    return Color{
        static_cast<unsigned char>(color_index & 0xFF),         // R
        static_cast<unsigned char>((color_index >> 8) & 0xFF),  // G
        static_cast<unsigned char>((color_index >> 16) & 0xFF), // B
        static_cast<unsigned char>((color_index >> 24) & 0xFF)  // A
    };
}

// ── build_raylib_meshes ───────────────────────────────────────────────────────

std::vector<::Mesh> build_raylib_meshes(
    const f4::models::ModelGeometry& geom,
    const f4::models::ColorBank& color_bank,
    Float3 (*transform)(float, float, float))
{
    std::vector<::Mesh> result;
    result.reserve(geom.meshes.size());

    for (const auto& src_mesh : geom.meshes) {
        if (src_mesh.vertices.empty()) continue;
        if (src_mesh.kind == f4::models::PrimitiveKind::Triangles &&
            src_mesh.triangles.empty()) continue;

        const bool mesh_is_textured = (src_mesh.tex_id >= 0);
        const int vert_count = static_cast<int>(src_mesh.vertices.size());
        const int tri_count  = static_cast<int>(src_mesh.triangles.size());

        ::Mesh rm = {};
        rm.vertexCount   = vert_count;
        rm.triangleCount = tri_count;

        // Allocate with RL_MALLOC so UnloadMesh can RL_FREE
        rm.vertices  = static_cast<float*>(RL_MALLOC(vert_count * 3 * sizeof(float)));
        rm.normals   = static_cast<float*>(RL_MALLOC(vert_count * 3 * sizeof(float)));
        rm.texcoords = static_cast<float*>(RL_MALLOC(vert_count * 2 * sizeof(float)));
        rm.colors    = static_cast<unsigned char*>(RL_MALLOC(vert_count * 4 * sizeof(unsigned char)));

        if (tri_count > 0) {
            rm.indices = static_cast<unsigned short*>(RL_MALLOC(tri_count * 3 * sizeof(unsigned short)));
        }

        // Fill vertex attributes
        for (int i = 0; i < vert_count; ++i) {
            const auto& v = src_mesh.vertices[static_cast<std::size_t>(i)];

            const Float3 pos = transform(v.position.x, v.position.y, v.position.z);
            rm.vertices[i * 3 + 0] = pos.x;
            rm.vertices[i * 3 + 1] = pos.y;
            rm.vertices[i * 3 + 2] = pos.z;

            const Float3 nrm = transform(v.normal.x, v.normal.y, v.normal.z);
            rm.normals[i * 3 + 0] = nrm.x;
            rm.normals[i * 3 + 1] = nrm.y;
            rm.normals[i * 3 + 2] = nrm.z;

            rm.texcoords[i * 2 + 0] = v.uv.u;
            rm.texcoords[i * 2 + 1] = v.uv.v;

            const Color c = resolve_vertex_color(v.color, color_bank, mesh_is_textured);
            rm.colors[i * 4 + 0] = c.r;
            rm.colors[i * 4 + 1] = c.g;
            rm.colors[i * 4 + 2] = c.b;
            rm.colors[i * 4 + 3] = c.a;
        }

        // Fill triangle indices
        if (tri_count > 0) {
            for (int i = 0; i < tri_count; ++i) {
                const auto& tri = src_mesh.triangles[static_cast<std::size_t>(i)];
                rm.indices[i * 3 + 0] = static_cast<unsigned short>(tri.v0);
                rm.indices[i * 3 + 1] = static_cast<unsigned short>(tri.v1);
                rm.indices[i * 3 + 2] = static_cast<unsigned short>(tri.v2);
            }
        }

        UploadMesh(&rm, false);
        result.push_back(rm);
    }

    return result;
}

// ── build_mesh_entries ────────────────────────────────────────────────────────

std::vector<MeshEntry> build_mesh_entries(
    const f4::models::ModelGeometry& geom,
    const std::vector<::Mesh>& raylib_meshes)
{
    std::vector<MeshEntry> entries;
    entries.reserve(geom.meshes.size());
    for (std::size_t i = 0; i < geom.meshes.size(); ++i) {
        MeshEntry entry;
        if (i < raylib_meshes.size()) {
            entry.mesh = raylib_meshes[i];
        }
        entry.tex_id = geom.meshes[i].tex_id;
        entries.push_back(entry);
    }
    return entries;
}

// ── unload_meshes ─────────────────────────────────────────────────────────────

void unload_meshes(std::vector<::Mesh>& meshes) {
    for (auto& m : meshes) {
        UnloadMesh(m);
    }
    meshes.clear();
}

} // namespace f4::renderer

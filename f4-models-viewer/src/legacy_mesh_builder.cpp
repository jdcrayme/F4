// f4-models-viewer/src/legacy_mesh_builder.cpp
//
// Legacy KoreaObj geometry pipeline (see legacy_mesh_builder.hpp).
// The conversion + texture decode code mirrors what f4-renderer's
// mesh_builder / texture_cache provided before the Tranche 0d glTF
// rewire — kept here for the one tool that still reads KoreaObj binary
// by design.

#include "legacy_mesh_builder.hpp"

#include <f4/renderer/coord_transform.hpp>  // model_vertex_to_raylib
#include <f4/models/texture.hpp>

#include <raylib.h>

#include <cstring>
#include <vector>

namespace f4::models_viewer {

namespace {

// Resolve a ColorBank index to a Raylib Color (mirrors the former
// f4::renderer::resolve_vertex_color — Prim.rgba is an index into the
// ColorBank, not packed ABGR).
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

}  // namespace

// ── build_legacy_meshes ──────────────────────────────────────────────────────

std::vector<f4::renderer::MeshEntry> build_legacy_meshes(
    const f4::models::ModelGeometry& geom,
    const f4::models::ColorBank& color_bank)
{
    std::vector<f4::renderer::MeshEntry> result;
    result.reserve(geom.meshes.size());

    for (const auto& src_mesh : geom.meshes) {
        f4::renderer::MeshEntry entry;
        entry.tex_id = src_mesh.tex_id;

        if (src_mesh.vertices.empty() ||
            (src_mesh.kind == f4::models::PrimitiveKind::Triangles &&
             src_mesh.triangles.empty())) {
            result.push_back(entry);
            continue;
        }

        const bool mesh_is_textured = (src_mesh.tex_id >= 0);
        const int vert_count = static_cast<int>(src_mesh.vertices.size());
        const int tri_count  = static_cast<int>(src_mesh.triangles.size());

        ::Mesh rm = {};
        rm.vertexCount   = vert_count;
        rm.triangleCount = tri_count;

        rm.vertices  = static_cast<float*>(RL_MALLOC(vert_count * 3 * sizeof(float)));
        rm.normals   = static_cast<float*>(RL_MALLOC(vert_count * 3 * sizeof(float)));
        rm.texcoords = static_cast<float*>(RL_MALLOC(vert_count * 2 * sizeof(float)));
        rm.colors    = static_cast<unsigned char*>(RL_MALLOC(vert_count * 4));
        if (tri_count > 0) {
            rm.indices = static_cast<unsigned short*>(
                RL_MALLOC(static_cast<std::size_t>(tri_count) * 3 * sizeof(unsigned short)));
        }

        for (int i = 0; i < vert_count; ++i) {
            const auto& v = src_mesh.vertices[static_cast<std::size_t>(i)];
            const auto pos = f4::renderer::model_vertex_to_raylib(
                v.position.x, v.position.y, v.position.z);
            rm.vertices[i * 3 + 0] = pos.x;
            rm.vertices[i * 3 + 1] = pos.y;
            rm.vertices[i * 3 + 2] = pos.z;
            const auto nrm = f4::renderer::model_vertex_to_raylib(
                v.normal.x, v.normal.y, v.normal.z);
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

        if (tri_count > 0) {
            for (int i = 0; i < tri_count; ++i) {
                const auto& tri = src_mesh.triangles[static_cast<std::size_t>(i)];
                rm.indices[i * 3 + 0] = static_cast<unsigned short>(tri.v0);
                rm.indices[i * 3 + 1] = static_cast<unsigned short>(tri.v1);
                rm.indices[i * 3 + 2] = static_cast<unsigned short>(tri.v2);
            }
        }

        UploadMesh(&rm, false);
        entry.mesh = rm;
        result.push_back(std::move(entry));
    }

    return result;
}

// ── upload_legacy_textures ───────────────────────────────────────────────────

void upload_legacy_textures(f4::renderer::TextureCache& cache,
                            f4::models::ModelDatabase& db,
                            const std::vector<int>& tex_ids) {
    for (const int tex_id : tex_ids) {
        if (tex_id < 0) continue;
        if (cache.contains(tex_id)) continue;  // already cached

        const auto* decoded = db.fetch_texture(tex_id);
        if (!decoded || !decoded->valid()) {
            f4::renderer::TexCacheEntry ce;
            ce.uploaded = false;
            cache.insert(tex_id, ce);
            continue;
        }

        Image img = {};
        img.data = RL_MALLOC(static_cast<std::size_t>(decoded->width) *
                             static_cast<std::size_t>(decoded->height) * 4);
        if (!img.data) {
            f4::renderer::TexCacheEntry ce;
            ce.uploaded = false;
            cache.insert(tex_id, ce);
            continue;
        }
        std::memcpy(img.data, decoded->rgba.data(),
                    static_cast<std::size_t>(decoded->width) *
                    static_cast<std::size_t>(decoded->height) * 4);
        img.width = decoded->width;
        img.height = decoded->height;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

        Texture2D tex = LoadTextureFromImage(img);
        UnloadImage(img);

        Material mat = LoadMaterialDefault();
        mat.maps[MATERIAL_MAP_DIFFUSE].texture = tex;
        mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

        f4::renderer::TexCacheEntry ce;
        ce.texture = tex;
        ce.material = mat;
        ce.has_alpha = decoded->has_alpha;
        ce.uploaded = true;
        cache.insert(tex_id, ce);
    }
}

} // namespace f4::models_viewer

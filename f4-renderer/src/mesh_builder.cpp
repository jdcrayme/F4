// f4-renderer/src/mesh_builder.cpp
//
// glTF primitive → Raylib ::Mesh conversion (see mesh_builder.hpp).

#include <f4/renderer/mesh_builder.hpp>

#include <f4/gltf/gltf_loader.hpp>

#include <raylib.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace f4::renderer {

namespace {

// KoreaObj texture bank id from a glTF image URI. The exporter writes
// "textures/NNNNN.png" (one PNG per TEX entry); NNNNN is the bank index
// the TextureCache is keyed by. Returns -1 when the URI doesn't carry a
// numeric basename.
int tex_id_from_uri(const std::string& uri) {
    if (uri.empty()) return -1;
    const auto slash = uri.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? uri
                                                    : uri.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    if (base.empty()) return -1;
    for (const char c : base) {
        if (c < '0' || c > '9') return -1;
    }
    try {
        return std::stoi(base);
    } catch (const std::exception&) {
        return -1;
    }
}

// Read a tightly-packed VEC2 float accessor element (TEXCOORD_0).
std::optional<std::array<float, 2>> read_vec2_float(
    const f4::gltf::GltfDocument& doc, std::size_t accessor_index,
    std::size_t element_index) {
    if (accessor_index >= doc.accessors.size()) return std::nullopt;
    const auto& a = doc.accessors[accessor_index];
    if (a.type != "VEC2" || a.component_type != 5126) return std::nullopt;
    const uint8_t* base = doc.accessor_data(accessor_index);
    if (!base) return std::nullopt;
    if (element_index >= a.count) return std::nullopt;
    const float* p = reinterpret_cast<const float*>(base) + element_index * 2;
    return std::array<float, 2>{p[0], p[1]};
}

}  // namespace

// ── extract_gltf_lod_geometry ────────────────────────────────────────────────

std::vector<GltfMeshData> extract_gltf_lod_geometry(
    const f4::gltf::GltfDocument& doc, int lod_level)
{
    std::vector<GltfMeshData> out;

    // The emitter names meshes "LOD_<level>" (one glTF mesh per LOD).
    const std::string want = "LOD_" + std::to_string(lod_level);
    const f4::gltf::Mesh* mesh = nullptr;
    for (const auto& m : doc.meshes) {
        if (m.name == want) {
            mesh = &m;
            break;
        }
    }
    // Fallback: a document with a single un-prefixed mesh (hand-authored
    // test fixtures) serves as LOD 0.
    if (!mesh && lod_level == 0) {
        for (const auto& m : doc.meshes) {
            if (m.name.empty() || m.name.rfind("LOD_", 0) != 0) {
                mesh = &m;
                break;
            }
        }
    }
    if (!mesh) return out;

    for (const auto& prim : mesh->primitives) {
        // TRIANGLES only — the same filter the old build_raylib_meshes
        // applied to f4::models::PrimitiveKind (the emitter emits mode 4).
        if (prim.mode != 4) continue;
        if (!prim.positions || *prim.positions >= doc.accessors.size()) continue;
        const auto& pos_acc = doc.accessors[*prim.positions];
        if (pos_acc.count == 0) continue;

        GltfMeshData data;

        // Texture binding through the material chain.
        data.texture_uri = doc.material_basecolor_uri(prim.material)
                               .value_or(std::string{});
        if (!data.texture_uri.empty()) {
            data.tex_id = tex_id_from_uri(data.texture_uri);
        }

        // Positions (glTF meters/+Y-up → Raylib feet/RH Y-up).
        data.positions.reserve(pos_acc.count * 3);
        for (std::size_t i = 0; i < pos_acc.count; ++i) {
            auto v = doc.read_vec3_float(*prim.positions, i);
            if (!v) break;
            const auto p = gltf_vertex_to_raylib((*v)[0], (*v)[1], (*v)[2]);
            data.positions.push_back(p.x);
            data.positions.push_back(p.y);
            data.positions.push_back(p.z);
        }
        if (data.positions.size() != pos_acc.count * 3) {
            continue;  // accessor read failed mid-way — skip this primitive
        }
        const std::size_t vert_count = pos_acc.count;

        // Normals (directions — no scale, see gltf_normal_to_raylib).
        if (prim.normals && *prim.normals < doc.accessors.size()) {
            data.normals.reserve(vert_count * 3);
            for (std::size_t i = 0; i < vert_count; ++i) {
                auto n = doc.read_vec3_float(*prim.normals, i);
                if (!n) break;
                const auto t = gltf_normal_to_raylib((*n)[0], (*n)[1], (*n)[2]);
                data.normals.push_back(t.x);
                data.normals.push_back(t.y);
                data.normals.push_back(t.z);
            }
        }
        if (data.normals.size() != vert_count * 3) {
            // Missing/unreadable normals — zero-fill so the attribute
            // layout stays complete (draws flat-shaded dark, never
            // crashes).
            data.normals.assign(vert_count * 3, 0.0f);
        }

        // UVs.
        if (prim.texcoords0 && *prim.texcoords0 < doc.accessors.size()) {
            data.texcoords.reserve(vert_count * 2);
            for (std::size_t i = 0; i < vert_count; ++i) {
                auto uv = read_vec2_float(doc, *prim.texcoords0, i);
                if (!uv) break;
                data.texcoords.push_back((*uv)[0]);
                data.texcoords.push_back((*uv)[1]);
            }
        }

        // Vertex colors (COLOR_0 — the exporter bakes ColorBank
        // resolution at export time).
        if (prim.colors0 && *prim.colors0 < doc.accessors.size()) {
            data.colors.reserve(vert_count * 4);
            for (std::size_t i = 0; i < vert_count; ++i) {
                auto c = doc.read_color_rgba(*prim.colors0, i);
                if (!c) break;
                auto to_u8 = [](float v) {
                    const float clamped = std::clamp(v, 0.0f, 1.0f) * 255.0f;
                    return static_cast<unsigned char>(clamped + 0.5f);
                };
                data.colors.push_back(to_u8((*c)[0]));
                data.colors.push_back(to_u8((*c)[1]));
                data.colors.push_back(to_u8((*c)[2]));
                data.colors.push_back(to_u8((*c)[3]));
            }
        }

        // Indices.
        if (prim.indices && *prim.indices < doc.accessors.size()) {
            const auto& idx_acc = doc.accessors[*prim.indices];
            data.indices.reserve(idx_acc.count);
            for (std::size_t i = 0; i < idx_acc.count; ++i) {
                auto idx = doc.read_index_u32(*prim.indices, i);
                if (!idx) break;
                data.indices.push_back(static_cast<unsigned short>(*idx));
            }
        }
        if (data.indices.size() < 3) {
            // Non-indexed or unreadable indices — sequential (same
            // triangle fan every 3 vertices).
            data.indices.resize(vert_count);
            for (std::size_t i = 0; i < vert_count; ++i) {
                data.indices[i] = static_cast<unsigned short>(i);
            }
        }

        out.push_back(std::move(data));
    }

    return out;
}

// ── build_gltf_mesh ──────────────────────────────────────────────────────────

::Mesh build_gltf_mesh(const GltfMeshData& data) {
    ::Mesh rm = {};
    if (data.positions.empty()) return rm;

    const int vert_count = static_cast<int>(data.positions.size() / 3);
    const int tri_count = static_cast<int>(data.indices.size() / 3);

    rm.vertexCount = vert_count;
    rm.triangleCount = tri_count;

    // Allocate with RL_MALLOC so UnloadMesh can RL_FREE.
    rm.vertices = static_cast<float*>(RL_MALLOC(data.positions.size() * sizeof(float)));
    std::memcpy(rm.vertices, data.positions.data(),
                data.positions.size() * sizeof(float));

    rm.normals = static_cast<float*>(RL_MALLOC(data.normals.size() * sizeof(float)));
    if (!data.normals.empty()) {
        std::memcpy(rm.normals, data.normals.data(),
                    data.normals.size() * sizeof(float));
    }

    rm.texcoords = static_cast<float*>(RL_MALLOC(vert_count * 2 * sizeof(float)));
    if (!data.texcoords.empty()) {
        std::memcpy(rm.texcoords, data.texcoords.data(),
                    data.texcoords.size() * sizeof(float));
    } else {
        std::memset(rm.texcoords, 0, vert_count * 2 * sizeof(float));
    }

    // Vertex colors: COLOR_0 when exported, else opaque white (the lit
    // shader's chroma-key discard would hide white-less meshes).
    rm.colors = static_cast<unsigned char*>(RL_MALLOC(vert_count * 4));
    if (data.colors.size() == static_cast<std::size_t>(vert_count) * 4) {
        std::memcpy(rm.colors, data.colors.data(), data.colors.size());
    } else {
        for (int i = 0; i < vert_count; ++i) {
            rm.colors[i * 4 + 0] = 255;
            rm.colors[i * 4 + 1] = 255;
            rm.colors[i * 4 + 2] = 255;
            rm.colors[i * 4 + 3] = 255;
        }
    }

    if (tri_count > 0) {
        rm.indices = static_cast<unsigned short*>(
            RL_MALLOC(static_cast<std::size_t>(tri_count) * 3 * sizeof(unsigned short)));
        std::memcpy(rm.indices, data.indices.data(),
                    data.indices.size() * sizeof(unsigned short));
    }

    UploadMesh(&rm, false);
    return rm;
}

// ── unload_meshes ────────────────────────────────────────────────────────────

void unload_meshes(std::vector<::Mesh>& meshes) {
    for (auto& m : meshes) {
        UnloadMesh(m);
    }
    meshes.clear();
}

} // namespace f4::renderer

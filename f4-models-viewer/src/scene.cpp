// f4-models-viewer/src/scene.cpp
//
// Converts f4::models::ModelGeometry → Raylib ::Mesh array.
// For each f4::models::Mesh:
//   - Create a Raylib ::Mesh with vertexCount and triangleCount
//   - Fill vertex positions (applying LH Y-up → RH Y-up conversion)
//   - Fill normals (same conversion)
//   - Fill texcoords if present
//   - Fill colors by resolving ColorBank indices (Prim.rgba is an INT INDEX
//     into the ColorBank, NOT packed ABGR — see f4-models/include/f4/models/
//     model_database.hpp). Falls back to a light gray for textured meshes
//     (their color comes from the texture, not the ColorBank).
//   - Call UploadMesh(&mesh, true) to upload to GPU

#include "viewer_state.hpp"
#include "scene.hpp"

#include <f4/models/geometry.hpp>
#include <f4/models/model_database.hpp>

#include <raylib.h>

#include <cstring>
#include <vector>

namespace f4::models_viewer {

// ── ColorBank index → Raylib Color ─────────────────────────────────────────
//
// f4::models::Vertex::color stores `Prim.rgba`, which in FreeFalcon is an
// `int rgba` INDEX into the ColorBank (see graphics/include/polylib.h and
// ColorBankClass::GetColorEntry). The previous implementation treated this
// as packed ABGR — for index 873 that gave RGBA=(105,3,0,0), which is fully
// transparent and therefore invisible.
//
// We resolve through the ColorBank and fall back to a neutral grey so
// textured meshes (whose color comes from the texture, not the index) and
// uncolored meshes still render visibly.

static Color resolve_vertex_color(uint32_t color_index,
                                  const f4::models::ColorBank& color_bank,
                                  bool mesh_is_textured) noexcept {
    // color_index is a uint32_t but the original field is int32_t; treat
    // very large values as "no color" rather than as huge indices.
    if (color_index == 0) {
        // No color — pick a visible default depending on whether the mesh
        // has a texture. Textured meshes will get their color from the
        // texture, so a neutral white tint is correct. Untextured meshes
        // have no color source at all, so use a mid-grey so they show up
        // against the dark background.
        return mesh_is_textured ? Color{255, 255, 255, 255}
                                : Color{180, 180, 180, 255};
    }
    if (color_index < 4096) {
        // Almost certainly a ColorBank index. Resolve through the bank.
        const int idx = static_cast<int>(color_index);
        const uint32_t rgba = color_bank.rgba_at(idx);
        if (rgba != 0) {
            // rgba is packed 0xRRGGBBAA (per ColorBank::rgba_at).
            return Color{
                static_cast<unsigned char>((rgba >> 24) & 0xFF),  // R
                static_cast<unsigned char>((rgba >> 16) & 0xFF),  // G
                static_cast<unsigned char>((rgba >> 8)  & 0xFF),  // B
                static_cast<unsigned char>(rgba & 0xFF)           // A
            };
        }
        // Index out of range — fall through to fallback.
    }
    // Large value — assume the caller really did pack RGBA. Keep the old
    // unpack path for backward compatibility (rare in practice).
    return Color{
        static_cast<unsigned char>(color_index & 0xFF),         // R
        static_cast<unsigned char>((color_index >> 8) & 0xFF),  // G
        static_cast<unsigned char>((color_index >> 16) & 0xFF), // B
        static_cast<unsigned char>((color_index >> 24) & 0xFF)  // A
    };
}

// ── build_raylib_meshes ────────────────────────────────────────────────────
std::vector<::Mesh> build_raylib_meshes(
    const f4::models::ModelGeometry& geom,
    const f4::models::ColorBank& color_bank)
{
    std::vector<::Mesh> result;
    result.reserve(geom.meshes.size());

    for (const auto& src_mesh : geom.meshes) {
        // Skip meshes with no vertex data. (A triangle-only mesh with 0
        // triangles but >0 vertices would be unusual but not invalid; we
        // skip it to avoid an empty UploadMesh.)
        if (src_mesh.vertices.empty()) continue;
        if (src_mesh.kind == f4::models::PrimitiveKind::Triangles &&
            src_mesh.triangles.empty()) continue;

        const bool mesh_is_textured = (src_mesh.tex_id >= 0);

        const int vert_count = static_cast<int>(src_mesh.vertices.size());
        const int tri_count  = static_cast<int>(src_mesh.triangles.size());

        ::Mesh rm = {};
        rm.vertexCount   = vert_count;
        rm.triangleCount = tri_count;

        // Allocate Raylib mesh arrays. Raylib expects these as float*
        // (3 floats per vertex for positions/normals, 2 for texcoords).
        rm.vertices = new float[vert_count * 3];
        rm.normals  = new float[vert_count * 3];
        rm.texcoords = new float[vert_count * 2];
        rm.colors   = new unsigned char[vert_count * 4];

        if (tri_count > 0) {
            // Triangle list (indices: 3 per triangle).
            rm.indices = new unsigned short[tri_count * 3];
        }

        // Fill vertex attributes
        for (int i = 0; i < vert_count; ++i) {
            const auto& v = src_mesh.vertices[static_cast<std::size_t>(i)];

            // Position: LH Y-up → RH Y-up
            const Vector3 pos = to_raylib(v.position.x, v.position.y, v.position.z);
            rm.vertices[i * 3 + 0] = pos.x;
            rm.vertices[i * 3 + 1] = pos.y;
            rm.vertices[i * 3 + 2] = pos.z;

            // Normal: same conversion
            const Vector3 nrm = to_raylib(v.normal.x, v.normal.y, v.normal.z);
            rm.normals[i * 3 + 0] = nrm.x;
            rm.normals[i * 3 + 1] = nrm.y;
            rm.normals[i * 3 + 2] = nrm.z;

            // Texcoords (default to 0,0 — Raylib wants valid floats here)
            rm.texcoords[i * 2 + 0] = v.uv.u;
            rm.texcoords[i * 2 + 1] = v.uv.v;

            // Color: resolve through ColorBank
            const Color c = resolve_vertex_color(v.color, color_bank, mesh_is_textured);
            rm.colors[i * 4 + 0] = c.r;
            rm.colors[i * 4 + 1] = c.g;
            rm.colors[i * 4 + 2] = c.b;
            rm.colors[i * 4 + 3] = c.a;
        }

        // Fill indices for triangles
        if (tri_count > 0) {
            for (int i = 0; i < tri_count; ++i) {
                const auto& tri = src_mesh.triangles[static_cast<std::size_t>(i)];
                rm.indices[i * 3 + 0] = static_cast<unsigned short>(tri.v0);
                rm.indices[i * 3 + 1] = static_cast<unsigned short>(tri.v1);
                rm.indices[i * 3 + 2] = static_cast<unsigned short>(tri.v2);
            }
        }

        // Upload to GPU (dynamic = false since we don't update per-frame)
        UploadMesh(&rm, false);

        result.push_back(rm);
    }

    return result;
}

// ── unload_meshes ──────────────────────────────────────────────────────────
void unload_meshes(std::vector<::Mesh>& meshes) {
    for (auto& m : meshes) {
        UnloadMesh(m);
    }
    meshes.clear();
}

// ── Impl wrappers ─────────────────────────────────────────────────────────
// These are called from the main loop via Impl.

void ViewerApp::Impl::rebuild_meshes() {
    // Free old meshes
    f4::models_viewer::unload_meshes(raylib_meshes);
    line_segs.clear();
    point_marks.clear();
    total_tri_count = 0;

    if (!doc_loaded || selected_parent < 0) {
        meshes_dirty = false;
        return;
    }

    // Parse the selected LOD if not already parsed
    const auto* rec = db.model(selected_parent);
    if (!rec) {
        meshes_dirty = false;
        return;
    }

    // Ensure the selected LOD is parsed
    if (selected_lod >= 0 && selected_lod < static_cast<int>(rec->lods.size())) {
        std::string err = db.parse_lod(selected_parent, selected_lod);
        if (!err.empty()) {
            status_msg = "Parse error: " + err;
            meshes_dirty = false;
            return;
        }
    }

    // Update DOF ranges from the BSP tree (if parsed)
    // The BSP tree nodes contain actual min/max for BXDofNode/BTransNode,
    // which are more accurate than the hardcoded [0, 2π] defaults.
    const auto* bsp = db.bsp_tree(selected_parent, selected_lod);
    if (bsp) {
        for (const auto& node : bsp->nodes) {
            if (node.type == f4::models::BspNodeType::BXDofNode ||
                node.type == f4::models::BspNodeType::BTransNode ||
                node.type == f4::models::BspNodeType::BScaleNode) {
                // Find the matching DofState and update its range
                for (auto& ds : model_state.dofs) {
                    if (ds.dof_number == node.dof_number) {
                        if (node.dof_min != 0 || node.dof_max != 0) {
                            ds.min = node.dof_min;
                            ds.max = node.dof_max;
                        }
                        break;
                    }
                }
            }
        }
    }

    // Extract geometry with current model state
    auto geom = db.extract_model_geometry(selected_parent, selected_lod, model_state);

    if (geom.meshes.empty()) {
        status_msg = "No geometry extracted for model " +
                     std::to_string(selected_parent) + " LOD " +
                     std::to_string(selected_lod);
        meshes_dirty = false;
        return;
    }

    // Convert to Raylib meshes, resolving ColorBank indices to RGBA.
    raylib_meshes = build_raylib_meshes(geom, db.color_bank());

    // Collect lines and points into separate lists for canvas drawing.
    // Raylib's ::Mesh / DrawMesh path only handles triangle lists; lines
    // and points need DrawLine3D / DrawCube.
    for (const auto& m : geom.meshes) {
        if (m.kind == f4::models::PrimitiveKind::Lines) {
            for (const auto& ln : m.lines) {
                if (ln.v0 >= m.vertices.size() || ln.v1 >= m.vertices.size()) continue;
                const auto& va = m.vertices[ln.v0];
                const auto& vb = m.vertices[ln.v1];
                LineSeg seg;
                seg.a = to_raylib(va.position.x, va.position.y, va.position.z);
                seg.b = to_raylib(vb.position.x, vb.position.y, vb.position.z);
                // Use the first vertex's color as the line color. The
                // mesh_is_textured flag isn't really meaningful for LineF
                // (lines have no texture), so always resolve through CB.
                const uint32_t ci = va.color;
                if (ci != 0 && ci < 4096) {
                    const uint32_t rgba = db.color_bank().rgba_at(static_cast<int>(ci));
                    if (rgba != 0) {
                        seg.color = Color{
                            static_cast<unsigned char>((rgba >> 24) & 0xFF),
                            static_cast<unsigned char>((rgba >> 16) & 0xFF),
                            static_cast<unsigned char>((rgba >> 8) & 0xFF),
                            static_cast<unsigned char>(rgba & 0xFF)
                        };
                    } else {
                        seg.color = Color{200, 200, 200, 255};
                    }
                } else {
                    seg.color = Color{200, 200, 200, 255};
                }
                line_segs.push_back(seg);
            }
        } else if (m.kind == f4::models::PrimitiveKind::Points) {
            for (const auto& pt : m.points) {
                if (pt.v0 >= m.vertices.size()) continue;
                const auto& va = m.vertices[pt.v0];
                PointMark pm;
                pm.p = to_raylib(va.position.x, va.position.y, va.position.z);
                pm.size = 1.0f;
                const uint32_t ci = va.color;
                if (ci != 0 && ci < 4096) {
                    const uint32_t rgba = db.color_bank().rgba_at(static_cast<int>(ci));
                    if (rgba != 0) {
                        pm.color = Color{
                            static_cast<unsigned char>((rgba >> 24) & 0xFF),
                            static_cast<unsigned char>((rgba >> 16) & 0xFF),
                            static_cast<unsigned char>((rgba >> 8) & 0xFF),
                            static_cast<unsigned char>(rgba & 0xFF)
                        };
                    } else {
                        pm.color = Color{255, 255, 100, 255};
                    }
                } else {
                    pm.color = Color{255, 255, 100, 255};
                }
                point_marks.push_back(pm);
            }
        } else {
            total_tri_count += m.triangles.size();
        }
    }

    meshes_dirty = false;
    status_msg = "Loaded model " + std::to_string(selected_parent) +
                 " LOD " + std::to_string(selected_lod) +
                 " — " + std::to_string(total_tri_count) + " triangles" +
                 " + " + std::to_string(line_segs.size()) + " lines" +
                 " + " + std::to_string(point_marks.size()) + " points";
}

void ViewerApp::Impl::unload_meshes() {
    f4::models_viewer::unload_meshes(raylib_meshes);
    line_segs.clear();
    point_marks.clear();
    total_tri_count = 0;
}

} // namespace f4::models_viewer

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
//
// Texture upload:
//   - After meshes are built, each MeshEntry has a tex_id.
//   - rebuild_meshes() calls texture_cache.upload(db, tex_ids), which
//     lazily decodes TEX blobs via ModelDatabase::fetch_texture and
//     uploads the RGBA8 data to GPU as Texture2D + Material.
//   - canvas3d.cpp uses the per-mesh material for DrawMesh.

#include "viewer_state.hpp"

#include <f4/models/geometry.hpp>
#include <f4/models/model_database.hpp>
#include <f4/models/texture.hpp>

#include <f4/renderer/mesh_builder.hpp>
#include <f4/renderer/texture_cache.hpp>

#include <raylib.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace f4::models_viewer {

// ── resolve_vertex_color / build_raylib_meshes / unload_meshes ─────────────
// Now provided by f4::renderer — see f4/renderer/mesh_builder.hpp.
// The local implementations have been removed; this file delegates to
// f4::renderer::build_raylib_meshes(), f4::renderer::build_mesh_entries(),
// f4::renderer::unload_meshes(), and f4::renderer::resolve_vertex_color().

// ── sync_model_state_with_bsp_tree ─────────────────────────────────────────
// Walks the BSP tree and reconciles model_state.dofs / model_state.switches
// with what's actually present in the tree.
//
// Background: the HDR record stores n_dof/n_switch counts (uint8) plus
// extended n_dofs/n_switches counts (int16). The viewer previously created
// `effective_dofs()` DofState entries (dof_number = 0..N-1) and
// `effective_switches()` SwitchState entries (n_children hardcoded to 2).
//
// Problems with the previous approach:
//   1. Not every dof_number in 0..N-1 is actually used in the BSP tree.
//      E.g. model 829 (helicopter) declares effective_dofs=5 but the tree
//      only has BDofNodes with dof_number 2, 3, 4. The sliders for DOF 0
//      and DOF 1 do nothing.
//   2. Not every switch_number in 0..N-1 is used either. Model 1052 (F-16)
//      declares effective_switches=20 but only 7 switches exist (sw#0,1,4,
//      10,11,12,14). 13 of the 20 switch combos do nothing.
//   3. n_children was hardcoded to 2. Model 829's switches have actual
//      n_children of {2, 1, 1}, so switches 1 and 2 showed a "Child 1"
//      option that didn't exist. Model 1052's sw#10 has 12 children, but
//      only "Child 0" and "Child 1" were visible — 10 loadouts were
//      unreachable.
//
// This function:
//   - Collects the actual dof_numbers and switch_numbers from the tree.
//   - Removes DofState/SwitchState entries for numbers not in the tree.
//   - Adds entries for numbers that ARE in the tree but not in model_state
//     (preserving existing values for entries that remain).
//   - Updates SwitchState::n_children from the tree (max across all
//     instances of the same switch_number).
//   - Clamps SwitchState::active_child to be valid (or -1 = show all).
//   - Also synchronizes the `animations` vector to match the filtered
//     DOF list.
static void sync_model_state_with_bsp_tree(
    const f4::models::BspTree* bsp,
    f4::models::ModelState& model_state,
    std::vector<ViewerApp::Impl::AnimationTrack>& animations)
{
    if (!bsp) return;

    // Collect actual dof_numbers and switch_numbers from the tree.
    // Filter out garbage values (dof_number/switch_number >= 1000 are
    // almost certainly float bit patterns from corrupted walk off-track).
    std::set<int> tree_dof_numbers;
    std::map<int, int> tree_switch_max_children;  // switch_number -> max n_children

    for (const auto& node : bsp->nodes) {
        if (node.type == f4::models::BspNodeType::BDofNode ||
            node.type == f4::models::BspNodeType::BXDofNode ||
            node.type == f4::models::BspNodeType::BTransNode ||
            node.type == f4::models::BspNodeType::BScaleNode) {
            if (node.dof_number >= 0 && node.dof_number < 1000) {
                tree_dof_numbers.insert(node.dof_number);
            }
        }
        if (node.type == f4::models::BspNodeType::BSwitchNode ||
            node.type == f4::models::BspNodeType::BXSwitchNode) {
            if (node.switch_number >= 0 && node.switch_number < 1000) {
                auto& mx = tree_switch_max_children[node.switch_number];
                mx = std::max(mx, node.n_children);
            }
        }
    }

    // ── Synchronize model_state.dofs ──
    // Preserve existing values/limits for DOFs that remain, add new DOFs
    // with sensible defaults, remove DOFs not in the tree.
    std::vector<f4::models::DofState> new_dofs;
    new_dofs.reserve(tree_dof_numbers.size());
    for (int dn : tree_dof_numbers) {
        // Look for an existing DofState with this dof_number
        const f4::models::DofState* existing = nullptr;
        for (const auto& ds : model_state.dofs) {
            if (ds.dof_number == dn) { existing = &ds; break; }
        }
        if (existing) {
            new_dofs.push_back(*existing);
        } else {
            f4::models::DofState ds;
            ds.dof_number = dn;
            ds.value = 0;
            ds.min = 0;
            ds.max = 6.28318530718f;  // 2π default for rotational DOFs
            new_dofs.push_back(ds);
        }
    }
    model_state.dofs = std::move(new_dofs);

    // ── Update DOF ranges from BXDofNode/BTransNode/BScaleNode ──
    // These extended DOF node types carry actual min/max in their on-disk
    // struct. Plain BDofNode does not — its slider stays at [0, 2π].
    for (const auto& node : bsp->nodes) {
        if (node.type == f4::models::BspNodeType::BXDofNode ||
            node.type == f4::models::BspNodeType::BTransNode ||
            node.type == f4::models::BspNodeType::BScaleNode) {
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

    // ── Synchronize model_state.switches ──
    std::vector<f4::models::SwitchState> new_switches;
    new_switches.reserve(tree_switch_max_children.size());
    for (const auto& [sw_num, n_children] : tree_switch_max_children) {
        // Look for an existing SwitchState with this switch_number
        const f4::models::SwitchState* existing = nullptr;
        for (const auto& ss : model_state.switches) {
            if (ss.switch_number == sw_num) { existing = &ss; break; }
        }
        f4::models::SwitchState ss;
        if (existing) {
            ss = *existing;
        } else {
            ss.switch_number = sw_num;
            // Default to "Show All" (active_child = -1).
            //
            // FreeFalcon's BSwitchNode is a bitmask selector — each child
            // has a bit, and any combination of children can be visible
            // simultaneously. A model viewer should show ALL geometry by
            // default so the user can see the complete model. The user
            // can then switch to a specific child via the combo box if
            // they want to isolate one.
            //
            // Concrete example: model 1052 (F-16) sw#10 has 12 children
            // (one per pylon store). With active_child=0, only 446
            // triangles are visible. With "Show All", 941 triangles are
            // visible — the difference is the 11 hidden pylon stores.
            ss.active_child = -1;  // "Show All"
        }
        ss.n_children = n_children;
        // Clamp active_child:
        //   - If active_child is -2 ("None") or -1 ("Show All"), keep it.
        //   - If active_child >= n_children, reset to "Show All" (-1).
        //   - active_child < -2 shouldn't happen; treat as "Show All".
        if (ss.active_child != -1 && ss.active_child != -2 &&
            ss.active_child >= n_children) {
            ss.active_child = -1;
        }
        if (ss.active_child < -2) {
            ss.active_child = -1;
        }
        new_switches.push_back(ss);
    }
    model_state.switches = std::move(new_switches);

    // ── Synchronize animations to match the filtered DOF list ──
    // Preserve existing track settings for DOFs that remain, add default
    // tracks for new DOFs, remove tracks for DOFs no longer present.
    std::vector<ViewerApp::Impl::AnimationTrack> new_animations;
    new_animations.reserve(model_state.dofs.size());
    for (const auto& ds : model_state.dofs) {
        const ViewerApp::Impl::AnimationTrack* existing = nullptr;
        for (const auto& t : animations) {
            if (t.dof_number == ds.dof_number) { existing = &t; break; }
        }
        if (existing) {
            new_animations.push_back(*existing);
        } else {
            ViewerApp::Impl::AnimationTrack t;
            t.dof_number = ds.dof_number;
            t.enabled = false;
            t.speed = 1.0f;
            t.phase = 0.0f;
            t.wrap_2pi = (ds.max - ds.min > 1.0f);
            new_animations.push_back(t);
        }
    }
    animations = std::move(new_animations);
}

// ── resolve_color_index ─────────────────────────────────────────────────────
// Resolve a vertex color index (Prim.rgba) to a Raylib Color via the
// ColorBank. Falls back to `fallback` if the index is out of range or
// the ColorBank entry is empty. Used for LineF/PointF primitives which
// can't go through the triangle-mesh build_raylib_meshes() path.
static Color resolve_color_index(uint32_t ci,
                                  const f4::models::ColorBank& cb,
                                  Color fallback) noexcept
{
    if (ci == 0 || ci >= 4096) return fallback;
    const uint32_t rgba = cb.rgba_at(static_cast<int>(ci));
    if (rgba == 0) return fallback;
    return Color{
        static_cast<unsigned char>((rgba >> 24) & 0xFF),
        static_cast<unsigned char>((rgba >> 16) & 0xFF),
        static_cast<unsigned char>((rgba >> 8) & 0xFF),
        static_cast<unsigned char>(rgba & 0xFF)
    };
}

// ── Impl wrappers ─────────────────────────────────────────────────────────
// These are called from the main loop via Impl.

void ViewerApp::Impl::rebuild_meshes() {
    // Free old meshes and textures
    f4::renderer::unload_meshes(raylib_meshes);
    mesh_entries.clear();
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

    // Synchronize model_state with the actual BSP tree contents.
    // This removes DOF/switch sliders that don't correspond to any real
    // node in the tree, adds sliders for DOFs/switches that ARE in the
    // tree but weren't in the initial effective_dofs()/effective_switches()
    // range, and sets SwitchState::n_children from the tree.
    const auto* bsp = db.bsp_tree(selected_parent, selected_lod);
    sync_model_state_with_bsp_tree(bsp, model_state, animations);

    // Extract geometry with current model state.
    //
    // Pass the selected texture set + the model's n_texture_sets so the
    // geometry extractor can apply the FreeFalcon textureSetOffset
    // (Phase T9). For single-set models this is a no-op; for multi-set
    // models (summer/winter/desert) it selects the correct third of
    // the BRoot's tex_ids pool.
    model_state.texture_set   = selected_texture_set;
    model_state.n_texture_sets = rec ? std::max(1, static_cast<int>(rec->n_texture_sets)) : 1;
    auto geom = db.extract_model_geometry(selected_parent, selected_lod, model_state);

    if (geom.meshes.empty()) {
        status_msg = "No geometry extracted for model " +
                     std::to_string(selected_parent) + " LOD " +
                     std::to_string(selected_lod);
        meshes_dirty = false;
        return;
    }

    // Convert to Raylib meshes, resolving ColorBank indices to RGBA.
    raylib_meshes = f4::renderer::build_raylib_meshes(geom, db.color_bank());

    // Build mesh_entries with tex_id for per-mesh material lookup
    mesh_entries = f4::renderer::build_mesh_entries(geom, raylib_meshes);

    // Collect lines and points into separate lists for canvas drawing.
    // Raylib's ::Mesh / DrawMesh path only handles triangle lists; lines
    // and points need DrawLine3D / DrawCube. Color resolution goes through
    // the shared resolve_color_index() helper above.
    static constexpr Color LINE_FALLBACK = {200, 200, 200, 255};
    static constexpr Color POINT_FALLBACK = {255, 255, 100, 255};
    for (const auto& m : geom.meshes) {
        if (m.kind == f4::models::PrimitiveKind::Lines) {
            for (const auto& ln : m.lines) {
                if (ln.v0 >= m.vertices.size() || ln.v1 >= m.vertices.size()) continue;
                const auto& va = m.vertices[ln.v0];
                const auto& vb = m.vertices[ln.v1];
                LineSeg seg;
                seg.a = to_raylib(va.position.x, va.position.y, va.position.z);
                seg.b = to_raylib(vb.position.x, vb.position.y, vb.position.z);
                seg.color = resolve_color_index(va.color, db.color_bank(), LINE_FALLBACK);
                line_segs.push_back(seg);
            }
        } else if (m.kind == f4::models::PrimitiveKind::Points) {
            for (const auto& pt : m.points) {
                if (pt.v0 >= m.vertices.size()) continue;
                const auto& va = m.vertices[pt.v0];
                PointMark pm;
                pm.p = to_raylib(va.position.x, va.position.y, va.position.z);
                pm.size = 1.0f;
                pm.color = resolve_color_index(va.color, db.color_bank(), POINT_FALLBACK);
                point_marks.push_back(pm);
            }
        } else {
            total_tri_count += m.triangles.size();
        }
    }

    meshes_dirty = false;

    // Upload textures for the new meshes
    // Collect tex_ids for the TextureCache::upload() call
    std::vector<int> tex_ids;
    tex_ids.reserve(mesh_entries.size());
    for (const auto& me : mesh_entries) {
        if (me.tex_id >= 0) tex_ids.push_back(me.tex_id);
    }
    texture_cache.upload(db, tex_ids);

    // Count textured meshes for status
    int n_textured = 0;
    for (const auto& me : mesh_entries) {
        if (me.tex_id >= 0) ++n_textured;
    }

    status_msg = "Loaded model " + std::to_string(selected_parent) +
                 " LOD " + std::to_string(selected_lod) +
                 " — " + std::to_string(total_tri_count) + " triangles" +
                 " + " + std::to_string(line_segs.size()) + " lines" +
                 " + " + std::to_string(point_marks.size()) + " points" +
                 " | " + std::to_string(n_textured) + " textured meshes";
}

void ViewerApp::Impl::unload_meshes() {
    texture_cache.unload_all();
    f4::renderer::unload_meshes(raylib_meshes);
    mesh_entries.clear();
    line_segs.clear();
    point_marks.clear();
    total_tri_count = 0;
}

} // namespace f4::models_viewer

// f4-models/src/geometry_extractor.cpp
//
// BSP tree traversal → renderable geometry.
//
// Strategy: Walk the tree depth-first. At each node:
//   BRoot/BSubTree       → push this subtree's coord/normals/tex_ids pool
//                          onto the active-pool stack, recurse, pop.
//   BSlotNode            → recurse into subtree (slot marker)
//   BDofNode/BXDofNode   → compute DOF rotation transform, push onto
//                          transform stack, recurse, pop.
//                          Transform = T(dof_translation) * dof_rotation * Rz(value)
//   BTransNode           → compute DOF translation transform, push onto
//                          transform stack, recurse, pop.
//   BScaleNode           → compute scale transform, push onto transform
//                          stack, recurse, pop.
//   BSwitchNode/BXSwitchNode → select one child, recurse
//   BSplitterNode        → recurse both front and back (no frustum culling yet)
//   BPrimitiveNode       → decode Prim and emit triangles/lines/points,
//                          resolving vertex positions through the TOP of
//                          the active-pool stack (NOT the global tree.coords),
//                          then applying the accumulated transform stack.
//   BCulledPrimitiveNode → same as BPrimitiveNode
//   BLitPrimitiveNode    → decode front + back Poly
//   BSpecialXform        → push subtree pool, recurse, pop
//   BRenderControlNode   → skip (render state, no geometry)
//   BLightStringNode     → decode prim (light marker)
//
// CRITICAL: Per-subtree coord pool binding. Each BSubTree-derived node
// (BRoot, BDofNode, BSwitchNode children, BSpecialXform, etc.) carries
// its own pCoords / pNormals / pTexIDs pools in the on-disk layout.
// Primitives in those subtrees index into the SUBTREE's pool, NOT the
// root's pool. The previous implementation always used tree.coords, which
// produced garbage positions for any primitive not in the root subtree.
//
// CRITICAL: DOF transform application. BDofNode/BXDofNode define a
// coordinate frame (dof_rotation + dof_translation) for their subtree.
// The DOF value rotates the subtree around the local Z axis. Without
// applying this transform, rotors appear at the origin, control surfaces
// are misplaced, and DOF sliders have no visual effect.

#include "geometry_extractor.hpp"
#include "poly_parser.hpp"

#include <cmath>
#include <cstring>

namespace f4::models::detail {

namespace {

// AffineTransform is defined in poly_parser.hpp (shared between
// poly_parser.cpp and geometry_extractor.cpp). The anonymous-namespace
// duplicate was removed to fix C2027/C2664 type-mismatch errors.

/// Build a rotation-about-Z matrix for a given angle in radians.
/// Rz(θ) = [cos θ  -sin θ  0]
///         [sin θ   cos θ  0]
///         [  0       0    1]
AffineTransform make_rotation_z(float angle_rad) {
    AffineTransform t;
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    t.rotation.m[0][0] = c;  t.rotation.m[0][1] = -s; t.rotation.m[0][2] = 0;
    t.rotation.m[1][0] = s;  t.rotation.m[1][1] = c;  t.rotation.m[1][2] = 0;
    t.rotation.m[2][0] = 0;  t.rotation.m[2][1] = 0;  t.rotation.m[2][2] = 1;
    return t;
}

/// Build a rotation-about-X matrix for a given angle in radians.
/// Rx(θ) = [1    0       0   ]
///         [0  cos θ  -sin θ ]
///         [0  sin θ   cos θ ]
AffineTransform make_rotation_x(float angle_rad) {
    AffineTransform t;
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    t.rotation.m[0][0] = 1; t.rotation.m[0][1] = 0;  t.rotation.m[0][2] = 0;
    t.rotation.m[1][0] = 0; t.rotation.m[1][1] = c;  t.rotation.m[1][2] = -s;
    t.rotation.m[2][0] = 0; t.rotation.m[2][1] = s;  t.rotation.m[2][2] = c;
    return t;
}

/// Build a rotation-about-Y matrix for a given angle in radians.
/// Ry(θ) = [ cos θ  0  sin θ]
///         [   0    1    0  ]
///         [-sin θ  0  cos θ]
AffineTransform make_rotation_y(float angle_rad) {
    AffineTransform t;
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    t.rotation.m[0][0] = c;  t.rotation.m[0][1] = 0; t.rotation.m[0][2] = s;
    t.rotation.m[1][0] = 0;  t.rotation.m[1][1] = 1; t.rotation.m[1][2] = 0;
    t.rotation.m[2][0] = -s; t.rotation.m[2][1] = 0; t.rotation.m[2][2] = c;
    return t;
}

/// Look up the current value of a DOF from the ModelState.
/// Returns default_value if the DOF is not found in the state.
float get_dof_value(const ModelState& state, int dof_number,
                    float default_value = 0.f) {
    for (const auto& dof : state.dofs) {
        if (dof.dof_number == dof_number) return dof.value;
    }
    return default_value;
}

/// A descriptor for an active coord/normals/tex_ids pool.
/// All pointers are into `tree.lod_buffer`. They're only valid for the
/// lifetime of the WalkContext (which holds a const ref to the tree).
struct ActivePool {
    const f4::models::Vec3*  coords    = nullptr;
    std::size_t             n_coords  = 0;
    const f4::models::Vec3*  normals   = nullptr;
    std::size_t             n_normals = 0;
    const int32_t*          tex_ids   = nullptr;
    std::size_t             n_tex_ids = 0;
};

/// Context for tree traversal.
struct WalkContext {
    const BspTree& tree;
    const ModelState& state;
    ModelGeometry& geometry;
    int max_depth;
    int current_depth = 0;

    // Track visited nodes to prevent infinite loops
    std::vector<bool> visited;

    // Stack of active coord/normals/tex_ids pools. The TOP describes the
    // subtree whose primitives we're currently processing. Pushed when
    // entering a BSubTree-derived node; popped when leaving.
    std::vector<ActivePool> pool_stack;

    // Stack of accumulated affine transforms. The TOP is the combined
    // transform from the root to the current subtree. Pushed when entering
    // a DOF/Trans/Scale node; popped when leaving. If empty, no transform
    // is needed (identity).
    std::vector<AffineTransform> transform_stack;

    WalkContext(const BspTree& t, const ModelState& s,
                ModelGeometry& g, int md)
        : tree(t), state(s), geometry(g), max_depth(md),
          visited(t.nodes.size(), false) {}

    /// Push a subtree's pools onto the stack if they're non-empty.
    /// Returns true if anything was pushed (caller must pop).
    bool push_pools(const BspNode& node) {
        const auto* buf = tree.lod_buffer.data();
        const auto  sz  = tree.lod_buffer.size();
        ActivePool p;

        // Coords
        if (node.n_coords > 0 && node.coords_offset >= 0) {
            auto off = static_cast<std::size_t>(node.coords_offset);
            auto cnt = static_cast<std::size_t>(node.n_coords);
            if (off + cnt * sizeof(f4::models::Vec3) <= sz) {
                p.coords = reinterpret_cast<const f4::models::Vec3*>(buf + off);
                p.n_coords = cnt;
            }
        }

        // Normals
        if (node.n_normals > 0 && node.normals_offset >= 0) {
            auto off = static_cast<std::size_t>(node.normals_offset);
            auto cnt = static_cast<std::size_t>(node.n_normals);
            if (off + cnt * sizeof(f4::models::Vec3) <= sz) {
                p.normals = reinterpret_cast<const f4::models::Vec3*>(buf + off);
                p.n_normals = cnt;
            }
        }

        // Tex IDs
        if (node.n_tex_ids > 0 && node.tex_ids_offset >= 0) {
            auto off = static_cast<std::size_t>(node.tex_ids_offset);
            auto cnt = static_cast<std::size_t>(node.n_tex_ids);
            if (off + cnt * sizeof(int32_t) <= sz) {
                p.tex_ids = reinterpret_cast<const int32_t*>(buf + off);
                p.n_tex_ids = cnt;
            }
        }

        // Only push if this subtree actually has any pools. Subtrees
        // without their own pools inherit the parent's pools.
        if (!p.coords && !p.normals && !p.tex_ids) {
            return false;
        }

        // If we have a parent pool, fill in any missing fields from it
        // so children can resolve coords even if their subtree only
        // declares normals (which is common — the subtree inherits the
        // parent's coord pool).
        if (!pool_stack.empty()) {
            const auto& parent = pool_stack.back();
            if (!p.coords && parent.coords) {
                p.coords = parent.coords;
                p.n_coords = parent.n_coords;
            }
            if (!p.normals && parent.normals) {
                p.normals = parent.normals;
                p.n_normals = parent.n_normals;
            }
            if (!p.tex_ids && parent.tex_ids) {
                p.tex_ids = parent.tex_ids;
                p.n_tex_ids = parent.n_tex_ids;
            }
        } else {
            // No parent — fall back to the global tree pools (which were
            // populated by bsp_parser.cpp from the first BRoot).
            if (!p.coords && !tree.coords.empty()) {
                p.coords = tree.coords.data();
                p.n_coords = tree.coords.size();
            }
            if (!p.normals && !tree.normals.empty()) {
                p.normals = tree.normals.data();
                p.n_normals = tree.normals.size();
            }
            if (!p.tex_ids && !tree.tex_ids.empty()) {
                p.tex_ids = tree.tex_ids.data();
                p.n_tex_ids = tree.tex_ids.size();
            }
        }

        pool_stack.push_back(p);
        return true;
    }

    void pop_pools() { pool_stack.pop_back(); }

    /// Get the currently active pool. Falls back to tree.coords if the
    /// stack is empty (e.g. primitives at the root level before any
    /// BSubTree has been entered).
    ActivePool active() const {
        if (!pool_stack.empty()) return pool_stack.back();
        ActivePool p;
        if (!tree.coords.empty()) {
            p.coords = tree.coords.data();
            p.n_coords = tree.coords.size();
        }
        if (!tree.normals.empty()) {
            p.normals = tree.normals.data();
            p.n_normals = tree.normals.size();
        }
        if (!tree.tex_ids.empty()) {
            p.tex_ids = tree.tex_ids.data();
            p.n_tex_ids = tree.tex_ids.size();
        }
        return p;
    }

    /// Get the current accumulated transform, or nullptr if identity.
    const AffineTransform* current_transform() const {
        if (transform_stack.empty()) return nullptr;
        if (transform_stack.back().is_identity()) return nullptr;
        return &transform_stack.back();
    }

    /// Push a local transform by composing with the current stack top.
    void push_transform(const AffineTransform& local) {
        if (transform_stack.empty()) {
            transform_stack.push_back(local);
        } else {
            transform_stack.push_back(
                AffineTransform::compose(transform_stack.back(), local));
        }
    }

    void pop_transform() { transform_stack.pop_back(); }
};

/// Process a primitive: decode from lod_buffer and add to geometry.
/// prim_offset is LOD-record-relative (byte 0 = start of LOD record).
/// Uses the active pool stack to resolve vertex positions.
/// Applies the current transform from the transform stack to vertices.
void process_prim(WalkContext& ctx, int32_t prim_offset)
{
    if (prim_offset < 0) return;
    if (ctx.tree.lod_buffer.empty()) return;
    if (static_cast<std::size_t>(prim_offset) >= ctx.tree.lod_buffer.size()) return;

    DecodedPrim prim;
    std::string err;
    if (!decode_prim(ctx.tree.lod_buffer.data(),
                     ctx.tree.lod_buffer.size(),
                     prim_offset, prim, err)) {
        // Decoding failed — skip this prim
        return;
    }

    if (prim.type == PolyType::Unknown) return;
    if (prim.n_verts <= 0) return;

    // Safety: skip prims with unreasonable vertex counts
    if (prim.n_verts > 1000) return;

    // Classify the primitive kind based on its PolyType.
    PrimitiveKind kind = PrimitiveKind::Triangles;
    if (prim.type == PolyType::PointF) {
        kind = PrimitiveKind::Points;
    } else if (prim.type == PolyType::LineF) {
        kind = PrimitiveKind::Lines;
    } else if (prim.n_verts < 3) {
        return;
    }

    // Resolve the texture ID using the ACTIVE pool (not tree.tex_ids).
    // This is the key fix for per-subtree pool binding.
    ActivePool pool = ctx.active();
    int32_t tex_id = -1;
    if (prim.tex_index >= 0 && pool.tex_ids &&
        prim.tex_index < static_cast<int>(pool.n_tex_ids)) {
        tex_id = pool.tex_ids[prim.tex_index];
    }

    // Find or create a mesh for this (texture, primitive_kind) pair.
    Mesh* mesh = nullptr;
    for (auto& m : ctx.geometry.meshes) {
        if (m.tex_id == tex_id && m.kind == kind) { mesh = &m; break; }
    }
    if (!mesh) {
        ctx.geometry.meshes.emplace_back();
        auto& nm = ctx.geometry.meshes.back();
        nm.tex_id = tex_id;
        nm.kind = kind;
        mesh = &nm;
    }

    // Convert prim to vertices + indices using the active pool.
    // Apply the accumulated DOF transform if present.
    const AffineTransform* xform = ctx.current_transform();
    prim_to_mesh(prim, ctx.tree, *mesh, pool.coords, pool.n_coords,
                 pool.tex_ids, pool.n_tex_ids, xform);
}

/// Walk the BSP tree starting from a given node index.
void walk_node(WalkContext& ctx, NodeIdx node_idx)
{
    if (node_idx < 0 || node_idx >= static_cast<NodeIdx>(ctx.tree.nodes.size()))
        return;

    // Depth limit
    if (ctx.max_depth > 0 && ctx.current_depth >= ctx.max_depth)
        return;

    // Cycle detection
    if (ctx.visited[static_cast<std::size_t>(node_idx)]) return;
    ctx.visited[static_cast<std::size_t>(node_idx)] = true;

    const auto& node = ctx.tree.nodes[static_cast<std::size_t>(node_idx)];
    ctx.current_depth++;

    // Determine whether this node has its own subtree pool that needs
    // pushing. BRoot, BSubTree, BDofNode, BXDofNode, BTransNode, BScaleNode,
    // and BSpecialXform all carry their own coords/normals/tex_ids.
    bool pushed = false;
    switch (node.type) {
        case BspNodeType::BRoot:
        case BspNodeType::BSubTree:
        case BspNodeType::BDofNode:
        case BspNodeType::BXDofNode:
        case BspNodeType::BTransNode:
        case BspNodeType::BScaleNode:
        case BspNodeType::BSpecialXform:
            pushed = ctx.push_pools(node);
            break;
        default:
            break;
    }

    // Track whether we pushed a transform (to pop it later)
    bool pushed_transform = false;

    switch (node.type) {
    case BspNodeType::BNode:
        // Abstract base — just sibling
        break;

    case BspNodeType::BSubTree:
    case BspNodeType::BRoot:
        // Root/subtree — recurse into subtree
        if (node.subtree >= 0) walk_node(ctx, node.subtree);
        break;

    case BspNodeType::BSlotNode:
        // Slot marker — no subtree in BSP tree.
        // Slot child models are resolved externally at the object level.
        break;

    case BspNodeType::BDofNode:
    case BspNodeType::BXDofNode: {
        // DOF rotational transform.
        //
        // In FreeFalcon, a BDofNode defines a coordinate frame for its
        // subtree using dof_rotation (orientation into parent space) and
        // dof_translation (origin in parent space). The DOF value rotates
        // the subtree around the local Z axis.
        //
        // Full transform for a vertex v in the DOF's local frame:
        //   v_parent = dof_translation + dof_rotation * Rz(value) * v
        //
        // As an AffineTransform:
        //   rotation    = dof_rotation * Rz(value)
        //   translation = dof_translation
        const float dof_value = get_dof_value(ctx.state, node.dof_number);

        // Build Rz(dof_value)
        AffineTransform rz = make_rotation_x(dof_value);

        // Compose: dof_rotation * Rz(value)
        AffineTransform dof_xform;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                dof_xform.rotation.m[i][j] = 0;
                for (int k = 0; k < 3; ++k) {
                    dof_xform.rotation.m[i][j] +=
                        node.dof_rotation.m[i][k] * rz.rotation.m[k][j];
                }
            }
        }
        dof_xform.translation = node.dof_translation;

        ctx.push_transform(dof_xform);
        pushed_transform = true;

        if (node.subtree >= 0) walk_node(ctx, node.subtree);
        break;
    }

    case BspNodeType::BTransNode: {
        // DOF translational transform.
        //
        // BTransNode translates its subtree. The dof_translation is the
        // base position offset, and the DOF value drives additional
        // translation along the local Z axis (scaled by multiplier).
        //
        // v_parent = v_local + dof_translation + (0, 0, value * multiplier)
        const float dof_value = get_dof_value(ctx.state, node.dof_number);

        AffineTransform trans_xform; // identity rotation
        trans_xform.translation = node.dof_translation;
        // Add DOF-driven Z translation
        trans_xform.translation.z += dof_value * node.dof_multiplier;

        ctx.push_transform(trans_xform);
        pushed_transform = true;

        if (node.subtree >= 0) walk_node(ctx, node.subtree);
        break;
    }

    case BspNodeType::BScaleNode: {
        // Scale transform.
        //
        // BScaleNode scales its subtree by the scale factors and offsets
        // by dof_translation.
        //
        // v_parent = dof_translation + diag(scale) * v_local
        //
        // We represent this by storing the scale in the rotation matrix
        // (which is no longer orthogonal, but works for position
        // transforms). Normals will be approximately correct for uniform
        // scale; non-uniform scale may cause slight normal errors.
        AffineTransform scale_xform;
        scale_xform.rotation.m[0][0] = node.scale.x;
        scale_xform.rotation.m[0][1] = 0; scale_xform.rotation.m[0][2] = 0;
        scale_xform.rotation.m[1][0] = 0;
        scale_xform.rotation.m[1][1] = node.scale.y;
        scale_xform.rotation.m[1][2] = 0;
        scale_xform.rotation.m[2][0] = 0; scale_xform.rotation.m[2][1] = 0;
        scale_xform.rotation.m[2][2] = node.scale.z;
        scale_xform.translation = node.dof_translation;

        ctx.push_transform(scale_xform);
        pushed_transform = true;

        if (node.subtree >= 0) walk_node(ctx, node.subtree);
        break;
    }

    case BspNodeType::BSwitchNode:
    case BspNodeType::BXSwitchNode: {
        // Switch nodes have an array of child subtree roots in
        // tree.switch_children, indexed by node.switch_children_offset.
        // Default behavior: traverse ALL children (show everything).
        // Interactive: check state.switches for an active_child override.
        if (node.n_children > 0 && node.switch_children_offset >= 0) {
            // Check if ModelState has a specific child selection
            int active_child = -1;
            for (const auto& sw : ctx.state.switches) {
                if (sw.switch_number == node.switch_number) {
                    active_child = sw.active_child;
                    break;
                }
            }

            auto base = static_cast<std::size_t>(node.switch_children_offset);
            auto count = static_cast<std::size_t>(node.n_children);

            // Bounds check: base + count must fit within switch_children
            if (base + count > ctx.tree.switch_children.size()) {
                break;  // corrupted switch children — skip
            }

            if (active_child >= 0 && active_child < static_cast<int>(count)) {
                // Walk only the selected child
                auto child_idx = ctx.tree.switch_children[base + static_cast<std::size_t>(active_child)];
                if (child_idx >= 0) walk_node(ctx, child_idx);
            } else {
                // No state override — walk all children
                for (std::size_t k = 0; k < count; ++k) {
                    auto child_idx = ctx.tree.switch_children[base + k];
                    if (child_idx >= 0) walk_node(ctx, child_idx);
                }
            }
        }
        break;
    }

    case BspNodeType::BSplitterNode:
        // Splitter — traverse both front and back
        if (node.front >= 0) walk_node(ctx, node.front);
        if (node.back  >= 0) walk_node(ctx, node.back);
        break;

    case BspNodeType::BPrimitiveNode:
    case BspNodeType::BCulledPrimitiveNode:
        // Leaf with one prim/polygon
        process_prim(ctx, node.prim_offset);
        break;

    case BspNodeType::BLitPrimitiveNode:
        // Leaf with front + back lit polygon
        process_prim(ctx, node.prim_offset);
        if (node.back_poly_offset >= 0) {
            process_prim(ctx, node.back_poly_offset);
        }
        break;

    case BspNodeType::BSpecialXform:
        // Billboard/tree transform — recurse into subtree
        // TODO: Apply billboard/tree special transform
        if (node.subtree >= 0) walk_node(ctx, node.subtree);
        break;

    case BspNodeType::BLightStringNode:
        // Light string — decode prim
        process_prim(ctx, node.prim_offset);
        break;

    case BspNodeType::BRenderControlNode:
        // Render control — no geometry
        break;

    default:
        break;
    }

    if (pushed_transform) ctx.pop_transform();
    if (pushed) ctx.pop_pools();

    // Walk sibling
    if (node.sibling >= 0) walk_node(ctx, node.sibling);

    ctx.current_depth--;
}

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────

f4::models::ModelGeometry extract_geometry(
    const BspTree& tree,
    const ModelState& state,
    int max_depth,
    std::string& err)
{
    (void)err;

    f4::models::ModelGeometry geometry;

    if (tree.nodes.empty()) return geometry;

    WalkContext ctx(tree, state, geometry, max_depth);

    // Start from node 0 (root)
    walk_node(ctx, 0);

    return geometry;
}

} // namespace f4::models::detail

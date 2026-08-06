// f4-models/src/geometry_extractor.cpp
//
// BSP tree traversal → renderable geometry.
//
// Strategy: Walk the tree depth-first. At each node:
//   BRoot/BSubTree       → recurse into subtree
//   BSlotNode            → recurse into subtree (slot marker)
//   BDofNode/BXDofNode   → recurse into subtree (optionally apply DOF transform)
//   BSwitchNode/BXSwitchNode → select one child, recurse
//   BSplitterNode        → recurse both front and back (no frustum culling yet)
//   BPrimitiveNode       → decode Prim and emit triangles
//   BCulledPrimitiveNode → decode Poly and emit triangles
//   BLitPrimitiveNode    → decode front Poly and emit triangles
//   BSpecialXform        → recurse into subtree (billboard transform)
//   BTransNode/BScaleNode → recurse into subtree (optionally apply transform)
//   BRenderControlNode   → skip (render state, no geometry)
//   BLightStringNode     → decode prim (light marker)

#include "geometry_extractor.hpp"
#include "poly_parser.hpp"

namespace f4::models::detail {

namespace {

/// Context for tree traversal.
struct WalkContext {
    const BspTree& tree;
    const ModelState& state;
    ModelGeometry& geometry;
    int max_depth;
    int current_depth = 0;

    // Track visited nodes to prevent infinite loops
    std::vector<bool> visited;

    WalkContext(const BspTree& t, const ModelState& s,
                ModelGeometry& g, int md)
        : tree(t), state(s), geometry(g), max_depth(md),
          visited(t.nodes.size(), false) {}
};

/// Process a primitive: decode from lod_buffer and add to geometry.
/// prim_offset is LOD-record-relative (byte 0 = start of LOD record).
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

    if (prim.n_verts < 3 || prim.type == PolyType::Unknown) return;  // need at least a triangle

    // Safety: skip prims with unreasonable vertex counts
    if (prim.n_verts > 1000) return;

    // Determine texture ID for mesh grouping
    int32_t tex_id = -1;
    if (prim.tex_index >= 0 &&
        prim.tex_index < static_cast<int>(ctx.tree.tex_ids.size())) {
        tex_id = ctx.tree.tex_ids[prim.tex_index];
    }

    // Find or create a mesh for this texture
    Mesh* mesh = nullptr;
    for (auto& m : ctx.geometry.meshes) {
        if (m.tex_id == tex_id) { mesh = &m; break; }
    }
    if (!mesh) {
        ctx.geometry.meshes.emplace_back();
        ctx.geometry.meshes.back().tex_id = tex_id;
        mesh = &ctx.geometry.meshes.back();
    }

    // Convert prim to triangles
    prim_to_mesh(prim, ctx.tree, *mesh);
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
    case BspNodeType::BXDofNode:
    case BspNodeType::BTransNode:
    case BspNodeType::BScaleNode:
        // DOF/transform — recurse into subtree
        // (Future: apply transform to accumulated vertices)
        if (node.subtree >= 0) walk_node(ctx, node.subtree);
        break;

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

// f4-models/include/f4/models/geometry_grouped.hpp
//
// Grouped geometry extraction — the hierarchy-preserving counterpart to
// geometry.hpp's flat ModelGeometry (Docs/AIRCRAFT_ANIMATION_PLAN.md
// §4.1). Where extract_geometry() bakes every tagged node's transform
// into world-space vertices, extract_geometry_grouped() keeps each
// mesh's vertices in the LOCAL space of its deepest tagged ancestor and
// records the chain of tagged nodes (DOF / translator / scale / switch
// branches) that position it. The glTF emitter turns each chain into a
// node hierarchy so the runtime can animate the same nodes the original
// engine did.
//
// Vertex space contract:
//   - Empty chain  → vertices in model-root space (same as the flat
//     extractor's output, since only tagged nodes carry transforms).
//   - Non-empty    → vertices in the local space of chain.back() (its
//     frame transform NOT applied; between the deepest tagged node and
//     the primitives there are no transform-carrying nodes).
//
// Switch semantics (FreeFalcon bspnodes.cpp BSwitchNode::Draw): switch
// values are BITMASKS — child k is drawn when bit k is set. Grouped
// extraction walks EVERY child branch and records sw_child = k so the
// runtime can apply the same mask.

#pragma once

#include <f4/models/bsp_node.hpp>
#include <f4/models/geometry.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::models {

/// One tagged ancestor in a mesh's node chain, with every parameter the
/// runtime needs to re-apply (and animate) the node's transform.
struct TaggedAncestor {
    /// BDofNode/BXDofNode (rot), BTransNode (translate), BScaleNode
    /// (scale), or BSwitchNode/BXSwitchNode (visibility branch).
    BspNodeType type = BspNodeType::Unknown;

    /// Index into BspTree::nodes — provenance, kept forever (§6.2).
    int32_t node_index = -1;

    /// dof_number for DOF/trans/scale nodes, switch_number for switches.
    int32_t number = -1;

    /// Static frame into parent space (dof_rotation / dof_translation).
    /// Identity rotation for trans/scale/switch nodes.
    Mat3x3 frame_rotation{};
    Vec3   frame_translation{};

    /// XDOF value processing parameters (verbatim from the node).
    float   dof_min = 0;
    float   dof_max = 0;
    float   dof_multiplier = 1;
    int32_t dof_flags = 0;

    /// BScaleNode target scale (scale at value 1).
    Vec3 scale_target{1, 1, 1};

    /// For switch ancestors: which child branch this is (bit index in
    /// the FreeFalcon switch mask).
    int32_t switch_child = -1;

    /// Switch flags (XSWT_REVERSED_EFFECT = bit 0 → mask inverted).
    int32_t switch_flags = 0;
};

/// One output mesh grouped under its tagged-node chain.
struct GroupedMesh {
    /// Vertices in the local space of chain.back() (root space when the
    /// chain is empty), grouped by (texture, primitive kind) exactly
    /// like the flat extractor.
    Mesh mesh;

    /// Tagged ancestors, outermost first. Empty = static model geometry.
    std::vector<TaggedAncestor> chain;
};

/// The grouped extraction result.
struct GroupedGeometry {
    std::vector<GroupedMesh> meshes;

    [[nodiscard]] std::size_t total_vertices() const noexcept {
        std::size_t n = 0;
        for (const auto& gm : meshes) n += gm.mesh.vertices.size();
        return n;
    }
    [[nodiscard]] std::size_t total_triangles() const noexcept {
        std::size_t n = 0;
        for (const auto& gm : meshes) n += gm.mesh.triangles.size();
        return n;
    }
};

} // namespace f4::models

namespace f4::models::detail {

/// Extract geometry grouped under tagged-node chains (see
/// geometry_grouped.hpp). All switch children are walked (grouped
/// extraction is for export — visibility is a runtime decision).
/// DOF values from `state` are IGNORED for vertex positions (vertices
/// stay in ancestor-local space); the walk uses value 0 = frame only.
[[nodiscard]] f4::models::GroupedGeometry extract_geometry_grouped(
    const BspTree& tree,
    const ModelState& state,
    int max_depth,
    std::string& err);

} // namespace f4::models::detail

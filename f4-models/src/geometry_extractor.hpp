// f4-models/src/geometry_extractor.hpp
//
// Walk a BSP tree to extract renderable geometry (vertices + triangles).
// Handles DOF transforms, switch selection, and slot placeholders.
//
// Internal to f4-models — not a public header.

#pragma once

#include <f4/models/bsp_node.hpp>
#include <f4/models/geometry.hpp>

#include <string>
#include <vector>

namespace f4::models::detail {

/// Extract geometry from a BSP tree.
/// Walks the tree starting from node 0 (root), collecting all
/// leaf primitives into renderable meshes.
///
/// @param tree          The parsed BSP tree
/// @param state         DOF/switch/lod state controls
/// @param max_depth     Maximum traversal depth (0 = unlimited)
/// @param err           Error output
/// @return              Extracted model geometry
[[nodiscard]] f4::models::ModelGeometry extract_geometry(
    const BspTree& tree,
    const ModelState& state,
    int max_depth,
    std::string& err);

} // namespace f4::models::detail

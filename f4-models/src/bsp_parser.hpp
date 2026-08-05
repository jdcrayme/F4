// f4-models/src/bsp_parser.hpp
//
// BSP tree parser — reads the classic BSP format from KoreaObj.LOD.
// Produces a BspTree with all nodes, coords, normals, and tex IDs.
//
// Internal to f4-models — not a public header.

#pragma once

#include <f4/models/bsp_node.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::models::detail {

/// Parse a classic BSP tree from raw LOD record bytes.
/// The bytes should be the LOD record data (offset..offset+size from
/// the LOD file).
///
/// On success, returns true and fills `tree`.
/// On failure, returns false and sets `err`.
[[nodiscard]] bool parse_bsp_tree(
    const uint8_t* data, std::size_t size,
    BspTree& tree,
    std::string& err);

} // namespace f4::models::detail

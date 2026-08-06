// f4-models/src/poly_parser.hpp
//
// Decode Prim/Poly structures from the raw BSP poly_data buffer.
// All "pointers" on disk are 32-bit byte offsets from the BSP
// node data base address. Variable-length arrays (xyz, rgba, I, uv)
// are also stored as offsets in the same buffer.
//
// Internal to f4-models — not a public header.

#pragma once

#include <f4/models/bsp_node.hpp>
#include <f4/models/geometry.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::models::detail {

// ── Decoded Primitive ─────────────────────────────────────────────────────

struct DecodedPrim {
    PolyType type = PolyType::Unknown;
    int32_t  n_verts = 0;

    /// Vertex position indices (into BspTree::coords).
    /// On disk: int32[nVerts] offset → int indices.
    std::vector<int32_t> xyz_indices;

    /// Plane equation (for Poly and derived: F, FL, G, GL, Tex*, etc.).
    Plane plane = {};

    /// Flat color index (for FC variants).
    int32_t rgba = -1;

    /// Per-vertex color indices (for VC variants).
    std::vector<int32_t> rgba_indices;

    /// Flat intensity index (for FCN variants).
    int32_t intensity = -1;

    /// Per-vertex intensity indices (for VCN variants).
    std::vector<int32_t> intensity_indices;

    /// Texture ID (local index, maps to BspTree::tex_ids).
    int32_t tex_index = -1;

    /// Per-vertex texture coordinates.
    std::vector<TexCoord> uv_coords;
};

/// Decode one Prim/Poly from the BSP data buffer.
/// `base` is the start of the entire BSP node data buffer.
/// `prim_offset` is the byte offset of the Prim within that buffer.
/// `buffer_size` is the total size of the buffer.
///
/// Returns true on success, false on truncation/corruption.
[[nodiscard]] bool decode_prim(
    const uint8_t* base,
    std::size_t buffer_size,
    int32_t prim_offset,
    DecodedPrim& out,
    std::string& err);

/// Get the on-disk size of a Prim header (struct only, no variable arrays)
/// for the given PolyType.
[[nodiscard]] int32_t prim_header_size(PolyType type) noexcept;

/// Convert a decoded Prim + BSP tree data into renderable vertices/triangles.
/// The resulting triangles are appended to `mesh`.
///
/// `coords` / `n_coords` — the active coord pool (from the current
/// subtree, NOT necessarily tree.coords). This is critical: prims inside
/// a BSubTree/BDofNode/etc. index into that subtree's local coord pool,
/// not the root's. Pass the active pool from the geometry_extractor's
/// pool stack.
///
/// `tex_ids` / `n_tex_ids` — the active tex_ids pool, same reasoning.
void prim_to_mesh(
    const DecodedPrim& prim,
    const BspTree& tree,
    Mesh& mesh,
    const Vec3* coords, std::size_t n_coords,
    const int32_t* tex_ids, std::size_t n_tex_ids);

} // namespace f4::models::detail

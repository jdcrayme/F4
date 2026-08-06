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

/// Affine transform: v' = rotation * v + translation.
/// Used to accumulate DOF/translate/scale transforms during tree walk.
/// nullptr means identity (no transform).
struct AffineTransform {
    Mat3x3 rotation;    // default = identity
    Vec3   translation; // default = zero

    AffineTransform() {
        // Identity transform
        rotation.m[0][0] = 1; rotation.m[0][1] = 0; rotation.m[0][2] = 0;
        rotation.m[1][0] = 0; rotation.m[1][1] = 1; rotation.m[1][2] = 0;
        rotation.m[2][0] = 0; rotation.m[2][1] = 0; rotation.m[2][2] = 1;
        translation = {0, 0, 0};
    }

    /// Apply to a point: rotation * p + translation
    Vec3 apply_point(const Vec3& p) const {
        return {
            rotation.m[0][0]*p.x + rotation.m[0][1]*p.y + rotation.m[0][2]*p.z + translation.x,
            rotation.m[1][0]*p.x + rotation.m[1][1]*p.y + rotation.m[1][2]*p.z + translation.y,
            rotation.m[2][0]*p.x + rotation.m[2][1]*p.y + rotation.m[2][2]*p.z + translation.z
        };
    }

    /// Apply rotation only to a direction (normal): rotation * n
    Vec3 apply_direction(const Vec3& n) const {
        return {
            rotation.m[0][0]*n.x + rotation.m[0][1]*n.y + rotation.m[0][2]*n.z,
            rotation.m[1][0]*n.x + rotation.m[1][1]*n.y + rotation.m[1][2]*n.z,
            rotation.m[2][0]*n.x + rotation.m[2][1]*n.y + rotation.m[2][2]*n.z
        };
    }

    /// Check if this is approximately the identity transform.
    bool is_identity() const {
        return rotation.m[0][0] == 1.f && rotation.m[0][1] == 0.f && rotation.m[0][2] == 0.f &&
               rotation.m[1][0] == 0.f && rotation.m[1][1] == 1.f && rotation.m[1][2] == 0.f &&
               rotation.m[2][0] == 0.f && rotation.m[2][1] == 0.f && rotation.m[2][2] == 1.f &&
               translation.x == 0.f && translation.y == 0.f && translation.z == 0.f;
    }

    /// Compose two transforms: result = a ∘ b  (apply b first, then a)
    /// result.rotation    = a.rotation * b.rotation
    /// result.translation = a.rotation * b.translation + a.translation
    static AffineTransform compose(const AffineTransform& a,
                                   const AffineTransform& b) {
        AffineTransform result;
        // Matrix multiply: a.rotation * b.rotation
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                result.rotation.m[i][j] = 0;
                for (int k = 0; k < 3; ++k) {
                    result.rotation.m[i][j] += a.rotation.m[i][k] * b.rotation.m[k][j];
                }
            }
        }
        // Translation: a.rotation * b.translation + a.translation
        result.translation = a.apply_direction(b.translation) + a.translation;
        return result;
    }
};

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
///
/// `transform` — optional affine transform to apply to vertex positions
/// and normals (from accumulated DOF/translate/scale stack).
/// nullptr means no transform (identity).
void prim_to_mesh(
    const DecodedPrim& prim,
    const BspTree& tree,
    Mesh& mesh,
    const Vec3* coords, std::size_t n_coords,
    const int32_t* tex_ids, std::size_t n_tex_ids,
    const AffineTransform* transform = nullptr);

} // namespace f4::models::detail

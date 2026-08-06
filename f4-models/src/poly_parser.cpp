// f4-models/src/poly_parser.cpp
//
// Decode Prim/Poly from the BSP flat buffer.
//
// All "pointers" on disk are byte offsets from the BSP node data
// base address. Variable arrays (xyz[], rgba[], I[], uv[]) are
// stored separately, referenced by offset.
//
// On-disk Prim sizes (from FreeFalcon polylib.h):
//   PointF/LineF  → PrimPointFC/PrimLineFC = 16 bytes
//   F/AF          → PolyFC    = 32 bytes
//   FL/AFL        → PolyFCN   = 36 bytes
//   G/AG          → PolyVC    = 32 bytes
//   GL/AGL        → PolyVCN   = 36 bytes
//   Tex/ATex/CTex/CATex/BAptTex → PolyTexFC = 40
//   TexL/ATexL/CTexL/CATexL     → PolyTexFCN = 44
//   TexG/ATexG/CTexG/CATexG     → PolyTexVC = 40
//   TexGL/ATexGL/CTexGL/CATexGL → PolyTexVCN = 44
//
// References:
//   FreeFalcon: src/graphics/include/polylib.h (class hierarchy)
//   FreeFalcon: src/graphics/bsplib/bspnodes.cpp (RestorePrimPointers)

#include "poly_parser.hpp"
#include "bin_reader.hpp"

namespace f4::models::detail {

// ── Prim header sizes on disk ─────────────────────────────────────────────

int32_t prim_header_size(PolyType type) noexcept {
    switch (type) {
        case PolyType::PointF:
        case PolyType::LineF:
            return 16;  // PrimPointFC / PrimLineFC

        case PolyType::F:
        case PolyType::AF:
            return 32;  // PolyFC

        case PolyType::FL:
        case PolyType::AFL:
            return 36;  // PolyFCN

        case PolyType::G:
        case PolyType::AG:
            return 32;  // PolyVC

        case PolyType::GL:
        case PolyType::AGL:
            return 36;  // PolyVCN

        case PolyType::Tex:
        case PolyType::ATex:
        case PolyType::CTex:
        case PolyType::CATex:
        case PolyType::BAptTex:
            return 40;  // PolyTexFC

        case PolyType::TexL:
        case PolyType::ATexL:
        case PolyType::CTexL:
        case PolyType::CATexL:
            return 44;  // PolyTexFCN

        case PolyType::TexG:
        case PolyType::ATexG:
        case PolyType::CTexG:
        case PolyType::CATexG:
            return 40;  // PolyTexVC

        case PolyType::TexGL:
        case PolyType::ATexGL:
        case PolyType::CTexGL:
        case PolyType::CATexGL:
            return 44;  // PolyTexVCN

        default:
            return 0;  // unknown
    }
}

// ── Trait queries ─────────────────────────────────────────────────────────

[[nodiscard]] static bool has_plane(PolyType t) noexcept {
    // All types >= F (2) have a plane equation (Poly-derived)
    return static_cast<int>(t) >= 2;
}

[[nodiscard]] static bool has_flat_color(PolyType t) noexcept {
    // FC variants: PointF, LineF, F, FL, Tex, TexL, CTex, CTexL,
    //              AF, AFL, ATex, ATexL, CATex, CATexL, BAptTex
    int v = static_cast<int>(t);
    return v <= 1 ||   // PointF, LineF
           v == 2 || v == 3 ||   // F, FL
           v == 6 || v == 7 ||   // Tex, TexL
           v == 10 || v == 11 ||  // CTex, CTexL
           v == 14 || v == 15 ||  // AF, AFL
           v == 18 || v == 19 ||  // ATex, ATexL
           v == 22 || v == 23 ||  // CATex, CATexL
           v == 26;               // BAptTex
}

[[nodiscard]] static bool has_vertex_colors(PolyType t) noexcept {
    // VC variants: G, GL, TexG, TexGL, CTexG, CTexGL, AG, AGL, ATexG, ATexGL, CATexG, CATexGL
    int v = static_cast<int>(t);
    return v == 4 || v == 5 ||    // G, GL
           v == 8 || v == 9 ||    // TexG, TexGL
           v == 12 || v == 13 ||  // CTexG, CTexGL
           v == 16 || v == 17 ||  // AG, AGL
           v == 20 || v == 21 ||  // ATexG, ATexGL
           v == 24 || v == 25;    // CATexG, CATexGL
}

[[nodiscard]] static bool has_flat_intensity(PolyType t) noexcept {
    // FCN variants: FL, TexL, CTexL, AFL, ATexL, CATexL
    int v = static_cast<int>(t);
    return v == 3 || v == 7 || v == 11 ||
           v == 15 || v == 19 || v == 23;
}

[[nodiscard]] static bool has_vertex_intensities(PolyType t) noexcept {
    // VCN variants: GL, TexGL, CTexGL, AGL, ATexGL, CATexGL
    int v = static_cast<int>(t);
    return v == 5 || v == 9 || v == 13 ||
           v == 17 || v == 21 || v == 25;
}

[[nodiscard]] static bool has_texture(PolyType t) noexcept {
    // All Tex variants: 6-13, 18-26
    int v = static_cast<int>(t);
    return (v >= 6 && v <= 13) || (v >= 18 && v <= 26);
}

// ── Decode Prim ───────────────────────────────────────────────────────────

bool decode_prim(
    const uint8_t* base,
    std::size_t buffer_size,
    int32_t prim_offset,
    DecodedPrim& out,
    std::string& err)
{
    if (prim_offset < 0 || static_cast<std::size_t>(prim_offset) >= buffer_size) {
        err = "prim offset out of bounds";
        return false;
    }

    BinReader r{base, buffer_size};
    r.seek(static_cast<std::size_t>(prim_offset));

    // Read Prim base: type(4) + nVerts(4) + xyz_offset(4)
    int32_t type_val;
    if (!r.read(type_val) || !r.read(out.n_verts)) {
        err = "prim header truncated";
        return false;
    }

    if (type_val < 0 || type_val >= 27) {
        // Invalid type — skip
        out.type = PolyType::Unknown;
        return true;
    }
    out.type = static_cast<PolyType>(type_val);

    // Sanity check nVerts — Falcon polygons typically have 3-20 vertices.
    // Values > 1000 are almost certainly corruption.
    if (out.n_verts < 0 || out.n_verts > 1000) {
        err = "prim nVerts out of range: " + std::to_string(out.n_verts);
        out.type = PolyType::Unknown;
        out.n_verts = 0;
        return true;  // skip this prim, not fatal
    }

    int32_t xyz_offset;
    if (!r.read(xyz_offset)) {
        err = "prim xyz offset truncated";
        return false;
    }

    // Read type-specific fields
    if (has_plane(out.type)) {
        // Poly: A, B, C, D
        if (!r.read(out.plane.a) || !r.read(out.plane.b) ||
            !r.read(out.plane.c) || !r.read(out.plane.d)) {
            err = "prim plane truncated";
            return false;
        }
    }

    if (has_flat_color(out.type) && !has_plane(out.type)) {
        // PrimPointFC / PrimLineFC: rgba (no plane before it)
        if (!r.read(out.rgba)) {
            err = "prim flat color truncated";
            return false;
        }
    } else if (has_flat_color(out.type) && has_plane(out.type)) {
        // PolyFC / PolyFCN / PolyTexFC / PolyTexFCN: rgba after plane
        if (!r.read(out.rgba)) {
            err = "prim flat color truncated";
            return false;
        }
    } else if (has_vertex_colors(out.type)) {
        // PolyVC / PolyVCN / PolyTexVC / PolyTexVCN: rgba offset
        int32_t rgba_offset;
        if (!r.read(rgba_offset)) {
            err = "prim vertex color offset truncated";
            return false;
        }
        // Read vertex color indices
        if (out.n_verts > 0 && rgba_offset >= 0) {
            auto off = static_cast<std::size_t>(rgba_offset);
            auto nv = static_cast<std::size_t>(out.n_verts);
            if (off + nv * sizeof(int32_t) <= buffer_size) {
                out.rgba_indices.resize(nv);
                std::memcpy(out.rgba_indices.data(), base + off,
                            nv * sizeof(int32_t));
            }
        }
    }

    // CRITICAL: Read texture fields BEFORE intensity fields for textured types.
    // On-disk C++ class hierarchy order:
    //   PolyTexFC  = PolyFC + texIndex + uv          (40 bytes)
    //   PolyTexFCN = PolyTexFC + I                   (44 bytes)
    //   PolyTexVC  = PolyVC + texIndex + uv          (40 bytes)
    //   PolyTexVCN = PolyTexVC + I_offset            (44 bytes)
    // For non-textured FCN/VCN types (FL, AFL, GL, AGL), intensity
    // comes right after the color field (no texture in between).

    if (has_texture(out.type)) {
        // texIndex + uv offset (comes BEFORE intensity in Tex variants)
        if (!r.read(out.tex_index)) {
            err = "prim tex index truncated";
            return false;
        }
        int32_t uv_offset;
        if (!r.read(uv_offset)) {
            err = "prim uv offset truncated";
            return false;
        }
        // Read UV coords
        if (out.n_verts > 0 && uv_offset >= 0) {
            auto off = static_cast<std::size_t>(uv_offset);
            auto nv = static_cast<std::size_t>(out.n_verts);
            if (off + nv * sizeof(TexCoord) <= buffer_size) {
                out.uv_coords.resize(nv);
                std::memcpy(out.uv_coords.data(), base + off,
                            nv * sizeof(TexCoord));
            }
        }
    }

    if (has_flat_intensity(out.type)) {
        // PolyFCN / PolyTexFCN: intensity (AFTER texture for TexFCN types)
        if (!r.read(out.intensity)) {
            err = "prim intensity truncated";
            return false;
        }
    } else if (has_vertex_intensities(out.type)) {
        // PolyVCN / PolyTexVCN: intensity offset (AFTER texture for TexVCN types)
        int32_t i_offset;
        if (!r.read(i_offset)) {
            err = "prim intensity offset truncated";
            return false;
        }
        if (out.n_verts > 0 && i_offset >= 0) {
            auto off = static_cast<std::size_t>(i_offset);
            auto nv = static_cast<std::size_t>(out.n_verts);
            if (off + nv * sizeof(int32_t) <= buffer_size) {
                out.intensity_indices.resize(nv);
                std::memcpy(out.intensity_indices.data(), base + off,
                            nv * sizeof(int32_t));
            }
        }
    }

    // Read xyz vertex position indices (common to all Prim types)
    if (out.n_verts > 0 && xyz_offset >= 0) {
        auto off = static_cast<std::size_t>(xyz_offset);
        auto nv = static_cast<std::size_t>(out.n_verts);
        if (off + nv * sizeof(int32_t) <= buffer_size) {
            out.xyz_indices.resize(nv);
            std::memcpy(out.xyz_indices.data(), base + off,
                        nv * sizeof(int32_t));
        }
    }

    return true;
}

// ── Prim → Mesh conversion ───────────────────────────────────────────────

void prim_to_mesh(
    const DecodedPrim& prim,
    const BspTree& tree,
    Mesh& mesh,
    const Vec3* coords, std::size_t n_coords,
    const int32_t* tex_ids, std::size_t n_tex_ids)
{
    (void)tree;  // kept for backward compat / future use
    if (prim.n_verts <= 0 || prim.type == PolyType::Unknown) return;

    auto base_vert = static_cast<uint32_t>(mesh.vertices.size());

    // Resolve vertex positions from the supplied active coord pool.
    // (Previously this always used tree.coords, which produced garbage
    // for prims inside non-root subtrees — see Fix #4.)
    for (int i = 0; i < prim.n_verts; ++i) {
        Vertex v;

        // Position from xyz index → active coords pool
        if (i < static_cast<int>(prim.xyz_indices.size())) {
            int32_t idx = prim.xyz_indices[i];
            if (idx >= 0 && coords &&
                static_cast<std::size_t>(idx) < n_coords) {
                v.position = coords[idx];
            }
        }

        // Normal from plane (if available, use face normal)
        if (prim.plane.a != 0 || prim.plane.b != 0 || prim.plane.c != 0) {
            v.normal = {prim.plane.a, prim.plane.b, prim.plane.c};
        }

        // UV coords
        if (i < static_cast<int>(prim.uv_coords.size())) {
            v.uv = prim.uv_coords[i];
        }

        // Texture ID (resolve local tex_index through the ACTIVE tex_ids pool)
        if (prim.tex_index >= 0 && tex_ids &&
            prim.tex_index < static_cast<int>(n_tex_ids)) {
            v.tex_id = tex_ids[prim.tex_index];
        }

        // Color (flat or per-vertex)
        if (prim.rgba >= 0) {
            v.color = static_cast<uint32_t>(prim.rgba);
        } else if (i < static_cast<int>(prim.rgba_indices.size())) {
            v.color = static_cast<uint32_t>(prim.rgba_indices[i]);
        }

        mesh.vertices.push_back(v);
    }

    // Resolve the texture id (for tagging the primitives). We share the
    // mesh's tex_id field already, but the per-primitive tag is useful
    // for exporters that walk triangles/lines individually.
    int32_t tex = -1;
    if (prim.tex_index >= 0 && tex_ids &&
        prim.tex_index < static_cast<int>(n_tex_ids)) {
        tex = tex_ids[prim.tex_index];
    }

    // Emit primitive indices based on the PolyType.
    //
    // PointF (n=1)         → 1 point
    // LineF  (n=2)         → 1 line
    // F/FL/G/GL/Tex*/...   → (n-2) triangles via fan triangulation
    //
    // Fan triangulation assumes convex polygons (Falcon's prim writer
    // guarantees convexity — see bspbuild.exe's FLT importer).
    if (prim.type == PolyType::PointF) {
        Point pt;
        pt.v0 = base_vert;
        pt.tex_id = tex;
        mesh.points.push_back(pt);
    } else if (prim.type == PolyType::LineF) {
        Line ln;
        ln.v0 = base_vert;
        ln.v1 = (prim.n_verts >= 2) ? base_vert + 1 : base_vert;
        ln.tex_id = tex;
        mesh.lines.push_back(ln);
    } else {
        // Triangle fan: (v0, vi, vi+1) for i in [1, n-1)
        for (int i = 1; i < prim.n_verts - 1; ++i) {
            Triangle tri;
            tri.v0 = base_vert;
            tri.v1 = base_vert + static_cast<uint32_t>(i);
            tri.v2 = base_vert + static_cast<uint32_t>(i + 1);
            tri.tex_id = tex;
            mesh.triangles.push_back(tri);
        }
    }
}

} // namespace f4::models::detail

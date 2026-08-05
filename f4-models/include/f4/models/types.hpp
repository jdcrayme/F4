// f4-models/include/f4/models/types.hpp
//
// Core enums and math types for the Falcon 4.0 3D model system.
// Engine-agnostic — no rendering or GPU dependencies.
//
// References:
//   FreeFalcon: src/graphics/include/bspnodes.h  (BNodeType)
//   FreeFalcon: src/graphics/include/polylib.h   (PpolyType)
//   FreeFalcon: src/graphics/dxengine/dxdefines.h (DX types)

#pragma once

#include <cstdint>

namespace f4::models {

// ── BSP Node Types ────────────────────────────────────────────────────────
// Tag codes stored in the BSP tag list. Each code determines which
// subclass constructor to invoke when reading the flat node buffer.

enum class BspNodeType : int8_t {
    BNode              = 0,  // abstract base (sibling only)
    BSubTree           = 1,  // subtree with coords/normals
    BRoot              = 2,  // root of an LOD tree
    BSlotNode          = 3,  // slot attachment point
    BDofNode           = 4,  // degree of freedom (rotation/translation)
    BSwitchNode        = 5,  // visibility switch
    BSplitterNode      = 6,  // BSP splitting plane
    BPrimitiveNode     = 7,  // leaf with a Prim
    BLitPrimitiveNode  = 8,  // leaf with front + back lit Poly
    BCulledPrimitiveNode = 9,  // leaf with back-face-culled Poly
    BSpecialXform      = 10, // billboard/tree transform
    BLightStringNode   = 11, // light string point
    BTransNode         = 12, // translator node (DOF-driven)
    BScaleNode         = 13, // scale node (DOF-driven)
    BXDofNode          = 14, // extended DOF (min/max/mult/flags)
    BXSwitchNode       = 15, // extended switch (flags)
    BRenderControlNode = 16, // render control (Z-bias etc.)
    Unknown            = -1,
};

/// Number of distinct BSP node type codes (0..16).
constexpr int BSP_NODE_TYPE_COUNT = 17;

/// Get the name of a BSP node type (e.g. "BRoot").
[[nodiscard]] const char* bsp_node_type_name(BspNodeType t) noexcept;

// ── LOD Format ────────────────────────────────────────────────────────────

enum class LodFormat : uint8_t {
    Bsp = 0,  // classic BSP (tag list + flat node buffer)
    Dx  = 1,  // DX engine (DxDbHeader + DX node stream)
};

// ── Transform Types ───────────────────────────────────────────────────────

enum class TransformType : uint8_t {
    Normal   = 0,
    Billboard = 1,
    Tree     = 2,
};

// ── Render Control Types ──────────────────────────────────────────────────

enum class RenderControlType : uint8_t {
    NoOp  = 0,
    ZBias = 1,
};

// ── Polygon Primitive Types ───────────────────────────────────────────────
// From FreeFalcon's PpolyType enum in polylib.h. Determines vertex
// attributes (color, normal, texture, alpha, etc.).

enum class PolyType : uint8_t {
    PointF   = 0,
    LineF    = 1,
    F        = 2,   // flat-shaded
    FL       = 3,   // flat-shaded + lit
    G        = 4,   // Gouraud-shaded
    GL       = 5,   // Gouraud + lit
    Tex      = 6,   // textured
    TexL     = 7,   // textured + lit
    TexG     = 8,   // textured Gouraud
    TexGL    = 9,   // textured Gouraud + lit
    CTex     = 10,  // chromakey textured
    CTexL    = 11,
    CTexG    = 12,
    CTexGL   = 13,
    AF       = 14,  // alpha flat
    AFL      = 15,
    AG       = 16,  // alpha Gouraud
    AGL      = 17,
    ATex     = 18,  // alpha textured
    ATexL    = 19,
    ATexG    = 20,
    ATexGL   = 21,
    CATex    = 22,  // chromakey alpha textured
    CATexL   = 23,
    CATexG   = 24,
    CATexGL  = 25,
    BAptTex  = 26,  // apt textured
    Unknown  = 27,
};

/// Get the name of a polygon type.
[[nodiscard]] const char* poly_type_name(PolyType t) noexcept;

// ── Math Types ────────────────────────────────────────────────────────────

struct Vec3 {
    float x = 0, y = 0, z = 0;

    [[nodiscard]] bool operator==(const Vec3& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct Mat3x3 {
    // Row-major: m[row][col]
    float m[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
};

struct Plane {
    // Plane equation: a*x + b*y + c*z + d > 0
    float a = 0, b = 0, c = 0, d = 0;
};

struct TexCoord {
    float u = 0, v = 0;
};

// ── Node Index ────────────────────────────────────────────────────────────
// Offsets/indices into the flat node array. -1 means null (no node).

using NodeIdx = int32_t;
constexpr NodeIdx NULL_NODE = -1;

} // namespace f4::models

// f4-models/include/f4/models/bsp_node.hpp
//
// BSP tree node — flat representation of one node in a classic BSP tree.
// All nodes are stored in a contiguous array (BspTree::nodes) and
// reference each other by index (NodeIdx). This mirrors the on-disk
// format where pointers are byte offsets into a flat buffer.
//
// Design: "fat struct" — one struct with all possible fields.
// Only the fields relevant to the node's BspNodeType are valid.
// This avoids inheritance/virtual overhead and is natural for the
// binary format where we read fields based on the tag code.
//
// References:
//   FreeFalcon: src/graphics/include/bspnodes.h (class hierarchy)
//   FreeFalcon: src/graphics/bsputil/bspnodewriter.cpp (disk layout)

#pragma once

#include <f4/models/types.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace f4::models {

// ── BSP Node ──────────────────────────────────────────────────────────────

struct BspNode {
    BspNodeType type = BspNodeType::BNode;

    // ── Common (all nodes) ────────────────────────────────────────────
    NodeIdx sibling = NULL_NODE;  ///< next sibling in linked list

    // ── BSubTree / BRoot / BDofNode / BXDofNode / BTransNode / BScaleNode ──
    NodeIdx subtree = NULL_NODE;  ///< child subtree root
    int32_t n_coords = 0;         ///< number of coordinate vertices
    int32_t n_dynamic_coords = 0; ///< number of dynamic vertices
    int32_t dynamic_coord_offset = 0; ///< offset into dynamic coord array
    int32_t n_normals = 0;        ///< number of normal vectors
    int32_t coords_offset = 0;    ///< byte offset into coords pool
    int32_t normals_offset = 0;   ///< byte offset into normals pool

    // ── BRoot specific ────────────────────────────────────────────────
    int32_t n_tex_ids = 0;        ///< number of texture IDs
    int32_t tex_ids_offset = 0;   ///< byte offset into tex ID pool
    int32_t script_number = 0;    ///< script index (0 = none)

    // ── BSlotNode specific ────────────────────────────────────────────
    Mat3x3 slot_rotation = {};    ///< slot orientation matrix
    Vec3   slot_origin = {};      ///< slot position
    int32_t slot_number = -1;     ///< which slot index

    // ── BSplitterNode specific ────────────────────────────────────────
    Plane  splitter_plane = {};   ///< splitting plane equation
    NodeIdx front = NULL_NODE;    ///< front subtree
    NodeIdx back  = NULL_NODE;    ///< back subtree

    // ── BSwitchNode / BXSwitchNode specific ──────────────────────────
    int32_t switch_number = -1;   ///< which switch index
    int32_t n_children = 0;       ///< number of switch children
    int32_t switch_children_offset = 0; ///< offset into switch-children array
    int32_t switch_flags = 0;     ///< XSWT_REVERSED_EFFECT = (1<<0)

    // ── BDofNode / BXDofNode / BTransNode / BScaleNode specific ──────
    int32_t dof_number = -1;      ///< which DOF index
    Mat3x3 dof_rotation = {};     ///< rotation into parent coordinate system
    Vec3   dof_translation = {};  ///< translation offset
    float  dof_min = 0;           ///< DOF range minimum
    float  dof_max = 0;           ///< DOF range maximum
    float  dof_multiplier = 1;    ///< DOF scaling multiplier
    float  dof_future = 0;        ///< reserved / future use
    int32_t dof_flags = 0;        ///< XDOF flags (NEGATE, MINMAX, SUBRANGE, ISDOF)
    Vec3   scale = {1, 1, 1};     ///< scale factors (BScaleNode)

    // ── BPrimitiveNode / BLitPrimitiveNode / BCulledPrimitiveNode ─────
    int32_t prim_offset = -1;     ///< byte offset to Prim/Poly data
    int32_t back_poly_offset = -1;///< byte offset to back-facing Poly (BLitPrimitiveNode)

    // ── BSpecialXform specific ────────────────────────────────────────
    TransformType transform_type = TransformType::Normal;

    // ── BLightStringNode specific ─────────────────────────────────────
    Plane  light_dir = {};        ///< direction for front/back selection
    int32_t rgba_front = 0;       ///< front color index
    int32_t rgba_back = 0;        ///< back color index

    // ── BRenderControlNode specific ───────────────────────────────────
    RenderControlType control_type = RenderControlType::NoOp;
    std::array<int32_t, 4> i_arg = {};
    std::array<float, 4> f_arg = {};
};

// ── BSP Tree ──────────────────────────────────────────────────────────────
/// Complete parsed BSP tree with all shared data pools.

struct BspTree {
    /// Flat node array. Node 0 is the root. Children/siblings are
    /// referenced by index into this array.
    std::vector<BspNode> nodes;

    /// Tag list — one BspNodeType per node, in disk order.
    /// nodes[i] has type tags[i]. Useful for debugging.
    std::vector<BspNodeType> tags;

    /// Shared coordinate pool (all Vec3 points referenced by nodes).
    std::vector<Vec3> coords;

    /// Shared normal pool (all Vec3 normals referenced by nodes).
    std::vector<Vec3> normals;

    /// Shared texture ID pool (int per tex ID).
    std::vector<int32_t> tex_ids;

    /// Switch children — flat array of NodeIdx.
    /// BSwitchNode::switch_children_offset indexes into this.
    std::vector<NodeIdx> switch_children;

    /// NodeTreeData buffer (nodes + shared pools, AFTER tag count + tag list).
    /// ALL on-disk offsets in the BSP tree (subtree, sibling, front, back,
    /// prim_offset, xyz, rgba, uv, coords, normals, tex_ids) are relative
    /// to byte 0 of this buffer. The poly_parser and geometry_extractor
    /// use this buffer directly with unadjusted offsets.
    std::vector<uint8_t> lod_buffer;

    /// Total tag count (from the BSP header).
    int32_t tag_count = 0;

    /// Byte offset where node data starts (after tag list).
    int32_t data_start = 0;

    /// Total size of the BSP record in bytes.
    int32_t data_size = 0;
};

// ── DX Node Types ─────────────────────────────────────────────────────────
// Minimal representation of the DX engine format.
// Full DX parsing is deferred — this stores the raw DX node stream
// for tools that want to process it.

enum class DxItemType : uint8_t {
    Root     = 0,
    Surface  = 1,
    Material = 2,
    Texture  = 3,
    Dof      = 4,
    EndDof   = 5,
    Slot     = 6,
    Switch   = 7,
    Light    = 8,
    ModelEnd = 9,
};

/// DX format header — prefix of every LOD record in KoreaObj.DXL.
struct DxHeader {
    uint32_t version = 0;
    uint32_t id = 0;
    uint32_t vb_class = 0;
    uint32_t model_size = 0;
    uint32_t n_vertices = 0;
    uint32_t pool_size = 0;
    uint32_t vertex_pool_offset = 0;
    uint32_t n_nodes = 0;
    uint32_t n_lights = 0;
    uint32_t lights_pool_offset = 0;
    uint32_t n_textures = 0;
    std::vector<uint32_t> texture_ids;
};

} // namespace f4::models

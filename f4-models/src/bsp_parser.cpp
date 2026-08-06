// f4-models/src/bsp_parser.cpp
//
// Classic BSP tree parser — walks the tree using stored byte offsets
// (like FreeFalcon's RestorePointers), NOT sequential layout.
//
// Binary layout of a BSP LOD record:
//
//   [0..3]     uint32  tagListLength     — number of tags
//   [4..4+N*4] int32[tagListLength]     — tag list (BNodeType codes)
//   [data_start..] nodeTreeData         — nodes + shared data
//
// FreeFalcon strips tagCount+tagList FIRST, then sets base =
// nodeTreeData. ALL on-disk offsets (subtree, sibling, front, back,
// prim_offset, coords_offset, normals_offset, tex_ids_offset,
// switch_children_offset, xyz, rgba, uv, I) are byte offsets
// relative to the START of nodeTreeData (NOT the LOD record start).
//
// Nodes are at ARBITRARY positions within nodeTreeData. The tag list
// gives the TYPE of each node in tree-walk order, but byte positions
// come from the stored offset values in each node's fields.
//
// We walk the tree starting at offset 0 (root), consuming tags as we
// go, and build a flat node array indexed by discovery order.
//
// References:
//   FreeFalcon: src/graphics/texture/objectlod.cpp (LoaderCallBack)
//   FreeFalcon: src/graphics/bsplib/bspnodes.cpp (RestorePointers)

#include "bsp_parser.hpp"
#include "bin_reader.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace f4::models::detail {

namespace {

// ── Node sizes on disk (bytes, including 4-byte vtable) ─────────────────
// Determined from FreeFalcon class definitions + sizeof on 32-bit MSVC.

constexpr int SZ_BNODE = 8;               // vtable(4) + sibling(4)
constexpr int SZ_BSUBTREE = 36;           // + pC(4)+nC(4)+nDC(4)+DCO(4)+pN(4)+nN(4)+sub(4)
constexpr int SZ_BROOT = 48;             // BSubTree + pT(4)+nT(4)+scr(4)
constexpr int SZ_BSLOTNODE = 60;          // vtable+sib + rot(36)+org(12)+slot(4)
constexpr int SZ_BDOFNODE = 88;           // BSubTree + dof(4)+rot(36)+trans(12)
constexpr int SZ_BSPLITTERNODE = 32;      // vtable+sib + ABCD(16)+f(4)+b(4)
constexpr int SZ_BSWITCHNODE = 20;        // vtable+sib + sw(4)+nc(4)+sub(4)
constexpr int SZ_BXSWITCHNODE = 24;       // vtable+sib + sw(4)+fl(4)+nc(4)+sub(4)
constexpr int SZ_BPRIMITIVENODE = 12;     // vtable?+?sb + prim(4)
constexpr int SZ_BLITPRIMITIVENODE = 16;  // vtable+sib + poly(4)+bpoly(4)
constexpr int SZ_BCULLEDPRIMITIVENODE = 12; // vtable+sib + poly(4)
constexpr int SZ_BXDOFNODE = 108;         // BSubTree + dof(4)+min(4)+max(4)+mult(4)+fut(4)+fl(4)+rot(36)+trans(12)
constexpr int SZ_BTRANSNODE = 72;         // BSubTree + dof(4)+min(4)+max(4)+mult(4)+fut(4)+fl(4)+trans(12)
constexpr int SZ_BSCALENODE = 84;         // BSubTree + dof(4)+min(4)+max(4)+mult(4)+fut(4)+fl(4)+scale(12)+trans(12)
constexpr int SZ_BSPECIALXFORM = 24;      // vtable+sib + pC(4)+nC(4)+type(4)+sub(4)
constexpr int SZ_BLIGHTSTRINGNODE = 36;   // BPrimNode(12) + ABCD(16)+rf(4)+rb(4) = 36
constexpr int SZ_BRENDERCONTROLNODE = 44; // vtable+sib + ctrl(4)+IArg[4](16)+FArg[4](16)

} // anonymous namespace

// ── Tree-walk parser ────────────────────────────────────────────────────

namespace {

/// Context for the offset-based tree walk.
struct WalkCtx {
    const uint8_t* base;        ///< start of nodeTreeData
    std::size_t base_size;      ///< size of nodeTreeData
    const BspNodeType* tags;    ///< tag list
    int tag_count;              ///< number of tags
    int tag_pos;                ///< current position in tag list

    BspTree& tree;

    // Map: byte offset → node index in tree.nodes
    std::unordered_map<int32_t, NodeIdx> offset_map;

    // Track visited offsets to prevent infinite loops
    std::unordered_map<int32_t, int> visit_count;

    std::string& err;

    WalkCtx(const uint8_t* b, std::size_t bs,
            const BspNodeType* t, int tc,
            BspTree& tr, std::string& e)
        : base(b), base_size(bs), tags(t), tag_count(tc),
          tag_pos(0), tree(tr), err(e) {}
};

/// Read a raw int32 from nodeTreeData at a given offset.
[[nodiscard]] bool read_i32(WalkCtx& ctx, int32_t offset, int32_t& out) {
    if (offset < 0 || static_cast<std::size_t>(offset) + 4 > ctx.base_size) return false;
    std::memcpy(&out, ctx.base + offset, 4);
    return true;
}

/// Read a raw float from nodeTreeData at a given offset.
[[nodiscard]] bool read_f32(WalkCtx& ctx, int32_t offset, float& out) {
    if (offset < 0 || static_cast<std::size_t>(offset) + 4 > ctx.base_size) return false;
    std::memcpy(&out, ctx.base + offset, 4);
    return true;
}

/// Walk one node at the given byte offset. Returns the node index.
NodeIdx walk_node(WalkCtx& ctx, int32_t offset) {
    if (offset < 0 || static_cast<std::size_t>(offset) >= ctx.base_size) {
        return NULL_NODE;
    }

    // Check for existing mapping (shared nodes or revisits)
    auto it = ctx.offset_map.find(offset);
    if (it != ctx.offset_map.end()) {
        return it->second;
    }

    // Cycle detection: allow up to 2 visits (some nodes are shared)
    auto& vc = ctx.visit_count[offset];
    if (vc >= 2) return NULL_NODE;
    vc++;

    // Consume next tag
    if (ctx.tag_pos >= ctx.tag_count) {
        ctx.err = "tag list exhausted at offset " + std::to_string(offset);
        return NULL_NODE;
    }
    BspNodeType tag = ctx.tags[ctx.tag_pos++];

    // Create node
    NodeIdx idx = static_cast<NodeIdx>(ctx.tree.nodes.size());
    ctx.offset_map[offset] = idx;

    ctx.tree.nodes.emplace_back();
    auto& node = ctx.tree.nodes.back();
    node.type = tag;

    // Read raw bytes at base+offset using BinReader
    // Skip the 4-byte vtable pointer
    BinReader r{ctx.base + offset, ctx.base_size - static_cast<std::size_t>(offset)};
    uint32_t vtable = 0;
    if (!r.read(vtable)) { ctx.err = "vtable truncated"; return idx; }

    // Read sibling offset (common to all nodes)
    int32_t sibling_off = -1;
    if (!r.read(sibling_off)) { ctx.err = "sibling truncated"; return idx; }

    // Parse type-specific fields and walk children
    switch (tag) {

    case BspNodeType::BNode:
        break;

    case BspNodeType::BSubTree: {
        int32_t coords_off, normals_off, subtree_off;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(node.n_dynamic_coords)) goto trunc;
        if (!r.read(node.dynamic_coord_offset)) goto trunc;
        if (!r.read(normals_off)) goto trunc;
        if (!r.read(node.n_normals)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        node.normals_offset = normals_off;
        // Walk subtree
        if (subtree_off >= 0) {
            node.subtree = walk_node(ctx, subtree_off);
        }
        break;
    }

    case BspNodeType::BRoot: {
        int32_t coords_off, normals_off, subtree_off;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(node.n_dynamic_coords)) goto trunc;
        if (!r.read(node.dynamic_coord_offset)) goto trunc;
        if (!r.read(normals_off)) goto trunc;
        if (!r.read(node.n_normals)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        node.normals_offset = normals_off;
        int32_t tex_off;
        if (!r.read(tex_off)) goto trunc;
        if (!r.read(node.n_tex_ids)) goto trunc;
        if (!r.read(node.script_number)) goto trunc;
        node.tex_ids_offset = tex_off;
        // Walk subtree
        if (subtree_off >= 0) {
            node.subtree = walk_node(ctx, subtree_off);
        }
        break;
    }

    case BspNodeType::BSlotNode: {
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                if (!r.read(node.slot_rotation.m[row][col])) goto trunc;
        if (!r.read(node.slot_origin.x)) goto trunc;
        if (!r.read(node.slot_origin.y)) goto trunc;
        if (!r.read(node.slot_origin.z)) goto trunc;
        if (!r.read(node.slot_number)) goto trunc;
        // BSlotNode has NO subTree — slots are resolved externally
        break;
    }

    case BspNodeType::BDofNode: {
        int32_t coords_off, normals_off, subtree_off;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(node.n_dynamic_coords)) goto trunc;
        if (!r.read(node.dynamic_coord_offset)) goto trunc;
        if (!r.read(normals_off)) goto trunc;
        if (!r.read(node.n_normals)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        node.normals_offset = normals_off;
        if (!r.read(node.dof_number)) goto trunc;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                if (!r.read(node.dof_rotation.m[row][col])) goto trunc;
        if (!r.read(node.dof_translation.x)) goto trunc;
        if (!r.read(node.dof_translation.y)) goto trunc;
        if (!r.read(node.dof_translation.z)) goto trunc;
        if (subtree_off >= 0) node.subtree = walk_node(ctx, subtree_off);
        break;
    }

    case BspNodeType::BXDofNode: {
        int32_t coords_off, normals_off, subtree_off;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(node.n_dynamic_coords)) goto trunc;
        if (!r.read(node.dynamic_coord_offset)) goto trunc;
        if (!r.read(normals_off)) goto trunc;
        if (!r.read(node.n_normals)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        node.normals_offset = normals_off;
        if (!r.read(node.dof_number)) goto trunc;
        if (!r.read(node.dof_min)) goto trunc;
        if (!r.read(node.dof_max)) goto trunc;
        if (!r.read(node.dof_multiplier)) goto trunc;
        if (!r.read(node.dof_future)) goto trunc;
        if (!r.read(node.dof_flags)) goto trunc;
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                if (!r.read(node.dof_rotation.m[row][col])) goto trunc;
        if (!r.read(node.dof_translation.x)) goto trunc;
        if (!r.read(node.dof_translation.y)) goto trunc;
        if (!r.read(node.dof_translation.z)) goto trunc;
        if (subtree_off >= 0) node.subtree = walk_node(ctx, subtree_off);
        break;
    }

    case BspNodeType::BTransNode: {
        int32_t coords_off, normals_off, subtree_off;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(node.n_dynamic_coords)) goto trunc;
        if (!r.read(node.dynamic_coord_offset)) goto trunc;
        if (!r.read(normals_off)) goto trunc;
        if (!r.read(node.n_normals)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        node.normals_offset = normals_off;
        if (!r.read(node.dof_number)) goto trunc;
        if (!r.read(node.dof_min)) goto trunc;
        if (!r.read(node.dof_max)) goto trunc;
        if (!r.read(node.dof_multiplier)) goto trunc;
        if (!r.read(node.dof_future)) goto trunc;
        if (!r.read(node.dof_flags)) goto trunc;
        if (!r.read(node.dof_translation.x)) goto trunc;
        if (!r.read(node.dof_translation.y)) goto trunc;
        if (!r.read(node.dof_translation.z)) goto trunc;
        if (subtree_off >= 0) node.subtree = walk_node(ctx, subtree_off);
        break;
    }

    case BspNodeType::BScaleNode: {
        int32_t coords_off, normals_off, subtree_off;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(node.n_dynamic_coords)) goto trunc;
        if (!r.read(node.dynamic_coord_offset)) goto trunc;
        if (!r.read(normals_off)) goto trunc;
        if (!r.read(node.n_normals)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        node.normals_offset = normals_off;
        if (!r.read(node.dof_number)) goto trunc;
        if (!r.read(node.dof_min)) goto trunc;
        if (!r.read(node.dof_max)) goto trunc;
        if (!r.read(node.dof_multiplier)) goto trunc;
        if (!r.read(node.dof_future)) goto trunc;
        if (!r.read(node.dof_flags)) goto trunc;
        if (!r.read(node.scale.x)) goto trunc;
        if (!r.read(node.scale.y)) goto trunc;
        if (!r.read(node.scale.z)) goto trunc;
        if (!r.read(node.dof_translation.x)) goto trunc;
        if (!r.read(node.dof_translation.y)) goto trunc;
        if (!r.read(node.dof_translation.z)) goto trunc;
        if (subtree_off >= 0) node.subtree = walk_node(ctx, subtree_off);
        break;
    }

    case BspNodeType::BSplitterNode: {
        int32_t front_off, back_off;
        if (!r.read(node.splitter_plane.a)) goto trunc;
        if (!r.read(node.splitter_plane.b)) goto trunc;
        if (!r.read(node.splitter_plane.c)) goto trunc;
        if (!r.read(node.splitter_plane.d)) goto trunc;
        if (!r.read(front_off)) goto trunc;
        if (!r.read(back_off)) goto trunc;
        if (front_off >= 0) node.front = walk_node(ctx, front_off);
        if (back_off  >= 0) node.back  = walk_node(ctx, back_off);
        break;
    }

    case BspNodeType::BSwitchNode: {
        // On-disk BSwitchNode layout (from FreeFalcon bspnodes.h):
        //   vtable(4) + sibling(4) + switch_number(4) + n_children(4)
        //            + child_offsets_ptr(4)
        //
        // The child_offsets_ptr points to an int32 array of n_children
        // byte offsets. We read the pointer, then walk each child inline
        // so they end up in tree.nodes / offset_map.
        //
        // Some BSwitchNode variants store n_children=0 but still have
        // children (the count field is unreliable for certain modded
        // files). We handle this with a buffer-wide vtable scan in a
        // second pass below (see parse_bsp_tree post-pass).
        int32_t children_off;
        if (!r.read(node.switch_number)) goto trunc;
        if (!r.read(node.n_children)) goto trunc;
        if (!r.read(children_off)) goto trunc;
        node.switch_children_offset = children_off;

        // Walk each child NOW so they end up in tree.nodes and offset_map.
        if (children_off >= 0 && node.n_children > 0 && node.n_children < 64) {
            auto base = static_cast<std::size_t>(children_off);
            auto count = static_cast<std::size_t>(node.n_children);
            if (base + count * sizeof(int32_t) <= ctx.base_size) {
                for (std::size_t k = 0; k < count; ++k) {
                    int32_t child_off = -1;
                    std::memcpy(&child_off,
                                ctx.base + base + k * sizeof(int32_t),
                                sizeof(int32_t));
                    if (child_off >= 0) {
                        (void)walk_node(ctx, child_off);
                    }
                }
            }
        }
        break;
    }

    case BspNodeType::BXSwitchNode: {
        int32_t children_off;
        if (!r.read(node.switch_number)) goto trunc;
        if (!r.read(node.switch_flags)) goto trunc;
        if (!r.read(node.n_children)) goto trunc;
        if (!r.read(children_off)) goto trunc;
        node.switch_children_offset = children_off;

        if (children_off >= 0 && node.n_children > 0 && node.n_children < 64) {
            auto base = static_cast<std::size_t>(children_off);
            auto count = static_cast<std::size_t>(node.n_children);
            if (base + count * sizeof(int32_t) <= ctx.base_size) {
                for (std::size_t k = 0; k < count; ++k) {
                    int32_t child_off = -1;
                    std::memcpy(&child_off,
                                ctx.base + base + k * sizeof(int32_t),
                                sizeof(int32_t));
                    if (child_off >= 0) {
                        (void)walk_node(ctx, child_off);
                    }
                }
            }
        }
        break;
    }

    case BspNodeType::BPrimitiveNode: {
        if (!r.read(node.prim_offset)) goto trunc;
        break;
    }

    case BspNodeType::BLitPrimitiveNode: {
        if (!r.read(node.prim_offset)) goto trunc;
        if (!r.read(node.back_poly_offset)) goto trunc;
        break;
    }

    case BspNodeType::BCulledPrimitiveNode: {
        if (!r.read(node.prim_offset)) goto trunc;
        break;
    }

    case BspNodeType::BSpecialXform: {
        int32_t coords_off, subtree_off, xform_type;
        if (!r.read(coords_off)) goto trunc;
        if (!r.read(node.n_coords)) goto trunc;
        if (!r.read(xform_type)) goto trunc;
        if (!r.read(subtree_off)) goto trunc;
        node.coords_offset = coords_off;
        if (xform_type >= 0 && xform_type <= 2)
            node.transform_type = static_cast<TransformType>(xform_type);
        if (subtree_off >= 0) node.subtree = walk_node(ctx, subtree_off);
        break;
    }

    case BspNodeType::BLightStringNode: {
        if (!r.read(node.prim_offset)) goto trunc;
        if (!r.read(node.light_dir.a)) goto trunc;
        if (!r.read(node.light_dir.b)) goto trunc;
        if (!r.read(node.light_dir.c)) goto trunc;
        if (!r.read(node.light_dir.d)) goto trunc;
        if (!r.read(node.rgba_front)) goto trunc;
        if (!r.read(node.rgba_back)) goto trunc;
        break;
    }

    case BspNodeType::BRenderControlNode: {
        int32_t ctrl;
        if (!r.read(ctrl)) goto trunc;
        if (ctrl == 1) node.control_type = RenderControlType::ZBias;
        for (int k = 0; k < 4; ++k)
            if (!r.read(node.i_arg[k])) goto trunc;
        for (int k = 0; k < 4; ++k)
            if (!r.read(node.f_arg[k])) goto trunc;
        break;
    }

    default:
        break;
    }

    // Convert sibling offset to index by walking
    if (sibling_off >= 0) {
        node.sibling = walk_node(ctx, sibling_off);
    }

    return idx;

trunc:
    ctx.err = "truncated at offset " + std::to_string(offset) +
              " (type " + std::to_string(static_cast<int>(tag)) + ")";
    return idx;
}

} // anonymous namespace

bool parse_bsp_tree(
    const uint8_t* data, std::size_t size,
    BspTree& tree,
    std::string& err)
{
    BinReader r{data, size};

    // Read tag count
    uint32_t tag_count = 0;
    if (!r.read(tag_count)) {
        err = "BSP data too small for tag count";
        return false;
    }
    if (tag_count > 100000) {
        err = "BSP tag count unreasonably large: " + std::to_string(tag_count);
        return false;
    }
    tree.tag_count = static_cast<int32_t>(tag_count);

    // Read tag list
    tree.tags.resize(tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        int32_t tag_val;
        if (!r.read(tag_val)) {
            err = "BSP truncated in tag list at tag " + std::to_string(i);
            return false;
        }
        if (tag_val >= 0 && tag_val < BSP_NODE_TYPE_COUNT) {
            tree.tags[i] = static_cast<BspNodeType>(tag_val);
        } else {
            tree.tags[i] = BspNodeType::Unknown;
        }
    }

    // nodeTreeData starts after tagCount + tagList
    tree.data_start = static_cast<int32_t>(r.pos);
    const uint8_t* node_data = data + r.pos;
    std::size_t node_data_size = size - r.pos;
    tree.data_size = static_cast<int32_t>(node_data_size);

    // Store nodeTreeData as lod_buffer (all offsets are relative to this)
    tree.lod_buffer.assign(node_data, node_data + node_data_size);

    // Reserve node capacity to prevent vector reallocation
    // during recursive walk (which would invalidate references).
    // At most tag_count nodes can be created.
    tree.nodes.reserve(tree.tag_count);

    // Walk the tree starting at offset 0
    WalkCtx ctx(node_data, node_data_size,
                tree.tags.data(), tree.tag_count,
                tree, err);

    NodeIdx root_idx = walk_node(ctx, 0);
    if (root_idx < 0) {
        if (err.empty()) err = "failed to walk BSP tree";
        return false;
    }

    // Check we consumed all tags (or close to it)
    // Some tags may be unused if the tree has shared nodes
    // but we should have consumed most of them

    // ── Extract shared data pools from BRoot/BSubTree nodes ─────────
    // The first BRoot with n_coords > 0 populates tree.coords (the
    // "default" pool used when no subtree-specific pool is active).
    // Nested BSubTree nodes inside BDofNode/BSwitchNode/etc. carry their
    // own pools — those are read on-demand by the geometry_extractor's
    // ActivePool stack, NOT copied here (they'd overwrite the root pool).
    for (const auto& node : tree.nodes) {
        if (node.type == BspNodeType::BRoot && node.n_coords > 0) {
            if (node.coords_offset >= 0) {
                auto off = static_cast<std::size_t>(node.coords_offset);
                if (off + static_cast<std::size_t>(node.n_coords) * sizeof(Vec3) <= node_data_size) {
                    if (tree.coords.empty()) {
                        tree.coords.resize(node.n_coords);
                        std::memcpy(tree.coords.data(), node_data + off,
                                    node.n_coords * sizeof(Vec3));
                    }
                }
            }
            if (node.n_normals > 0 && node.normals_offset >= 0) {
                auto off = static_cast<std::size_t>(node.normals_offset);
                if (off + static_cast<std::size_t>(node.n_normals) * sizeof(Vec3) <= node_data_size) {
                    if (tree.normals.empty()) {
                        tree.normals.resize(node.n_normals);
                        std::memcpy(tree.normals.data(), node_data + off,
                                    node.n_normals * sizeof(Vec3));
                    }
                }
            }
            if (node.n_tex_ids > 0 && node.tex_ids_offset >= 0) {
                auto off = static_cast<std::size_t>(node.tex_ids_offset);
                if (off + static_cast<std::size_t>(node.n_tex_ids) * sizeof(int32_t) <= node_data_size) {
                    if (tree.tex_ids.empty()) {
                        tree.tex_ids.resize(node.n_tex_ids);
                        std::memcpy(tree.tex_ids.data(), node_data + off,
                                    node.n_tex_ids * sizeof(int32_t));
                    }
                }
            }
        }
    }

    // ── Post-pass: resolve switch children + scan for unvisited nodes ─────
    //
    // 1. Resolve switch children byte offsets to NodeIdx values via offset_map.
    //    (The walk above already visited the children, so they're in the map.)
    //
    // 2. Buffer-wide vtable scan: some BSwitchNode variants store n_children=0
    //    but still have children. The walk above skipped them. We scan the
    //    entire lod_buffer for 4-byte values that look like byte offsets to
    //    nodes (i.e., the target starts with a vtable in 0x004651xx range).
    //    Any unvisited nodes found are walked, which may consume remaining
    //    tags and unlock geometry that was previously missed.

    // Pass 1: resolve switch children via offset_map
    for (auto& node : tree.nodes) {
        if ((node.type == BspNodeType::BSwitchNode ||
             node.type == BspNodeType::BXSwitchNode) &&
            node.n_children > 0 && node.switch_children_offset >= 0)
        {
            auto off = static_cast<std::size_t>(node.switch_children_offset);
            auto count = static_cast<std::size_t>(node.n_children);
            if (off + count * sizeof(int32_t) <= node_data_size) {
                std::vector<int32_t> child_offsets(count);
                std::memcpy(child_offsets.data(), node_data + off,
                            count * sizeof(int32_t));
                auto base = tree.switch_children.size();
                tree.switch_children.resize(base + count);
                for (std::size_t k = 0; k < count; ++k) {
                    auto child_off = child_offsets[k];
                    auto cit = ctx.offset_map.find(child_off);
                    if (cit != ctx.offset_map.end()) {
                        tree.switch_children[base + k] = cit->second;
                    } else {
                        tree.switch_children[base + k] = NULL_NODE;
                    }
                }
                node.switch_children_offset = static_cast<int32_t>(base);
            } else {
                node.switch_children_offset = -1;
                node.n_children = 0;
            }
        }
    }

    // Pass 2: buffer-wide vtable scan for unvisited nodes.
    // This catches switch children that were missed because n_children=0
    // or children_off was garbage. We scan every 4-byte-aligned position
    // in the buffer; if the value looks like a valid byte offset AND the
    // target starts with a vtable pattern AND the target hasn't been
    // visited yet, we walk it.
    //
    // This is O(buffer_size / 4) but only runs once per LOD parse, and
    // the walk_node calls are guarded by offset_map (already-visited
    // nodes are skipped).
    {
        const uint8_t* buf = node_data;
        std::size_t buf_sz = node_data_size;
        int tags_before = ctx.tag_pos;

        for (std::size_t i = 0; i + 4 <= buf_sz; i += 4) {
            int32_t off = -1;
            std::memcpy(&off, buf + i, 4);
            // Quick filter: offset must be within buffer and 4-byte aligned
            if (off < 0 || static_cast<std::size_t>(off) + 4 > buf_sz)
                continue;
            if (off % 4 != 0) continue;
            // Check if target looks like a node (starts with vtable)
            uint32_t target_vt = 0;
            std::memcpy(&target_vt, buf + off, 4);
            if (target_vt < 0x00460000 || target_vt > 0x00470000)
                continue;
            // Check if already visited
            if (ctx.offset_map.count(off) > 0) continue;
            // Walk it — this consumes a tag and populates the node
            if (ctx.tag_pos >= ctx.tag_count) break;
            (void)walk_node(ctx, static_cast<int32_t>(off));
        }
    }

    return true;
}

} // namespace f4::models::detail

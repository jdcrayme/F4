// f4-models/src/bsp_parser.cpp
//
// Classic BSP tree parser implementation.
//
// Binary layout of a BSP LOD record:
//
//   [0..3]     uint32  tagListLength     — number of tags
//   [4..4+N*4] int32[tagListLength]     — tag list (BNodeType codes)
//   [data_start..] node data buffer     — all nodes packed sequentially
//
// In the node data buffer, every node starts with a 4-byte vtable
// pointer (junk from 32-bit MSVC) that must be skipped. All
// "pointer" fields are stored as byte offsets relative to the start
// of the node data buffer. -1 means NULL.
//
// The tag list drives construction: tag[i] tells which subclass
// constructor to use for node i. Nodes are laid out sequentially
// in the same order as the tags.
//
// References:
//   FreeFalcon: src/graphics/bsplib/bspnodes.cpp (RestorePointers)
//   FreeFalcon: src/graphics/bsputil/bspnodewriter.cpp (WriteBspNodeRec)
//   f4-world-viewer: src/model_snapshot.cpp (BSP tag counting)

#include "bsp_parser.hpp"
#include "bin_reader.hpp"

#include <algorithm>
#include <cstring>

namespace f4::models::detail {

namespace {

// ── Node sizes on disk (bytes, excluding the 4-byte vtable prefix) ───────
// These are the sizes of each node type's data fields as stored on disk.
// The 4-byte vtable pointer at the start of each node is consumed but
// discarded.

// BSubTree (base for BRoot, BDofNode, BXDofNode, BTransNode, BScaleNode):
//   sibling(4) + pCoords(4) + nCoords(4) + nDynamicCoords(4) +
//   DynamicCoordOffset(4) + pNormals(4) + nNormals(4) + subTree(4)
//   = 32 bytes after vtable

// BRoot: BSubTree(32) + pTexIDs(4) + nTexIDs(4) + ScriptNumber(4) = 44
constexpr int SZ_BSUBTREE = 32;
constexpr int SZ_BROOT   = SZ_BSUBTREE + 12;

// BSlotNode: sibling(4) + rotation(36) + origin(12) + slotNumber(4) = 56
constexpr int SZ_BSLOTNODE = 56;

// BDofNode: BSubTree(32) + dofNumber(4) + rotation(36) + translation(12) = 84
constexpr int SZ_BDOFNODE = SZ_BSUBTREE + 52;

// BSplitterNode: sibling(4) + A(4) + B(4) + C(4) + D(4) + front(4) + back(4) = 28
constexpr int SZ_BSPLITTERNODE = 28;

// BSwitchNode: sibling(4) + switchNumber(4) + numChildren(4) + subTrees(4) = 16
constexpr int SZ_BSWITCHNODE = 16;

// BXSwitchNode: sibling(4) + switchNumber(4) + flags(4) + numChildren(4) + subTrees(4) = 20
constexpr int SZ_BXSWITCHNODE = 20;

// BPrimitiveNode: sibling(4) + prim(4) = 8
constexpr int SZ_BPRIMITIVENODE = 8;

// BLitPrimitiveNode: sibling(4) + poly(4) + backpoly(4) = 12
constexpr int SZ_BLITPRIMITIVENODE = 12;

// BCulledPrimitiveNode: sibling(4) + poly(4) = 8
constexpr int SZ_BCULLEDPRIMITIVENODE = 8;

// BXDofNode: BSubTree(32) + dofNumber(4) + min(4) + max(4) + mult(4) + future(4) +
//            flags(4) + rotation(36) + translation(12) = 104
constexpr int SZ_BXDOFNODE = SZ_BSUBTREE + 72;

// BTransNode: BSubTree(32) + dofNumber(4) + min(4) + max(4) + mult(4) + future(4) +
//             flags(4) + translation(12) = 68
constexpr int SZ_BTRANSNODE = SZ_BSUBTREE + 36;

// BScaleNode: BSubTree(32) + dofNumber(4) + min(4) + max(4) + mult(4) + future(4) +
//             flags(4) + scale(12) + translation(12) = 80
constexpr int SZ_BSCALENODE = SZ_BSUBTREE + 48;

// BSpecialXform: sibling(4) + pCoords(4) + nCoords(4) + type(4) + subTree(4) = 20
constexpr int SZ_BSPECIALXFORM = 20;

// BLightStringNode: BPrimitiveNode(8) + A(4) + B(4) + C(4) + D(4) +
//                    rgbaFront(4) + rgbaBack(4) = 32
constexpr int SZ_BLIGHTSTRINGNODE = 32;

// BRenderControlNode: sibling(4) + Control(4) + IArg[4](16) + FArg[4](16) = 40
constexpr int SZ_BRENDERCONTROLNODE = 40;

// BNode (abstract): sibling(4) only = 4
constexpr int SZ_BNODE = 4;

// ── Get the on-disk size of a node (including 4-byte vtable) ─────────────
[[nodiscard]] int node_disk_size(BspNodeType type) {
    switch (type) {
        case BspNodeType::BNode:              return 4 + SZ_BNODE;
        case BspNodeType::BSubTree:           return 4 + SZ_BSUBTREE;
        case BspNodeType::BRoot:              return 4 + SZ_BROOT;
        case BspNodeType::BSlotNode:          return 4 + SZ_BSLOTNODE;
        case BspNodeType::BDofNode:           return 4 + SZ_BDOFNODE;
        case BspNodeType::BSwitchNode:        return 4 + SZ_BSWITCHNODE;
        case BspNodeType::BSplitterNode:      return 4 + SZ_BSPLITTERNODE;
        case BspNodeType::BPrimitiveNode:     return 4 + SZ_BPRIMITIVENODE;
        case BspNodeType::BLitPrimitiveNode:  return 4 + SZ_BLITPRIMITIVENODE;
        case BspNodeType::BCulledPrimitiveNode: return 4 + SZ_BCULLEDPRIMITIVENODE;
        case BspNodeType::BSpecialXform:      return 4 + SZ_BSPECIALXFORM;
        case BspNodeType::BLightStringNode:   return 4 + SZ_BLIGHTSTRINGNODE;
        case BspNodeType::BTransNode:         return 4 + SZ_BTRANSNODE;
        case BspNodeType::BScaleNode:         return 4 + SZ_BSCALENODE;
        case BspNodeType::BXDofNode:          return 4 + SZ_BXDOFNODE;
        case BspNodeType::BXSwitchNode:       return 4 + SZ_BXSWITCHNODE;
        case BspNodeType::BRenderControlNode: return 4 + SZ_BRENDERCONTROLNODE;
        default:                              return 4 + SZ_BNODE; // fallback
    }
}

/// Convert a byte offset (relative to node data start) to a node index.
/// Returns NULL_NODE if offset is -1 or out of range.
[[nodiscard]] NodeIdx offset_to_idx(int32_t offset,
                                     const std::vector<std::size_t>& node_offsets)
{
    if (offset < 0) return NULL_NODE;
    auto abs = static_cast<std::size_t>(offset);
    // Binary search for the node at this offset
    auto it = std::lower_bound(node_offsets.begin(), node_offsets.end(), abs);
    if (it == node_offsets.end() || *it != abs) return NULL_NODE;
    return static_cast<NodeIdx>(it - node_offsets.begin());
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

    tree.data_start = static_cast<int32_t>(r.pos);
    tree.data_size = static_cast<int32_t>(size);

    // Parse nodes sequentially using the tag list
    // First pass: compute the byte offset of each node
    std::vector<std::size_t> node_offsets;
    node_offsets.reserve(tag_count);

    // Build a sub-reader for the node data portion
    const uint8_t* node_data = data + r.pos;
    std::size_t node_data_size = size - r.pos;

    // First pass: compute node offsets
    std::size_t cur_offset = 0;
    for (uint32_t i = 0; i < tag_count; ++i) {
        node_offsets.push_back(cur_offset);
        int sz = node_disk_size(tree.tags[i]);
        cur_offset += sz;
    }

    // Check we have enough data for all nodes
    if (cur_offset > node_data_size) {
        // Not enough data — parse as many nodes as we can
        // This can happen with corrupted data or slightly different formats
    }

    // Second pass: parse each node
    tree.nodes.resize(tag_count);
    BinReader nr{node_data, node_data_size};

    for (uint32_t i = 0; i < tag_count; ++i) {
        nr.seek(node_offsets[i]);

        auto& node = tree.nodes[i];
        node.type = tree.tags[i];

        // Every node starts with a 4-byte vtable pointer (skip it)
        uint32_t vtable = 0;
        if (!nr.read(vtable)) {
            err = "BSP truncated at vtable for node " + std::to_string(i);
            return false;
        }

        // Read sibling offset (common to all nodes)
        int32_t sibling_off = -1;
        if (!nr.read(sibling_off)) {
            err = "BSP truncated at sibling for node " + std::to_string(i);
            return false;
        }
        // Convert to index later (after all offsets known)

        // Parse type-specific fields
        switch (node.type) {
        case BspNodeType::BNode:
            // Just sibling — already read
            break;

        case BspNodeType::BSubTree: {
            // pCoords(4) + nCoords(4) + nDynamicCoords(4) + DynamicCoordOffset(4) +
            // pNormals(4) + nNormals(4) + subTree(4)
            int32_t coords_off, normals_off, subtree_off;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(node.n_dynamic_coords)) goto trunc;
            if (!nr.read(node.dynamic_coord_offset)) goto trunc;
            if (!nr.read(normals_off)) goto trunc;
            if (!nr.read(node.n_normals)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.normals_offset = normals_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            break;
        }

        case BspNodeType::BRoot: {
            // BSubTree fields first
            int32_t coords_off, normals_off, subtree_off;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(node.n_dynamic_coords)) goto trunc;
            if (!nr.read(node.dynamic_coord_offset)) goto trunc;
            if (!nr.read(normals_off)) goto trunc;
            if (!nr.read(node.n_normals)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.normals_offset = normals_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            // BRoot extras: pTexIDs(4) + nTexIDs(4) + ScriptNumber(4)
            int32_t tex_off;
            if (!nr.read(tex_off)) goto trunc;
            if (!nr.read(node.n_tex_ids)) goto trunc;
            if (!nr.read(node.script_number)) goto trunc;
            node.tex_ids_offset = tex_off;
            break;
        }

        case BspNodeType::BSlotNode: {
            // rotation (36 bytes) + origin (12 bytes) + slotNumber (4)
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    if (!nr.read(node.slot_rotation.m[row][col])) goto trunc;
            if (!nr.read(node.slot_origin.x)) goto trunc;
            if (!nr.read(node.slot_origin.y)) goto trunc;
            if (!nr.read(node.slot_origin.z)) goto trunc;
            if (!nr.read(node.slot_number)) goto trunc;
            break;
        }

        case BspNodeType::BDofNode: {
            // BSubTree fields
            int32_t coords_off, normals_off, subtree_off;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(node.n_dynamic_coords)) goto trunc;
            if (!nr.read(node.dynamic_coord_offset)) goto trunc;
            if (!nr.read(normals_off)) goto trunc;
            if (!nr.read(node.n_normals)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.normals_offset = normals_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            // DOF extras: dofNumber + rotation + translation
            if (!nr.read(node.dof_number)) goto trunc;
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    if (!nr.read(node.dof_rotation.m[row][col])) goto trunc;
            if (!nr.read(node.dof_translation.x)) goto trunc;
            if (!nr.read(node.dof_translation.y)) goto trunc;
            if (!nr.read(node.dof_translation.z)) goto trunc;
            break;
        }

        case BspNodeType::BXDofNode: {
            // BSubTree fields
            int32_t coords_off, normals_off, subtree_off;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(node.n_dynamic_coords)) goto trunc;
            if (!nr.read(node.dynamic_coord_offset)) goto trunc;
            if (!nr.read(normals_off)) goto trunc;
            if (!nr.read(node.n_normals)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.normals_offset = normals_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            // Extended DOF: dofNumber + min + max + multiplier + future + flags + rotation + translation
            if (!nr.read(node.dof_number)) goto trunc;
            if (!nr.read(node.dof_min)) goto trunc;
            if (!nr.read(node.dof_max)) goto trunc;
            if (!nr.read(node.dof_multiplier)) goto trunc;
            if (!nr.read(node.dof_future)) goto trunc;
            if (!nr.read(node.dof_flags)) goto trunc;
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    if (!nr.read(node.dof_rotation.m[row][col])) goto trunc;
            if (!nr.read(node.dof_translation.x)) goto trunc;
            if (!nr.read(node.dof_translation.y)) goto trunc;
            if (!nr.read(node.dof_translation.z)) goto trunc;
            break;
        }

        case BspNodeType::BTransNode: {
            // BSubTree fields
            int32_t coords_off, normals_off, subtree_off;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(node.n_dynamic_coords)) goto trunc;
            if (!nr.read(node.dynamic_coord_offset)) goto trunc;
            if (!nr.read(normals_off)) goto trunc;
            if (!nr.read(node.n_normals)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.normals_offset = normals_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            // Trans: dofNumber + min + max + multiplier + future + flags + translation
            if (!nr.read(node.dof_number)) goto trunc;
            if (!nr.read(node.dof_min)) goto trunc;
            if (!nr.read(node.dof_max)) goto trunc;
            if (!nr.read(node.dof_multiplier)) goto trunc;
            if (!nr.read(node.dof_future)) goto trunc;
            if (!nr.read(node.dof_flags)) goto trunc;
            if (!nr.read(node.dof_translation.x)) goto trunc;
            if (!nr.read(node.dof_translation.y)) goto trunc;
            if (!nr.read(node.dof_translation.z)) goto trunc;
            break;
        }

        case BspNodeType::BScaleNode: {
            // BSubTree fields
            int32_t coords_off, normals_off, subtree_off;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(node.n_dynamic_coords)) goto trunc;
            if (!nr.read(node.dynamic_coord_offset)) goto trunc;
            if (!nr.read(normals_off)) goto trunc;
            if (!nr.read(node.n_normals)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.normals_offset = normals_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            // Scale: dofNumber + min + max + multiplier + future + flags + scale + translation
            if (!nr.read(node.dof_number)) goto trunc;
            if (!nr.read(node.dof_min)) goto trunc;
            if (!nr.read(node.dof_max)) goto trunc;
            if (!nr.read(node.dof_multiplier)) goto trunc;
            if (!nr.read(node.dof_future)) goto trunc;
            if (!nr.read(node.dof_flags)) goto trunc;
            if (!nr.read(node.scale.x)) goto trunc;
            if (!nr.read(node.scale.y)) goto trunc;
            if (!nr.read(node.scale.z)) goto trunc;
            if (!nr.read(node.dof_translation.x)) goto trunc;
            if (!nr.read(node.dof_translation.y)) goto trunc;
            if (!nr.read(node.dof_translation.z)) goto trunc;
            break;
        }

        case BspNodeType::BSplitterNode: {
            // A + B + C + D + front + back
            int32_t front_off, back_off;
            if (!nr.read(node.splitter_plane.a)) goto trunc;
            if (!nr.read(node.splitter_plane.b)) goto trunc;
            if (!nr.read(node.splitter_plane.c)) goto trunc;
            if (!nr.read(node.splitter_plane.d)) goto trunc;
            if (!nr.read(front_off)) goto trunc;
            if (!nr.read(back_off)) goto trunc;
            node.front = offset_to_idx(front_off, node_offsets);
            node.back  = offset_to_idx(back_off, node_offsets);
            break;
        }

        case BspNodeType::BSwitchNode: {
            // switchNumber + numChildren + subTrees (offset to array)
            int32_t children_off;
            if (!nr.read(node.switch_number)) goto trunc;
            if (!nr.read(node.n_children)) goto trunc;
            if (!nr.read(children_off)) goto trunc;
            node.switch_children_offset = children_off;
            break;
        }

        case BspNodeType::BXSwitchNode: {
            // switchNumber + flags + numChildren + subTrees
            int32_t children_off;
            if (!nr.read(node.switch_number)) goto trunc;
            if (!nr.read(node.switch_flags)) goto trunc;
            if (!nr.read(node.n_children)) goto trunc;
            if (!nr.read(children_off)) goto trunc;
            node.switch_children_offset = children_off;
            break;
        }

        case BspNodeType::BPrimitiveNode: {
            // prim offset
            if (!nr.read(node.prim_offset)) goto trunc;
            break;
        }

        case BspNodeType::BLitPrimitiveNode: {
            // poly + backpoly
            if (!nr.read(node.prim_offset)) goto trunc;
            if (!nr.read(node.back_poly_offset)) goto trunc;
            break;
        }

        case BspNodeType::BCulledPrimitiveNode: {
            // poly offset
            if (!nr.read(node.prim_offset)) goto trunc;
            break;
        }

        case BspNodeType::BSpecialXform: {
            // pCoords + nCoords + type + subTree
            int32_t coords_off, subtree_off;
            int32_t xform_type;
            if (!nr.read(coords_off)) goto trunc;
            if (!nr.read(node.n_coords)) goto trunc;
            if (!nr.read(xform_type)) goto trunc;
            if (!nr.read(subtree_off)) goto trunc;
            node.coords_offset = coords_off;
            node.subtree = offset_to_idx(subtree_off, node_offsets);
            if (xform_type >= 0 && xform_type <= 2)
                node.transform_type = static_cast<TransformType>(xform_type);
            break;
        }

        case BspNodeType::BLightStringNode: {
            // BPrimitiveNode fields first
            if (!nr.read(node.prim_offset)) goto trunc;
            // Light string extras: A + B + C + D + rgbaFront + rgbaBack
            if (!nr.read(node.light_dir.a)) goto trunc;
            if (!nr.read(node.light_dir.b)) goto trunc;
            if (!nr.read(node.light_dir.c)) goto trunc;
            if (!nr.read(node.light_dir.d)) goto trunc;
            if (!nr.read(node.rgba_front)) goto trunc;
            if (!nr.read(node.rgba_back)) goto trunc;
            break;
        }

        case BspNodeType::BRenderControlNode: {
            // Control + IArg[4] + FArg[4]
            int32_t ctrl;
            if (!nr.read(ctrl)) goto trunc;
            if (ctrl == 1) node.control_type = RenderControlType::ZBias;
            for (int k = 0; k < 4; ++k)
                if (!nr.read(node.i_arg[k])) goto trunc;
            for (int k = 0; k < 4; ++k)
                if (!nr.read(node.f_arg[k])) goto trunc;
            break;
        }

        default:
            // Unknown type — skip remaining fields as best we can
            break;
        }

        // Convert sibling offset to index
        node.sibling = offset_to_idx(sibling_off, node_offsets);

        continue;

    trunc:
        err = "BSP truncated at node " + std::to_string(i) +
              " (type " + std::to_string(static_cast<int>(node.type)) + ")";
        return false;
    }

    // ── Parse shared data pools from BRoot nodes ──────────────────────
    // The coords, normals, and tex IDs are stored as contiguous arrays
    // in the node data buffer. BRoot nodes reference them by offset.
    // We find the BRoot(s) and extract the pools.

    for (const auto& node : tree.nodes) {
        if (node.type == BspNodeType::BRoot && node.n_coords > 0) {
            // Extract coords
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

            // Extract normals
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

            // Extract tex IDs
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

    // ── Parse switch children arrays ──────────────────────────────────
    for (const auto& node : tree.nodes) {
        if ((node.type == BspNodeType::BSwitchNode ||
             node.type == BspNodeType::BXSwitchNode) &&
            node.n_children > 0 && node.switch_children_offset >= 0)
        {
            auto off = static_cast<std::size_t>(node.switch_children_offset);
            auto count = static_cast<std::size_t>(node.n_children);
            if (off + count * sizeof(int32_t) <= node_data_size) {
                // Read the array of child offsets and convert to indices
                std::vector<int32_t> child_offsets(count);
                std::memcpy(child_offsets.data(), node_data + off,
                            count * sizeof(int32_t));
                auto base = tree.switch_children.size();
                tree.switch_children.resize(base + count);
                for (std::size_t k = 0; k < count; ++k) {
                    tree.switch_children[base + k] =
                        offset_to_idx(child_offsets[k], node_offsets);
                }
            }
        }
    }

    // Store raw poly data (everything after the nodes)
    // This allows deferred polygon parsing
    if (node_data_size > cur_offset) {
        tree.poly_data.assign(node_data + cur_offset,
                              node_data + node_data_size);
    }

    return true;
}

} // namespace f4::models::detail

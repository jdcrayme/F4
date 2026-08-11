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
// gives the TYPE of each node in tree-walk order — specifically, the
// SIBLING-FIRST depth-first order that FreeFalcon's RestorePointers
// produces by having the BNode base-class constructor walk the sibling
// BEFORE the BSubTree derived-class body walks the subtree:
//
//     BNode::BNode(base, tagListPtr) {           // base ctor runs FIRST
//         if (sibling >= 0)
//             sibling = RestorePointers(base, sibling, tagListPtr);
//     }
//     BSubTree::BSubTree(base, tagListPtr)
//       : BNode(base, tagListPtr) {              // derived body runs AFTER
//         subTree = RestorePointers(base, subTree, tagListPtr);
//     }
//
// So for each node N: tag[N+1] is N's SIBLING's tag (and the sibling's
// whole subtree follows in the same sibling-first order), and only
// AFTER the sibling chain is exhausted does tag[N+k] give N's subtree
// (child) tag. We mirror this by walking sibling BEFORE subtree
// (see the SIBLING-FIRST WALK block in walk_node below).
//
// References:
//   FreeFalcon: src/graphics/texture/objectlod.cpp (LoaderCallBack)
//   FreeFalcon: src/graphics/bsplib/bspnodes.cpp (RestorePointers)

#include "bsp_parser.hpp"
#include "bin_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

// DIAGNOSTIC: When env var F4_DIAG=1 is set, the parser emits detailed
// stderr diagnostics about the vtable scan and switch nodes. Off by
// default so production behavior is unchanged.
static bool diag_enabled() {
    static bool v = ([](){
        const char* e = std::getenv("F4_DIAG");
        return e && e[0] == '1';
    })();
    return v;
}

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

    // ── SIBLING-FIRST WALK (matches FreeFalcon's RestorePointers) ──────
    //
    // FreeFalcon's BNode base-class constructor runs BEFORE the BSubTree
    // derived-class body. The base ctor walks the sibling; the derived
    // body walks the subtree/children. So the tag-list order is:
    //   tag[i]   = THIS node
    //   tag[i+1] = sibling's tag (and recursively the sibling's whole
    //              subtree, sibling-first as well)
    //   tag[i+k] = subtree/child's tag (after the sibling chain is done)
    //
    // Walking the sibling HERE (before the switch that walks subtree/
    // children) is what makes our tag consumption match FreeFalcon's
    // expected tag-list order. Doing it the other way around consumes
    // the wrong tag for any node that has BOTH a sibling AND a subtree,
    // which cascades into a flood of mis-typed garbage nodes and the
    // "orphan prim" symptom that requires a rescue pass.
    if (sibling_off >= 0) {
        node.sibling = walk_node(ctx, sibling_off);
    }

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

        // LAYER-B FIX: Validate fields to reject garbage BSwitchNodes.
        //
        // The tag-driven recovery pass has validators that reject
        // candidates with switch_number > 64 or n_children > 64. But
        // the MAIN walk has no such validation — it trusts whatever
        // bytes happen to be at the offset. When the main walk missteps
        // into a coordinate or float-data region that happens to start
        // with a vtable-pattern (0x00465000..0x004651FF), it creates a
        // garbage BSwitchNode with switch_number like 100816 or
        // 1048996681 (float-bit-patterns). These garbage nodes consume
        // real tags and create unreachable subtrees, hiding geometry.
        //
        // We now apply the SAME validation as the tag-driven recovery
        // (lines ~862-877): switch_number ∈ [-1, 64], n_children ∈
        // [0, 64], children_off either -1 or in-range. If the fields
        // don't validate, we treat this as a misstep and DON'T walk
        // the (garbage) children — we still keep the node in
        // tree.nodes so it's accounted for, but we mark it as
        // "untrusted" by setting n_children=0.
        bool switch_valid = (node.switch_number >= -1 && node.switch_number <= 64) &&
                            (node.n_children >= 0 && node.n_children <= 64);
        if (switch_valid && node.n_children > 0) {
            if (children_off < 0) {
                switch_valid = false;
            } else {
                std::size_t need = static_cast<std::size_t>(children_off)
                                 + static_cast<std::size_t>(node.n_children) * sizeof(int32_t);
                if (need > ctx.base_size) switch_valid = false;
            }
        }

        // DIAGNOSTIC: log every BSwitchNode, especially n_children==0
        if (diag_enabled()) {
            std::fprintf(stderr,
                "[DIAG] BSwitchNode @ off=%d switch_number=%d n_children=%d children_off=%d valid=%d\n",
                offset, node.switch_number, node.n_children, children_off,
                (int)switch_valid);
        }

        if (!switch_valid) {
            // Garbage BSwitchNode — don't walk children, mark as untrusted.
            if (diag_enabled()) {
                std::fprintf(stderr,
                    "[DIAG]   ^-- REJECT: garbage fields (switch_number=%d n_children=%d); zeroing n_children\n",
                    node.switch_number, node.n_children);
            }
            node.n_children = 0;
            node.switch_children_offset = -1;
            break;
        }

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
        } else if (diag_enabled() && children_off >= 0 && node.n_children == 0) {
            // n_children==0 with non-null children_off — suspect #2.
            // Try to peek at the first child offset to see if there's
            // actually data there.
            int32_t peek = -1;
            auto base = static_cast<std::size_t>(children_off);
            if (base + sizeof(int32_t) <= ctx.base_size) {
                std::memcpy(&peek, ctx.base + base, sizeof(int32_t));
            }
            std::fprintf(stderr,
                "[DIAG]   ^-- SUSPECT: n_children=0 but children_off=%d (first child_off would be %d)\n",
                children_off, peek);
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

        // LAYER-B FIX: same validation as BSwitchNode, plus flags check
        // (only bit 0 is defined; garbage float patterns usually have
        // many bits set).
        bool xswitch_valid = (node.switch_number >= -1 && node.switch_number <= 64) &&
                             (node.n_children >= 0 && node.n_children <= 64) &&
                             (node.switch_flags >= 0 && node.switch_flags <= 0xFFFF);
        if (xswitch_valid && node.n_children > 0) {
            if (children_off < 0) {
                xswitch_valid = false;
            } else {
                std::size_t need = static_cast<std::size_t>(children_off)
                                 + static_cast<std::size_t>(node.n_children) * sizeof(int32_t);
                if (need > ctx.base_size) xswitch_valid = false;
            }
        }

        if (diag_enabled()) {
            std::fprintf(stderr,
                "[DIAG] BXSwitchNode @ off=%d switch_number=%d flags=%d n_children=%d children_off=%d valid=%d\n",
                offset, node.switch_number, node.switch_flags, node.n_children, children_off,
                (int)xswitch_valid);
        }

        if (!xswitch_valid) {
            if (diag_enabled()) {
                std::fprintf(stderr,
                    "[DIAG]   ^-- REJECT: garbage fields; zeroing n_children\n");
            }
            node.n_children = 0;
            node.switch_children_offset = -1;
            break;
        }

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
        } else if (diag_enabled() && children_off >= 0 && node.n_children == 0) {
            int32_t peek = -1;
            auto base = static_cast<std::size_t>(children_off);
            if (base + sizeof(int32_t) <= ctx.base_size) {
                std::memcpy(&peek, ctx.base + base, sizeof(int32_t));
            }
            std::fprintf(stderr,
                "[DIAG]   ^-- SUSPECT: BXSwitchNode n_children=0 but children_off=%d (first child_off would be %d)\n",
                children_off, peek);
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

    // (Sibling walk now happens BEFORE the switch — see comment above.
    //  FreeFalcon's BNode base-class ctor walks sibling before the
    //  BSubTree derived body walks subtree/children.)

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

    // Pass 2: TAG-DRIVEN RECOVERY (replaces vtable scan).
    //
    // The previous implementation scanned the buffer in byte-offset
    // order and walked any unvisited position whose 4-byte value
    // matched the BNode-class vtable range (0x00465000..0x004651FF).
    // This consumed tags in TAG-LIST order regardless of which buffer
    // position was actually correct for that tag, producing massive
    // false positives (positions in coordinate/normal/prim data where
    // bytes happened to match the vtable pattern). Each false positive
    // consumed a real tag and created a fake node whose fields were
    // garbage float bit patterns.
    //
    // Diagnostic on model 1052 LOD 0: the previous scan consumed 695
    // tags and produced 663 fake nodes (19 of 26 BSwitchNode tags were
    // consumed by fakes with garbage switch_number values like
    // 1048996681 = 0x3E880029 ≈ 0.25 as a float). 83 prims were
    // rejected as type_val=27 because their prim_offset was a
    // coordinate value misread as int32.
    //
    // NEW STRATEGY: For each unconsumed tag, scan all unvisited
    // vtable-pattern positions and walk the FIRST one whose
    // type-specific fields validate cleanly against the tag's
    // expected node layout. If no candidate validates, skip the tag
    // (better to leave a tag unconsumed than to create a fake node
    // that will pollute the tree with garbage geometry).
    //
    // Validation rules per node type are derived from the on-disk
    // layouts in walk_node() above. Ranges are conservative bounds
    // that real Falcon data always satisfies but garbage float
    // patterns almost never do.

    const int tags_consumed = ctx.tag_pos;
    const double consumption_ratio =
        ctx.tag_count > 0
            ? static_cast<double>(tags_consumed) / static_cast<double>(ctx.tag_count)
            : 1.0;
    (void)consumption_ratio;

    // Pre-compute the list of unvisited vtable-pattern positions.
    // A position qualifies if:
    //   - bytes [0..3] little-endian decode to a value in 0x00465000..0x004651FF
    //   - the position is NOT already in ctx.offset_map (already visited)
    // We iterate this list per-tag below.
    struct VtHit { int32_t off; uint32_t vt; };
    std::vector<VtHit> unvisited_vt_hits;
    {
        for (std::size_t i = 0; i + 4 <= node_data_size; i += 4) {
            // Quick reject on the high bytes of the little-endian vtable.
            if (node_data[i + 3] != 0x00) continue;
            if (node_data[i + 2] != 0x46) continue;
            if (node_data[i + 1] != 0x50 && node_data[i + 1] != 0x51) continue;
            uint32_t vt = 0;
            std::memcpy(&vt, node_data + i, 4);
            if (vt < 0x00465000 || vt > 0x004651FF) continue;
            int32_t off = static_cast<int32_t>(i);
            if (ctx.offset_map.count(off) > 0) continue;
            unvisited_vt_hits.push_back({off, vt});
        }
    }

    // Per-node-type validators. Each reads the type-specific fields
    // at the candidate position and returns true if they look sane.
    //
    // node_data layout reminder (from walk_node above):
    //   [0..3]    uint32  vtable  (already validated by the pre-filter)
    //   [4..7]    int32   sibling_off  (validated below per-type)
    //   [8..]     type-specific fields

    auto read_i32_at = [&](std::size_t off, int32_t& out) -> bool {
        if (off + 4 > node_data_size) return false;
        std::memcpy(&out, node_data + off, 4);
        return true;
    };

    // Validate that an offset is either -1 (null) or in-range.
    auto valid_off = [&](int32_t v) -> bool {
        return v == -1 || (v >= 0 && static_cast<std::size_t>(v) < node_data_size);
    };

    // Validate a pool descriptor (used by BRoot/BSubTree/BDofNode/etc.).
    // node layout at offset `base`:
    //   [base+8..11]   coords_off
    //   [base+12..15]  n_coords
    //   [base+16..19]  n_dynamic_coords
    //   [base+20..23]  dynamic_coord_offset
    //   [base+24..27]  normals_off
    //   [base+28..31]  n_normals
    //   [base+32..35]  subtree_off   (we don't validate subtree — it's
    //                                  walked recursively and will be
    //                                  visited via offset_map)
    auto valid_subtree_pool = [&](std::size_t base) -> bool {
        int32_t coords_off=0, n_coords=0, n_dyn=0, dyn_off=0,
                normals_off=0, n_normals=0;
        if (!read_i32_at(base + 8,  coords_off))    return false;
        if (!read_i32_at(base + 12, n_coords))      return false;
        if (!read_i32_at(base + 16, n_dyn))         return false;
        if (!read_i32_at(base + 20, dyn_off))       return false;
        if (!read_i32_at(base + 24, normals_off))   return false;
        if (!read_i32_at(base + 28, n_normals))     return false;
        // Counts: real Falcon trees have < 100k coords/normals per subtree.
        if (n_coords < 0 || n_coords > 100000) return false;
        if (n_normals < 0 || n_normals > 100000) return false;
        if (n_dyn < 0 || n_dyn > 100000) return false;
        // Offsets: -1 (null) or in-range.
        if (!valid_off(coords_off))   return false;
        if (!valid_off(normals_off))  return false;
        // If a count is > 0, the corresponding offset must be in-range
        // AND the pool [off, off + count*sizeof(Vec3)) must fit.
        if (n_coords > 0) {
            if (coords_off < 0) return false;
            std::size_t need = static_cast<std::size_t>(coords_off)
                             + static_cast<std::size_t>(n_coords) * sizeof(Vec3);
            if (need > node_data_size) return false;
        }
        if (n_normals > 0) {
            if (normals_off < 0) return false;
            std::size_t need = static_cast<std::size_t>(normals_off)
                             + static_cast<std::size_t>(n_normals) * sizeof(Vec3);
            if (need > node_data_size) return false;
        }
        return true;
    };

    // Validate a prim header at the given offset.
    // Prim header layout: type(4) + nVerts(4) + xyz_offset(4)
    //   type ∈ [0, 26], nVerts ∈ [3, 1000] (per poly_parser sanity)
    auto valid_prim_header = [&](int32_t prim_off) -> bool {
        if (prim_off < 0) return false;
        std::size_t po = static_cast<std::size_t>(prim_off);
        if (po + 12 > node_data_size) return false;
        int32_t type_val=0, n_verts=0, xyz_off=0;
        std::memcpy(&type_val, node_data + po,      4);
        std::memcpy(&n_verts,  node_data + po + 4,  4);
        std::memcpy(&xyz_off,  node_data + po + 8,  4);
        if (type_val < 0 || type_val >= 27) return false;
        if (n_verts < 3 || n_verts > 1000)  return false;
        // xyz_off must point to a vertex array of n_verts * 3 int32 indices
        // (each index refers to a coord in the active pool). We don't
        // know the active pool here, but the byte range must be valid.
        if (xyz_off < 0) return false;
        std::size_t need = static_cast<std::size_t>(xyz_off)
                         + static_cast<std::size_t>(n_verts) * 3 * sizeof(int32_t);
        if (need > node_data_size) return false;
        return true;
    };

    // Per-type validator. `base` is the absolute offset of the node
    // (i.e., the vtable-pattern position). Returns true if the bytes
    // at [base, base + node_size_for_type] look like a real node of
    // the given type.
    auto validate_node = [&](std::size_t base, BspNodeType tag) -> bool {
        // All nodes have vtable(4) + sibling(4).
        int32_t sib_off = 0;
        if (!read_i32_at(base + 4, sib_off)) return false;
        if (!valid_off(sib_off)) return false;

        switch (tag) {
        case BspNodeType::BNode:
            // vtable(4) + sibling(4) — no type-specific fields.
            return true;

        case BspNodeType::BSubTree:
            return valid_subtree_pool(base);

        case BspNodeType::BRoot: {
            if (!valid_subtree_pool(base)) return false;
            // BRoot adds: tex_off(4) + n_tex_ids(4) + script(4)
            // after the BSubTree fields. Subtree fields end at base+36.
            int32_t tex_off=0, n_tex_ids=0, script=0;
            if (!read_i32_at(base + 36, tex_off))    return false;
            if (!read_i32_at(base + 40, n_tex_ids))  return false;
            if (!read_i32_at(base + 44, script))     return false;
            if (n_tex_ids < 0 || n_tex_ids > 100000) return false;
            if (!valid_off(tex_off)) return false;
            if (n_tex_ids > 0) {
                if (tex_off < 0) return false;
                std::size_t need = static_cast<std::size_t>(tex_off)
                                 + static_cast<std::size_t>(n_tex_ids) * sizeof(int32_t);
                if (need > node_data_size) return false;
            }
            // script_number is typically 0 or small (index into script table).
            // Garbage float patterns often produce huge script values.
            if (script < -1 || script > 1024) return false;
            return true;
        }

        case BspNodeType::BSlotNode: {
            // BSlotNode: vtable(4)+sib(4) + rotation(36) + origin(12) + slot_number(4)
            // Total 60 bytes. Only validate slot_number — rotations/origins
            // are arbitrary floats.
            int32_t slot_num = 0;
            if (!read_i32_at(base + 52, slot_num)) return false;
            // Real slot numbers are 0..63. Garbage float bit patterns
            // in [0..63] are extremely rare (the float would have to
            // be a tiny denormal).
            if (slot_num < 0 || slot_num > 256) return false;
            return true;
        }

        case BspNodeType::BDofNode:
        case BspNodeType::BXDofNode:
        case BspNodeType::BTransNode:
        case BspNodeType::BScaleNode: {
            // All start with BSubTree fields (valid_subtree_pool).
            if (!valid_subtree_pool(base)) return false;
            // After subtree fields (base+36): dof_number(4).
            int32_t dof_num = 0;
            if (!read_i32_at(base + 36, dof_num)) return false;
            // Real DOF numbers are 0..31 (rarely up to 63). -1 means "no DOF".
            // Garbage float bit patterns produce huge values here.
            if (dof_num < -1 || dof_num > 128) return false;
            return true;
        }

        case BspNodeType::BSwitchNode: {
            // vtable(4)+sib(4)+switch_number(4)+n_children(4)+children_off(4) = 20 bytes
            int32_t sw_num=0, n_ch=0, ch_off=0;
            if (!read_i32_at(base + 8,  sw_num)) return false;
            if (!read_i32_at(base + 12, n_ch))   return false;
            if (!read_i32_at(base + 16, ch_off)) return false;
            if (sw_num < -1 || sw_num > 64) return false;
            if (n_ch < 0 || n_ch > 64) return false;
            if (n_ch > 0) {
                if (ch_off < 0) return false;
                std::size_t need = static_cast<std::size_t>(ch_off)
                                 + static_cast<std::size_t>(n_ch) * sizeof(int32_t);
                if (need > node_data_size) return false;
            }
            return true;
        }

        case BspNodeType::BXSwitchNode: {
            // vtable(4)+sib(4)+switch_number(4)+flags(4)+n_children(4)+children_off(4) = 24 bytes
            int32_t sw_num=0, flags=0, n_ch=0, ch_off=0;
            if (!read_i32_at(base + 8,  sw_num)) return false;
            if (!read_i32_at(base + 12, flags))  return false;
            if (!read_i32_at(base + 16, n_ch))   return false;
            if (!read_i32_at(base + 20, ch_off)) return false;
            if (sw_num < -1 || sw_num > 64) return false;
            if (n_ch < 0 || n_ch > 64) return false;
            // flags: only bit 0 defined (XSWT_REVERSED_EFFECT). Garbage
            // float patterns usually have many bits set.
            if (flags < 0 || flags > 0xFFFF) return false;
            if (n_ch > 0) {
                if (ch_off < 0) return false;
                std::size_t need = static_cast<std::size_t>(ch_off)
                                 + static_cast<std::size_t>(n_ch) * sizeof(int32_t);
                if (need > node_data_size) return false;
            }
            return true;
        }

        case BspNodeType::BSplitterNode: {
            // vtable(4)+sib(4) + plane ABCD(16) + front(4) + back(4) = 32 bytes
            int32_t front_off=0, back_off=0;
            if (!read_i32_at(base + 24, front_off)) return false;
            if (!read_i32_at(base + 28, back_off))  return false;
            if (!valid_off(front_off)) return false;
            if (!valid_off(back_off))  return false;
            // Plane coefficients (a,b,c,d) are floats; we don't validate them
            // because real planes have arbitrary values. (We rely on the
            // front/back offset validation as the main signal.)
            return true;
        }

        case BspNodeType::BPrimitiveNode: {
            // vtable(4)+sib(4)+prim_offset(4) = 12 bytes
            int32_t prim_off = 0;
            if (!read_i32_at(base + 8, prim_off)) return false;
            return valid_prim_header(prim_off);
        }

        case BspNodeType::BLitPrimitiveNode: {
            // vtable(4)+sib(4)+prim_offset(4)+back_poly_offset(4) = 16 bytes
            int32_t prim_off=0, back_off=0;
            if (!read_i32_at(base + 8,  prim_off)) return false;
            if (!read_i32_at(base + 12, back_off)) return false;
            if (!valid_prim_header(prim_off)) return false;
            // back_poly_offset should also point to a valid prim header.
            // (Some BLightStringNode-like variants may store -1, but
            // a real BLitPrimitiveNode always has both polys.)
            if (!valid_prim_header(back_off)) return false;
            return true;
        }

        case BspNodeType::BCulledPrimitiveNode: {
            // Same as BPrimitiveNode.
            int32_t prim_off = 0;
            if (!read_i32_at(base + 8, prim_off)) return false;
            return valid_prim_header(prim_off);
        }

        case BspNodeType::BSpecialXform: {
            // vtable(4)+sib(4)+coords_off(4)+n_coords(4)+type(4)+subtree_off(4) = 24 bytes
            int32_t coords_off=0, n_coords=0, xform_type=0, sub_off=0;
            if (!read_i32_at(base + 8,  coords_off)) return false;
            if (!read_i32_at(base + 12, n_coords))   return false;
            if (!read_i32_at(base + 16, xform_type)) return false;
            if (!read_i32_at(base + 20, sub_off))    return false;
            if (n_coords < 0 || n_coords > 100000) return false;
            if (!valid_off(coords_off)) return false;
            if (xform_type < 0 || xform_type > 2) return false;
            if (!valid_off(sub_off)) return false;
            return true;
        }

        case BspNodeType::BLightStringNode: {
            // BPrimNode(12) + light_dir ABCD(16) + rgba_front(4) + rgba_back(4) = 36 bytes
            int32_t prim_off = 0;
            if (!read_i32_at(base + 8, prim_off)) return false;
            if (!valid_prim_header(prim_off)) return false;
            // rgba_front / rgba_back are color indices (typically < 256).
            // Validate to reject float-bit-pattern false positives.
            int32_t rgba_f = 0, rgba_b = 0;
            if (!read_i32_at(base + 32, rgba_f)) return false;
            if (!read_i32_at(base + 36, rgba_b)) return false;
            // Falcon color bank indices are < 65536 typically. -1 = no color.
            if (rgba_f < -1 || rgba_f > 65536) return false;
            if (rgba_b < -1 || rgba_b > 65536) return false;
            return true;
        }

        case BspNodeType::BRenderControlNode: {
            // vtable(4)+sib(4)+ctrl(4)+iArg[4](16)+fArg[4](16) = 44 bytes
            int32_t ctrl = 0;
            if (!read_i32_at(base + 8, ctrl)) return false;
            // ctrl ∈ {0, 1} per types.hpp (RenderControlType).
            if (ctrl < 0 || ctrl > 1) return false;
            // iArg values are typically small ints (< 65536) or -1.
            // fArg values are typically 0.0 or small floats in [0, 1].
            // We can't validate fArg without parsing floats, so we rely
            // on iArg. Garbage float-bit-pattern false positives usually
            // have at least one iArg with a huge value.
            for (int k = 0; k < 4; ++k) {
                int32_t ia = 0;
                if (!read_i32_at(base + 12 + k * 4, ia)) return false;
                if (ia < -1 || ia > 0x10000) return false;
            }
            return true;
        }

        default:
            return false;
        }
    };

    // Tag-driven recovery: for each unconsumed tag, find the first
    // unvisited vtable-pattern position that validates for that tag's
    // node type, and walk it.
    int diag_tags_recovered = 0;
    int diag_tags_skipped = 0;
    int diag_candidates_rejected = 0;
    int diag_vt_hits_total = static_cast<int>(unvisited_vt_hits.size());

    while (ctx.tag_pos < ctx.tag_count) {
        BspNodeType tag = ctx.tags[ctx.tag_pos];
        if (tag == BspNodeType::Unknown) {
            // Skip unknown tags (parser already flagged them).
            ++ctx.tag_pos;
            ++diag_tags_skipped;
            continue;
        }

        // Find the first unvisited vtable hit that validates for this tag.
        bool recovered = false;
        for (auto& hit : unvisited_vt_hits) {
            if (hit.off < 0) continue;  // already consumed (marked below)
            if (ctx.offset_map.count(hit.off) > 0) {
                // Visited since we built the list (e.g., by a previous tag's walk).
                hit.off = -1;
                continue;
            }
            if (!validate_node(static_cast<std::size_t>(hit.off), tag)) {
                ++diag_candidates_rejected;
                continue;
            }
            // Validate passed — walk this position. This consumes the tag.
            if (diag_enabled()) {
                std::fprintf(stderr,
                    "[DIAG] RECOVER tag[%d/%d]=%d at off=%d vt=0x%08X\n",
                    ctx.tag_pos, ctx.tag_count, static_cast<int>(tag),
                    hit.off, hit.vt);
            }
            (void)walk_node(ctx, hit.off);
            hit.off = -1;  // mark consumed so we don't try it again
            ++diag_tags_recovered;
            recovered = true;
            break;
        }

        if (!recovered) {
            // No candidate validated for this tag. Skip it rather than
            // risking a fake node. (The tag's geometry is lost either way;
            // skipping avoids polluting the tree with garbage.)
            if (diag_enabled()) {
                std::fprintf(stderr,
                    "[DIAG] SKIP tag[%d/%d]=%d (no valid candidate; %zu hits tried)\n",
                    ctx.tag_pos, ctx.tag_count, static_cast<int>(tag),
                    unvisited_vt_hits.size());
            }
            ++ctx.tag_pos;
            ++diag_tags_skipped;
        }
    }

    if (diag_enabled()) {
        std::fprintf(stderr,
            "[DIAG] === tag-driven recovery summary ===\n"
            "[DIAG]   tag_count=%d tags_consumed_before_recovery=%d (%.1f%%)\n"
            "[DIAG]   tags_consumed_after_recovery=%d (%.1f%%)\n"
            "[DIAG]   unvisited_vt_hits=%d\n"
            "[DIAG]   tags_recovered=%d tags_skipped=%d candidates_rejected=%d\n"
            "[DIAG]   total_nodes=%zu\n",
            ctx.tag_count, tags_consumed, consumption_ratio * 100.0,
            ctx.tag_pos,
            ctx.tag_count > 0 ? 100.0 * static_cast<double>(ctx.tag_pos) / static_cast<double>(ctx.tag_count) : 0.0,
            diag_vt_hits_total,
            diag_tags_recovered, diag_tags_skipped, diag_candidates_rejected,
            tree.nodes.size());
    }

    return true;
}

} // namespace f4::models::detail

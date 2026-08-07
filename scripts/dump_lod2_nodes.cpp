// dump_lod2_nodes.cpp
// Dump the node types and structure of model 829 LOD 2 to see why
// geometry extraction produces 0 meshes.

#include <f4/models/model_database.hpp>
#include <f4/models/bsp_node.hpp>
#include "bsp_parser.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <map>

// using-namespace in a standalone script file — acceptable (not a header).
using namespace f4::models;
using namespace f4::models::detail;

namespace {

void walk_and_print(const BspTree& tree, const BspNode& node, int depth, int& prim_count) {
    auto indent = [&]() { for (int i = 0; i < depth; ++i) printf("  "); };

    indent();
    printf("node[%d] type=%s", -1, bsp_node_type_name(node.type));
    if (node.n_coords > 0) printf(" nCoords=%d", node.n_coords);
    if (node.n_normals > 0) printf(" nNormals=%d", node.n_normals);
    if (node.n_tex_ids > 0) printf(" nTexIDs=%d", node.n_tex_ids);
    if (node.prim_offset >= 0) { printf(" primOff=%d", node.prim_offset); ++prim_count; }
    if (node.back_poly_offset >= 0) printf(" backPoly=%d", node.back_poly_offset);
    if (node.sibling >= 0) printf(" sib=%d", node.sibling);
    if (node.subtree >= 0) printf(" sub=%d", node.subtree);
    if (node.front >= 0) printf(" front=%d", node.front);
    if (node.back >= 0) printf(" back=%d", node.back);
    if (node.n_children > 0) printf(" nChildren=%d swOff=%d", node.n_children, node.switch_children_offset);
    printf("\n");

    // Recurse
    if (node.subtree >= 0 && static_cast<std::size_t>(node.subtree) < tree.nodes.size())
        walk_and_print(tree, tree.nodes[node.subtree], depth+1, prim_count);
    if (node.front >= 0 && static_cast<std::size_t>(node.front) < tree.nodes.size())
        walk_and_print(tree, tree.nodes[node.front], depth+1, prim_count);
    if (node.back >= 0 && static_cast<std::size_t>(node.back) < tree.nodes.size())
        walk_and_print(tree, tree.nodes[node.back], depth+1, prim_count);
    if (node.sibling >= 0 && static_cast<std::size_t>(node.sibling) < tree.nodes.size())
        walk_and_print(tree, tree.nodes[node.sibling], depth, prim_count);
    if (node.n_children > 0 && node.switch_children_offset >= 0) {
        auto base = static_cast<std::size_t>(node.switch_children_offset);
        auto cnt = static_cast<std::size_t>(node.n_children);
        if (base + cnt <= tree.switch_children.size()) {
            for (std::size_t k = 0; k < cnt; ++k) {
                auto c = tree.switch_children[base + k];
                if (c >= 0 && static_cast<std::size_t>(c) < tree.nodes.size())
                    walk_and_print(tree, tree.nodes[c], depth+1, prim_count);
            }
        }
    }
}

} // namespace

int main() {
    std::filesystem::path hdr_path = "../temp/KoreaObj.HDR";
    std::filesystem::path lod_path = "../temp/KoreaObj.LOD";

    ModelDatabase db;
    std::string err = db.load(hdr_path, lod_path);
    if (!err.empty()) { printf("ERROR: %s\n", err.c_str()); return 1; }

    // Parse LOD 2 of model 829
    err = db.parse_lod(829, 2);
    if (!err.empty()) { printf("parse_lod error: %s\n", err.c_str()); return 1; }

    const auto* bsp = db.bsp_tree(829, 2);
    if (!bsp) { printf("no BSP tree\n"); return 1; }

    printf("BSP tree: %zu nodes, %zu coords, %zu normals, %zu tex_ids, buffer=%zu\n",
            bsp->nodes.size(), bsp->coords.size(), bsp->normals.size(),
            bsp->tex_ids.size(), bsp->lod_buffer.size());

    if (!bsp->tex_ids.empty()) {
        printf("tex_ids:");
        for (auto t : bsp->tex_ids) printf(" %d", t);
        printf("\n");
    }

    // Count node types
    std::map<BspNodeType, int> type_counts;
    for (const auto& n : bsp->nodes) type_counts[n.type]++;
    printf("\nNode type counts:\n");
    for (const auto& [t, c] : type_counts) {
        printf("  %-25s %d\n", bsp_node_type_name(t), c);
    }

    // Walk the tree from root
    printf("\nTree structure (first 30 nodes):\n");
    int prim_count = 0;
    if (!bsp->nodes.empty()) {
        walk_and_print(*bsp, bsp->nodes[0], 0, prim_count);
    }
    printf("\nTotal prim nodes found in walk: %d\n", prim_count);

    // Now try extracting geometry and report
    ModelState state;
    auto geom = db.extract_model_geometry(829, 2, state);
    printf("\nExtracted geometry: %zu meshes\n", geom.meshes.size());
    for (std::size_t i = 0; i < geom.meshes.size(); ++i) {
        printf("  mesh[%zu] tex_id=%d verts=%zu tris=%zu\n",
                i, geom.meshes[i].tex_id,
                geom.meshes[i].vertices.size(),
                geom.meshes[i].triangles.size());
    }

    return 0;
}

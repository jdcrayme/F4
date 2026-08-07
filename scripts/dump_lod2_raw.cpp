// dump_lod2_raw.cpp
// Dump raw bytes of the BSP tree for model 829 LOD 2, showing the
// tag list and node byte offsets so we can find why the BSwitchNode
// has n_children=0 and the BDofNode has subtree=-1.

#include <f4/models/model_database.hpp>
#include <f4/models/bsp_node.hpp>
#include "bsp_parser.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>

using namespace f4::models;
using namespace f4::models::detail;

int main() {
    // Load the LOD file raw data
    std::ifstream f("../temp/KoreaObj.LOD", std::ios::binary);
    if (!f) { printf("cannot open LOD file\n"); return 1; }

    // Get the LOD 2 entry from the database
    ModelDatabase db;
    db.load("../temp/KoreaObj.HDR", "../temp/KoreaObj.LOD");
    const auto* rec = db.model(829);
    const auto& lod_table = db.lod_table();
    int idx = rec->lods[2].lod_table_idx;
    const auto& entry = lod_table[idx];

    // Read the LOD 2 raw data
    f.seekg(entry.offset);
    std::vector<uint8_t> buf(entry.size);
    f.read(reinterpret_cast<char*>(buf.data()), entry.size);

    // Parse the tag list header
    // BSP record: [0..3] tagListLength, [4..] tagList, [data_start..] nodeTreeData
    uint32_t tag_count;
    std::memcpy(&tag_count, buf.data(), 4);
    printf("tagListLength = %u\n", tag_count);

    // Read tags
    std::vector<int32_t> tags(tag_count);
    std::memcpy(tags.data(), buf.data() + 4, tag_count * 4);
    printf("Tags (%u):", tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        const char* name = bsp_node_type_name(static_cast<BspNodeType>(tags[i]));
        printf(" [%u]=%s", i, name);
        if (i % 10 == 9) printf("\n  ");
    }
    printf("\n");

    // Data starts after tag count + tag list
    uint32_t data_start = 4 + tag_count * 4;
    printf("data_start = %u\n", data_start);
    printf("buffer size = %zu\n", buf.size());
    printf("node data size = %zu\n", buf.size() - data_start);

    // Now parse the BSP tree properly
    BspTree tree;
    std::string perr;
    bool ok = parse_bsp_tree(buf.data(), buf.size(), tree, perr);
    if (!ok) { printf("parse failed: %s\n", perr.c_str()); return 1; }

    printf("\nParsed: %zu nodes, %zu switch_children\n",
            tree.nodes.size(), tree.switch_children.size());

    // Print each node's type and key fields
    printf("\nNode array:\n");
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        const auto& n = tree.nodes[i];
        printf("  [%2zu] %-25s", i, bsp_node_type_name(n.type));
        if (n.sibling >= 0) printf(" sib=%d", n.sibling);
        if (n.subtree >= 0) printf(" sub=%d", n.subtree);
        if (n.front >= 0) printf(" front=%d", n.front);
        if (n.back >= 0) printf(" back=%d", n.back);
        if (n.prim_offset >= 0) printf(" prim=%d", n.prim_offset);
        if (n.n_children > 0) printf(" nChild=%d swOff=%d", n.n_children, n.switch_children_offset);
        if (n.n_children == 0 && (n.type == BspNodeType::BSwitchNode || n.type == BspNodeType::BXSwitchNode))
            printf(" nChild=0 swOff=%d", n.switch_children_offset);
        if (n.dof_number >= 0) printf(" dof=%d", n.dof_number);
        printf("\n");
    }

    // Check switch_children array
    if (!tree.switch_children.empty()) {
        printf("\nswitch_children array (%zu entries):\n", tree.switch_children.size());
        for (std::size_t i = 0; i < tree.switch_children.size(); ++i) {
            printf("  [%2zu] = %d\n", i, tree.switch_children[i]);
        }
    }

    return 0;
}

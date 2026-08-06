// Direct BSP tree inspection for model 1
#include <f4/models/f4_models.hpp>
#include "hdr_parser.hpp"
#include "bsp_parser.hpp"
#include "bin_reader.hpp"
#include <cstdio>

// We need to directly parse and inspect the BSP tree
int main() {
    // Read HDR
    auto hdr_data = f4::models::detail::read_file(
        "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.HDR");
    auto lod_data = f4::models::detail::read_file(
        "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD");

    f4::models::detail::HdrParseResult hdr;
    std::string err;
    if (!f4::models::detail::parse_hdr(hdr_data.data(), hdr_data.size(), hdr, err)) {
        printf("HDR parse error: %s\n", err.c_str());
        return 1;
    }

    // Model 1, LOD 0
    auto& parent = hdr.parents[1];
    int lod_idx = parent.lods[0].lod_table_idx;
    auto& entry = hdr.lod_entries[lod_idx];
    printf("Model 1 LOD 0: lod_table_idx=%d offset=%u size=%u\n",
           lod_idx, entry.offset, entry.size);

    // Parse BSP
    f4::models::BspTree tree;
    if (!f4::models::detail::parse_bsp_tree(
            lod_data.data() + entry.offset, entry.size, tree, err)) {
        printf("BSP parse error: %s\n", err.c_str());
        return 1;
    }

    printf("BSP tree: %zu nodes, %zu tags, tag_count=%d\n",
           tree.nodes.size(), tree.tags.size(), tree.tag_count);
    printf("  data_start=%d data_size=%d\n", tree.data_start, tree.data_size);
    printf("  lod_buffer size=%zu\n", tree.lod_buffer.size());
    printf("  coords=%zu normals=%zu tex_ids=%zu\n",
           tree.coords.size(), tree.normals.size(), tree.tex_ids.size());

    // Count node types
    int type_counts[17] = {};
    int prim_count = 0;
    for (const auto& n : tree.nodes) {
        int t = static_cast<int>(n.type);
        if (t >= 0 && t < 17) type_counts[t]++;
        if (n.type == f4::models::BspNodeType::BPrimitiveNode ||
            n.type == f4::models::BspNodeType::BCulledPrimitiveNode ||
            n.type == f4::models::BspNodeType::BLitPrimitiveNode) {
            prim_count++;
        }
    }

    for (int i = 0; i < 17; ++i) {
        if (type_counts[i] > 0) {
            printf("  %s: %d\n", f4::models::bsp_node_type_name(
                static_cast<f4::models::BspNodeType>(i)), type_counts[i]);
        }
    }
    printf("  Primitive nodes: %d\n", prim_count);

    // Show first few primitive nodes
    int shown = 0;
    for (std::size_t i = 0; i < tree.nodes.size() && shown < 5; ++i) {
        const auto& n = tree.nodes[i];
        if (n.type == f4::models::BspNodeType::BPrimitiveNode ||
            n.type == f4::models::BspNodeType::BCulledPrimitiveNode ||
            n.type == f4::models::BspNodeType::BLitPrimitiveNode) {
            printf("  node %zu: type=%s prim_offset=%d sibling=%d\n",
                   i, f4::models::bsp_node_type_name(n.type),
                   n.prim_offset, n.sibling);
            shown++;
        }
    }

    // Check node 0
    if (!tree.nodes.empty()) {
        const auto& root = tree.nodes[0];
        printf("Node 0: type=%s sibling=%d subtree=%d\n",
               f4::models::bsp_node_type_name(root.type),
               root.sibling, root.subtree);
    }

    return 0;
}

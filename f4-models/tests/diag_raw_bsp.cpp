// Direct BSP: debug node 0 subtree and first few nodes
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

int main() {
    // Read LOD file
    FILE* f = fopen("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> lod(sz);
    fread(lod.data(), 1, sz, f); fclose(f);

    // Model 1 LOD 0: offset=152, size=43552
    const uint8_t* data = lod.data() + 152;
    std::size_t size = 43552;

    // Read tag count
    uint32_t tag_count;
    memcpy(&tag_count, data, 4);
    printf("tag_count=%u\n", tag_count);

    // Read tags
    std::vector<int32_t> tags(tag_count);
    memcpy(tags.data(), data + 4, tag_count * 4);

    // Node data starts after tag list
    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* node_data = data + data_start;
    std::size_t node_data_size = size - data_start;
    printf("data_start=%zu node_data_size=%zu\n", data_start, node_data_size);

    // Compute node offsets (same as bsp_parser)
    auto node_disk_size = [](int type) -> int {
        // Including 4-byte vtable
        switch (type) {
            case 0: return 8;   // BNode
            case 1: return 36;  // BSubTree
            case 2: return 48;  // BRoot
            case 3: return 60;  // BSlotNode
            case 4: return 88;  // BDofNode
            case 5: return 20;  // BSwitchNode
            case 6: return 32;  // BSplitterNode
            case 7: return 12;  // BPrimitiveNode
            case 8: return 16;  // BLitPrimitiveNode
            case 9: return 12;  // BCulledPrimitiveNode
            case 10: return 24; // BSpecialXform
            case 11: return 36; // BLightStringNode
            case 12: return 72; // BTransNode
            case 13: return 84; // BScaleNode
            case 14: return 108;// BXDofNode
            case 15: return 24; // BXSwitchNode
            case 16: return 44; // BRenderControlNode
            default: return 8;
        }
    };

    std::vector<std::size_t> node_offsets;
    std::size_t cur = 0;
    for (uint32_t i = 0; i < tag_count; ++i) {
        node_offsets.push_back(cur);
        cur += node_disk_size(tags[i]);
    }
    printf("Total node bytes: %zu (buffer: %zu)\n", cur, node_data_size);

    // Read node 0 (BRoot) raw
    printf("\nNode 0 (BRoot) at offset 0:\n");
    int32_t vtable, sibling;
    memcpy(&vtable, node_data, 4);
    memcpy(&sibling, node_data + 4, 4);
    printf("  vtable=%d sibling=%d\n", vtable, sibling);

    int32_t coords_off, n_coords, n_dyn, dyn_off, normals_off, n_normals, subtree_off;
    memcpy(&coords_off, node_data + 8, 4);
    memcpy(&n_coords, node_data + 12, 4);
    memcpy(&n_dyn, node_data + 16, 4);
    memcpy(&dyn_off, node_data + 20, 4);
    memcpy(&normals_off, node_data + 24, 4);
    memcpy(&n_normals, node_data + 28, 4);
    memcpy(&subtree_off, node_data + 32, 4);
    printf("  coords_off=%d n_coords=%d n_dyn=%d dyn_off=%d\n", coords_off, n_coords, n_dyn, dyn_off);
    printf("  normals_off=%d n_normals=%d subtree_off=%d\n", normals_off, n_normals, subtree_off);

    int32_t tex_off, n_tex, script;
    memcpy(&tex_off, node_data + 36, 4);
    memcpy(&n_tex, node_data + 40, 4);
    memcpy(&script, node_data + 44, 4);
    printf("  tex_off=%d n_tex=%d script=%d\n", tex_off, n_tex, script);

    // Check if subtree_off matches any node offset
    if (subtree_off >= 0) {
        bool found = false;
        for (uint32_t i = 0; i < tag_count; ++i) {
            if (node_offsets[i] == static_cast<std::size_t>(subtree_off)) {
                printf("  subtree_off %d matches node %d\n", subtree_off, i);
                found = true;
                break;
            }
        }
        if (!found) {
            printf("  subtree_off %d does NOT match any node offset!\n", subtree_off);
            // Find closest
            std::size_t closest = 0;
            for (uint32_t i = 0; i < tag_count; ++i) {
                if (node_offsets[i] < static_cast<std::size_t>(subtree_off)) closest = i;
            }
            printf("  Closest: node %d at offset %zu\n", closest, node_offsets[closest]);
        }
    }

    // Check first few primitive nodes
    printf("\nFirst primitive nodes:\n");
    int shown = 0;
    for (uint32_t i = 0; i < tag_count && shown < 5; ++i) {
        if (tags[i] == 7 || tags[i] == 9) { // BPrimitiveNode or BCulledPrimitiveNode
            int32_t prim_off;
            memcpy(&prim_off, node_data + node_offsets[i] + 8, 4);
            printf("  node %d (tag=%d) at offset %zu: prim_offset=%d\n",
                   i, tags[i], node_offsets[i], prim_off);
            shown++;
        }
    }

    return 0;
}

// Correct BSlotNode field reading: subtree is at offset 60 from node start
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

int main() {
    FILE* f = fopen("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> lod(sz);
    fread(lod.data(), 1, sz, f); fclose(f);

    const uint8_t* nd = lod.data() + 152;  // LOD record start
    std::size_t size = 43552;

    uint32_t tag_count;
    memcpy(&tag_count, nd, 4);
    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* node_data = nd + data_start;

    printf("tag_count=%u data_start=%zu\n\n", tag_count, data_start);

    // Read tags
    std::vector<int32_t> tags(tag_count);
    memcpy(tags.data(), nd + 4, tag_count * 4);

    // Node sizes (with BSlotNode = 64)
    auto node_size = [](int t) -> int {
        switch (t) {
            case 0: return 8; case 1: return 36; case 2: return 48;
            case 3: return 64; case 4: return 88; case 5: return 20;
            case 6: return 32; case 7: return 12; case 8: return 16;
            case 9: return 12; case 10: return 24; case 11: return 36;
            case 12: return 72; case 13: return 84; case 14: return 108;
            case 15: return 24; case 16: return 44;
            default: return 8;
        }
    };

    // Compute node offsets
    std::vector<std::size_t> offsets;
    std::size_t cur = 0;
    for (uint32_t i = 0; i < tag_count; ++i) {
        offsets.push_back(cur);
        cur += node_size(tags[i]);
    }
    printf("Total node bytes: %zu (buffer: %zu)\n\n", cur, size - data_start);

    // Check BRoot (node 0)
    printf("Node 0 (BRoot) at offset 0:\n");
    // subtree is at offset 32 from node start (after vtable+sibling+7fields)
    int32_t subtree_off;
    memcpy(&subtree_off, node_data + 0 + 32, 4);
    printf("  subtree_off=%d\n", subtree_off);
    // Check if it matches any node offset
    for (uint32_t i = 0; i < tag_count; ++i) {
        if (offsets[i] == static_cast<std::size_t>(subtree_off)) {
            printf("  -> matches node %u (tag=%d)\n", i, tags[i]);
            break;
        }
    }

    // Check first 8 BSlotNodes
    int slot_count = 0;
    for (uint32_t i = 0; i < tag_count && slot_count < 8; ++i) {
        if (tags[i] != 3) continue;
        slot_count++;
        // BSlotNode: vtable(4) + sibling(4) + rotation(36) + origin(12) + slotNumber(4) + subTree(4)
        // subtree at offset 60 from node start
        int32_t slot_subtree;
        memcpy(&slot_subtree, node_data + offsets[i] + 60, 4);
        int32_t slot_num;
        memcpy(&slot_num, node_data + offsets[i] + 56, 4);
        printf("Node %u (BSlotNode) at offset %zu: slotNumber=%d subtree_off=%d",
               i, offsets[i], slot_num, slot_subtree);
        // Check match
        for (uint32_t j = 0; j < tag_count; ++j) {
            if (offsets[j] == static_cast<std::size_t>(slot_subtree)) {
                printf(" -> node %u (tag=%d)", j, tags[j]);
                break;
            }
        }
        printf("\n");
    }

    // Check first few primitive nodes
    printf("\nFirst primitive nodes:\n");
    int shown = 0;
    for (uint32_t i = 0; i < tag_count && shown < 5; ++i) {
        if (tags[i] == 7 || tags[i] == 9) {
            int32_t prim_off;
            memcpy(&prim_off, node_data + offsets[i] + 8, 4);
            printf("  node %u (tag=%d) at offset %zu: prim_offset=%d\n",
                   i, tags[i], offsets[i], prim_off);
            shown++;
        }
    }

    return 0;
}

// Try BSlotNode sizes 56, 60, 64 and check which gives valid slotNumbers
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

int main() {
    FILE* f = fopen("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> lod(sz);
    fread(lod.data(), 1, sz, f); fclose(f);

    const uint8_t* nd = lod.data() + 152;
    std::size_t size = 43552;
    uint32_t tag_count;
    memcpy(&tag_count, nd, 4);
    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* node_data = nd + data_start;
    std::vector<int32_t> tags(tag_count);
    memcpy(tags.data(), nd + 4, tag_count * 4);

    for (int slot_sz : {56, 60, 64}) {
        printf("=== BSlotNode size = %d ===\n", slot_sz);
        auto node_size = [&](int t) -> int {
            switch (t) {
                case 0: return 8; case 1: return 36; case 2: return 48;
                case 3: return slot_sz; case 4: return 88; case 5: return 20;
                case 6: return 32; case 7: return 12; case 8: return 16;
                case 9: return 12; case 10: return 24; case 11: return 36;
                case 12: return 72; case 13: return 84; case 14: return 108;
                case 15: return 24; case 16: return 44;
                default: return 8;
            }
        };

        std::vector<std::size_t> offsets;
        std::size_t cur = 0;
        bool ok = true;
        for (uint32_t i = 0; i < tag_count; ++i) {
            offsets.push_back(cur);
            cur += node_size(tags[i]);
        }

        // Check BRoot subtree
        int32_t root_subtree;
        memcpy(&root_subtree, node_data + 32, 4);
        bool root_match = false;
        for (uint32_t i = 0; i < tag_count; ++i) {
            if (offsets[i] == static_cast<std::size_t>(root_subtree)) {
                root_match = true;
                printf("  BRoot subtree=%d -> node %u (tag=%d)\n", root_subtree, i, tags[i]);
                break;
            }
        }
        if (!root_match) printf("  BRoot subtree=%d -> NO MATCH\n", root_subtree);

        // Check BSlotNodes
        int valid_slots = 0, total_slots = 0;
        int slot_subtree_field = slot_sz - 4;  // subtree is last field (if present)
        int slot_num_field = slot_sz - 8;       // slotNumber is 2nd-to-last (if subtree present)
        if (slot_sz == 56) { slot_num_field = -1; slot_subtree_field = -1; } // no slotNumber or subtree
        if (slot_sz == 60) { slot_num_field = 56; slot_subtree_field = -1; } // slotNumber but no subtree
        if (slot_sz == 64) { slot_num_field = 56; slot_subtree_field = 60; }

        for (uint32_t i = 0; i < tag_count; ++i) {
            if (tags[i] != 3) continue;
            total_slots++;
            if (slot_num_field > 0 && offsets[i] + slot_num_field + 4 < size - data_start) {
                int32_t slot_num;
                memcpy(&slot_num, node_data + offsets[i] + slot_num_field, 4);
                if (slot_num >= 0 && slot_num < 20) valid_slots++;
                if (total_slots <= 8)
                    printf("  BSlotNode %u at %zu: slotNumber=%d", i, offsets[i], slot_num);
            }
            if (slot_subtree_field > 0 && offsets[i] + slot_subtree_field + 4 < size - data_start) {
                int32_t sub_off;
                memcpy(&sub_off, node_data + offsets[i] + slot_subtree_field, 4);
                if (total_slots <= 8) {
                    printf(" subtree=%d", sub_off);
                    // Check match
                    for (uint32_t j = 0; j < tag_count; ++j) {
                        if (offsets[j] == static_cast<std::size_t>(sub_off)) {
                            printf("->node%u", j);
                            break;
                        }
                    }
                }
            }
            if (total_slots <= 8) printf("\n");
        }
        printf("  Valid slotNumbers: %d/%d\n", valid_slots, total_slots);
        printf("\n");
    }

    return 0;
}

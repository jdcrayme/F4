// Print all tags and compute expected offsets, comparing with actual data
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

const char* tag_name(int t) {
    static const char* names[] = {
        "BNode","BSubTree","BRoot","BSlotNode","BDofNode","BSwitchNode",
        "BSplitterNode","BPrimitiveNode","BLitPrimitiveNode","BCulledPrimitiveNode",
        "BSpecialXform","BLightStringNode","BTransNode","BScaleNode",
        "BXDofNode","BXSwitchNode","BRenderControlNode"
    };
    if (t >= 0 && t < 17) return names[t];
    return "?";
}

int main() {
    FILE* f = fopen("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> lod(sz);
    fread(lod.data(), 1, sz, f); fclose(f);

    const uint8_t* data = lod.data() + 152; // Model 1 LOD 0
    std::size_t size = 43552;

    uint32_t tag_count;
    memcpy(&tag_count, data, 4);
    printf("tag_count=%u\n\n", tag_count);

    std::vector<int32_t> tags(tag_count);
    memcpy(tags.data(), data + 4, tag_count * 4);

    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* nd = data + data_start;
    std::size_t nd_size = size - data_start;

    // Print tags in order with node sizes
    auto node_disk_size = [](int type) -> int {
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

    // Print first 20 tags with offsets
    std::size_t cur = 0;
    for (uint32_t i = 0; i < tag_count && i < 20; ++i) {
        int sz_i = node_disk_size(tags[i]);
        printf("  node %3u: tag=%2d %-22s offset=%5zu size=%3d", i, tags[i], tag_name(tags[i]), cur, sz_i);

        // For BRoot, show subtree_off
        if (tags[i] == 2 && cur + 36 < nd_size) {
            int32_t subtree_off;
            memcpy(&subtree_off, nd + cur + 32, 4);  // vtable(4)+sibling(4)+7*4=32
            printf("  subtree_off=%d", subtree_off);
        }
        // For BSlotNode, show subtree
        if (tags[i] == 3 && cur + 56 < nd_size) {
            int32_t subtree_off;
            memcpy(&subtree_off, nd + cur + 52, 4);  // after rotation+origin+slotNumber
            printf("  subtree_off=%d", subtree_off);
        }
        // For BSubTree, show subtree
        if (tags[i] == 1 && cur + 36 < nd_size) {
            int32_t subtree_off;
            memcpy(&subtree_off, nd + cur + 32, 4);
            printf("  subtree_off=%d", subtree_off);
        }
        // For BSplitterNode, show front/back
        if (tags[i] == 6 && cur + 32 < nd_size) {
            int32_t front, back;
            memcpy(&front, nd + cur + 24, 4);
            memcpy(&back, nd + cur + 28, 4);
            printf("  front=%d back=%d", front, back);
        }
        // For BPrimitiveNode/BCulledPrimitiveNode, show prim_offset
        if ((tags[i] == 7 || tags[i] == 9) && cur + 12 < nd_size) {
            int32_t prim_off;
            memcpy(&prim_off, nd + cur + 8, 4);
            printf("  prim_off=%d", prim_off);
        }

        printf("\n");
        cur += sz_i;
    }

    // Check if subtree_off of BRoot (32788) could be the BYTE offset from
    // the start of the entire LOD record (including tag list)
    printf("\nBRoot subtree_off=32788\n");
    printf("  As offset from LOD record start: byte 32788\n");
    printf("  As offset from node data start: byte %d\n", 32788 - (int)data_start);
    printf("  data_start=%zu\n", data_start);
    printf("  If subtree_off is from LOD record start, then node is at %d in node buffer\n",
           32788 - (int)data_start);

    // Read what's at offset 32788 from the LOD record start
    if (32788 < size) {
        int32_t val;
        memcpy(&val, data + 32788, 4);
        printf("  Data at LOD offset 32788: %d (0x%08X)\n", val, val);
    }

    return 0;
}

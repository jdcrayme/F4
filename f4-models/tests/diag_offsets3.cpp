// Quick diagnostic: check node offsets and raw data for model 1
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>

int main() {
    std::ifstream lf("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", std::ios::binary | std::ios::ate);
    auto lod_sz = lf.tellg();
    lf.seekg(0);
    std::vector<uint8_t> lod_data(lod_sz);
    lf.read(reinterpret_cast<char*>(lod_data.data()), lod_sz);

    // LOD entry for model 1: offset=152, size=43552
    const uint8_t* data = lod_data.data() + 152;
    std::size_t size = 43552;

    // Read tag count
    uint32_t tag_count = 0;
    std::memcpy(&tag_count, data, 4);
    printf("tag_count = %u\n", tag_count);

    // Node sizes (including vtable)
    auto node_sz = [](int t) -> int {
        switch(t) {
            case 0: return 8; case 1: return 36; case 2: return 48;
            case 3: return 60; case 4: return 88; case 5: return 20;
            case 6: return 32; case 7: return 12; case 8: return 16;
            case 9: return 12; case 10: return 24; case 11: return 36;
            case 12: return 72; case 13: return 84; case 14: return 108;
            case 15: return 24; case 16: return 44; default: return 8;
        }
    };
    auto node_nm = [](int t) -> const char* {
        switch(t) {
            case 0: return "BNode"; case 1: return "BSubTree"; case 2: return "BRoot";
            case 3: return "BSlot"; case 4: return "BDof"; case 5: return "BSwitch";
            case 6: return "BSplit"; case 7: return "BPrim"; case 8: return "BLitPrim";
            case 9: return "BCulled"; case 10: return "BSpecXf"; case 11: return "BLight";
            case 12: return "BTrans"; case 13: return "BScale"; case 14: return "BXDof";
            case 15: return "BXSwitch"; case 16: return "BRender"; default: return "?";
        }
    };

    // Read tags
    std::vector<int> tags(tag_count);
    for (uint32_t i = 0; i < tag_count; i++) {
        int32_t v;
        std::memcpy(&v, data + 4 + i * 4, 4);
        tags[i] = (v >= 0 && v <= 16) ? v : -1;
    }

    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* nd = data + data_start;
    std::size_t nd_size = size - data_start;
    printf("data_start=%zu nd_size=%zu\n", data_start, nd_size);

    // Compute node offsets
    std::vector<std::size_t> offsets;
    std::size_t cur = 0;
    for (uint32_t i = 0; i < tag_count; i++) {
        offsets.push_back(cur);
        cur += node_sz(tags[i]);
    }
    printf("Total node bytes: %zu\n", cur);

    // Print first 50 nodes
    printf("\nNode offsets and fields:\n");
    for (uint32_t i = 0; i < std::min(tag_count, 50u); i++) {
        printf("[%3u] %-10s off=%5zu", i, node_nm(tags[i]), offsets[i]);
        if (offsets[i] + 8 <= nd_size) {
            int32_t sib;
            std::memcpy(&sib, nd + offsets[i] + 4, 4);
            printf(" sib=%d", sib);
            if (tags[i] == 7 || tags[i] == 9) { // BPrim/BCulled
                int32_t po;
                std::memcpy(&po, nd + offsets[i] + 8, 4);
                printf(" prim=%d", po);
            }
            if (tags[i] == 5) { // BSwitch
                int32_t sw, nc, co;
                std::memcpy(&sw, nd + offsets[i] + 8, 4);
                std::memcpy(&nc, nd + offsets[i] + 12, 4);
                std::memcpy(&co, nd + offsets[i] + 16, 4);
                printf(" sw=%d nc=%d co=%d", sw, nc, co);
            }
        }
        printf("\n");
    }

    // Find and print ALL switch nodes
    printf("\nAll switch nodes:\n");
    for (uint32_t i = 0; i < tag_count; i++) {
        if (tags[i] == 5 || tags[i] == 15) {
            if (offsets[i] + 20 <= nd_size) {
                int32_t sw, nc, co;
                std::memcpy(&sw, nd + offsets[i] + 8, 4);
                std::memcpy(&nc, nd + offsets[i] + 12, 4);
                std::memcpy(&co, nd + offsets[i] + 16, 4);
                printf("[%3u] %s off=%zu sw=%d nc=%d co=%d\n",
                       i, node_nm(tags[i]), offsets[i], sw, nc, co);
            }
        }
    }

    return 0;
}

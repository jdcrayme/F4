// Print tag values from LOD record
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

    const uint8_t* data = lod_data.data() + 152;  // LOD entry offset
    std::size_t size = 43552;

    uint32_t tag_count = 0;
    std::memcpy(&tag_count, data, 4);
    printf("tag_count = %u\n\n", tag_count);

    auto tag_name = [](int v) -> const char* {
        switch(v) {
            case 0: return "BNode"; case 1: return "BSubTree"; case 2: return "BRoot";
            case 3: return "BSlotNode"; case 4: return "BDofNode"; case 5: return "BSwitchNode";
            case 6: return "BSplitterNode"; case 7: return "BPrimitiveNode";
            case 8: return "BLitPrimitiveNode"; case 9: return "BCulledPrimitiveNode";
            case 10: return "BSpecialXform"; case 11: return "BLightStringNode";
            case 12: return "BTransNode"; case 13: return "BScaleNode";
            case 14: return "BXDofNode"; case 15: return "BXSwitchNode";
            case 16: return "BRenderControlNode"; default: return "UNKNOWN";
        }
    };

    // Print all tag values
    for (uint32_t i = 0; i < tag_count; i++) {
        int32_t v;
        std::memcpy(&v, data + 4 + i * 4, 4);
        printf("[%3u] raw=%2d %s\n", i, v, tag_name(v));
    }

    return 0;
}

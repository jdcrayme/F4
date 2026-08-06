// Hex dump at specific offsets in nodeTreeData
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

    // Skip tag count + tag list
    uint32_t tag_count = 0;
    std::memcpy(&tag_count, data, 4);
    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* nd = data + data_start;
    std::size_t nd_size = size - data_start;

    // Hex dump function
    auto hexdump = [&](const char* label, std::size_t off, int bytes) {
        printf("%s (offset %zu):\n", label, off);
        for (int i = 0; i < bytes; i++) {
            if (off + i < nd_size)
                printf("%02X ", nd[off + i]);
            else
                printf("?? ");
            if ((i + 1) % 16 == 0) printf("\n");
        }
        if (bytes % 16 != 0) printf("\n");

        // Also show as floats and ints
        printf("  As int32:");
        for (int i = 0; i < bytes && i < 48; i += 4) {
            if (off + i + 4 <= nd_size) {
                int32_t v; std::memcpy(&v, nd + off + i, 4);
                printf(" %d", v);
            }
        }
        printf("\n  As float:");
        for (int i = 0; i < bytes && i < 48; i += 4) {
            if (off + i + 4 <= nd_size) {
                float v; std::memcpy(&v, nd + off + i, 4);
                printf(" %g", v);
            }
        }
        printf("\n");
    };

    // Dump BRoot at offset 0
    hexdump("BRoot", 0, 48);

    // Dump BSlot nodes (size 60 each after fix)
    // Node 1 at offset 48
    hexdump("BSlot1", 48, 60);

    // Node 2 at offset 108
    hexdump("BSlot2", 108, 60);

    // Node 3 at offset 168
    hexdump("BSlot3", 168, 60);

    // Node 4 at offset 228
    hexdump("BSlot4", 228, 60);

    return 0;
}

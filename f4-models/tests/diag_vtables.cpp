// Find vtable signatures to determine actual node boundaries
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

    const uint8_t* data = lod_data.data() + 152;
    std::size_t size = 43552;

    uint32_t tag_count = 0;
    std::memcpy(&tag_count, data, 4);
    std::size_t data_start = 4 + tag_count * 4;
    const uint8_t* nd = data + data_start;
    std::size_t nd_size = size - data_start;

    // Search for vtable signatures (0x004651xx or similar patterns)
    // In MSVC, vtables are typically in the .rdata section at addresses
    // like 0x0046xxxx. Look for 4-byte values in this range.
    printf("Searching for vtable signatures in nodeTreeData...\n");
    std::vector<std::size_t> vtable_offsets;
    for (std::size_t i = 0; i + 4 <= nd_size; i += 4) {
        uint32_t v;
        std::memcpy(&v, nd + i, 4);
        // Check for vtable-like values (0x00460000 - 0x00470000)
        if (v >= 0x00460000 && v <= 0x00470000) {
            vtable_offsets.push_back(i);
        }
    }

    printf("Found %zu vtable-like offsets\n", vtable_offsets.size());
    printf("First 30:\n");
    for (std::size_t i = 0; i < std::min(vtable_offsets.size(), (std::size_t)30); i++) {
        uint32_t v;
        std::memcpy(&v, nd + vtable_offsets[i], 4);
        int32_t sib;
        std::memcpy(&sib, nd + vtable_offsets[i] + 4, 4);
        printf("[%2zu] offset=%5zu vtable=0x%08X sib=%d", i, vtable_offsets[i], v, sib);
        if (i > 0) {
            printf(" (gap=%zu)", vtable_offsets[i] - vtable_offsets[i-1]);
        }
        printf("\n");
    }

    return 0;
}

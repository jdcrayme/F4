// Dump first 200 bytes of node data buffer to understand layout
#include <cstdio>
#include <cstdint>
#include <cstring>

int main() {
    FILE* f = fopen("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* lod = new uint8_t[sz];
    fread(lod, 1, sz, f); fclose(f);

    const uint8_t* data = lod + 152;  // Model 1 LOD 0
    std::size_t size = 43552;

    uint32_t tag_count;
    memcpy(&tag_count, data, 4);
    std::size_t data_start = 4 + tag_count * 4;

    printf("data_start = %zu\n", data_start);
    printf("BRoot subtree_off = 32788 (LOD-record-relative)\n");
    printf("Node-data-relative: %d\n", 32788 - (int)data_start);
    printf("\n");

    // Dump what's at LOD offset 32788
    printf("=== Bytes at LOD offset 32788 (BRoot's claimed subtree) ===\n");
    for (int row = 0; row < 8; ++row) {
        printf("%04x: ", 32788 + row*16);
        for (int col = 0; col < 16; ++col) {
            std::size_t pos = 32788 + row*16 + col;
            if (pos < size) printf("%02x ", data[pos]);
        }
        printf("  ");
        for (int col = 0; col < 16; ++col) {
            std::size_t pos = 32788 + row*16 + col;
            if (pos < size) {
                char c = data[pos];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
        }
        printf("\n");
    }

    // Dump first node data section (after tag list)
    printf("\n=== First 256 bytes of node data (after tag list) ===\n");
    for (int row = 0; row < 16; ++row) {
        printf("%04x: ", row*16);
        for (int col = 0; col < 16; ++col) {
            std::size_t pos = data_start + row*16 + col;
            if (pos < size) printf("%02x ", data[pos]);
        }
        printf("\n");
    }

    // Check what's just after BRoot (48 bytes)
    printf("\n=== Bytes after BRoot (offset 48-112 in node data) ===\n");
    for (int row = 0; row < 4; ++row) {
        printf("%04x: ", 48 + row*16);
        for (int col = 0; col < 16; ++col) {
            std::size_t pos = data_start + 48 + row*16 + col;
            if (pos < size) printf("%02x ", data[pos]);
        }
        printf("  ");
        for (int col = 0; col < 16; ++col) {
            std::size_t pos = data_start + 48 + row*16 + col;
            if (pos < size) {
                char c = data[pos];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
        }
        printf("\n");
    }

    delete[] lod;
    return 0;
}

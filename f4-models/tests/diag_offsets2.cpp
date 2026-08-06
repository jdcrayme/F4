// Check what subtree_off=32788 actually points to
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

int main() {
    FILE* f = fopen("/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD", "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> lod(sz);
    fread(lod.data(), 1, sz, f); fclose(f);

    // Model 1 LOD 0 starts at offset 152 in the LOD file
    const uint8_t* data = lod.data() + 152;
    std::size_t size = 43552;

    uint32_t tag_count;
    memcpy(&tag_count, data, 4);
    std::size_t data_start = 4 + + tag_count * 4;  // 1236

    printf("tag_count=%u data_start=%zu\n", tag_count, data_start);
    printf("subtree_off from BRoot = 32788\n\n");

    // Interpretation A: offset relative to LOD record byte 0
    printf("=== Interpretation A: offset from LOD record byte 0 ===\n");
    int off_a = 32788;
    if (off_a < (int)size) {
        printf("Byte %d of LOD record:\n", off_a);
        for (int i = 0; i < 16; ++i) {
            if (off_a + i < (int)size)
                printf(" %02X", data[off_a + i]);
        }
        printf("\n");
        // As ints
        int32_t v;
        memcpy(&v, data + off_a, 4);
        printf("  As int32: %d\n", v);
    }

    // Interpretation B: offset relative to node data start (byte data_start)
    printf("\n=== Interpretation B: offset from node data start ===\n");
    int off_b = 32788;
    if (off_b + data_start < size) {
        printf("Byte %zu of LOD record (data_start + off):\n", data_start + off_b);
        for (int i = 0; i < 16; ++i) {
            std::size_t pos = data_start + off_b + i;
            if (pos < size)
                printf(" %02X", data[pos]);
        }
        printf("\n");
        int32_t v;
        memcpy(&v, data + data_start + off_b, 4);
        printf("  As int32: %d\n", v);
    }

    // Interpretation C: offset in DWORDs from LOD record start
    printf("\n=== Interpretation C: offset in DWORDs from LOD record byte 0 ===\n");
    int off_c = 32788 * 4;
    printf("  byte offset = %d (way beyond buffer size %zu)\n", off_c, size);

    // Let's also check the Python tool's output for model 1
    // to see what it thinks the subtree is
    printf("\n=== What does the Python tool say? ===\n");
    printf("From model_1.json: root has subtree with type BSubTree\n");

    // Try reading what's at different offsets
    // Maybe the pointer arithmetic is DWORD-based but relative to
    // a different base. Let me check if the offsets make sense
    // as indices into the tag list
    printf("\n=== Checking if subtree_off could be a node INDEX ===\n");
    printf("32788 as node index -> way beyond 308 nodes\n");
    printf("But what if it's (index * some_factor)?\n");

    // Check coords_off=32848 — is this also an LOD-record-relative offset?
    // And normals_off=39052
    printf("\n=== Checking coords/normals offsets ===\n");
    printf("BRoot: coords_off=32848, normals_off=39052, tex_off=42112\n");
    printf("If these are LOD-record-relative:\n");
    // coords: 517 * 12 = 6204 bytes
    if (32848 + 517*12 <= (int)size) {
        printf("  At 32848: first coord = ");
        float x, y, z;
        memcpy(&x, data + 32848, 4);
        memcpy(&y, data + 32848 + 4, 4);
        memcpy(&z, data + 32848 + 8, 4);
        printf("(%.1f, %.1f, %.1f)\n", x, y, z);
    }
    // normals: 255 * 12 = 3060 bytes
    if (39052 + 255*12 <= (int)size) {
        printf("  At 39052: first normal = ");
        float i, j, k;
        memcpy(&i, data + 39052, 4);
        memcpy(&j, data + 39052 + 4, 4);
        memcpy(&k, data + 39052 + 8, 4);
        printf("(%.3f, %.3f, %.3f)\n", i, j, k);
    }
    // tex: 17 * 4 = 68 bytes
    if (42112 + 17*4 <= (int)size) {
        printf("  At 42112: first tex_id = ");
        int32_t tid;
        memcpy(&tid, data + 42112, 4);
        printf("%d\n", tid);
    }

    // If coords work, then offsets ARE LOD-record-relative
    // and subtree_off=32788 is also LOD-record-relative

    return 0;
}

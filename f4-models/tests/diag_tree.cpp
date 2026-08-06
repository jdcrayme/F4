// Diagnostic: check BSP tree structure for model 1
#include <f4/models/f4_models.hpp>
#include <cstdio>

int main() {
    f4::models::ModelDatabase db;
    auto err = db.load(
        "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.HDR",
        "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD");
    if (!err.empty()) { printf("Error: %s\n", err.c_str()); return 1; }

    auto pe = db.parse_lod(1, 0);
    if (!pe.empty()) { printf("Parse error: %s\n", pe.c_str()); return 1; }

    // We need access to the parsed LOD to inspect the BSP tree.
    // Since parsed_lods_ is private, let's just re-parse directly.
    // Actually, let's just use extract_model_geometry and check what happens.
    // The issue is likely in the traversal or prim decoding.

    // Let's directly check: does the tree have BPrimitiveNode nodes?
    // We can't access the tree directly through the public API.
    // Let me check by using the BSP tree JSON output.

    // Actually, let me just print what the model's LOD looks like
    auto* m = db.model(1);
    printf("Model 1: n_lods=%d\n", m->n_lods);
    for (int i = 0; i < (int)m->lods.size(); ++i) {
        printf("  LOD %d: idx=%d max_range=%.0f\n", i, m->lods[i].lod_table_idx, m->lods[i].max_range);
    }

    // Try extracting geometry
    auto geom = db.extract_model_geometry(1, 0);
    printf("Geometry: %zu meshes, %zu verts, %zu tris\n",
           geom.meshes.size(), geom.total_vertices(), geom.total_triangles());

    // The problem is likely that the BSP tree's node_buffer is empty,
    // or the prim offsets are not being decoded. Let me check the
    // parse_lod path more carefully.

    return 0;
}

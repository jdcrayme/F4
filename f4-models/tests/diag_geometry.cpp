// Diagnostic: extract geometry from several models and print stats
#include <f4/models/f4_models.hpp>
#include <cstdio>

int main() {
    f4::models::ModelDatabase db;
    auto err = db.load(
        "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.HDR",
        "/home/z/my-project/f4-repo/f4-models/tests/fixtures/KoreaObj.LOD");
    if (!err.empty()) { printf("Load error: %s\n", err.c_str()); return 1; }

    printf("Loaded %d models\n", db.n_models());

    // Test geometry extraction for a range of models
    for (int idx : {1, 2, 3, 5? 5 : 0, 42, 100}) {
        if (idx >= db.n_models()) continue;
        auto* m = db.model(idx);
        if (!m || m->lods.empty()) continue;

        auto pe = db.parse_lod(idx, 0);
        if (!pe.empty()) { printf("  model %d: parse error: %s\n", idx, pe.c_str()); continue; }

        auto geom = db.extract_model_geometry(idx, 0);
        printf("model %d: class=%-7s radius=%.1f n_lods=%d slots=%d dofs=%d → %zu meshes, %zu verts, %zu tris\n",
               idx, m->visual_class().data(), m->radius,
               m->n_lods, m->n_slots, m->effective_dofs(),
               geom.meshes.size(), geom.total_vertices(), geom.total_triangles());

        for (std::size_t i = 0; i < geom.meshes.size(); ++i) {
            const auto& mesh = geom.meshes[i];
            printf("  mesh %zu: tex=%d verts=%zu tris=%zu\n",
                   i, mesh.tex_id, mesh.vertices.size(), mesh.triangles.size());
        }
    }

    return 0;
}

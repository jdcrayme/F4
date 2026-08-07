// dump_model_textures.cpp
//
// Standalone diagnostic: extract geometry for a model and report
// what tex_ids the meshes ended up with.

#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/model_record.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char* argv[]) {
    std::filesystem::path hdr_path = "../temp/KoreaObj.HDR";
    std::filesystem::path lod_path = "../temp/KoreaObj.LOD";
    std::filesystem::path tex_path = "../temp/KoreaObj.TEX";
    int target_parent = 829;
    int target_lod = 0;

    if (argc >= 2) hdr_path = argv[1];
    if (argc >= 3) lod_path = argv[2];
    if (argc >= 4) tex_path = argv[3];
    if (argc >= 5) target_parent = std::atoi(argv[4]);
    if (argc >= 6) target_lod = std::atoi(argv[5]);

    f4::models::ModelDatabase db;
    std::string err = db.load(hdr_path, lod_path);
    if (!err.empty()) {
        std::printf("ERROR: load: %s\n", err.c_str());
        return 1;
    }
    err = db.load_tex(tex_path);
    if (!err.empty()) {
        std::printf("WARNING: load_tex: %s\n", err.c_str());
    }

    const auto* rec = db.model(target_parent);
    if (!rec) {
        std::printf("ERROR: model %d not found\n", target_parent);
        return 1;
    }

    std::printf("Model %d: visual_class=%s, n_lods=%zu, n_texture_sets=%d, n_slots=%d, dofs=%d\n",
                target_parent, rec->visual_class().data(),
                rec->lods.size(),
                static_cast<int>(rec->n_texture_sets),
                static_cast<int>(rec->n_slots),
                rec->effective_dofs());
    std::printf("  bbox: x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]  radius=%.1f\n",
                rec->bbox.min_x, rec->bbox.max_x,
                rec->bbox.min_y, rec->bbox.max_y,
                rec->bbox.min_z, rec->bbox.max_z,
                rec->radius);

    for (std::size_t li = 0; li < rec->lods.size(); ++li) {
        const auto& lod = rec->lods[li];
        std::printf("  LOD %zu: name='%s' lod_table_idx=%d max_range=%.1f\n",
                    li, lod.name.c_str(), lod.lod_table_idx, lod.max_range);
    }

    if (target_lod < 0 || target_lod >= static_cast<int>(rec->lods.size())) {
        std::printf("ERROR: target_lod %d out of range (have %zu lods)\n",
                    target_lod, rec->lods.size());
        return 1;
    }

    err = db.parse_lod(target_parent, target_lod);
    if (!err.empty()) {
        std::printf("ERROR: parse_lod: %s\n", err.c_str());
        return 1;
    }

    f4::models::ModelState state;
    auto geom = db.extract_model_geometry(target_parent, target_lod, state);

    std::printf("\nExtracted %zu meshes, %zu triangles total:\n",
                geom.meshes.size(),
                [&](){ std::size_t t = 0;
                       for (const auto& m : geom.meshes) t += m.triangles.size();
                       return t; }());

    int n_textured = 0;
    int n_untextured = 0;
    int n_tri_textured = 0;
    int n_tri_untextured = 0;
    for (std::size_t i = 0; i < geom.meshes.size(); ++i) {
        const auto& m = geom.meshes[i];
        if (m.tex_id >= 0) {
            ++n_textured;
            n_tri_textured += m.triangles.size();
        } else {
            ++n_untextured;
            n_tri_untextured += m.triangles.size();
        }
    }

    std::printf("  Textured meshes:   %d  (%zu triangles)\n", n_textured, n_tri_textured);
    std::printf("  Untextured meshes: %d  (%zu triangles)\n", n_untextured, n_tri_untextured);

    // Look at unique tex_ids
    std::printf("\nMesh details:\n");
    for (std::size_t i = 0; i < geom.meshes.size(); ++i) {
        const auto& m = geom.meshes[i];
        const char* kind_str =
            (m.kind == f4::models::PrimitiveKind::Triangles) ? "tri" :
            (m.kind == f4::models::PrimitiveKind::Lines)     ? "lin" :
            (m.kind == f4::models::PrimitiveKind::Points)    ? "pnt" : "?";
        std::printf("  mesh[%3zu] tex_id=%-5d kind=%s verts=%zu tris=%zu\n",
                    i, m.tex_id, kind_str,
                    m.vertices.size(), m.triangles.size());
    }

    // Show texture bank info for the tex_ids in use
    std::printf("\nTexture bank entries referenced:\n");
    const auto& tex_entries = db.tex_entries();
    for (const auto& m : geom.meshes) {
        if (m.tex_id < 0) continue;
        if (static_cast<std::size_t>(m.tex_id) >= tex_entries.size()) {
            std::printf("  tex_id=%d OUT OF RANGE (tex_entries.size=%zu)\n",
                        m.tex_id, tex_entries.size());
            continue;
        }
        const auto& te = tex_entries[static_cast<std::size_t>(m.tex_id)];
        std::printf("  tex_id=%d  dim=%u  pal=%d  size=%u  chroma=0x%08X  flags=0x%X\n",
                    m.tex_id, te.dimension, te.palette_id,
                    te.file_size, te.chroma_key, te.flags);
    }

    return 0;
}

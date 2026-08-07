// test_tex_pipeline.cpp
//
// Standalone test: load KoreaObj.HDR + TEX, decode some textures,
// and report stats. Does NOT require Raylib or the viewer.

#include <f4/models/model_database.hpp>
#include <f4/models/texture.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char* argv[]) {
    std::filesystem::path hdr_path = "../temp/KoreaObj.HDR";
    std::filesystem::path tex_path = "../temp/KoreaObj.TEX";

    if (argc >= 2) hdr_path = argv[1];
    if (argc >= 3) tex_path = argv[2];

    std::printf("=== F4 TEX Pipeline Test ===\n");

    f4::models::ModelDatabase db;
    std::string err = db.load_hdr(hdr_path);
    if (!err.empty()) {
        std::printf("ERROR: load_hdr failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("HDR loaded: %d models, %d textures, %zu palettes, %zu tex_entries\n",
                db.n_models(), db.n_textures(),
                db.palettes().size(), db.tex_entries().size());

    err = db.load_tex(tex_path);
    if (!err.empty()) {
        std::printf("ERROR: load_tex failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("TEX loaded successfully\n");

    const int n_textures = static_cast<int>(db.tex_entries().size());
    const int max_decode = (n_textures > 20) ? 20 : n_textures;
    int n_success = 0;
    int n_fail = 0;

    std::printf("\nDecoding first %d textures:\n", max_decode);
    for (int i = 0; i < max_decode; ++i) {
        const auto* tex = db.fetch_texture(i);
        if (tex && tex->valid()) {
            std::printf("  [%d] %dx%d  pal=%d  alpha=%s  chroma=0x%08X  source=%s  OK\n",
                        i, tex->width, tex->height, tex->pal_id,
                        tex->has_alpha ? "yes" : "no",
                        tex->chroma_key,
                        tex->source == f4::models::DecodedTexture::Source::TEX ? "TEX" : "DDS");
            ++n_success;
        } else {
            const auto& entry = db.tex_entries()[i];
            std::printf("  [%d] dim=%u  pal=%d  offset=%u  size=%u  FAILED\n",
                        i, entry.dimension, entry.palette_id,
                        entry.file_offset, entry.file_size);
            ++n_fail;
        }
    }

    std::printf("\nDecoding all %d textures...\n", n_textures);
    int total_ok = 0;
    int total_fail = 0;
    int total_alpha = 0;
    int max_dim = 0;
    for (int i = 0; i < n_textures; ++i) {
        const auto* tex = db.fetch_texture(i);
        if (tex && tex->valid()) {
            ++total_ok;
            if (tex->has_alpha) ++total_alpha;
            if (tex->width > max_dim) max_dim = tex->width;
        } else {
            ++total_fail;
        }
    }

    std::printf("\n=== Results ===\n");
    std::printf("Total textures:  %d\n", n_textures);
    std::printf("Decoded OK:      %d (%.1f%%)\n", total_ok,
                n_textures > 0 ? 100.0 * total_ok / n_textures : 0.0);
    std::printf("Decode failures: %d (%.1f%%)\n", total_fail,
                n_textures > 0 ? 100.0 * total_fail / n_textures : 0.0);
    std::printf("With alpha:      %d\n", total_alpha);
    std::printf("Max dimension:   %d\n", max_dim);
    std::printf("Palettes:        %zu\n", db.palettes().size());

    return (total_ok > 0) ? 0 : 1;
}

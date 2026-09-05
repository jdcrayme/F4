// f4-import/include/f4/import/texture_png.hpp
//
// KoreaObj.TEX → PNG extraction (Tranche 0c of NO_BINARY_RUNTIME_PLAN.md).
//
// The ModelDatabase already decodes textures to RGBA8 (LZSS decompress →
// palette resolve → chroma key); this module just writes that buffer out
// as a PNG so the Data/ tree is self-contained — no KoreaObj.TEX needed
// at runtime.
//
// File layout mirrors the glTF emitter's material references: textures
// land in <out_dir>/NNNNN.png (5-digit zero-padded texture index), and
// emitted .gltf files reference them as "textures/NNNNN.png" relative to
// Data/Models/koreaobj/.

#pragma once

#include <f4/models/model_database.hpp>

#include <filesystem>

namespace f4::import {

/// Result of one texture → PNG conversion.
struct TexturePngResult {
    std::filesystem::path png_path;  // the written .png file
    int width = 0;
    int height = 0;
    bool has_alpha = false;    // any pixel with a < 255 (incl. chroma-keyed)
    bool chroma_keyed = false; // TexBankEntry declared a chroma key
};

/// Decode one texture from the database and write it as a PNG.
///
/// The database must have load_tex() called (fetch_texture needs the TEX
/// blobs); HDR/LOD or HDR alone provides the texture bank + palettes.
///
/// @param db         ModelDatabase with load_tex() applied.
/// @param tex_index  Texture bank index (0 .. n_textures()-1).
/// @param out_dir    Output directory (created if missing).
/// @returns          Path/dimension info for the written PNG.
/// @throws std::runtime_error on decode or write failure.
TexturePngResult write_texture_png(
    const f4::models::ModelDatabase& db,
    int tex_index,
    const std::filesystem::path& out_dir);

} // namespace f4::import

// f4-import/src/texture_png.cpp
//
// KoreaObj.TEX → PNG extraction. See texture_png.hpp for the module doc.

#include <f4/import/texture_png.hpp>

#include <stb_image_write.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace f4::import {

TexturePngResult write_texture_png(
    const f4::models::ModelDatabase& db,
    int tex_index,
    const std::filesystem::path& out_dir) {

    const f4::models::DecodedTexture* tex = db.fetch_texture(tex_index);
    if (!tex) {
        throw std::runtime_error(
            "texture " + std::to_string(tex_index) +
            " not decodable (is KoreaObj.TEX loaded? index in range?)");
    }

    std::filesystem::create_directories(out_dir);

    char name[16];
    std::snprintf(name, sizeof(name), "%05d.png", tex_index);
    std::filesystem::path png_path = out_dir / name;

    // RGBA8, top-down rows — the same orientation Falcon's D3D renderer
    // and glTF's UV convention both use, so no flip is needed.
    const int stride = tex->width * 4;
    const int ok = stbi_write_png(
        png_path.string().c_str(),
        tex->width, tex->height, 4,
        tex->rgba.data(), stride);
    if (ok == 0) {
        throw std::runtime_error("stbi_write_png failed for " + png_path.string());
    }

    TexturePngResult result;
    result.png_path = png_path;
    result.width = tex->width;
    result.height = tex->height;
    result.has_alpha = tex->has_alpha;

    const auto& entries = db.tex_entries();
    if (tex_index >= 0 && static_cast<std::size_t>(tex_index) < entries.size()) {
        result.chroma_keyed = entries[static_cast<std::size_t>(tex_index)].chroma_key != 0;
    }
    return result;
}

} // namespace f4::import

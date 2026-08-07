// f4-models/src/tex_reader.cpp
//
// Reads and decodes texture blobs from KoreaObj.Tex.
//
// Pipeline:
//   1. Seek to TexBankEntry.file_offset in .TEX data
//   2. Extract TexBankEntry.file_size bytes of LZSS-compressed data
//   3. Decompress to 8-bit indexed pixels (dimension × dimension bytes)
//   4. Resolve each byte through the DiskPalette → ARGB
//   5. Apply chroma key: pixels matching the key get alpha = 0
//   6. Convert ARGB → RGBA8 for GPU upload
//
// References:
//   FreeFalcon: src/graphics/bsplib/texturebank.cpp (TextureBankClass::OpenTextureFile)
//   FreeFalcon: src/graphics/image/imagebuf.cpp (ImageMemClass::Expand)

#include "tex_reader.hpp"
#include "bin_reader.hpp"

#include <f4/lzss/lzss.hpp>

#include <cmath>
#include <cstring>

namespace f4::models::detail {

bool read_tex_blob(
    const uint8_t* tex_data, std::size_t tex_size,
    const TexBankEntry& entry,
    const std::vector<DiskPalette>& palettes,
    int tex_index,
    DecodedTexture& out,
    std::string& err)
{
    out = {};

    // Validate entry
    if (entry.file_offset >= tex_size) {
        err = "texture offset out of TEX file bounds: " +
              std::to_string(entry.file_offset) + " >= " +
              std::to_string(tex_size);
        return false;
    }
    if (entry.file_offset + entry.file_size > tex_size) {
        err = "texture offset+size exceeds TEX file: " +
              std::to_string(entry.file_offset + entry.file_size) + " > " +
              std::to_string(tex_size);
        return false;
    }
    if (entry.dimension == 0 || entry.dimension > 8192) {
        err = "invalid texture dimension: " + std::to_string(entry.dimension);
        return false;
    }
    // Validate dimension is a power of 2
    if ((entry.dimension & (entry.dimension - 1)) != 0) {
        err = "texture dimension not power of 2: " + std::to_string(entry.dimension);
        return false;
    }
    if (entry.palette_id < 0 ||
        static_cast<std::size_t>(entry.palette_id) >= palettes.size()) {
        err = "palette ID out of range: " + std::to_string(entry.palette_id) +
              " (have " + std::to_string(palettes.size()) + " palettes)";
        return false;
    }

    const int width  = static_cast<int>(entry.dimension);
    const int height = static_cast<int>(entry.dimension);
    const std::size_t expected_pixels = static_cast<std::size_t>(width * height);

    // Step 1-2: Extract compressed data
    const uint8_t* compressed = tex_data + entry.file_offset;
    const std::size_t compressed_size = entry.file_size;

    // Step 3: LZSS decompress to 8-bit indexed pixels
    std::vector<uint8_t> indexed = f4::lzss::decompress(
        compressed, compressed_size, expected_pixels);

    if (indexed.empty()) {
        err = "LZSS decompression failed for texture " + std::to_string(tex_index);
        return false;
    }
    if (indexed.size() != expected_pixels) {
        // Dimension might not match — try using the actual decompressed size
        const auto sz = indexed.size();
        const int dim = static_cast<int>(std::sqrt(static_cast<double>(sz)));
        if (dim > 0 && static_cast<std::size_t>(dim * dim) == sz) {
            // It's a square texture with a different dimension
            out.width  = dim;
            out.height = dim;
        } else {
            // Non-square or unexpected — use the declared dimension and
            // pad/truncate
            out.width  = width;
            out.height = height;
            indexed.resize(expected_pixels, 0);
        }
    } else {
        out.width  = width;
        out.height = height;
    }

    const std::size_t total_pixels =
        static_cast<std::size_t>(out.width * out.height);

    // Step 4-6: Resolve palette indices → RGBA8 with chroma key
    const auto& palette = palettes[static_cast<std::size_t>(entry.palette_id)];

    // Extract chroma key RGB components for comparison.
    //
    // The chroma_key field in TexBankEntry is stored in the same ABGR
    // (DirectDraw) layout as the palette entries — see the comment on
    // DiskPalette::resolve in texture.hpp. We must extract R/B from the
    // same byte positions used for palette resolution, otherwise the
    // chroma-key comparison would compare the wrong channels and the
    // transparent pixels would never match.
    //
    // Reference value: 0xFFFF0000 = pure blue (sky/backdrop chroma key)
    //   With ABGR extraction: ck_r=0x00, ck_g=0x00, ck_b=0xFF → blue ✓
    //   With ARGB extraction: ck_r=0xFF, ck_g=0x00, ck_b=0x00 → red ✗
    const uint32_t ck = entry.chroma_key;
    const uint8_t ck_r = static_cast<uint8_t>(ck & 0xFF);
    const uint8_t ck_g = static_cast<uint8_t>((ck >> 8)  & 0xFF);
    const uint8_t ck_b = static_cast<uint8_t>((ck >> 16) & 0xFF);
    const bool has_chroma_key = (ck != 0);

    out.rgba.resize(total_pixels * 4);
    out.has_alpha = false;

    for (std::size_t i = 0; i < total_pixels; ++i) {
        const int pal_idx = (i < indexed.size())
            ? static_cast<int>(indexed[i])
            : 0;

        uint8_t r, g, b, a;
        palette.resolve(pal_idx, r, g, b, a);

        // Apply chroma key: if pixel color matches the key, make it transparent
        if (has_chroma_key && r == ck_r && g == ck_g && b == ck_b) {
            a = 0;
        }

        // Store as RGBA8 (Raylib's expected format)
        const std::size_t off = i * 4;
        out.rgba[off + 0] = r;
        out.rgba[off + 1] = g;
        out.rgba[off + 2] = b;
        out.rgba[off + 3] = a;

        if (a < 255) out.has_alpha = true;
    }

    out.tex_id     = tex_index;
    out.pal_id     = entry.palette_id;
    out.chroma_key = entry.chroma_key;
    out.source     = DecodedTexture::Source::TEX;

    return true;
}

} // namespace f4::models::detail

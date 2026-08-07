// f4-models/include/f4/models/texture.hpp
//
// Decoded texture — the output of TEX/DDS reading.
// Engine-agnostic: RGBA8 pixel data usable by any renderer.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace f4::models {

/// A decoded texture ready for upload to any GPU API.
///
/// For .TEX textures (FreeFalcon native):
///   - LZSS-compressed 8-bit indexed blobs are decompressed,
///     resolved through the PaletteBank, and chroma-key pixels
///     have alpha = 0.
///
/// For .DDS textures (DirectDraw Surface):
///   - BCn-compressed blocks are decoded to RGBA8.
struct DecodedTexture {
    int32_t     tex_id     = -1;    ///< index into TextureBank
    int32_t     pal_id     = -1;    ///< index into PaletteBank (-1 if DDS)
    int32_t     width      = 0;     ///< pixel width
    int32_t     height     = 0;     ///< pixel height
    std::vector<uint8_t> rgba;      ///< RGBA8 pixel data (width * height * 4)
    bool        has_alpha  = false;  ///< true if any pixel has alpha < 255
    uint32_t    chroma_key = 0;      ///< transparent pixel color (from TexBankEntry)

    /// Source format of this texture.
    enum class Source : uint8_t {
        Unknown = 0,
        TEX     = 1,   ///< FreeFalcon paletted (KoreaObj.Tex)
        DDS     = 2,   ///< DirectDraw Surface (.dds)
    };
    Source source = Source::Unknown;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width * height * 4);
    }
};

/// One entry from the HDR TextureBank (40 bytes on disk).
///
/// The on-disk layout (TempTexBankEntry in FreeFalcon's objectparent.h)
/// is 40 bytes per entry. The fields are:
///   [0]  fileOffset  — byte offset into KoreaObj.Tex
///   [4]  fileSize    — compressed size in bytes
///   [8]  dimension   — texture width = height (power of 2: 32, 64, 128, 256)
///   [12] palette     — palette index in PaletteBank
///   [16] spare1      — (unused)
///   [20] format      — 0xA0 = standard paletted, 0xE0 = extended
///   [24] chromaKey   — transparency color key (e.g., 0xFFFF0000 = blue)
///   [28] spare2      — (unused)
///   [32] extra       — additional flags (0, 1, or 2)
///   [36] spare3      — (unused)
struct TexBankEntry {
    uint32_t file_offset = 0;    ///< byte offset into .TEX
    uint32_t file_size   = 0;    ///< compressed size in bytes
    uint32_t dimension   = 0;    ///< texture width = height (power of 2)
    int32_t  palette_id  = 0;    ///< palette index in PaletteBank
    uint32_t format      = 0;    ///< 0xA0 = standard, 0xE0 = extended
    uint32_t chroma_key  = 0;    ///< transparency color key
    uint32_t extra       = 0;    ///< additional flags
};

/// One palette from the PaletteBank.
///
/// On disk: 1032 bytes = 256 × 4-byte ARGB entries + 8 bytes padding.
/// Each ARGB entry is a uint32: bits [31:24]=A, [23:16]=R, [15:8]=G, [7:0]=B.
struct DiskPalette {
    /// 256 ARGB entries (uint32 each, as stored on disk).
    std::array<uint32_t, 256> colors = {};

    /// Resolve a palette index to RGBA8 components.
    /// @param index  0..255 palette index
    /// @param r,g,b,a  output components
    void resolve(int index, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const noexcept {
        if (index < 0 || index > 255) { r = g = b = 0; a = 255; return; }
        const uint32_t argb = colors[static_cast<std::size_t>(index)];
        a = static_cast<uint8_t>((argb >> 24) & 0xFF);
        r = static_cast<uint8_t>((argb >> 16) & 0xFF);
        g = static_cast<uint8_t>((argb >> 8)  & 0xFF);
        b = static_cast<uint8_t>(argb & 0xFF);
    }
};

} // namespace f4::models

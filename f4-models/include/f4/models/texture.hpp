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
/// The on-disk layout corresponds to FreeFalcon's TempTexBankEntry
/// (texbank.h), which embeds a Texture object by value. The 40-byte
/// entry is 10 uint32s laid out as:
///
///   [0] fileOffset  — byte offset into KoreaObj.Tex
///   [1] fileSize    — compressed size in bytes
///   [2] dimension   — texture width = height (power of 2: 32..256)
///   [3] imageData   — runtime pointer (always 0 on disk)
///   [4] unused      — legacy field / padding (always 0)
///   [5] flags       — MPR_TI_* flags (0xA0 = CHROMAKEY|ALPHA|PALETTE)
///   [6] chromaKey   — transparency color key (ABGR format)
///   [7] palette     — runtime pointer (always 0 on disk)
///   [8] palID       — index into PaletteBank
///   [9] refCount    — runtime counter (always 0 on disk)
///
/// CRITICAL: palID is at index [8]. The previous implementation read
/// it from [3] (the imageData pointer, always 0), causing all textures
/// to use palette 0. Textures needing palette 1+ appeared as noise.
struct TexBankEntry {
    uint32_t file_offset = 0;    ///< byte offset into .TEX
    uint32_t file_size   = 0;    ///< compressed size in bytes
    uint32_t dimension   = 0;    ///< texture width = height (power of 2)
    int32_t  palette_id  = 0;    ///< index into PaletteBank (from [8])
    uint32_t flags       = 0;    ///< MPR_TI_* flags (0xA0=standard, 0xE0=extended)
    uint32_t chroma_key  = 0;    ///< transparency color key (ABGR format)
};

/// One palette from the PaletteBank.
///
/// On disk: 1032 bytes = 256 × 4-byte entries + 8 bytes padding.
/// Each entry is a uint32 stored in **ABGR** order (DirectDraw's
/// D3DFMT_A8R8G8B8 little-endian byte order): bits [31:24]=A,
/// [23:16]=B, [15:8]=G, [7:0]=R.
///
/// Empirical verification: the most common chroma-key value found in
/// TexBankEntry is `0xFFFF0000`, which FreeFalcon's documentation and
/// runtime both treat as "pure blue" (sky/backdrop transparency). Under
/// ARGB that hex value would be `A=0xFF,R=0xFF,G=0x00,B=0x00` = pure
/// red — contradicting every FreeFalcon reference. Under ABGR it is
/// `A=0xFF,B=0xFF,G=0x00,R=0x00` = pure blue, matching the reference.
/// The previous implementation interpreted these as ARGB, which
/// silently swapped the red and blue channels of every paletted
/// texture and made chroma-keyed regions appear as solid red instead
/// of transparent.
struct DiskPalette {
    /// 256 ABGR entries (uint32 each, as stored on disk).
    std::array<uint32_t, 256> colors = {};

    /// Resolve a palette index to RGBA8 components.
    /// @param index  0..255 palette index
    /// @param r,g,b,a  output components
    void resolve(int index, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const noexcept {
        if (index < 0 || index > 255) { r = g = b = 0; a = 255; return; }
        // On-disk layout is ABGR (DirectDraw little-endian):
        //   bits [31:24] = A
        //   bits [23:16] = B
        //   bits [15:8]  = G
        //   bits [7:0]   = R
        const uint32_t abgr = colors[static_cast<std::size_t>(index)];
        a = static_cast<uint8_t>((abgr >> 24) & 0xFF);
        b = static_cast<uint8_t>((abgr >> 16) & 0xFF);
        g = static_cast<uint8_t>((abgr >> 8)  & 0xFF);
        r = static_cast<uint8_t>(abgr & 0xFF);
    }
};

} // namespace f4::models

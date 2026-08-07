// f4-models/src/tex_reader.hpp
//
// Internal: .TEX file reader — decompresses LZSS-compressed paletted
// texture blobs from KoreaObj.Tex and resolves through PaletteBank.

#pragma once

#include <f4/models/texture.hpp>

#include <cstdint>
#include <string>

namespace f4::models::detail {

/// Read and decode one texture blob from the .TEX file.
///
/// @param tex_data      raw .TEX file bytes
/// @param tex_size      size of .TEX file
/// @param entry         TexBankEntry (offset, size, dimension, palette, chroma key)
/// @param palettes      PaletteBank data (array of DiskPalette)
/// @param tex_index     index into TextureBank (stored in DecodedTexture::tex_id)
/// @param out           output DecodedTexture
/// @param err           error string on failure
/// @return true on success
bool read_tex_blob(
    const uint8_t* tex_data, std::size_t tex_size,
    const TexBankEntry& entry,
    const std::vector<DiskPalette>& palettes,
    int tex_index,
    DecodedTexture& out,
    std::string& err);

} // namespace f4::models::detail

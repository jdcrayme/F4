// f4-models/src/hdr_parser.hpp
//
// HDR binary format parser — reads KoreaObj.HDR / KoreaObj.DXH.
// Produces ModelRecord and LodTableEntry vectors.
//
// Internal to f4-models — not a public header.

#pragma once

#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>
#include <f4/models/texture.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace f4::models::detail {

struct HdrParseResult {
    uint32_t version = 0;
    int n_colors = 0;
    int n_dark_colors = 0;
    int n_palettes = 0;
    int n_textures = 0;
    int max_tags = 0;
    int n_lod_entries = 0;
    int n_parents = 0;
    bool is_new_format = false;
    bool has_lod_names = false;

    ColorBank color_bank;  ///< Parsed ColorBank (empty if parse fails)

    std::vector<DiskPalette> palettes;    ///< Parsed PaletteBank (256 ARGB entries each)
    std::vector<TexBankEntry> tex_entries; ///< Parsed TextureBank (40 bytes each)

    std::vector<LodTableEntry> lod_entries;
    std::vector<ModelRecord> parents;
};

/// Parse the entire HDR file.
[[nodiscard]] bool parse_hdr(
    const uint8_t* data, std::size_t size,
    HdrParseResult& result,
    std::string& err);

} // namespace f4::models::detail

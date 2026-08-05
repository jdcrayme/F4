// f4-models/src/dx_parser.hpp
//
// DX format parser — reads the DX engine LOD format from KoreaObj.DXL.
// Produces DxLodData with header, vertices, and raw node stream.
//
// Internal to f4-models — not a public header.

#pragma once

#include <f4/models/bsp_node.hpp>
#include <f4/models/model_lod.hpp>

#include <cstdint>
#include <string>

namespace f4::models::detail {

/// Check if the first 4 bytes of a LOD record indicate DX format.
/// DX format header has a checksum: (version & 0xFFFF) == (~version >> 16)
[[nodiscard]] bool is_dx_format(uint32_t first4) noexcept;

/// Parse a DX LOD record.
/// `data` points to the LOD record bytes (offset..offset+size from LOD file).
[[nodiscard]] bool parse_dx_lod(
    const uint8_t* data, std::size_t size,
    DxLodData& result,
    std::string& err);

} // namespace f4::models::detail

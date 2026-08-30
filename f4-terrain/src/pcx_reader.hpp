// f4-terrain/src/pcx_reader.hpp
//
// Minimal 8-bit PCX decoder (internal to f4-terrain). Falcon's theater
// tile art (texture.zip) is single-plane, 8-bits-per-pixel, RLE-coded
// PCX with a 256-entry palette in the trailing 769-byte chunk.
//
// FreeFalcon reads these through its own image library (CheckImageType
// accepted PCX/TGA/GIF); stock Korea ships PCX exclusively.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace f4::terrain {

struct PcxImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;   ///< width*height*4
};

/// Decode an 8-bit palettized PCX. Returns false and fills `err` on
/// unsupported/invalid input (wrong signature, non-8-bit, multi-plane).
bool decode_pcx(const uint8_t* data, std::size_t size, PcxImage& out,
                std::string& err);

} // namespace f4::terrain

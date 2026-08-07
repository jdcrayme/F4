// f4-lzss/include/f4/lzss/lzss.hpp
//
// LZSS decompression — FreeFalcon / Falcon 4.0 variant.
//
// FreeFalcon uses a 16-bit sliding window LZSS scheme for compressing
// texture blobs (KoreaObj.Tex) and campaign save archives (.cam).
// The algorithm is implemented in FreeFalcon's ImageMemClass::Expand
// (graphics/image/imagebuf.cpp) and Falcon4.h::Decompress.
//
// Format:
//   Stream of (flag_byte, 8 tokens) groups.
//   Flag byte: bit N (0..7) indicates token type:
//     0 = literal byte (1 byte)
//     1 = match reference (2 bytes: 12-bit offset + 4-bit length)
//   Match encoding (2 bytes, little-endian):
//     byte0 = (offset_high << 4) | length_minus_3
//     byte1 = offset_low
//     offset = (offset_high << 8) | offset_low   (0..4095)
//     length = length_minus_3 + 3                 (3..18)
//   The sliding window is 4096 bytes (12-bit offset).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::lzss {

/// Decompress LZSS-compressed data.
/// @param src           compressed input buffer
/// @param src_size      size of compressed data in bytes
/// @param dst           output buffer (caller-allocated, must be >= expected_size)
/// @param dst_capacity  capacity of output buffer
/// @return number of bytes written to dst, or 0 on error
std::size_t decompress(
    const uint8_t* src, std::size_t src_size,
    uint8_t* dst, std::size_t dst_capacity);

/// Decompress LZSS-compressed data into a vector.
/// @param src            compressed input buffer
/// @param src_size       size of compressed data in bytes
/// @param expected_size  hint for output size (0 = unknown, grow dynamically)
/// @return decompressed bytes; empty on error
std::vector<uint8_t> decompress(
    const uint8_t* src, std::size_t src_size,
    std::size_t expected_size = 0);

} // namespace f4::lzss

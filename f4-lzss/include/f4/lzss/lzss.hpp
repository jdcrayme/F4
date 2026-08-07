// f4-lzss/include/f4/lzss/lzss.hpp
//
// LZSS decompression — FreeFalcon / Falcon 4.0 variant.
//
// FreeFalcon uses a 12-bit sliding window LZSS scheme for compressing
// texture blobs (KoreaObj.Tex) and campaign save archives (.cam).
// The algorithm is implemented in FreeFalcon's LZSS_Expand()
// (src/utils/lzss.cpp) and used by ImageMemClass::Expand.
//
// Format:
//   Stream of (flag_byte, 8 tokens) groups.
//   Flag byte: bit N (0..7, LSB first) indicates token type:
//     1 = literal byte (1 byte)
//     0 = match reference (2 bytes)
//
//   Match encoding (2 bytes):
//     byte0: high nibble = length (0..15, stored raw)
//            low nibble  = position bits [11:8]
//     byte1: position bits [7:0]
//     position = byte1 | ((byte0 & 0x0F) << 8)   → 12-bit window index
//     length   = (byte0 >> 4) + BREAK_EVEN       → 1..16 (BREAK_EVEN=1)
//
//   The sliding window is a 4096-byte RING BUFFER (not the output
//   buffer). current_position starts at 1 and wraps with MOD_WINDOW.
//   Match positions index into this ring buffer.

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

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

// ──────────────────────────────────────────────────────────────────────────
// Compression — a faithful port of FreeFalcon's LZSS_Compress() (the
// Nelson & Gailly carman LZSS the game ships in src/utils/lzss.cpp).
//
// compress(src, src_size) is BYTE-IDENTICAL to FreeFalcon's compressor:
// same tree-descent match choices (the book's `i >= match_length` tie
// rule), same token stream, same trailing partial group. Verified
// byte-for-byte against every LZSS sub-file of both committed .cam
// fixtures (see f4-lzss/src/compress.cpp for the algorithm notes and the
// fixture scorecard). decompress() reads it back to the exact original.
// ──────────────────────────────────────────────────────────────────────────

/// Compress `src` into an LZSS stream readable by decompress().
/// @param src       raw input buffer
/// @param src_size  input size in bytes
/// @return compressed bytes (empty input → empty output)
[[nodiscard]] std::vector<uint8_t> compress(
    const uint8_t* src, std::size_t src_size);

/// Compress a std::vector<uint8_t> (convenience overload).
[[nodiscard]] std::vector<uint8_t> compress(const std::vector<uint8_t>& src);

} // namespace f4::lzss

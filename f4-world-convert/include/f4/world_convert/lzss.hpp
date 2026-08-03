// f4-world-convert/include/f4/convert/lzss.hpp
//
// LZSS decompressor — faithful port of FreeFalcon's LZSS_Expand()
// (src/utils/lzss.cpp). The campaign metadata sub-file (.cmp) inside a .cam
// archive is LZSS-compressed; this decompresses it so the campaign header
// fields (CurrentTime, team names, etc.) can be parsed.
//
// Algorithm: classic Nelson & Gailly LZSS with "blocked I/O":
//   - 12-bit window index  (WINDOW_SIZE = 4096)
//   - 4-bit match length   (BREAK_EVEN = 1, so match length = 1..16)
//   - Flag byte: 8 bits, LSB-first. Bit set (1) = literal byte;
//     bit clear (0) = (position,length) match pair (2 bytes).
//   - Match token layout: [len_hi:4 | pos_hi:4] [pos_lo:8]
//       position = ((byteA & 0x0F) << 8) | byteB   (12-bit)
//       length   = (byteA >> 4) + BREAK_EVEN        (1..16)
//
// This is a byte-exact port verified against the real save1.cam fixture.

#pragma once

#include <cstdint>
#include <vector>

namespace f4::world_convert {

constexpr int LZSS_INDEX_BIT_COUNT = 12;
constexpr int LZSS_LENGTH_BIT_COUNT = 4;
constexpr int LZSS_WINDOW_SIZE = 1 << LZSS_INDEX_BIT_COUNT;          // 4096
constexpr int LZSS_RAW_LOOK_AHEAD = 1 << LZSS_LENGTH_BIT_COUNT;       // 16
constexpr int LZSS_BREAK_EVEN = (1 + LZSS_INDEX_BIT_COUNT + LZSS_LENGTH_BIT_COUNT) / 9;  // 1

/// Decompress LZSS-compressed data. Returns the decompressed bytes.
/// `uncomp_size` is the expected decompressed size (read from the .cmp
/// header); expansion stops when that many bytes have been produced.
/// Throws std::runtime_error on malformed input.
[[nodiscard]] std::vector<uint8_t> lzss_expand(const uint8_t* input,
                                               std::size_t src_size,
                                               std::size_t uncomp_size);

} // namespace f4::world_convert

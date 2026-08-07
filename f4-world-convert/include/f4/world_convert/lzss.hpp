// f4-world-convert/include/f4/world_convert/lzss.hpp
//
// LZSS decompression adapter — delegates to f4::lzss::decompress().
//
// The campaign metadata sub-file (.cmp) inside a .cam archive is
// LZSS-compressed; lzss_expand() decompresses it so the campaign header
// fields (CurrentTime, team names, etc.) can be parsed.
//
// The canonical implementation lives in f4-lzss (f4::lzss::decompress).
// This module provides a compatibility wrapper that translates the
// error convention (f4::lzss returns empty on failure; lzss_expand
// throws std::runtime_error).
//
// Algorithm details: see f4-lzss/include/f4/lzss/lzss.hpp.

#pragma once

#include <cstdint>
#include <vector>

namespace f4::world_convert {

// Legacy constants — kept for backward compatibility. The canonical
// definitions live in f4-lzss; these are duplicated here only so that
// existing code referencing f4::world_convert::LZSS_WINDOW_SIZE etc.
// continues to compile. New code should use f4::lzss directly.
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

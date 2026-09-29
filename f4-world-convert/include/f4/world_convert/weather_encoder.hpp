// f4-world-convert/include/f4/world_convert/weather_encoder.hpp
//
// .wth encoder — the inverse of weather_decoder.cpp's decode_wth().
// Field order, widths, and the COVersion marker mirror the decoder
// exactly (that file is the ground truth; this one reproduces its byte
// sequence). See weather_decoder.hpp for the three-layout contract.
//
// The .wth sub-file is raw (not LZSS-compressed) in the .cam container,
// so the encoder writes directly — no compress step, same as the .tea
// encoder.

#pragma once

#include <f4/world_convert/weather_decoder.hpp>

#include <cstdint>
#include <vector>

namespace f4::world_convert {

/// Encode a decoded .wth face back to the on-disk byte sequence.
/// Byte-identity against the original sub-file holds because the
/// decoder captures every byte the semantic projection drops
/// (trailing_raw) and floats round-trip bit-exactly through the
/// 4-byte memcpy both sides use.
[[nodiscard]] std::vector<uint8_t> encode_wth(const DecodedWeather& w);

} // namespace f4::world_convert

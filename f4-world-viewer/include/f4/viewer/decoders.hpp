// f4-world-viewer/include/f4/viewer/decoders.hpp
//
// Per-format decoders for the Hex Inspector. Each decoder is a pure
// function that takes a HexModel (already loaded with bytes) and
// returns a list of Annotations describing what the bytes mean.
//
// Decoders are intentionally permissive: they don't throw on malformed
// input, they just emit fewer annotations. The Hex Inspector shows
// whatever was decoded and lets the user poke at the rest manually.
//
// Adding a new decoder:
//   1. Add a function here: std::vector<Annotation> decode_xxx(const HexModel&);
//   2. Wire it into apply_decoder() in hex_model.cpp.
//   3. Add tests in test_hex_model.cpp.
//
// Current decoders:
//   - decode_cam_manifest   — .cam container directory (uses f4-world-convert)
//   - decode_cmp_header     — .cmp LZSS-compressed campaign metadata
//   - decode_theater_map    — THEATER.MAP header + palette
//   - decode_falcon4_ct     — FALCON4.ct class table
//   - decode_generic        — fallback: magic, size, entropy, ASCII strings

#pragma once

#include <f4/viewer/hex_model.hpp>

#include <vector>

namespace f4::viewer {

/// Decode a .cam ("campressed") container's manifest. The .cam format
/// starts with [int32 manifest_offset], then sub-file data, then a
/// trailing manifest directory. We delegate to f4-world-convert's
/// CamArchive for the actual parsing and translate its SubFile list
/// into Annotations covering each sub-file's byte range.
[[nodiscard]] std::vector<Annotation> decode_cam_manifest(const HexModel& m);

/// Decode a .cmp sub-file's header. The .cmp format is:
///   [4 bytes reserved_skip] [4 bytes decompressed_size]
///   [LZSS-compressed payload]
/// We annotate the 8-byte header and leave the compressed payload as
/// "unknown" (LZSS-compressed bytes aren't human-readable).
[[nodiscard]] std::vector<Annotation> decode_cmp_header(const HexModel& m);

/// Decode a THEATER.MAP file's header. Format (from f4-terrain):
///   [0..3]   uint32 magic       (0x444CFFAE)
///   [4..7]   uint32 width       (e.g. 128)
///   [8..11]  uint32 height      (e.g. 128)
///   [12..15] uint32 ft_to_cell  (feet-to-cell conversion)
///   [16..]   RGBA palette[]     (271 entries × 4 bytes = 1084 bytes)
[[nodiscard]] std::vector<Annotation> decode_theater_map(const HexModel& m);

/// Decode a FALCON4.ct class table file. Format (from f4-world-convert):
///   [0..1]   int16 num_entities
///   [2..]    num_entities × 81-byte entries
/// Each entry's classInfo_[8] at offset 8 maps to (domain, class, type, stype).
/// We annotate the count + the first few entries' classInfo fields.
[[nodiscard]] std::vector<Annotation> decode_falcon4_ct(const HexModel& m);

/// Generic decoder — runs on unknown files. Computes:
///   - File size annotation
///   - First 16 bytes as a "magic" annotation (hex + ASCII preview)
///   - Entropy estimate (compressed/encrypted vs. plaintext)
///   - ASCII string runs >= 4 chars (offset + length + preview)
/// This is the fallback when identify_file() returns Unknown.
[[nodiscard]] std::vector<Annotation> decode_generic(const HexModel& m);

} // namespace f4::viewer

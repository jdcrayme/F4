// f4-world-convert/include/f4/world_convert/objective_encoder.hpp
//
// .obj objective sub-file encoder — the inverse of objective_decoder's
// decode_obj(). Serializes a DecodedObjectives back into the .obj sub-file's
// raw bytes: a 10-byte header (i16 count + i32 uncompressed_size + i32
// compressed_size) followed by the LZSS-compressed record buffer.
//
// The decode side (objective_decoder.cpp) is the ground truth for field
// order; this encoder reproduces that byte sequence so that:
//   decode_obj(encode_obj(d)) == d   (struct equality)
//
// Byte-identity of the decompressed buffer holds for every field except
// where the original carried non-zero padding in fixed-width-string fields
// (the same class of difference as encode_cmp — see SAVE_WRITE_PLAN.md §4).
// The decoded structs are identical.
//
// Dependencies: f4-lzss (compress), f4-world-convert (objective_decoder for
// the DecodedObjectives type). No new external deps.

#pragma once

#include <f4/world_convert/objective_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::world_convert {

/// Encode a DecodedObjectives into a .obj sub-file's raw bytes.
///
/// @param dec           the decoded objectives to serialize
/// @param camp_version  gCampDataVersion (gates the pos_.z_ float at v>=70)
/// @return the .obj bytes: 10-byte header + LZSS-compressed record buffer
[[nodiscard]] std::vector<uint8_t> encode_obj(
    const DecodedObjectives& dec, int camp_version = 63);

/// Build the decompressed .obj record buffer (the flat record sequence)
/// from a DecodedObjectives. Exposed so tests can compare it byte-for-byte
/// against the original decompressed buffer.
[[nodiscard]] std::vector<uint8_t> encode_obj_payload(
    const DecodedObjectives& dec, int camp_version = 63);

} // namespace f4::world_convert

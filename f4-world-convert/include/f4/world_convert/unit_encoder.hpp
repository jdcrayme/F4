// f4-world-convert/include/f4/world_convert/unit_encoder.hpp
//
// .uni unit sub-file encoder — the inverse of unit_decoder's decode_uni().
// Serializes a DecodedUnits back into the .uni sub-file's raw bytes: a
// 10-byte header (i32 outer_size + i16 count + i32 inner_size) followed by
// the LZSS-compressed record buffer.
//
// The decode side (unit_decoder.cpp) is the ground truth for field order;
// this encoder reproduces that byte sequence so that:
//   decode_uni(encode_uni(d)) == d   (struct equality)
//
// Byte-identity scope: struct-faithful, not byte-faithful. Fields the
// decoder skips (squadron stores[] / schedule[] / rating[], flight duplicate
// loadout entries and skipped timing slots) are zeroed on re-encode; the
// decoder reads 0 for them on the second pass, matching the default-
// constructed struct. The decoded structs are identical — a saved .uni
// loads to the same unit state. (Same class of difference as .cmp/.tea
// string padding; see SAVE_WRITE_PLAN.md §4.)
//
// Subclass dispatch: the encoder writes the tail matching UnitRecord::
// unit_class. The decoder's trial-and-error dispatch is not needed on
// encode — the class is known. The Package branch (small vs big) is chosen
// from subclass.package_branch (set by the decoder); if None, the encoder
// picks based on (unit_flags & U_FINAL) && wait_cycles == 0 (same rule as
// the decoder).
//
// Dependencies: f4-lzss (compress), f4-world-convert (unit_decoder for the
// DecodedUnits type). No new external deps.

#pragma once

#include <f4/world_convert/unit_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::world_convert {

/// Encode a DecodedUnits into a .uni sub-file's raw bytes.
///
/// @param dec           the decoded units to serialize
/// @param camp_version  gCampDataVersion (gates the v>=70 pos_.z_, v>=71
///                       current_wp/wp_count ushort, v>65 flight fields,
///                       v>=72 refuel, v<=72/v>=73 loadout width, v<69/69-71/>=72
///                       squadron stores size)
/// @return the .uni bytes: 10-byte header + LZSS-compressed record buffer
[[nodiscard]] std::vector<uint8_t> encode_uni(
    const DecodedUnits& dec, int camp_version = 63);

/// Build the decompressed .uni record buffer (the flat record sequence)
/// from a DecodedUnits. Exposed so tests can compare it byte-for-byte
/// against the original decompressed buffer.
[[nodiscard]] std::vector<uint8_t> encode_uni_payload(
    const DecodedUnits& dec, int camp_version = 63);

} // namespace f4::world_convert

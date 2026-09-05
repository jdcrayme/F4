// f4-world-convert/include/f4/world_convert/cmp_encoder.hpp
//
// .cmp campaign-metadata encoder — the inverse of campaign_decoder's
// decode_cmp(). Serializes a CampaignHeader back into the on-disk .cmp
// sub-file: an 8-byte header (reserved_skip + decompressed_size) followed
// by the LZSS-compressed flat payload (CampaignClass::Decode's struct
// sequence, written in the exact order decode_cmp reads it).
//
// The decode side (campaign_decoder.cpp) is the ground truth for field
// order, widths, and NUL-padding. This encoder reproduces that byte
// sequence so that:
//   decode_cmp(encode_cmp(h)) == h        (struct equality — the contract)
//
// Byte-identity of the DECOMPRESSED payload against a FreeFalcon-produced
// .cmp holds for every fixed-width field (the struct fields, fixed-width
// NUL-padded strings, the 8-byte team block, the squadron preload list,
// the camp map, the creator block) because those are deterministic given
// the struct. The UI event-queue text fields are re-encoded in their
// minimal form (len = text.size(), no trailing NUL/padding); if the
// original carried NUL padding inside `len`, the decompressed bytes differ
// there but decode to the identical struct. (Capturing the original
// text_len in CampaignEvent is a documented follow-on for full byte-
// identity against FreeFalcon files; it is not required for the save/load
// loop, where struct equality is what matters.)
//
// Dependencies: f4-lzss (compress), f4-world-convert (campaign_decoder for
// the CampaignHeader type). No new external deps.

#pragma once

#include <f4/world_convert/campaign_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::world_convert {

/// Encode a CampaignHeader into a .cmp sub-file's raw bytes.
///
/// @param header         the campaign state to serialize
/// @param camp_version   gCampDataVersion (gates the v>=19/31/43 fields,
///                       matching decode_cmp's version parameter). Defaults
///                       to 63 (the save1.cam fixture's version).
/// @return the .cmp bytes: 8-byte header + LZSS-compressed payload
[[nodiscard]] std::vector<uint8_t> encode_cmp(
    const CampaignHeader& header, int camp_version = 63);

/// Build the decompressed .cmp payload (the flat struct sequence) from a
/// CampaignHeader. Exposed so tests can compare it byte-for-byte against
/// the original decompressed payload (the byte-identity check for the
/// deterministic region).
[[nodiscard]] std::vector<uint8_t> encode_cmp_payload(
    const CampaignHeader& header, int camp_version = 63);

} // namespace f4::world_convert

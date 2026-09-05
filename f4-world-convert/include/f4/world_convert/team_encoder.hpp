// f4-world-convert/include/f4/world_convert/team_encoder.hpp
//
// .tea team sub-file encoder — the inverse of team_decoder's decode_tea().
// Serializes a DecodedTeams back into the .tea sub-file's raw bytes.
//
// Unlike .cmp and .obj, the .tea sub-file is NOT LZSS-compressed — it is
// a raw byte stream: [i16 count] then per team: TeamClass(739 at v63/v71)
// + ATM(variable) + GTM(15) + NTM(15). The encoder writes these directly.
//
// The decode side (team_decoder.cpp) is the ground truth for field order;
// this encoder reproduces that byte sequence so that:
//   decode_tea(encode_tea(d)) == d   (struct equality)
//
// Byte-identity holds for every fixed-width field. The GTM/NTM records are
// reproduced verbatim from TeamRecord::gtm_raw / ntm_raw (captured by the
// decoder), so the full .tea is byte-faithful when round-tripping a decoded
// file. A hand-constructed TeamRecord with empty gtm_raw/ntm_raw writes 15
// zero bytes for each (structurally valid; FreeFalcon reads zeros as a
// manager with VU_ID 0/0, entity_type 0, owner 0).

#pragma once

#include <f4/world_convert/team_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::world_convert {

/// Encode a DecodedTeams into a .tea sub-file's raw bytes.
///
/// @param dec           the decoded teams to serialize
/// @param camp_version  gCampDataVersion (gates the v>4/11/30/32/33/53 fields)
/// @return the .tea bytes: i16 count + per-team (TeamClass + ATM + GTM + NTM)
[[nodiscard]] std::vector<uint8_t> encode_tea(
    const DecodedTeams& dec, int camp_version = 63);

} // namespace f4::world_convert

// f4-world-convert/include/f4/convert/world_json.hpp
//
// Emits the combined world JSON from a parsed .cam archive. Mirrors the
// f4-convert dat2json pattern: the converter owns the binary-to-JSON step
// so downstream libraries (f4-world) never see binary formats.

#pragma once

#include <f4/convert/cam_archive.hpp>
#include <f4/convert/campaign_decoder.hpp>
#include <string>

namespace f4::convert {

/// Build the world JSON document from a loaded .cam archive.
/// Includes: the container manifest (all sub-files), the decoded .ver
/// version, the decoded .cmp campaign header (CurrentTime, TE block, all
/// 8 team name/motto slots), and raw sub-file bytes (base64) for types
/// not yet decoded.
[[nodiscard]] std::string to_world_json(const CamArchive& cam);

} // namespace f4::convert

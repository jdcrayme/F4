// f4-terrain-convert/include/f4/convert/terrain_converter.hpp
//
// Wraps f4-terrain's binary loader + JSON writer in a single call. The CLI
// (terrain2json) is a thin shell around convert_terrain_dir(); the library
// is also linked by f4-world-viewer so the viewer can import THEATER.*
// files in-process without spawning a subprocess.

#pragma once

#include <filesystem>
#include <string>

namespace f4::terrain_convert {

/// Load THEATER.* from `terrain_dir` and write the terrain JSON to `out`.
/// Returns the byte count written. Throws on I/O or parse error.
std::size_t convert_terrain_dir(const std::filesystem::path& terrain_dir,
                                 const std::filesystem::path& out,
                                 const std::string& theater_name = "korea");

} // namespace f4::terrain_convert

// f4-terrain/include/f4/terrain/terrain_convert.hpp
//
// convert_terrain_dir — THEATER.* binary -> terrain JSON in one call.
// Formerly f4-terrain-convert (a static library wrapping exactly these
// two lines); folded into f4-terrain, its only real dependency. The
// terrain2json CLI (tools/) is a thin shell around this; the world
// viewer spawns that CLI as a subprocess rather than linking the
// binary parser.

#pragma once

#include <filesystem>
#include <string>

#include <f4/terrain/terrain_data.hpp>

namespace f4::terrain {

/// Load THEATER.* from `terrain_dir` and write the terrain JSON to
/// `out`. Returns the byte count written. Throws on I/O or parse error.
inline std::size_t convert_terrain_dir(
    const std::filesystem::path& terrain_dir,
    const std::filesystem::path& out,
    const std::string& theater_name = "korea") {
    TerrainData td;
    td.load(terrain_dir);  // throws on parse error
    td.save_terrain_json(out, theater_name);
    return td.to_terrain_json(theater_name).size();
}

} // namespace f4::terrain

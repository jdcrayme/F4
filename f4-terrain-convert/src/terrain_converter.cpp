// f4-terrain-convert/src/terrain_converter.cpp

#include <f4/terrain_convert/terrain_converter.hpp>
#include <f4/terrain/terrain_data.hpp>

namespace f4::terrain_convert {

std::size_t convert_terrain_dir(const std::filesystem::path& terrain_dir,
                                 const std::filesystem::path& out,
                                 const std::string& theater_name) {
    f4::terrain::TerrainData td;
    td.load(terrain_dir);  // throws on parse error
    td.save_terrain_json(out, theater_name);
    return td.to_terrain_json(theater_name).size();
}

} // namespace f4::terrain_convert

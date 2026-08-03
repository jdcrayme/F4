// f4-terrain-convert/cli/terrain2json.cpp
//
// CLI: convert FreeFalcon THEATER.* binary files to an open terrain JSON.
//
//   terrain2json <terrain_dir>            -> writes <terrain_dir>/terrain.json
//   terrain2json <terrain_dir> out.json   -> writes out.json
//   terrain2json <terrain_dir> out.json --name korea
//
// The terrain_dir must contain THEATER.MAP, THEATER.MEA, and THEATER.O2.

#include <f4/convert/terrain_converter.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: terrain2json <terrain_dir> [out.json] [--name <theater>]\n";
        return 2;
    }

    std::filesystem::path terrain_dir = argv[1];
    std::filesystem::path out = (argc >= 3 && std::string(argv[2]) != "--name")
        ? std::filesystem::path(argv[2])
        : terrain_dir / "terrain.json";

    std::string theater_name = "korea";
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--name") {
            theater_name = argv[i + 1];
            ++i;
        }
    }

    try {
        const std::size_t bytes = f4::convert::convert_terrain_dir(terrain_dir, out, theater_name);
        std::cout << "wrote " << out << " (" << bytes << " bytes) from " << terrain_dir << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "terrain2json: error: " << e.what() << "\n";
        return 1;
    }
}

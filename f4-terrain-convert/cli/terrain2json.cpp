// f4-terrain-convert/cli/terrain2json.cpp
//
// CLI: convert FreeFalcon THEATER.* binary files to an open terrain JSON.
//
//   terrain2json <terrain_dir>            -> writes <terrain_dir>/terrain.json
//   terrain2json <terrain_dir> out.json   -> writes out.json
//   terrain2json <terrain_dir> out.json --name korea
//
// Asset-pipeline mode (Stage 1, ASSET_PIPELINE_SPEC.md):
//   terrain2json <terrain_dir> --data-dir ./Data [--name korea]
//     - Writes to   ./Data/Theater/<id>/terrain.json
//     - Updates     ./Data/manifest.json with a theater:<id> entry

#include <f4/terrain_convert/terrain_converter.hpp>
#include <f4/import/manifest_writer.hpp>
#include <f4/assets/manifest.hpp>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: terrain2json <terrain_dir> [out.json] [--name <theater>] "
                     "[--data-dir <Data>]\n";
        return 2;
    }

    fs::path terrain_dir = argv[1];
    fs::path out;
    if (argc >= 3 && std::string(argv[2]).substr(0, 2) != "--") {
        out = fs::path(argv[2]);
    } else {
        out = terrain_dir / "terrain.json";
    }

    std::string theater_name = "korea";
    fs::path data_dir;
    bool asset_mode = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--name" && i + 1 < argc) {
            theater_name = argv[++i];
        } else if (a == "--data-dir" && i + 1 < argc) {
            data_dir = fs::path(argv[++i]);
            asset_mode = true;
        }
    }

    if (asset_mode) {
        const auto theater_id = f4::import::theater_id_from_name(theater_name);
        if (!theater_id.valid()) {
            std::cerr << "terrain2json: --data-dir: invalid theater name '"
                      << theater_name << "'\n";
            return 2;
        }
        out = data_dir / "Theater" / theater_id.local_id / "terrain.json";
    }

    try {
        std::error_code mk_ec;
        fs::create_directories(out.parent_path(), mk_ec);
        const std::size_t bytes = f4::terrain_convert::convert_terrain_dir(terrain_dir, out, theater_name);
        std::cout << "wrote " << out << " (" << bytes << " bytes) from " << terrain_dir << "\n";

        if (asset_mode) {
            const auto theater_id = f4::import::theater_id_from_name(theater_name);
            const std::string rel_path = "Theater/" + theater_id.local_id + "/terrain.json";
            std::vector<f4::assets::Capability> caps;
            caps.push_back({"map",        f4::assets::CapabilityStatus::present, {}});
            caps.push_back({"posts",      f4::assets::CapabilityStatus::present, {}});
            caps.push_back({"tiles_near", f4::assets::CapabilityStatus::present, {}});
            caps.push_back({"tiles_far",  f4::assets::CapabilityStatus::present, {}});
            std::vector<f4::assets::AssetSource> sources;
            for (const char* fname : {"THEATER.MAP", "THEATER.MEA", "THEATER.O2",
                                       "TEXTURE.BIN", "FArtILES.PAL", "FArtILES.RAW"}) {
                const fs::path p = terrain_dir / fname;
                std::error_code ec;
                if (fs::exists(p, ec)) {
                    sources.push_back({/*path=*/p.string(), /*role=*/"theater",
                                       /*sha256=*/""});
                }
            }
            (void)f4::import::update_manifest_for_asset(
                data_dir, theater_id, rel_path,
                /*format_version=*/1,
                std::move(caps), std::move(sources),
                /*generator=*/"terrain2json (f4import 0.4.0)");
            std::cerr << "  manifest:    updated (" << (data_dir / "manifest.json") << ")\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "terrain2json: error: " << e.what() << "\n";
        return 1;
    }
}

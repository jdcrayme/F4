// f4-world-convert/cli/cam2json.cpp
//
// CLI: convert a FreeFalcon .cam campaign archive to open JSON.
//
//   cam2json save1.cam                                -> writes save1.world.json
//   cam2json save1.cam out.json                       -> writes out.json
//   cam2json save1.cam out.json --theater korea --terrain korea.terrain.json
//   cam2json save1.cam out.json --theater-data ./terrdata/objects
//
// Asset-pipeline mode (Stage 1, ASSET_PIPELINE_SPEC.md):
//   cam2json save1.cam --data-dir ./Data [--theater korea]
//     - Writes to   ./Data/World/save1.world.json
//     - Records terrain_file as "@asset:theater:korea" (the asset-id form)
//     - Updates ./Data/manifest.json with a campaign:save1 entry

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/world_json.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/theater_data.hpp>
#include <f4/import/manifest_writer.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cam2json <input.cam> [output.json] "
                     "[--theater <name>] [--terrain <file>] "
                     "[--class-table <FALCON4.ct>] "
                     "[--theater-data <dir>] "
                     "[--data-dir <Data>]\n"
                     "  --theater-data: directory containing Falcon4.OCD/.PHD/.PD/"
                     ".UCD/.VCD/.FED/.FCD (typically <install>/terrdata/objects).\n"
                     "                  When provided, the world JSON is enriched with\n"
                     "                  objective class names, airbase ground layouts,\n"
                     "                  unit class names, and per-group vehicle composition.\n"
                     "  --data-dir:    asset-pipeline mode (Stage 1). Writes to\n"
                     "                  <Data>/World/<id>.world.json, records terrain_file\n"
                     "                  as @asset:theater:<id>, updates <Data>/manifest.json.\n";
        return 2;
    }
    const fs::path in = argv[1];
    fs::path out;
    if (argc >= 3 && std::string(argv[2]).substr(0, 2) != "--") {
        out = fs::path(argv[2]);
    } else {
        out = in;
        out.replace_extension(".world.json");
    }

    f4::world_convert::WorldJsonOptions opts;
    fs::path explicit_ct;
    fs::path theater_data_dir;
    fs::path data_dir;
    bool asset_mode = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--theater" && i + 1 < argc) {
            opts.theater = argv[++i];
        } else if (a == "--terrain" && i + 1 < argc) {
            opts.terrain_file = argv[++i];
        } else if (a == "--class-table" && i + 1 < argc) {
            explicit_ct = argv[++i];
        } else if (a == "--theater-data" && i + 1 < argc) {
            theater_data_dir = argv[++i];
        } else if (a == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
            asset_mode = true;
        }
    }

    if (asset_mode) {
        const auto campaign_id = f4::import::campaign_id_from_cam_path(in);
        if (!campaign_id.valid()) {
            std::cerr << "cam2json: --data-dir: cannot derive campaign id from "
                      << in << "\n";
            return 2;
        }
        const auto theater_id = f4::import::theater_id_from_name(opts.theater);
        out = data_dir / "World" / (campaign_id.local_id + ".world.json");
        if (opts.terrain_file == "korea.terrain.json") {
            opts.terrain_file = "@asset:" + theater_id.to_string();
        }
    }

    try {
        f4::world_convert::CamArchive cam;
        cam.load(in);

        f4::world_convert::ClassTable class_table;
        fs::path ct_path;
        if (!explicit_ct.empty()) {
            ct_path = explicit_ct;
        } else {
            ct_path = f4::world_convert::find_class_table(in);
        }
        if (!ct_path.empty()) {
            try {
                class_table.load(ct_path);
                opts.class_table = &class_table;
                std::cerr << "  class_table: " << class_table.size()
                          << " entries (from " << ct_path << ")\n";
            } catch (const std::exception& e) {
                std::cerr << "  class_table: failed to load (" << e.what()
                          << ") — proceeding without\n";
            }
        } else {
            std::cerr << "  class_table: FALCON4.ct not found — objectives will"
                      << " lack objective_type (use --class-table to specify)\n";
        }

        f4::world_convert::TheaterObjectDatabase theater_db;
        if (!theater_data_dir.empty()) {
            theater_db.load_all(theater_data_dir);
            if (theater_db.loaded()) {
                opts.theater_db = &theater_db;
                std::cerr << "  theater_db: loaded ("
                          << "OCD=" << theater_db.objectives.size() << ", "
                          << "PHD=" << theater_db.pt_headers.size() << ", "
                          << "PD="  << theater_db.pt_data.size() << ", "
                          << "UCD=" << theater_db.units.size() << ", "
                          << "VCD=" << theater_db.vehicles.size() << ", "
                          << "FCD=" << theater_db.features.size() << ", "
                          << "FED=" << theater_db.feature_entries.size() << ")\n";
            } else {
                std::cerr << "  theater_db: no Falcon4.* data files found in "
                          << theater_data_dir << " — proceeding without\n";
            }
        }

        std::error_code mk_ec;
        fs::create_directories(out.parent_path(), mk_ec);

        const std::string json = f4::world_convert::to_world_json(cam, opts);
        std::ofstream f(out);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        f << json;
        std::cout << "wrote " << out << " (" << json.size() << " bytes) from " << in << "\n";

        std::cerr << "  theater:      " << opts.theater << "\n";
        std::cerr << "  terrain_file: " << opts.terrain_file << "\n";
        std::cerr << "  subfiles:     " << cam.subfiles().size() << "\n";
        if (const auto* v = cam.find("ver")) {
            std::cerr << "  version:      " << f4::world_convert::read_version(v->data.data(), v->data.size()) << "\n";
        }

        if (asset_mode) {
            const auto campaign_id = f4::import::campaign_id_from_cam_path(in);
            const auto theater_id = f4::import::theater_id_from_name(opts.theater);
            std::vector<f4::assets::AssetSource> campaign_sources;
            campaign_sources.push_back({
                /*path=*/in.string(), /*role=*/"campaign", /*sha256=*/""});
            std::vector<f4::assets::Capability> campaign_caps;
            campaign_caps.push_back({"objectives", f4::assets::CapabilityStatus::present, {}});
            campaign_caps.push_back({"units",      f4::assets::CapabilityStatus::present, {}});
            campaign_caps.push_back({"teams",      f4::assets::CapabilityStatus::present, {}});
            campaign_caps.push_back({"pilot_files", f4::assets::CapabilityStatus::unknown, {}});

            const std::string rel_path = "World/" + campaign_id.local_id + ".world.json";
            (void)f4::import::update_manifest_for_asset(
                data_dir,
                campaign_id,
                rel_path,
                /*format_version=*/1,
                std::move(campaign_caps),
                std::move(campaign_sources),
                /*generator=*/"cam2json (f4import 0.4.0)");
            f4::assets::Manifest m = f4::import::load_or_create_manifest(data_dir);
            if (!m.find(theater_id)) {
                f4::import::upsert_asset(
                    m, theater_id,
                    "Theater/" + theater_id.local_id + "/theater.json",
                    /*format_version=*/1,
                    /*capabilities=*/{},
                    /*sources=*/{});
                f4::assets::write_manifest_file((data_dir / "manifest.json").string(), m);
            }
            std::cerr << "  manifest:    updated (" << (data_dir / "manifest.json") << ")\n";
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cam2json: error: " << e.what() << "\n";
        return 1;
    }
}

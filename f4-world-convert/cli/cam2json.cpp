// f4-world-convert/cli/cam2json.cpp
//
// CLI: convert a FreeFalcon .cam campaign archive to open JSON.
//
//   cam2json save1.cam                       -> writes save1.world.json
//   cam2json save1.cam out.json              -> writes out.json
//   cam2json save1.cam out.json --theater korea --terrain korea.terrain.json
//
// Mirrors f4-convert's dat2json: thin main() that calls into the library,
// so all logic is testable in-process.

#include <f4/convert/cam_archive.hpp>
#include <f4/convert/world_json.hpp>
#include <f4/convert/class_table.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 8) {
        std::cerr << "usage: cam2json <input.cam> [output.json] "
                     "[--theater <name>] [--terrain <file>] [--class-table <FALCON4.ct>]\n";
        return 2;
    }
    const std::filesystem::path in = argv[1];
    std::filesystem::path out;
    if (argc >= 3 && std::string(argv[2]).substr(0, 2) != "--") {
        out = std::filesystem::path(argv[2]);
    } else {
        out = in;
        out.replace_extension(".world.json");
    }

    f4::convert::WorldJsonOptions opts;
    std::filesystem::path explicit_ct;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--theater" && i + 1 < argc) {
            opts.theater = argv[++i];
        } else if (a == "--terrain" && i + 1 < argc) {
            opts.terrain_file = argv[++i];
        } else if (a == "--class-table" && i + 1 < argc) {
            explicit_ct = argv[++i];
        }
    }

    try {
        f4::convert::CamArchive cam;
        cam.load(in);

        // Resolve FALCON4.ct (the class table). Without it, objectives carry
        // only their raw entity_type and the viewer can't pick icons — they
        // all fall back to drawn circles. Search in this order:
        //   1. Explicit --class-table path (if provided)
        //   2. Same directory as the .cam (the game ships them together)
        //   3. CWD-relative well-known paths (build dir, source fixtures)
        f4::convert::ClassTable class_table;
        std::filesystem::path ct_path;
        if (!explicit_ct.empty()) {
            ct_path = explicit_ct;
        } else {
            ct_path = f4::convert::find_class_table(in);
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

        const std::string json = f4::convert::to_world_json(cam, opts);
        std::ofstream f(out);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        f << json;
        std::cout << "wrote " << out << " (" << json.size() << " bytes) from " << in << "\n";

        // Print a brief summary to stderr for quick CLI feedback.
        std::cerr << "  theater:      " << opts.theater << "\n";
        std::cerr << "  terrain_file: " << opts.terrain_file << "\n";
        std::cerr << "  subfiles:     " << cam.subfiles().size() << "\n";
        if (const auto* v = cam.find("ver")) {
            std::cerr << "  version:      " << f4::convert::read_version(v->data.data(), v->data.size()) << "\n";
        }
        for (const auto& sf : cam.subfiles()) {
            std::cerr << "    " << sf.name << "  (" << sf.size << " bytes)\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cam2json: error: " << e.what() << "\n";
        return 1;
    }
}

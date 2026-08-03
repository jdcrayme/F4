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

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 7) {
        std::cerr << "usage: cam2json <input.cam> [output.json] "
                     "[--theater <name>] [--terrain <file>]\n";
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
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--theater" && i + 1 < argc) {
            opts.theater = argv[++i];
        } else if (a == "--terrain" && i + 1 < argc) {
            opts.terrain_file = argv[++i];
        }
    }

    try {
        f4::convert::CamArchive cam;
        cam.load(in);
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

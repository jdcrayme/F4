// f4-world-convert/cli/cam2json.cpp
//
// CLI: convert a FreeFalcon .cam campaign archive to open JSON.
//
//   cam2json save1.cam            -> writes save1.json
//   cam2json save1.cam out.json   -> writes out.json
//
// Mirrors f4-convert's dat2json: thin main() that calls into the library,
// so all logic is testable in-process.

#include <f4/convert/cam_archive.hpp>
#include <f4/convert/world_json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: cam2json <input.cam> [output.json]\n";
        return 2;
    }
    const std::filesystem::path in = argv[1];
    std::filesystem::path out = (argc == 3) ? argv[2] : in;
    out.replace_extension(".json");

    try {
        f4::convert::CamArchive cam;
        cam.load(in);
        const std::string json = f4::convert::to_world_json(cam);
        std::ofstream f(out);
        if (!f) throw std::runtime_error("cannot write " + out.string());
        f << json;
        std::cout << "wrote " << out << " (" << json.size() << " bytes) from " << in << "\n";

        // Print a brief summary to stderr for quick CLI feedback.
        std::cerr << "  subfiles: " << cam.subfiles().size() << "\n";
        if (const auto* v = cam.find("ver")) {
            std::cerr << "  version: " << f4::convert::read_version(v->data.data(), v->data.size()) << "\n";
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

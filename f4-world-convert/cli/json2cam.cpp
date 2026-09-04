// f4-world-convert/cli/json2cam.cpp
//
// CLI: reassemble a FreeFalcon .cam campaign archive from a world JSON
// produced by cam2json --preserve-subfiles.
//
//   json2cam save1.world.json                       -> writes save1.cam
//   json2cam save1.world.json out.cam
//
//   json2cam save1.world.json out.cam --reencode-cmp
//     Re-encodes the .cmp sub-file from the "campaign" JSON block (instead
//     of passing the original .cmp through verbatim). Use this to persist a
//     MODIFIED campaign: edit current_time / team states / timers in the
//     JSON, then re-encode. The result loads to the mutated campaign state.
//
// The default mode is the byte-identical passthrough (every sub-file's raw
// bytes pass through from the "subfiles_b64" block). --reencode-cmp replaces
// only the .cmp sub-file with a freshly encoded one; all other sub-files
// pass through unchanged. Re-encoding the .obj/.uni/.tea sub-files is the
// documented follow-on (see Docs/SAVE_WRITE_PLAN.md §6).

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/cam_writer.hpp>
#include <f4/world_convert/campaign_json.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/io/read_file.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: json2cam <input.world.json> [output.cam] [--reencode-cmp]\n"
                     "  Reassembles a .cam from a world JSON produced by\n"
                     "  `cam2json --preserve-subfiles`. The output decodes\n"
                     "  (via cam2json or CamArchive::load) to the identical\n"
                     "  sub-files — the round-trip closure of the save-write\n"
                     "  tranche.\n"
                     "  --reencode-cmp: re-encode the .cmp sub-file from the\n"
                     "  \"campaign\" JSON block (modified-save path). Use this\n"
                     "  to persist a mutated campaign state.\n";
        return 2;
    }
    const fs::path in = argv[1];
    fs::path out;
    bool reencode_cmp = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--reencode-cmp") {
            reencode_cmp = true;
        } else if (out.empty() && a.substr(0, 2) != "--") {
            out = fs::path(a);
        }
    }
    if (out.empty()) {
        out = in;
        // save1.world.json -> save1.cam ; foo.json -> foo.cam
        if (out.extension() == ".json") {
            const std::string stem = out.stem().string();
            fs::path base = stem;
            if (base.extension() == ".world") base = base.stem();
            out = base;
            out.replace_extension(".cam");
        } else {
            out.replace_extension(".cam");
        }
    }

    try {
        auto raw = f4::io::read_file(in, "json2cam");
        std::string json(raw.begin(), raw.end());

        std::vector<uint8_t> cam_bytes;
        if (reencode_cmp) {
            // Modified-save path: re-encode the .cmp from the campaign JSON
            // block, pass every other sub-file through verbatim.
            const int camp_version = f4::world_convert::read_world_json_version(json);
            auto header = f4::world_convert::from_world_json_campaign(json, camp_version);
            auto cmp_bytes = f4::world_convert::encode_cmp(header, camp_version);

            // Build the .cam: new .cmp + every other sub-file from
            // subfiles_b64. We parse the subfiles_b64 block, replace the
            // .cmp entry, and assemble via CamWriter.
            //
            // The simplest path: use cam_from_world_json to get the
            // passthrough bytes, load them as a CamArchive, swap the .cmp,
            // and re-write. This reuses the proven parse+assemble path.
            auto passthrough = f4::world_convert::cam_from_world_json(json);
            auto tmp = fs::temp_directory_path() / "f4_json2cam_tmp.cam";
            {
                std::ofstream f(tmp, std::ios::binary);
                f.write(reinterpret_cast<const char*>(passthrough.data()),
                        static_cast<std::streamsize>(passthrough.size()));
            }
            f4::world_convert::CamArchive cam;
            cam.load(tmp);
            std::error_code ec;
            fs::remove(tmp, ec);

            // Find the .cmp sub-file's name (e.g. "save1.cmp") so we
            // replace the right entry.
            std::string cmp_name = "save1.cmp";
            for (const auto& sf : cam.subfiles()) {
                if (sf.ext() == "cmp") { cmp_name = sf.name; break; }
            }

            f4::world_convert::CamWriter w;
            for (const auto& sf : cam.subfiles()) {
                if (sf.ext() == "cmp") {
                    w.add(cmp_name, cmp_bytes);   // the re-encoded .cmp
                } else {
                    w.add(sf.name, sf.data);      // pass through verbatim
                }
            }
            cam_bytes = w.build();
            std::cerr << "  re-encoded .cmp from campaign JSON block (v"
                      << camp_version << ", " << cmp_bytes.size()
                      << " bytes)\n";
        } else {
            // Default: byte-identical passthrough.
            cam_bytes = f4::world_convert::cam_from_world_json(json);
        }

        std::error_code mk_ec;
        fs::create_directories(out.parent_path(), mk_ec);
        std::ofstream f(out, std::ios::binary);
        if (!f) throw std::runtime_error("json2cam: cannot write " + out.string());
        f.write(reinterpret_cast<const char*>(cam_bytes.data()),
                static_cast<std::streamsize>(cam_bytes.size()));
        if (!f) throw std::runtime_error("json2cam: short write to " + out.string());

        std::cout << "wrote " << out << " (" << cam_bytes.size()
                  << " bytes) from " << in;
        if (reencode_cmp) std::cout << " [cmp re-encoded]";
        std::cout << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "json2cam: error: " << e.what() << "\n";
        return 1;
    }
}

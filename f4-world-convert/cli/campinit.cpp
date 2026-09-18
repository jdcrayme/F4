// f4-world-convert/cli/campinit.cpp
//
// CLI: CAMP-INIT-1 — generate a fresh .cam campaign (and its world JSON)
// from a scenario pack.
//
//   campinit --pack war.pack.json --out-cam war.cam --out-world war.world.json
//   campinit --pack war.pack.json --out-cam war.cam --class-table FALCON4.ct
//
// The world JSON is produced by THE EXISTING READER (the generated .cam
// bytes load through CamArchive and emit via to_world_json — the same
// code path cam2json drives), so the archive and its runtime projection
// cannot drift apart.
//
// Exit codes (the importer CLIs' own convention — 0 ok / 1 error /
// 2 usage):
//   0  the archive (and world JSON, if requested) were written
//   1  a pack/build fault — the message is prefixed `pack: ` or `init: `
//   2  usage

#include <f4/world_convert/campaign_initializer.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/scenario_pack.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void usage(std::ostream& out) {
    out << "usage: campinit --pack <pack.json> "
           "(--out-cam <out.cam> | --out-world <out.json>) "
           "[--class-table <falcon4.ct.json>] [--quiet]\n"
           "  --pack:        the scenario pack (CAMP-INIT-1 schema — theater,\n"
           "                 OOB template, force levels, date/weather, seed).\n"
           "  --out-cam:     write the generated .cam archive here.\n"
           "  --out-world:   write the world JSON (decoded from the generated\n"
           "                 archive by the existing reader) here.\n"
           "  --class-table: falcon4.ct.json (or FALCON4.ct). Auto-located from\n"
           "                 the source tree when omitted.\n"
           "  --quiet:       suppress the summary line.\n";
}

bool write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!content.empty())
        out.write(reinterpret_cast<const char*>(content.data()),
                  static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

// The class-table auto-locate chain: --class-table, then the committed
// conversions (the same two wells campaignd drinks from).
fs::path locate_class_table(const fs::path& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;
    const fs::path candidates[] = {
#ifdef F4_CAMPINIT_CT_1
        F4_CAMPINIT_CT_1,
#endif
#ifdef F4_CAMPINIT_CT_2
        F4_CAMPINIT_CT_2,
#endif
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!c.empty() && fs::exists(c, ec)) return c;
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    fs::path pack_path;
    fs::path out_cam;
    fs::path out_world;
    fs::path explicit_ct;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--pack" && i + 1 < argc) {
            pack_path = argv[++i];
        } else if (a == "--out-cam" && i + 1 < argc) {
            out_cam = argv[++i];
        } else if (a == "--out-world" && i + 1 < argc) {
            out_world = argv[++i];
        } else if (a == "--class-table" && i + 1 < argc) {
            explicit_ct = argv[++i];
        } else if (a == "--quiet") {
            quiet = true;
        } else {
            usage(std::cerr);
            return 2;
        }
    }
    if (pack_path.empty() || (out_cam.empty() && out_world.empty())) {
        usage(std::cerr);
        return 2;
    }

    try {
        f4::world_convert::ScenarioPack pack =
            f4::world_convert::ScenarioPack::load(pack_path.string());

        const fs::path ct_path = locate_class_table(explicit_ct);
        if (ct_path.empty()) {
            std::cerr << "campinit: no class table found — pass "
                         "--class-table <falcon4.ct.json>\n";
            return 1;
        }
        f4::world_convert::ClassTable ct;
        ct.load_auto(ct_path);

        f4::world_convert::InitStats stats;
        f4::world_convert::InitBuildResult built =
            f4::world_convert::CampaignInitializer::build(pack, ct, &stats);

        if (!out_cam.empty()) {
            if (!write_file(out_cam, built.cam_bytes)) {
                std::cerr << "campinit: cannot write " << out_cam << "\n";
                return 1;
            }
        }
        if (!out_world.empty()) {
            const std::string world_json =
                f4::world_convert::CampaignInitializer::to_world_json(
                    built, ct, pack);
            if (!write_file(out_world, world_json)) {
                std::cerr << "campinit: cannot write " << out_world << "\n";
                return 1;
            }
        }
        if (!quiet) {
            std::cout << "campinit: " << pack.name << " — teams "
                      << stats.named_teams << ", objectives "
                      << stats.objectives << ", squadrons " << stats.squadrons
                      << ", battalions " << stats.battalions << ", last VU "
                      << stats.last_index_num << ", archive "
                      << stats.cam_bytes << " bytes";
            if (!out_cam.empty()) std::cout << " -> " << out_cam;
            if (!out_world.empty()) std::cout << " -> " << out_world;
            std::cout << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "campinit: " << e.what() << "\n";
        return 1;
    }
}

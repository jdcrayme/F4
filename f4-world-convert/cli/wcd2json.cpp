// f4-world-convert/cli/wcd2json.cpp
//
// CLI: convert the FreeFalcon Falcon4.WCD campaign weapon-class table to
// open JSON.
//
//   wcd2json <theater-db-dir>             -> writes falcon4.wcd.json
//   wcd2json <theater-db-dir> out.json    -> writes out.json
//   wcd2json <theater-db-dir> out.json --data-dir ./Data
//     - Writes to ./Data/Weapons/falcon4.wcd.json (asset-pipeline mode)
//     - (the manifest entry, like the other exports, is written by the
//       export script's generate_manifest.py pass — this CLI only
//       writes the file)
//
// The theater-db-dir is the terrdata/objects directory that holds the
// Falcon4.* tables (the same directory ct2json takes FALCON4.ct from).
// load_weapon_data() case-insensitively locates Falcon4.WCD inside it.
//
// This is the real-data tier's weapon deliverable (COMBAT_CHAIN_PLAN.md §5:
// "real weapon class data via f4-convert"). A note on the binary WST: WST
// is a FreeFalcon-runtime table that vanilla installs do not ship; the
// vanilla install's campaign weapon table IS Falcon4.WCD — the same table
// ClassTableEntry.data_ptr_index points at for data_type == DTYPE_WEAPON.
// f4-weapons' JSON loader (wcd_weapon_data.hpp) is the runtime consumer.
//
// JSON schema (one object per WeaponClassData record, file order):
// {
//   "format": "f4-weapon-class-table",
//   "version": 1,
//   "source": "FALCON4.WCD",
//   "source_fingerprint": "<fnv1a-64 of the binary>",
//   "count": N,
//   "entries": [
//     { "index": 0, "strength": 40, "damage_type": 2, "range_km": 48,
//       "flags": 512, "name": "AIM-120 AMRAAM",
//       "hit_chance": [0,0,0,0,0,0,0,0], "fire_rate": 1, "rarity": 1,
//       "guidance_flags": 0, "collective": 0, "simweap_index": -1,
//       "weight": 335, "drag_index": 0, "blast_radius": 50,
//       "radar_type": 0, "sim_data_idx": 0, "max_alt": 60 },
//     ...
//   ]
// }

#include <f4/world_convert/theater_data.hpp>
#include <f4/json/writer.hpp>
#include <f4/io/read_file.hpp>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace wc = f4::world_convert;
namespace json = f4::json;

namespace {

// FNV-1a 64-bit over the raw binary — the same staleness fingerprint
// ct2json writes (the manifest's "did this file change?" key).
std::string content_fingerprint(const std::vector<uint8_t>& data) {
    uint64_t h = 14695981039346656037ULL;  // FNV offset basis
    for (uint8_t b : data) {
        h ^= b;
        h *= 1099511628211ULL;  // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

int run(const fs::path& db_dir, const fs::path& out_path, bool data_dir_mode) {
    // Load the binary weapon table (the library's existing decoder; it
    // case-insensitively locates Falcon4.WCD under the directory and
    // throws with a human-readable cause when it is missing/corrupt).
    wc::WeaponClassTable wcd;
    try {
        wc::load_weapon_data(db_dir, wcd);
    } catch (const std::exception& e) {
        std::cerr << "wcd2json: failed to load Falcon4.WCD from "
                  << db_dir << ": " << e.what() << "\n";
        return 1;
    }
    if (wcd.entries.empty()) {
        std::cerr << "wcd2json: " << db_dir << " loaded zero weapon records\n";
        return 1;
    }

    // Read the raw binary for the manifest fingerprint (staleness key).
    // find_theater_file is internal; resolve the file the same way the
    // loader did — a case-insensitive sweep for Falcon4.WCD under the dir.
    std::vector<uint8_t> raw;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(db_dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::string name = e.path().filename().string();
        std::string upper;
        for (char c : name) upper.push_back(static_cast<char>(std::toupper(
            static_cast<unsigned char>(c))));
        if (upper == "FALCON4.WCD") {
            try {
                raw = f4::io::read_file(e.path(), "wcd2json");
            } catch (...) {
                // Non-fatal — the fingerprint is optional.
            }
            break;
        }
    }

    // Emit JSON.
    json::Writer w;
    w.raw("{\n");
    w.string("format"); w.raw(":"); w.string("f4-weapon-class-table"); w.raw(",\n");
    w.string("version"); w.raw(":"); w.number(1); w.raw(",\n");
    w.string("source"); w.raw(":"); w.string("FALCON4.WCD"); w.raw(",\n");
    w.string("source_fingerprint"); w.raw(":"); w.string(content_fingerprint(raw)); w.raw(",\n");
    w.string("count"); w.raw(":"); w.number(static_cast<std::uint64_t>(wcd.entries.size())); w.raw(",\n");
    w.string("entries"); w.raw(": [\n");
    for (std::size_t i = 0; i < wcd.entries.size(); ++i) {
        const auto& e = wcd.entries[i];
        w.raw("  {");
        w.number_key("index", static_cast<int>(e.index)); w.raw(", ");
        w.number_key("strength", static_cast<int>(e.strength)); w.raw(", ");
        w.number_key("damage_type", static_cast<int>(e.damage_type)); w.raw(", ");
        w.number_key("range_km", static_cast<int>(e.range_km)); w.raw(", ");
        w.number_key("flags", static_cast<int>(e.flags)); w.raw(", ");
        w.string("name"); w.raw(":"); w.string(e.name); w.raw(", ");
        w.raw("\"hit_chance\": [");
        for (std::size_t m = 0; m < e.hit_chance.size(); ++m) {
            if (m) w.raw(", ");
            w.number(static_cast<int>(e.hit_chance[m]));
        }
        w.raw("], ");
        w.number_key("fire_rate", static_cast<int>(e.fire_rate)); w.raw(", ");
        w.number_key("rarity", static_cast<int>(e.rarity)); w.raw(", ");
        w.number_key("guidance_flags", static_cast<int>(e.guidance_flags)); w.raw(", ");
        w.number_key("collective", static_cast<int>(e.collective)); w.raw(", ");
        w.number_key("simweap_index", static_cast<int>(e.simweap_index)); w.raw(", ");
        w.number_key("weight", static_cast<int>(e.weight)); w.raw(", ");
        w.number_key("drag_index", static_cast<int>(e.drag_index)); w.raw(", ");
        w.number_key("blast_radius", static_cast<int>(e.blast_radius)); w.raw(", ");
        w.number_key("radar_type", static_cast<int>(e.radar_type)); w.raw(", ");
        w.number_key("sim_data_idx", static_cast<int>(e.sim_data_idx)); w.raw(", ");
        w.number_key("max_alt", static_cast<int>(e.max_alt));
        w.raw("}");
        if (i + 1 < wcd.entries.size()) w.raw(",");
        w.raw("\n");
    }
    w.raw("]\n}\n");

    // Write to the output path (or Data/Weapons/ in data-dir mode).
    fs::path final_path = out_path;
    if (data_dir_mode) {
        fs::create_directories(out_path / "Weapons");
        final_path = out_path / "Weapons" / "falcon4.wcd.json";
    }
    std::ofstream f(final_path, std::ios::binary);
    if (!f) {
        std::cerr << "wcd2json: cannot write " << final_path << "\n";
        return 1;
    }
    f << w.str();
    f.close();

    std::cout << "wcd2json: " << db_dir << " -> " << final_path
              << " (" << wcd.entries.size() << " weapon records, "
              << (data_dir_mode ? "asset-pipeline" : "standalone") << " mode)\n";
    return 0;
}

void usage() {
    std::cerr <<
        "usage: wcd2json <theater-db-dir> [output.json] [--data-dir <Data>]\n"
        "  Converts the binary Falcon4.WCD weapon-class table to open JSON.\n"
        "  <theater-db-dir> is the terrdata/objects directory holding the\n"
        "  Falcon4.* tables (FALCON4.WCD is located case-insensitively).\n"
        "  --data-dir: asset-pipeline mode (writes <Data>/Weapons/falcon4.wcd.json).\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }

    fs::path db_dir = argv[1];
    fs::path out_path = "falcon4.wcd.json";
    bool data_dir_mode = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir") {
            data_dir_mode = true;
            if (i + 1 < argc) { out_path = argv[++i]; }
            else { usage(); return 1; }
        } else if (a == "-h" || a == "--help") {
            usage(); return 0;
        } else {
            out_path = a;
        }
    }

    if (!fs::exists(db_dir)) {
        std::cerr << "wcd2json: input not found: " << db_dir << "\n";
        return 1;
    }

    return run(db_dir, out_path, data_dir_mode);
}

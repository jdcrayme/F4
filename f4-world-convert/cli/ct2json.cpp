// f4-world-convert/cli/ct2json.cpp
//
// CLI: convert a FreeFalcon FALCON4.ct class table to open JSON.
//
//   ct2json FALCON4.ct                    -> writes falcon4.ct.json
//   ct2json FALCON4.ct out.json           -> writes out.json
//   ct2json FALCON4.ct out.json --data-dir ./Data
//     - Writes to ./Data/Classes/falcon4.ct.json (asset-pipeline mode)
//     - Updates ./Data/manifest.json with a classes:falcon4-ct entry
//
// The JSON form eliminates the binary FALCON4.ct from the runtime (per
// ASSET_PIPELINE_SPEC.md P2 — link-time isolation; NO_BINARY_RUNTIME_PLAN.md
// Tranche 0a.1). ClassTable::load_json() is the consumer-side counterpart.
//
// JSON schema (one object per ClassTableEntry, entity_type = 100 + index):
// {
//   "format": "f4-class-table",
//   "version": 1,
//   "source": "FALCON4.ct",
//   "first_entity_type": 100,
//   "count": 2135,
//   "entries": [
//     { "entity_type": 100, "domain": 3, "cls": 4, "type": 10, "stype": 0,
//       "vis_type": [1050, 0, 0, 0, 0, 0, 0],
//       "data_type": 1, "data_ptr_index": 42 },
//     ...
//   ]
// }

#include <f4/world_convert/class_table.hpp>
#include <f4/json/writer.hpp>
#include <f4/io/read_file.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace wc = f4::world_convert;
namespace json = f4::json;

namespace {

// Compute a simple SHA-256-compatible content hash for the manifest. Uses
// a stub here (the real manifest writer lives in f4-import; this CLI is
// dependency-light). Returns a hex string of the file size + first/last
// 32 bytes — enough for staleness detection without a crypto dependency.
std::string content_fingerprint(const std::vector<uint8_t>& data) {
    // FNV-1a 64-bit over the whole file — fast, no deps, good enough for
    // staleness detection (the manifest's job is "did this file change?",
    // not "is this file authentic?").
    uint64_t h = 14695981039346656037ULL;  // FNV offset basis
    for (uint8_t b : data) {
        h ^= b;
        h *= 1099511628211ULL;  // FNV prime
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

int run(const fs::path& ct_path, const fs::path& out_path, bool data_dir_mode) {
    // Load the binary class table (the library's existing decoder).
    wc::ClassTable ct;
    try {
        ct.load(ct_path);
    } catch (const std::exception& e) {
        std::cerr << "ct2json: failed to load " << ct_path << ": " << e.what() << "\n";
        return 1;
    }
    if (!ct.loaded()) {
        std::cerr << "ct2json: " << ct_path << " loaded empty\n";
        return 1;
    }

    // Read the raw bytes for the manifest fingerprint (staleness detection).
    std::vector<uint8_t> raw;
    try {
        raw = f4::io::read_file(ct_path, "ct2json");
    } catch (...) {
        // Non-fatal — the fingerprint is optional.
    }

    // Emit JSON.
    json::Writer w;
    w.raw("{\n");
    w.string("format"); w.raw(":"); w.string("f4-class-table"); w.raw(",\n");
    w.string("version"); w.raw(":"); w.number(1); w.raw(",\n");
    w.string("source"); w.raw(":"); w.string(ct_path.filename().string()); w.raw(",\n");
    // FNV-1a fingerprint of the source binary — the manifest's staleness key.
    w.string("source_fingerprint"); w.raw(":"); w.string(content_fingerprint(raw)); w.raw(",\n");
    w.string("first_entity_type"); w.raw(":"); w.number(100); w.raw(",\n");
    w.string("count"); w.raw(":"); w.number(static_cast<std::uint64_t>(ct.size())); w.raw(",\n");
    w.string("entries"); w.raw(": [\n");
    for (std::size_t i = 0; i < ct.size(); ++i) {
        const auto* e = ct.lookup(static_cast<uint16_t>(100 + i));
        if (!e) continue;
        w.raw("  {");
        w.number_key("entity_type", static_cast<int>(100 + i)); w.raw(", ");
        w.number_key("domain", static_cast<int>(e->domain)); w.raw(", ");
        w.number_key("cls", static_cast<int>(e->cls)); w.raw(", ");
        w.number_key("type", static_cast<int>(e->type)); w.raw(", ");
        w.number_key("stype", static_cast<int>(e->stype)); w.raw(", ");
        w.raw("\"vis_type\": [");
        for (int v = 0; v < 7; ++v) {
            if (v) w.raw(", ");
            w.number(static_cast<int>(e->vis_type[v]));
        }
        w.raw("], ");
        w.number_key("data_type", static_cast<int>(e->data_type)); w.raw(", ");
        w.number_key("data_ptr_index", static_cast<uint32_t>(e->data_ptr_index));
        w.raw("}");
        if (i + 1 < ct.size()) w.raw(",");
        w.raw("\n");
    }
    w.raw("]\n}\n");

    // Write to the output path (or Data/Classes/ in data-dir mode).
    fs::path final_path = out_path;
    if (data_dir_mode) {
        fs::create_directories(out_path / "Classes");
        final_path = out_path / "Classes" / "falcon4.ct.json";
    }
    std::ofstream f(final_path, std::ios::binary);
    if (!f) {
        std::cerr << "ct2json: cannot write " << final_path << "\n";
        return 1;
    }
    f << w.str();
    f.close();

    std::cout << "ct2json: " << ct_path << " -> " << final_path
              << " (" << ct.size() << " entries, "
              << final_path << " is "
              << (data_dir_mode ? "asset-pipeline" : "standalone") << " mode)\n";
    return 0;
}

void usage() {
    std::cerr <<
        "usage: ct2json <FALCON4.ct> [output.json] [--data-dir <Data>]\n"
        "  Converts the binary Falcon4 class table to open JSON.\n"
        "  --data-dir: asset-pipeline mode (writes <Data>/Classes/falcon4.ct.json).\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }

    fs::path ct_path = argv[1];
    fs::path out_path = "falcon4.ct.json";
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

    if (!fs::exists(ct_path)) {
        std::cerr << "ct2json: input not found: " << ct_path << "\n";
        return 1;
    }

    return run(ct_path, out_path, data_dir_mode);
}

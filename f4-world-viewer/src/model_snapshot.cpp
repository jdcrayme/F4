// f4-world-viewer/src/model_snapshot.cpp
//
// Implementation of HDR/LOD parsing for --list-models and --parse-model.
// Produces JSON output compatible with scripts/model_tool.py.

#include "model_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace f4::viewer {

namespace {

// ── Constants ────────────────────────────────────────────────────────────
constexpr uint32_t FORMAT_VERSION = 0x03087000;
constexpr int S_PFR       = 48;   // ParentFileRecord on disk
constexpr int S_LODREC    = 8;    // LOD record (obj + maxRange)
constexpr int S_PPOINT    = 12;   // 3 floats
constexpr int S_PCOLOR    = 16;
constexpr int S_DPAL      = 1032;
constexpr int S_TEXENTRY  = 40;
constexpr int S_DTEX      = 24;

// ── Binary reader helper ─────────────────────────────────────────────────
struct BinReader {
    const uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;

    bool ok() const { return pos <= size; }

    template<typename T>
    bool read(T& out) {
        if (pos + sizeof(T) > size) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool read_bytes(void* dst, std::size_t n) {
        if (pos + n > size) return false;
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }

    bool skip(std::size_t n) {
        if (pos + n > size) return false;
        pos += n;
        return true;
    }
};

// ── JSON builder helpers ─────────────────────────────────────────────────
void json_escape(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
}

std::string to_hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

// ── HDR structures ───────────────────────────────────────────────────────
struct LodTableEntry {
    int index;
    uint32_t offset;
    uint32_t size;
};

struct LodRecord {
    std::string name;
    int lod_table_idx;
    float max_range;
};

struct ParentRecord {
    int index;
    float radius;
    float min_x, max_x, min_y, max_y, min_z, max_z;
    float radar_sign, ir_sign;
    int16_t n_texture_sets;
    int16_t n_dynamic_coords;
    uint8_t n_lods;
    uint8_t n_switch;
    uint8_t n_dof;
    uint8_t n_slots;
    int16_t n_switches;
    int16_t n_dofs;
    std::vector<LodRecord> lods;
    // Slot positions stored as flat float triples
    std::vector<float> slot_positions; // 3 floats per slot
};

struct HdrData {
    uint32_t version;
    int n_colors, n_dark_colors;
    int n_palettes;
    int n_textures;
    int max_tags;
    int n_lod_entries;
    int n_parents;
    bool is_new_format;
    bool has_lod_names;
    std::vector<LodTableEntry> lod_entries;
    std::vector<ParentRecord> parents;
};

// ── HDR parser ───────────────────────────────────────────────────────────
bool parse_hdr(const uint8_t* data, std::size_t size, HdrData& hdr,
               std::string* err_out)
{
    BinReader r{data, size, 0};

    // Version
    if (!r.read(hdr.version)) {
        if (err_out) *err_out = "HDR too small for version";
        return false;
    }

    // ColorBank
    if (!r.read(hdr.n_colors) || !r.read(hdr.n_dark_colors)) return false;
    if (!r.skip(hdr.n_colors * S_PCOLOR)) return false;

    // PaletteBank
    if (!r.read(hdr.n_palettes)) return false;
    if (!r.skip(hdr.n_palettes * S_DPAL)) return false;

    // TextureBank
    int32_t n_tex;
    if (!r.read(n_tex)) return false;
    hdr.n_textures = n_tex;
    int32_t max_cs;
    if (!r.read(max_cs)) return false;
    hdr.is_new_format = (max_cs != 0xFEEF);
    if (!r.skip(hdr.n_textures * S_TEXENTRY)) return false;

    // LOD table
    if (!r.read(hdr.max_tags)) return false;
    if (!r.read(hdr.n_lod_entries)) return false;

    hdr.lod_entries.reserve(hdr.n_lod_entries);
    for (int i = 0; i < hdr.n_lod_entries; ++i) {
        LodTableEntry e;
        e.index = i;
        // Each entry is 20 bytes: 12 unknown + 4 offset + 4 size
        if (!r.skip(12)) return false;
        if (!r.read(e.offset) || !r.read(e.size)) return false;
        hdr.lod_entries.push_back(e);
    }

    // Parent list
    if (!r.read(hdr.n_parents)) return false;

    hdr.parents.reserve(hdr.n_parents);
    for (int i = 0; i < hdr.n_parents; ++i) {
        ParentRecord p{};
        p.index = i;

        // Read 48-byte ParentFileRecord
        auto start = r.pos;
        if (!r.read(p.radius)) return false;
        if (!r.read(p.min_x) || !r.read(p.max_x)) return false;
        if (!r.read(p.min_y) || !r.read(p.max_y)) return false;
        if (!r.read(p.min_z) || !r.read(p.max_z)) return false;
        if (!r.read(p.radar_sign) || !r.read(p.ir_sign)) return false;
        if (!r.read(p.n_texture_sets) || !r.read(p.n_dynamic_coords)) return false;
        if (!r.read(p.n_lods) || !r.read(p.n_switch) ||
            !r.read(p.n_dof) || !r.read(p.n_slots)) return false;
        if (!r.read(p.n_switches) || !r.read(p.n_dofs)) return false;
        r.pos = start + S_PFR; // ensure we consumed exactly 48 bytes

        hdr.parents.push_back(p);
    }

    // Detect LOD names (same heuristic as Python script)
    auto try_parse = [&](bool with_names) -> bool {
        auto base = r.pos;          // save entry position
        auto save_pos = r.pos;
        for (auto& p : hdr.parents) {
            if (p.n_lods == 0) continue;
            int nsd = p.n_slots + p.n_dynamic_coords;
            auto skip = nsd * S_PPOINT +
                        p.n_lods * (S_LODREC + (with_names ? 32 : 0));
            if (save_pos + skip > size) { r.pos = base; return false; }
            save_pos += skip;
        }
        bool matches = (save_pos == size);
        r.pos = base;               // ALWAYS restore position
        return matches;
    };

    hdr.has_lod_names = false;
    if (!try_parse(false)) {
        if (try_parse(true)) {
            hdr.has_lod_names = true;
        }
    }

    // Parse slot positions + LOD records for each parent
    for (auto& p : hdr.parents) {
        if (p.n_lods == 0) continue;
        int nsd = p.n_slots + p.n_dynamic_coords;

        // Read slot positions
        if (nsd > 0) {
            p.slot_positions.resize(nsd * 3);
            for (int j = 0; j < nsd * 3; ++j) {
                if (!r.read(p.slot_positions[j])) return false;
            }
        }

        // Read LOD records
        for (int j = 0; j < p.n_lods; ++j) {
            LodRecord lr;
            if (hdr.has_lod_names) {
                char name[33] = {};
                if (!r.read_bytes(name, 32)) return false;
                name[32] = '\0';
                lr.name = name;
                // Trim trailing nulls
                while (!lr.name.empty() && lr.name.back() == '\0')
                    lr.name.pop_back();
            }
            uint32_t obj;
            if (!r.read(obj) || !r.read(lr.max_range)) return false;
            lr.lod_table_idx = static_cast<int>(obj >> 1);
            p.lods.push_back(lr);
        }
    }

    return true;
}

} // anonymous namespace

// ── File finder ──────────────────────────────────────────────────────────
std::pair<std::filesystem::path, std::filesystem::path>
find_koreaobj_files(const std::filesystem::path& install_root)
{
    namespace fs = std::filesystem;
    fs::path hdr, lod;

    // Try common locations
    std::array<fs::path, 3> dirs = {
        install_root,
        install_root / "terrdata" / "objects",
        install_root / "terrdata" / "korea" / "objects",
    };

    std::array<std::string, 2> hdr_names = {"KoreaObj.HDR", "KoreaObj.DXH"};
    std::array<std::string, 2> lod_names = {"KoreaObj.LOD", "KoreaObj.DXL"};

    for (const auto& d : dirs) {
        if (!fs::exists(d)) continue;
        for (const auto& n : hdr_names) {
            auto p = d / n;
            if (fs::exists(p)) { hdr = p; break; }
        }
        for (const auto& n : lod_names) {
            auto p = d / n;
            if (fs::exists(p)) { lod = p; break; }
        }
        if (!hdr.empty() || !lod.empty()) break;
    }

    return {hdr, lod};
}

// ── Build model list JSON ────────────────────────────────────────────────
std::string build_model_list_json(
    const std::filesystem::path& hdr_path,
    const std::filesystem::path& lod_path,
    std::string* err_out)
{
    // Read HDR file
    std::ifstream f(hdr_path, std::ios::binary | std::ios::ate);
    if (!f) {
        if (err_out) *err_out = "cannot open " + hdr_path.string();
        return {};
    }
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), buf.size())) {
        if (err_out) *err_out = "cannot read " + hdr_path.string();
        return {};
    }

    HdrData hdr;
    if (!parse_hdr(buf.data(), buf.size(), hdr, err_out)) return {};

    // Build JSON
    std::ostringstream ss;
    ss << "{\n  \"command\": \"list-models\",\n";
    ss << "  \"hdr_file\": \"" << hdr_path.string() << "\",\n";
    ss << "  \"lod_file\": \"" << lod_path.string() << "\",\n";
    ss << "  \"version\": \"" << to_hex(hdr.version) << "\",\n";
    ss << "  \"n_models\": " << hdr.n_parents << ",\n";
    ss << "  \"n_lod_entries\": " << hdr.n_lod_entries << ",\n";
    ss << "  \"n_textures\": " << hdr.n_textures << ",\n";
    ss << "  \"has_lod_names\": " << (hdr.has_lod_names ? "true" : "false") << ",\n";
    ss << "  \"models\": [\n";

    for (int i = 0; i < hdr.n_parents; ++i) {
        const auto& p = hdr.parents[i];
        if (i > 0) ss << ",\n";
        ss << "    {\n";
        ss << "      \"index\": " << p.index << ",\n";
        ss << "      \"radius\": " << p.radius << ",\n";
        ss << "      \"bounding_box\": ["
           << p.min_x << ", " << p.max_x << ", "
           << p.min_y << ", " << p.max_y << ", "
           << p.min_z << ", " << p.max_z << "],\n";
        ss << "      \"radar_signature\": " << p.radar_sign << ",\n";
        ss << "      \"ir_signature\": " << p.ir_sign << ",\n";
        ss << "      \"nlod\": " << static_cast<int>(p.n_lods) << ",\n";
        ss << "      \"nslots\": " << static_cast<int>(p.n_slots) << ",\n";
        ss << "      \"nswitches\": " << p.n_switches << ",\n";
        ss << "      \"ndofs\": " << p.n_dofs << ",\n";
        ss << "      \"ntexture_sets\": " << p.n_texture_sets << ",\n";
        ss << "      \"ndynamic_coords\": " << p.n_dynamic_coords << ",\n";

        // LODs
        ss << "      \"lods\": [";
        for (int j = 0; j < static_cast<int>(p.lods.size()); ++j) {
            if (j > 0) ss << ", ";
            const auto& lr = p.lods[j];
            ss << "{\"name\": \"";
            json_escape(ss, lr.name);
            ss << "\", \"lod_table_idx\": " << lr.lod_table_idx
               << ", \"max_range\": " << lr.max_range << "}";
        }
        ss << "]\n";

        ss << "    }";
    }
    ss << "\n  ]\n}";

    return ss.str();
}

// ── Build model JSON ─────────────────────────────────────────────────────
std::string build_model_json(
    const std::filesystem::path& hdr_path,
    const std::filesystem::path& lod_path,
    int parent_index,
    std::string* err_out)
{
    // Read HDR
    std::ifstream hf(hdr_path, std::ios::binary | std::ios::ate);
    if (!hf) {
        if (err_out) *err_out = "cannot open " + hdr_path.string();
        return {};
    }
    auto hsz = hf.tellg();
    hf.seekg(0);
    std::vector<uint8_t> hbuf(static_cast<std::size_t>(hsz));
    if (!hf.read(reinterpret_cast<char*>(hbuf.data()), hbuf.size())) {
        if (err_out) *err_out = "cannot read " + hdr_path.string();
        return {};
    }

    HdrData hdr;
    if (!parse_hdr(hbuf.data(), hbuf.size(), hdr, err_out)) return {};

    if (parent_index < 0 || parent_index >= hdr.n_parents) {
        if (err_out) *err_out = "parent index out of range";
        return {};
    }

    const auto& parent = hdr.parents[parent_index];

    // Build JSON with parent info + per-LOD metadata
    std::ostringstream ss;
    ss << "{\n  \"command\": \"parse-model\",\n";
    ss << "  \"parent_index\": " << parent_index << ",\n";
    ss << "  \"parent\": {\n";
    ss << "    \"radius\": " << parent.radius << ",\n";
    ss << "    \"bounding_box\": ["
       << parent.min_x << ", " << parent.max_x << ", "
       << parent.min_y << ", " << parent.max_y << ", "
       << parent.min_z << ", " << parent.max_z << "],\n";
    ss << "    \"radar\": " << parent.radar_sign << ",\n";
    ss << "    \"ir\": " << parent.ir_sign << ",\n";
    ss << "    \"nlod\": " << static_cast<int>(parent.n_lods) << ",\n";
    ss << "    \"nslots\": " << static_cast<int>(parent.n_slots) << ",\n";
    ss << "    \"nswitches\": " << parent.n_switches << ",\n";
    ss << "    \"ndofs\": " << parent.n_dofs << ",\n";
    ss << "    \"lods\": [";
    for (int j = 0; j < static_cast<int>(parent.lods.size()); ++j) {
        if (j > 0) ss << ", ";
        const auto& lr = parent.lods[j];
        ss << "{\"name\": \"";
        json_escape(ss, lr.name);
        ss << "\", \"idx\": " << lr.lod_table_idx
           << ", \"mr\": " << lr.max_range << "}";
    }
    ss << "]\n  },\n";

    // Read LOD file metadata for each LOD of this parent
    ss << "  \"parsed_lods\": [\n";

    std::ifstream lf(lod_path, std::ios::binary);
    if (lf) {
        for (int j = 0; j < static_cast<int>(parent.lods.size()); ++j) {
            if (j > 0) ss << ",\n";
            const auto& lr = parent.lods[j];
            int lod_idx = lr.lod_table_idx;

            ss << "    {\n";
            ss << "      \"lod_table_idx\": " << lod_idx << ",\n";
            ss << "      \"name\": \"";
            json_escape(ss, lr.name);
            ss << "\",\n";
            ss << "      \"max_range\": " << lr.max_range << ",\n";

            if (lod_idx < 0 || lod_idx >= hdr.n_lod_entries) {
                ss << "      \"error\": \"lod index out of range\"\n";
                ss << "    }";
                continue;
            }

            const auto& entry = hdr.lod_entries[lod_idx];
            ss << "      \"offset\": " << entry.offset << ",\n";
            ss << "      \"size\": " << entry.size << ",\n";

            if (entry.offset == 0 || entry.size == 0) {
                ss << "      \"note\": \"empty LOD entry\"\n";
                ss << "    }";
                continue;
            }

            // Read first 4 bytes to determine tag count (BSP) or DX header
            lf.seekg(entry.offset);
            uint32_t first4 = 0;
            if (lf.read(reinterpret_cast<char*>(&first4), 4)) {
                // Check if DX format
                bool is_dx = ((first4 & 0xFFFF) == ((~first4 >> 16) & 0xFFFF));
                ss << "      \"format\": \"" << (is_dx ? "dx" : "bsp") << "\",\n";

                if (!is_dx && first4 < 50000) {
                    // BSP: first4 is tag count
                    ss << "      \"bsp_tag_count\": " << first4 << ",\n";

                    // Read tag list (limited to first 100 tags for size)
                    uint32_t tc = first4;
                    uint32_t tags_to_read = std::min(tc, uint32_t(100));
                    std::vector<int32_t> tags(tags_to_read);
                    lf.seekg(entry.offset + 4);
                    lf.read(reinterpret_cast<char*>(tags.data()),
                            tags_to_read * sizeof(int32_t));

                    // Count tag types
                    std::array<int, 17> type_counts = {};
                    const char* type_names[] = {
                        "BNode", "BSubTree", "BRoot", "BSlotNode", "BDofNode",
                        "BSwitchNode", "BSplitterNode", "BPrimitiveNode",
                        "BLitPrimitiveNode", "BCulledPrimitiveNode",
                        "BSpecialXform", "BLightStringNode", "BTransNode",
                        "BScaleNode", "BXDofNode", "BXSwitchNode",
                        "BRenderControlNode"
                    };
                    for (auto t : tags) {
                        if (t >= 0 && t < 17) type_counts[t]++;
                    }

                    ss << "      \"bsp_type_counts\": {";
                    bool first = true;
                    for (int k = 0; k < 17; ++k) {
                        if (type_counts[k] > 0) {
                            if (!first) ss << ", ";
                            ss << "\"" << type_names[k] << "\": " << type_counts[k];
                            first = false;
                        }
                    }
                    ss << "},\n";
                }
            }

            ss << "      \"note\": \"use scripts/model_tool.py for full BSP tree parsing\"\n";
            ss << "    }";
        }
    }

    ss << "\n  ]\n}";

    return ss.str();
}

} // namespace f4::viewer

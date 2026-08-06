// f4-models/src/model_database.cpp
//
// ModelDatabase implementation — top-level entry point for the f4-models library.

#include <f4/models/model_database.hpp>
#include <f4/models/model_record.hpp>
#include <f4/models/geometry.hpp>

#include "bin_reader.hpp"
#include "hdr_parser.hpp"
#include "bsp_parser.hpp"
#include "dx_parser.hpp"
#include "geometry_extractor.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>

namespace f4::models {

// ── Inline implementations from headers ───────────────────────────────────

const char* bsp_node_type_name(BspNodeType t) noexcept {
    switch (t) {
        case BspNodeType::BNode:              return "BNode";
        case BspNodeType::BSubTree:           return "BSubTree";
        case BspNodeType::BRoot:              return "BRoot";
        case BspNodeType::BSlotNode:          return "BSlotNode";
        case BspNodeType::BDofNode:           return "BDofNode";
        case BspNodeType::BSwitchNode:        return "BSwitchNode";
        case BspNodeType::BSplitterNode:      return "BSplitterNode";
        case BspNodeType::BPrimitiveNode:     return "BPrimitiveNode";
        case BspNodeType::BLitPrimitiveNode:  return "BLitPrimitiveNode";
        case BspNodeType::BCulledPrimitiveNode: return "BCulledPrimitiveNode";
        case BspNodeType::BSpecialXform:      return "BSpecialXform";
        case BspNodeType::BLightStringNode:   return "BLightStringNode";
        case BspNodeType::BTransNode:         return "BTransNode";
        case BspNodeType::BScaleNode:         return "BScaleNode";
        case BspNodeType::BXDofNode:          return "BXDofNode";
        case BspNodeType::BXSwitchNode:       return "BXSwitchNode";
        case BspNodeType::BRenderControlNode: return "BRenderControlNode";
        default:                              return "Unknown";
    }
}

const char* poly_type_name(PolyType t) noexcept {
    switch (t) {
        case PolyType::PointF:  return "PointF";
        case PolyType::LineF:   return "LineF";
        case PolyType::F:       return "F";
        case PolyType::FL:      return "FL";
        case PolyType::G:       return "G";
        case PolyType::GL:      return "GL";
        case PolyType::Tex:     return "Tex";
        case PolyType::TexL:    return "TexL";
        case PolyType::TexG:    return "TexG";
        case PolyType::TexGL:   return "TexGL";
        case PolyType::CTex:    return "CTex";
        case PolyType::CTexL:   return "CTexL";
        case PolyType::CTexG:   return "CTexG";
        case PolyType::CTexGL:  return "CTexGL";
        case PolyType::AF:      return "AF";
        case PolyType::AFL:     return "AFL";
        case PolyType::AG:      return "AG";
        case PolyType::AGL:     return "AGL";
        case PolyType::ATex:    return "ATex";
        case PolyType::ATexL:   return "ATexL";
        case PolyType::ATexG:   return "ATexG";
        case PolyType::ATexGL:  return "ATexGL";
        case PolyType::CATex:   return "CATex";
        case PolyType::CATexL:  return "CATexL";
        case PolyType::CATexG:  return "CATexG";
        case PolyType::CATexGL: return "CATexGL";
        case PolyType::BAptTex: return "BAptTex";
        default:                return "Unknown";
    }
}

std::string_view ModelRecord::visual_class() const noexcept {
    // Heuristic classification based on slot/switch/DOF counts.
    // Uses effective counts (max of legacy and extended).
    // Air models (aircraft): many slots (≥4), many DOFs (≥5)
    // Ground models (vehicles): some slots (1-3), some DOFs
    // Feature models (buildings): few slots, few DOFs
    int dofs = effective_dofs();
    if (n_slots >= 4 && dofs >= 5) return "air";
    if (n_slots >= 1 && n_slots <= 3 && dofs >= 1) return "ground";
    if (n_slots == 0 && dofs == 0) return "feature";
    if (n_slots > 0) return "ground";
    return "feature";
}

std::string TextureEntry::filename() const {
    // Find null terminator within the 40-byte raw field
    std::size_t len = 0;
    while (len < 40 && raw[len] != '\0') ++len;
    return std::string(raw.data(), len);
}

const BspTree* ModelLod::bsp_tree() const noexcept {
    auto* bsp = std::get_if<BspLodData>(&data);
    return bsp ? &bsp->tree : nullptr;
}

BspTree* ModelLod::bsp_tree() noexcept {
    auto* bsp = std::get_if<BspLodData>(&data);
    return bsp ? &bsp->tree : nullptr;
}

const DxLodData* ModelLod::dx_data() const noexcept {
    return std::get_if<DxLodData>(&data);
}

DxLodData* ModelLod::dx_data() noexcept {
    return std::get_if<DxLodData>(&data);
}

// ── File I/O helper ───────────────────────────────────────────────────────

namespace detail {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<std::size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), buf.size())) return {};
    return buf;
}

} // namespace detail

// ── ModelDatabase ─────────────────────────────────────────────────────────

std::string ModelDatabase::load(
    const std::filesystem::path& hdr_path,
    const std::filesystem::path& lod_path)
{
    // Load HDR first
    auto err = load_hdr(hdr_path);
    if (!err.empty()) return err;

    hdr_path_ = hdr_path;
    lod_path_ = lod_path;

    // Read LOD file into memory
    lod_data_ = detail::read_file(lod_path);
    if (lod_data_.empty()) {
        return "cannot read LOD file: " + lod_path.string();
    }
    lod_loaded_ = true;

    return {};
}

std::string ModelDatabase::load_hdr(
    const std::filesystem::path& hdr_path)
{
    hdr_path_ = hdr_path;

    auto buf = detail::read_file(hdr_path);
    if (buf.empty()) {
        return "cannot read HDR file: " + hdr_path.string();
    }
    hdr_data_ = std::move(buf);

    detail::HdrParseResult result;
    std::string err;
    if (!detail::parse_hdr(hdr_data_.data(), hdr_data_.size(), result, err)) {
        return "HDR parse error: " + err;
    }

    version_ = result.version;
    n_parents_ = result.n_parents;
    n_lod_entries_ = result.n_lod_entries;
    n_textures_ = result.n_textures;
    max_tags_ = result.max_tags;
    is_new_format_ = result.is_new_format;
    has_lod_names_ = result.has_lod_names;
    color_bank_ = std::move(result.color_bank);
    lod_entries_ = std::move(result.lod_entries);
    parents_ = std::move(result.parents);

    return {};
}

std::string ModelDatabase::parse_model(int parent_index) {
    if (parent_index < 0 || parent_index >= n_parents_) {
        return "parent index out of range: " + std::to_string(parent_index);
    }

    if (!lod_loaded_) {
        return "LOD file not loaded — call load() first";
    }

    auto& parent = parents_[parent_index];

    // Parse each LOD of this model
    for (int j = 0; j < static_cast<int>(parent.lods.size()); ++j) {
        auto lod_err = parse_lod(parent_index, j);
        if (!lod_err.empty()) return lod_err;
    }

    return {};
}

std::string ModelDatabase::parse_lod(int parent_index, int lod_index) {
    if (parent_index < 0 || parent_index >= n_parents_) {
        return "parent index out of range";
    }

    auto& parent = parents_[parent_index];
    if (lod_index < 0 || lod_index >= static_cast<int>(parent.lods.size())) {
        return "LOD index out of range for parent " + std::to_string(parent_index);
    }

    if (!lod_loaded_) {
        return "LOD file not loaded";
    }

    const auto& lod_ref = parent.lods[lod_index];
    int lod_idx = lod_ref.lod_table_idx;

    if (lod_idx < 0 || lod_idx >= n_lod_entries_) {
        return "LOD table index out of range: " + std::to_string(lod_idx);
    }

    const auto& entry = lod_entries_[lod_idx];
    if (entry.offset == 0 || entry.size == 0) {
        return {}; // empty LOD entry, skip
    }

    // Check bounds
    if (entry.offset + entry.size > lod_data_.size()) {
        return "LOD entry offset/size out of bounds";
    }

    // Determine format from first 4 bytes
    uint32_t first4 = 0;
    std::memcpy(&first4, lod_data_.data() + entry.offset, 4);
    bool is_dx = detail::is_dx_format(first4);

    ModelLod mlod;
    mlod.format = is_dx ? LodFormat::Dx : LodFormat::Bsp;
    mlod.lod_table_idx = lod_idx;
    mlod.name = lod_ref.name;
    mlod.max_range = lod_ref.max_range;
    mlod.offset = entry.offset;
    mlod.size = entry.size;

    std::string err;

    if (is_dx) {
        DxLodData dx_data;
        if (!detail::parse_dx_lod(lod_data_.data() + entry.offset,
                                   entry.size, dx_data, err)) {
            return "DX parse error for parent " + std::to_string(parent_index) +
                   " LOD " + std::to_string(lod_index) + ": " + err;
        }
        mlod.data = std::move(dx_data);
    } else {
        BspLodData bsp_data;
        if (!detail::parse_bsp_tree(lod_data_.data() + entry.offset,
                                     entry.size, bsp_data.tree, err)) {
            return "BSP parse error for parent " + std::to_string(parent_index) +
                   " LOD " + std::to_string(lod_index) + ": " + err;
        }
        mlod.data = std::move(bsp_data);
    }

    // Store the parsed LOD for later retrieval
    LodKey key{parent_index, lod_index};
    parsed_lods_[key] = std::move(mlod);

    return {};
}

const ModelRecord* ModelDatabase::model(int index) const noexcept {
    if (index < 0 || index >= static_cast<int>(parents_.size())) return nullptr;
    return &parents_[index];
}

ModelGeometry ModelDatabase::extract_model_geometry(
    int parent_index, int lod_index,
    const ModelState& state) const
{
    LodKey key{parent_index, lod_index};
    auto it = parsed_lods_.find(key);
    if (it == parsed_lods_.end()) {
        return {};  // not parsed yet
    }

    const auto& mlod = it->second;
    const BspTree* tree = mlod.bsp_tree();
    if (!tree) {
        return {};  // DX format not yet supported for extraction
    }

    std::string err;
    return detail::extract_geometry(*tree, state, 0, err);
}

const BspTree* ModelDatabase::bsp_tree(int parent_index, int lod_index) const {
    LodKey key{parent_index, lod_index};
    auto it = parsed_lods_.find(key);
    if (it == parsed_lods_.end()) return nullptr;
    return it->second.bsp_tree();
}

std::vector<const ModelRecord*> ModelDatabase::find_by_slots(
    int min_slots, int max_slots) const
{
    std::vector<const ModelRecord*> result;
    for (const auto& m : parents_) {
        if (m.n_slots >= min_slots && m.n_slots <= max_slots) {
            result.push_back(&m);
        }
    }
    return result;
}

std::vector<const ModelRecord*> ModelDatabase::find_by_radius(
    float min_radius, float max_radius) const
{
    std::vector<const ModelRecord*> result;
    for (const auto& m : parents_) {
        if (m.radius >= min_radius && m.radius <= max_radius) {
            result.push_back(&m);
        }
    }
    return result;
}

std::vector<const ModelRecord*> ModelDatabase::find_by_class(
    std::string_view class_name) const
{
    std::vector<const ModelRecord*> result;
    for (const auto& m : parents_) {
        if (m.visual_class() == class_name) {
            result.push_back(&m);
        }
    }
    return result;
}

std::pair<std::filesystem::path, std::filesystem::path>
ModelDatabase::find_koreaobj_files(const std::filesystem::path& install_root)
{
    namespace fs = std::filesystem;
    fs::path hdr, lod;

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

} // namespace f4::models

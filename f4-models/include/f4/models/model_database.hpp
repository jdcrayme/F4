// f4-models/include/f4/models/model_database.hpp
//
// Top-level model database — loads KoreaObj.HDR + LOD files and
// provides typed access to all model data. This is the main entry
// point for the f4-models library.
//
// Usage:
//   f4::models::ModelDatabase db;
//   auto err = db.load("path/to/terrdata/objects/KoreaObj.HDR",
//                       "path/to/terrdata/objects/KoreaObj.LOD");
//   if (!err.empty()) { /* handle error */ }
//   for (const auto& model : db.models()) { ... }
//
// References:
//   FreeFalcon: src/graphics/include/objectparent.h (ObjectParent)
//   FreeFalcon: src/graphics/bsplib/objectparent.cpp (file reading)

#pragma once

#include <f4/models/model_record.hpp>
#include <f4/models/model_lod.hpp>
#include <f4/models/geometry.hpp>

#include <array>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace f4::models {

// ── HDR Banks ─────────────────────────────────────────────────────────────

/// One color entry from the ColorBank (16 bytes on disk).
struct ColorEntry {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    // 12 bytes padding on disk (total 16)
};

/// One palette from the PaletteBank (1032 bytes on disk).
struct PaletteEntry {
    std::array<uint8_t, 256> indices = {};  // 256 index bytes
    // 776 bytes padding on disk (total 1032)
};

/// One texture entry from the TextureBank (40 bytes on disk).
struct TextureEntry {
    std::array<char, 40> raw = {};  // raw 40 bytes (filename + flags)
    /// Extract the texture filename (null-terminated within the 40 bytes).
    [[nodiscard]] std::string filename() const;
};

// ── LOD Table Entry ───────────────────────────────────────────────────────

/// One entry in the HDR LOD table — maps LOD index to offset/size
/// in the LOD file.
struct LodTableEntry {
    int index = -1;
    uint32_t offset = 0;   ///< byte offset in KoreaObj.LOD
    uint32_t size = 0;     ///< byte size in KoreaObj.LOD
};

// ── Model Database ────────────────────────────────────────────────────────

class ModelDatabase {
public:
    /// Load the model database from HDR + LOD files.
    /// Returns empty string on success, error message on failure.
    /// After loading, models() returns the full model list.
    [[nodiscard]] std::string load(
        const std::filesystem::path& hdr_path,
        const std::filesystem::path& lod_path);

    /// Load only the HDR file (model directory, no geometry).
    [[nodiscard]] std::string load_hdr(
        const std::filesystem::path& hdr_path);

    /// Parse geometry for one model (all LODs).
    /// Must be called after load() or load_hdr().
    /// Returns empty string on success, error message on failure.
    [[nodiscard]] std::string parse_model(int parent_index);

    /// Parse geometry for a specific LOD of a model.
    [[nodiscard]] std::string parse_lod(int parent_index, int lod_index);

    /// Extract renderable geometry from a parsed BSP tree.
    /// Call after parse_lod(). Returns the geometry for the given LOD.
    /// @param parent_index  Model index
    /// @param lod_index     LOD index within the model (0 = highest detail)
    /// @param state         DOF/switch state controls
    /// @return              Extracted geometry (empty on failure)
    [[nodiscard]] ModelGeometry extract_model_geometry(
        int parent_index, int lod_index,
        const ModelState& state = {}) const;

    // ── Accessors ─────────────────────────────────────────────────────

    [[nodiscard]] bool valid() const noexcept { return version_ != 0; }
    [[nodiscard]] uint32_t version() const noexcept { return version_; }
    [[nodiscard]] int n_models() const noexcept { return n_parents_; }
    [[nodiscard]] int n_lod_entries() const noexcept { return n_lod_entries_; }
    [[nodiscard]] int n_textures() const noexcept { return n_textures_; }
    [[nodiscard]] bool has_lod_names() const noexcept { return has_lod_names_; }
    [[nodiscard]] bool is_new_format() const noexcept { return is_new_format_; }

    /// All parent model records.
    [[nodiscard]] const std::vector<ModelRecord>& models() const noexcept { return parents_; }

    /// Access a model by index. Returns nullptr if out of range.
    [[nodiscard]] const ModelRecord* model(int index) const noexcept;

    /// LOD table entries (offset + size for each LOD in the LOD file).
    [[nodiscard]] const std::vector<LodTableEntry>& lod_table() const noexcept { return lod_entries_; }

    /// HDR file path used for loading.
    [[nodiscard]] const std::filesystem::path& hdr_path() const noexcept { return hdr_path_; }
    /// LOD file path used for loading.
    [[nodiscard]] const std::filesystem::path& lod_path() const noexcept { return lod_path_; }

    // ── Query ─────────────────────────────────────────────────────────

    /// Find models with the given number of slots range.
    [[nodiscard]] std::vector<const ModelRecord*> find_by_slots(
        int min_slots, int max_slots = INT_MAX) const;

    /// Find models with bounding sphere radius in the given range.
    [[nodiscard]] std::vector<const ModelRecord*> find_by_radius(
        float min_radius, float max_radius) const;

    /// Find models matching a visual class heuristic.
    [[nodiscard]] std::vector<const ModelRecord*> find_by_class(
        std::string_view class_name) const;

    /// Access the parsed BSP tree for a model/LOD. Returns nullptr if not parsed.
    [[nodiscard]] const BspTree* bsp_tree(int parent_index, int lod_index) const;

    // ── File Finder ───────────────────────────────────────────────────

    /// Find KoreaObj.HDR and KoreaObj.LOD in common locations.
    /// Searches for both classic (.HDR/.LOD) and DX (.DXH/.DXL) variants.
    static std::pair<std::filesystem::path, std::filesystem::path>
    find_koreaobj_files(const std::filesystem::path& install_root);

private:
    uint32_t version_ = 0;
    int n_parents_ = 0;
    int n_lod_entries_ = 0;
    int n_textures_ = 0;
    int max_tags_ = 0;
    bool is_new_format_ = false;
    bool has_lod_names_ = false;

    std::filesystem::path hdr_path_;
    std::filesystem::path lod_path_;

    std::vector<LodTableEntry> lod_entries_;
    std::vector<ModelRecord> parents_;

    // Raw LOD file data (mmap'd or read into memory on demand)
    std::vector<uint8_t> lod_data_;
    bool lod_loaded_ = false;

    // HDR raw data (kept for LOD parsing)
    std::vector<uint8_t> hdr_data_;

    // Parsed LOD data, keyed by (parent_index << 8 | lod_index)
    // Stored sparse — only filled when parse_lod() is called.
    struct LodKey {
        int parent_index;
        int lod_index;
        bool operator==(const LodKey& o) const {
            return parent_index == o.parent_index && lod_index == o.lod_index;
        }
    };
    struct LodKeyHash {
        std::size_t operator()(const LodKey& k) const {
            return static_cast<std::size_t>(k.parent_index) * 256 +
                   static_cast<std::size_t>(k.lod_index);
        }
    };
    std::unordered_map<LodKey, ModelLod, LodKeyHash> parsed_lods_;
};

} // namespace f4::models

// f4-world-viewer/src/hex/hex_model.cpp

#include <f4/viewer/hex_model.hpp>
#include <f4/viewer/decoders.hpp>
#include <f4/terrain/terrain_data.hpp>  // THEATER_MAP_MAGIC

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <array>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// FileType helpers
// ---------------------------------------------------------------------------

const char* file_type_name(FileType t) noexcept {
    switch (t) {
        case FileType::Unknown:      return "Unknown";
        case FileType::CamArchive:   return "Campaign Archive (.cam)";
        case FileType::CmpSubfile:   return "Campaign Metadata (.cmp)";
        case FileType::TheaterMap:   return "Theater Header (THEATER.MAP)";
        case FileType::TheaterMea:   return "Elevation Grid (THEATER.MEA)";
        case FileType::TheaterO2:    return "Overlay (THEATER.O2)";
        case FileType::Falcon4Ct:    return "Class Table (FALCON4.ct)";
        case FileType::Dat:          return "Aircraft Data (.dat)";
        case FileType::Ver:          return "Version File (.ver)";
        case FileType::Lua:          return "Lua Script (.lua)";
        case FileType::Text:         return "Text";
        case FileType::Binary:       return "Binary";
    }
    return "Unknown";
}

FileType identify_file_by_extension(const std::filesystem::path& path) {
    // Lowercase the extension for case-insensitive matching.
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Special filename checks (no extension or fixed names).
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "falcon4.ct")    return FileType::Falcon4Ct;
    if (name == "theater.map")   return FileType::TheaterMap;
    if (name == "theater.mea")   return FileType::TheaterMea;
    if (name == "theater.o2")    return FileType::TheaterO2;

    if (ext == ".cam")  return FileType::CamArchive;
    if (ext == ".cmp")  return FileType::CmpSubfile;
    if (ext == ".dat")  return FileType::Dat;
    if (ext == ".ver")  return FileType::Ver;
    if (ext == ".lua")  return FileType::Lua;
    if (ext == ".txt" || ext == ".ini" || ext == ".lst" || ext == ".cfg" ||
        ext == ".csv" || ext == ".json")  return FileType::Text;
    return FileType::Unknown;
}

FileType identify_file(const std::filesystem::path& path,
                        const uint8_t* data, std::size_t size) {
    // Start with extension.
    auto t = identify_file_by_extension(path);
    if (t != FileType::Unknown) return t;

    // No extension match — probe magic bytes.
    if (size >= 4) {
        const uint32_t magic = static_cast<uint32_t>(data[0]) |
                               (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) |
                               (static_cast<uint32_t>(data[3]) << 24);
        // THEATER.MAP magic — but Falcon ships it as a file with the name
        // THEATER.MAP, so the extension check above catches it. We still
        // probe in case it's been renamed.
        if (magic == f4::terrain::THEATER_MAP_MAGIC) return FileType::TheaterMap;
    }

    // Probe for text content (heuristic: > 80% printable ASCII in first 1KB).
    if (size > 0) {
        const std::size_t probe = std::min<std::size_t>(size, 1024);
        std::size_t printable = 0;
        for (std::size_t i = 0; i < probe; ++i) {
            const uint8_t b = data[i];
            if (b == '\n' || b == '\r' || b == '\t' ||
                (b >= 0x20 && b < 0x7F)) {
                ++printable;
            }
        }
        if (printable * 10 > probe * 8) return FileType::Text;
    }

    return FileType::Binary;
}

// ---------------------------------------------------------------------------
// HexModel
// ---------------------------------------------------------------------------

void HexModel::load_file(const std::filesystem::path& path) {
    path_ = path;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("hex_model: cannot open " + path.string());
    const auto sz = f.tellg();
    if (sz < 0) throw std::runtime_error("hex_model: tellg failed on " + path.string());
    bytes_.resize(static_cast<std::size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes_.data()), bytes_.size());
    if (!f && !f.eof()) {
        throw std::runtime_error("hex_model: short read on " + path.string());
    }
    file_type_ = identify_file(path_, bytes_.data(), bytes_.size());
    annotations_.clear();
    selection_ = {};
}

void HexModel::load_bytes(const std::filesystem::path& path,
                            const std::vector<uint8_t>& bytes) {
    path_ = path;
    bytes_ = bytes;
    file_type_ = identify_file(path_, bytes_.data(), bytes_.size());
    annotations_.clear();
    selection_ = {};
}

void HexModel::load_bytes(const std::filesystem::path& path,
                            std::vector<uint8_t>&& bytes) {
    path_ = path;
    bytes_ = std::move(bytes);
    file_type_ = identify_file(path_, bytes_.data(), bytes_.size());
    annotations_.clear();
    selection_ = {};
}

void HexModel::apply_decoder() {
    apply_decoder(file_type_);
}

void HexModel::apply_decoder(FileType type) {
    annotations_.clear();
    switch (type) {
        case FileType::CamArchive:
            annotations_ = decode_cam_manifest(*this);
            break;
        case FileType::CmpSubfile:
            annotations_ = decode_cmp_header(*this);
            break;
        case FileType::TheaterMap:
            annotations_ = decode_theater_map(*this);
            break;
        case FileType::Falcon4Ct:
            annotations_ = decode_falcon4_ct(*this);
            break;
        // For these types, fall through to generic — we don't have
        // dedicated decoders yet, but the generic decoder's ASCII
        // string extraction + entropy is still useful.
        case FileType::TheaterMea:
        case FileType::TheaterO2:
        case FileType::Dat:
        case FileType::Ver:
        case FileType::Lua:
        case FileType::Text:
        case FileType::Binary:
        case FileType::Unknown:
            annotations_ = decode_generic(*this);
            break;
    }
}

std::vector<uint8_t> HexModel::slice(std::size_t offset, std::size_t length) const {
    if (offset >= bytes_.size()) return {};
    const std::size_t end = std::min(offset + length, bytes_.size());
    return std::vector<uint8_t>(bytes_.begin() + offset, bytes_.begin() + end);
}

uint64_t HexModel::read_le(std::size_t offset, std::size_t length) const noexcept {
    if (length == 0 || length > 8) return 0;
    if (offset + length > bytes_.size()) return 0;
    uint64_t v = 0;
    for (std::size_t i = 0; i < length; ++i) {
        v |= static_cast<uint64_t>(bytes_[offset + i]) << (8 * i);
    }
    return v;
}

std::string HexModel::read_fixed_string(std::size_t offset,
                                          std::size_t max_len) const noexcept {
    if (offset >= bytes_.size()) return {};
    const std::size_t end = std::min(offset + max_len, bytes_.size());
    std::size_t len = 0;
    while (len < (end - offset) && bytes_[offset + len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(bytes_.data() + offset), len);
}

const Annotation* HexModel::annotation_at(std::size_t offset) const noexcept {
    for (const auto& a : annotations_) {
        if (a.range.contains(offset)) return &a;
    }
    return nullptr;
}

double HexModel::entropy() const noexcept {
    if (bytes_.empty()) return 0.0;
    // Byte frequency histogram.
    std::array<std::size_t, 256> freq{};
    for (uint8_t b : bytes_) ++freq[b];

    // Shannon entropy: -sum(p_i * log2(p_i)).
    const double n = static_cast<double>(bytes_.size());
    double h = 0.0;
    for (std::size_t count : freq) {
        if (count == 0) continue;
        const double p = static_cast<double>(count) / n;
        h -= p * std::log2(p);
    }
    return h;  // bits per byte, 0.0 to 8.0
}

std::vector<ByteRange> HexModel::find_ascii_strings(std::size_t min_length) const {
    std::vector<ByteRange> out;
    std::size_t start = 0;
    std::size_t len = 0;
    for (std::size_t i = 0; i < bytes_.size(); ++i) {
        const uint8_t b = bytes_[i];
        // Printable ASCII + tab + newline + CR (treat as part of string
        // so multi-line text shows up as one run).
        const bool printable = (b >= 0x20 && b < 0x7F) || b == '\t' || b == '\n' || b == '\r';
        if (printable) {
            if (len == 0) start = i;
            ++len;
        } else {
            if (len >= min_length) {
                out.push_back({start, len});
            }
            len = 0;
        }
    }
    if (len >= min_length) {
        out.push_back({start, len});
    }
    return out;
}

} // namespace f4::viewer

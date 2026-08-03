// f4-world-convert/src/cam_archive.cpp

#include <f4/world_convert/cam_archive.hpp>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

std::string SubFile::ext() const {
    const auto dot = name.rfind('.');
    return (dot == std::string::npos) ? std::string{} : name.substr(dot + 1);
}

std::string SubFile::stem() const {
    const auto dot = name.rfind('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

void CamArchive::load(const std::filesystem::path& cam_path) {
    subfiles_.clear();
    raw_.clear();

    // Read the whole file into memory — .cam files are small (~200 KB).
    FILE* fp = std::fopen(cam_path.string().c_str(), "rb");
    if (!fp) throw std::runtime_error("CamArchive: cannot open " + cam_path.string());
    std::fseek(fp, 0, SEEK_END);
    const long file_size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (file_size < 8) { std::fclose(fp); throw std::runtime_error("CamArchive: file too small"); }
    raw_.resize(static_cast<std::size_t>(file_size));
    const std::size_t got = std::fread(raw_.data(), 1, raw_.size(), fp);
    std::fclose(fp);
    if (got != raw_.size()) throw std::runtime_error("CamArchive: short read");

    // First int32 = manifest offset.
    int32_t manifest_off = 0;
    std::memcpy(&manifest_off, raw_.data(), 4);
    if (manifest_off < 0 || static_cast<std::size_t>(manifest_off) + 4 > raw_.size())
        throw std::runtime_error("CamArchive: manifest offset out of range");

    // Read num_subfiles + per-file directory entries.
    std::size_t pos = static_cast<std::size_t>(manifest_off);
    int32_t num_files = 0;
    std::memcpy(&num_files, raw_.data() + pos, 4); pos += 4;
    if (num_files < 0 || num_files > 256)
        throw std::runtime_error("CamArchive: implausible sub-file count");

    subfiles_.reserve(static_cast<std::size_t>(num_files));
    for (int i = 0; i < num_files; ++i) {
        if (pos >= raw_.size()) throw std::runtime_error("CamArchive: truncated manifest");
        const uint8_t name_len = raw_[pos++] & 0xFF;
        if (pos + name_len + 8 > raw_.size())
            throw std::runtime_error("CamArchive: truncated manifest entry");
        SubFile sf;
        sf.name.assign(reinterpret_cast<const char*>(raw_.data() + pos), name_len);
        pos += name_len;
        std::memcpy(&sf.offset, raw_.data() + pos, 4); pos += 4;
        std::memcpy(&sf.size, raw_.data() + pos, 4); pos += 4;
        if (sf.offset < 0 || sf.size < 0 ||
            static_cast<std::size_t>(sf.offset) + static_cast<std::size_t>(sf.size) > raw_.size())
            throw std::runtime_error("CamArchive: sub-file bounds out of range");
        sf.data.assign(raw_.begin() + sf.offset,
                       raw_.begin() + sf.offset + sf.size);
        subfiles_.push_back(std::move(sf));
    }
}

const SubFile* CamArchive::find(const std::string& want_ext) const {
    for (const auto& sf : subfiles_) {
        if (sf.ext() == want_ext) return &sf;
    }
    return nullptr;
}

} // namespace f4::world_convert

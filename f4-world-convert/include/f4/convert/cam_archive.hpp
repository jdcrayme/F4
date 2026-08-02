// f4-world-convert/include/f4/convert/cam_archive.hpp
//
// .cam file container parser. A .cam ("campressed") file is an archive of
// typed sub-files (.cmp campaign metadata, .obj objectives, .uni units,
// .tea teams, .wth weather, .ver version, etc.) packed into one binary:
//
//   [0..3]            int32  manifest_offset   (seek here for the directory)
//   [4..manifest_off) sub-file data (concatenated, possibly LZSS-compressed)
//   [manifest_off]    int32  num_subfiles
//   then per subfile:  uint8 name_len;  char[name_len] name;
//                      int32 data_offset;  int32 data_size;
//
// This mirrors FreeFalcon's StartReadCampFile / ReadCampFile (campaign.cpp).
// The container is the structural foundation; each sub-file type has its own
// decoder (campaign_decoder.hpp for .cmp, future decoders for .obj/.uni/...).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::convert {

struct SubFile {
    std::string name;          // e.g. "save1.cmp"
    int32_t offset = 0;        // byte offset within the .cam file
    int32_t size = 0;          // sub-file size in bytes
    std::vector<uint8_t> data; // the sub-file's raw bytes (extracted on load)

    /// Extension without the dot, e.g. "cmp", "obj", "tea", "ver".
    [[nodiscard]] std::string ext() const;
    /// Stem (filename before the dot), e.g. "save1".
    [[nodiscard]] std::string stem() const;
};

class CamArchive {
public:
    /// Load and parse a .cam file. Throws on I/O error or malformed manifest.
    void load(const std::filesystem::path& cam_path);

    [[nodiscard]] const std::vector<SubFile>& subfiles() const noexcept { return subfiles_; }

    /// Find a sub-file by extension ("cmp", "obj", "tea", ...). Returns
    /// nullptr if not present.
    [[nodiscard]] const SubFile* find(const std::string& ext) const;

    /// The raw .cam file bytes (available after load()).
    [[nodiscard]] const std::vector<uint8_t>& raw_bytes() const noexcept { return raw_; }

private:
    std::vector<uint8_t> raw_;
    std::vector<SubFile> subfiles_;
};

} // namespace f4::convert

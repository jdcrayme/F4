// f4-world-convert/include/f4/world_convert/cam_writer.hpp
//
// CamWriter — the .cam container writer, the inverse of CamArchive::load.
//
// A .cam ("campressed") file is an archive of typed sub-files packed into
// one binary (cam_archive.hpp documents the format):
//
//   [0..3]            int32  manifest_offset   (seek here for the directory)
//   [4..manifest_off) sub-file data (concatenated)
//   [manifest_off]    int32  num_subfiles
//   then per subfile:  uint8 name_len;  char[name_len] name;
//                      int32 data_offset;  int32 data_size;
//
// CamWriter assembles that layout from a list of (name, data) sub-files.
// Sub-file data is packed contiguously starting at offset 4, in the order
// added; the manifest follows. This mirrors the standard FreeFalcon
// layout, so a round-tripped archive (load → write → load) decodes to the
// same sub-files.
//
// Dependencies: standard library only.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::world_convert {

/// One sub-file to write into a .cam archive.
struct CamSubfileInput {
    std::string name;             // e.g. "save1.cmp"
    std::vector<uint8_t> data;    // raw sub-file bytes
};

/// Builds a .cam archive from sub-files. Add sub-files in the desired
/// manifest order (typically the order CamArchive::subfiles() returns
/// them, for a faithful round-trip), then call build() or write().
class CamWriter {
public:
    /// Append a sub-file. The data is moved into the writer.
    void add(std::string name, std::vector<uint8_t> data);

    /// Append a sub-file from a name + a pointer/size (copy).
    void add(std::string name, const uint8_t* data, std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept { return subfiles_.size(); }
    [[nodiscard]] bool empty() const noexcept { return subfiles_.empty(); }

    /// Assemble the .cam bytes.
    [[nodiscard]] std::vector<uint8_t> build() const;

    /// Assemble and write to `path`. Throws std::runtime_error on I/O error.
    void write(const std::filesystem::path& path) const;

private:
    std::vector<CamSubfileInput> subfiles_;
};

/// Reassemble a .cam archive from a world JSON document that carries a
/// `"subfiles_b64"` block (produced by `cam2json --preserve-subfiles` or
/// `to_world_json` with `WorldJsonOptions::preserve_all_subfiles`).
///
/// Walks the JSON, extracts each sub-file's name + base64 bytes, and builds
/// the .cam via CamWriter. Throws std::runtime_error if the block is absent
/// or malformed. This is the library core of the `json2cam` CLI — tests and
/// the CLI share one implementation.
[[nodiscard]] std::vector<uint8_t> cam_from_world_json(const std::string& world_json);

} // namespace f4::world_convert

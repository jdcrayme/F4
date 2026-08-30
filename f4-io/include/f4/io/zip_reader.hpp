// f4-io/include/f4/io/zip_reader.hpp
//
// f4::io::ZipReader — minimal read-only PKZIP archive reader.
//
// Purpose: Falcon's stock install packs the theater's near-tile art as
// terrdata/<theater>/texture/texture.zip. Every entry in that archive is
// STORED (compression method 0 — verified against the stock Korea zip),
// so a full inflate implementation is unnecessary; this reader supports
// stored entries only and reports deflated entries as an error rather
// than silently decompressing garbage.
//
// Structure walk (EOCD-first, the robust route):
//   * Find the End Of Central Directory record (sig 0x06054b50) by
//     scanning backwards from the end of the file (max 64 KB + 22 B).
//   * Walk the central directory (sig 0x02014b50): filename, compressed
//     size, and the local-header offset of each entry.
//   * Read an entry by jumping to its local header (sig 0x04034b50),
//     skipping name+extra, and slicing `size` bytes.
//
// Names are indexed lowercased; lookups are case-insensitive.
//
// Zero f4-* dependencies. C++20.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace f4::io {

class ZipReader {
public:
    /// Open and index a zip archive. Throws std::runtime_error on I/O
    /// error or when no End Of Central Directory record is found.
    void load(const std::filesystem::path& zip_path);

    /// Number of indexed entries.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// True when `name` (case-insensitive) exists in the archive.
    [[nodiscard]] bool has(const std::string& name) const;

    /// Read an entry's bytes. Throws std::runtime_error when the name is
    /// absent or the entry uses an unsupported compression method.
    [[nodiscard]] std::vector<uint8_t> read(const std::string& name) const;

private:
    struct Entry {
        uint64_t data_offset = 0;  ///< Byte offset of the entry's data.
        uint32_t size = 0;         ///< Uncompressed (= stored) size.
    };

    std::vector<uint8_t> data_;                          ///< Whole archive.
    std::map<std::string, Entry> entries_;               ///< Keyed by lowercase name.
};

} // namespace f4::io

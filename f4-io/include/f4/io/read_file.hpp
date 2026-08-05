// f4-io/include/f4/io/read_file.hpp
//
// f4::io::read_file — load a whole file into a std::vector<uint8_t>.
//
// Consolidates 3 prior anonymous-namespace helpers + 1 inlined copy:
//   * f4-world-convert/src/theater_data.cpp   (most defensive: has sz<0 check)
//   * f4-world-convert/src/class_table.cpp    (drops sz<0 check)
//   * f4-terrain/src/terrain_data.cpp         (byte-identical to class_table)
//   * f4-world-convert/src/cam_archive.cpp    (inlined into CamArchive::load)
//
// API: returns the file contents as a vector<uint8_t>. Throws
// std::runtime_error on open failure, ftell failure (sz < 0), or short
// read. Error messages include the path where applicable.
//
// Zero f4-* dependencies. Standard library only.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace f4::io {

// Read the entire file at `path` into a byte vector.
//
// Throws std::runtime_error if:
//   * the file cannot be opened (message includes the path)
//   * ftell returns a negative size (defensive — message is "ftell failed")
//   * fread returns fewer bytes than ftell reported (short read; message
//     includes the path)
//
// The `label` argument is optional; if present, it is prepended to the
// error messages (e.g. "theater_data: cannot open ..."). When omitted,
// the messages use a generic "read_file:" prefix. Callers that previously
// embedded their own module prefix can pass it here for byte-identical
// diagnostics; callers that don't care get a sensible default.
std::vector<uint8_t> read_file(const std::filesystem::path& path,
                                const char* label = "read_file");

} // namespace f4::io

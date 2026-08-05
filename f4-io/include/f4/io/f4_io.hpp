// f4-io/include/f4/io/f4_io.hpp
//
// Umbrella header for f4-io — binary I/O primitives shared by
// f4-world-convert and f4-terrain.
//
// Components:
//   f4::io::Cursor      — sequential little-endian byte reader with a
//                         sticky error flag (header-only)
//   f4::io::read_file   — load a whole file into a vector<uint8_t>
//
// Zero f4-* dependencies. Standard library only.

#pragma once

#include <f4/io/cursor.hpp>
#include <f4/io/read_file.hpp>

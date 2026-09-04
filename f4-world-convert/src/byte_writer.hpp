// f4-world-convert/src/byte_writer.hpp
//
// ByteWriter — a sequential little-endian byte appender, the write-side
// mirror of f4::io::Cursor (the read-side helper the decoders use).
//
// Used by the save-write tranche's encoders (cmp_encoder, cam_writer) to
// serialize structs back into the flat on-disk byte sequences the decoders
// parse. Internal to f4-world-convert (not in include/) because only the
// encoders need it; the decoders continue to use f4::io::Cursor.
//
// Endianness: writes use std::memcpy from the host's native representation.
// All F4 binary files are little-endian on disk; on little-endian hosts
// (the only platforms F4 targets) the bytes round-trip correctly. This
// matches Cursor's documented endianness contract.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace f4::world_convert {

struct ByteWriter {
    std::vector<uint8_t> buf;

    void u8 (uint8_t  v) { buf.push_back(v); }
    void s8 (int8_t   v) { buf.push_back(static_cast<uint8_t>(v)); }
    void u16(uint16_t v) { append(&v, 2); }
    void s16(int16_t  v) { append(&v, 2); }
    void u32(uint32_t v) { append(&v, 4); }
    void s32(int32_t  v) { append(&v, 4); }
    void f32(float    v) { append(&v, 4); }

    // i* spellings (aliases matching Cursor's API).
    void i8 (int8_t  v) { s8(v); }
    void i16(int16_t v) { s16(v); }
    void i32(int32_t v) { s32(v); }

    /// Append `n` raw bytes.
    void bytes(const void* src, std::size_t n) {
        const auto* p = static_cast<const uint8_t*>(src);
        buf.insert(buf.end(), p, p + n);
    }
    void bytes(const std::vector<uint8_t>& v) {
        buf.insert(buf.end(), v.begin(), v.end());
    }

    /// Write a fixed-width NUL-padded char field: the string content
    /// followed by NULs out to `width` bytes total. If the content is
    /// longer than `width`, it is truncated to `width` (the decoder reads
    /// exactly `width` bytes regardless). This mirrors Cursor::fixed_string
    /// semantics: the decoder reads `width` bytes and extracts the prefix
    /// before the first NUL, so content + NUL + NUL-padding reproduces the
    /// original on-disk form for NUL-initialized buffers (the C convention).
    void fixed_string(const std::string& s, std::size_t width) {
        const std::size_t n = std::min<std::size_t>(s.size(), width);
        buf.insert(buf.end(), s.begin(), s.begin() + n);
        for (std::size_t i = n; i < width; ++i) buf.push_back(0);
    }

private:
    void append(const void* src, std::size_t n) {
        const auto* p = static_cast<const uint8_t*>(src);
        buf.insert(buf.end(), p, p + n);
    }
};

} // namespace f4::world_convert

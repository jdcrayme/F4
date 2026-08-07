// f4-models/src/bin_reader.hpp
//
// Lightweight binary reader for parsing Falcon 4.0 binary files.
// Tracks position and provides checked reads. No allocation —
// works on a borrowed buffer.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::models::detail {

struct BinReader {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    BinReader() = default;
    BinReader(const uint8_t* d, std::size_t s) : data(d), size(s), pos(0) {}

    [[nodiscard]] bool ok() const noexcept { return pos <= size; }
    // Returns the number of bytes remaining in the buffer. Renamed from
    // `remaining()` (which returned bool via implicit conversion and was a
    // footgun: `auto n = r.remaining();` silently got 0 or 1, not the byte
    // count). Use has_remaining() for the boolean check.
    //
    // Guards against unsigned underflow when pos > size (e.g. after an OOB
    // read) — same fix as f4::io::Cursor::remaining() (CHANGES.md H9).
    [[nodiscard]] std::size_t remaining_bytes() const noexcept {
        return (pos <= size) ? (size - pos) : 0;
    }
    [[nodiscard]] bool has_remaining() const noexcept { return pos < size; }
    [[nodiscard]] std::size_t offset() const noexcept { return pos; }

    template<typename T>
    [[nodiscard]] bool read(T& out) {
        if (pos + sizeof(T) > size) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    [[nodiscard]] bool read_bytes(void* dst, std::size_t n) {
        if (pos + n > size) return false;
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }

    [[nodiscard]] bool skip(std::size_t n) {
        if (pos + n > size) return false;
        pos += n;
        return true;
    }

    /// Read a null-terminated string (max max_len bytes).
    [[nodiscard]] bool read_string(std::string& out, std::size_t max_len) {
        if (pos + max_len > size) return false;
        const char* start = reinterpret_cast<const char*>(data + pos);
        out.assign(start, strnlen(start, max_len));
        pos += max_len;
        return true;
    }

    /// Peek at the next T without advancing.
    template<typename T>
    [[nodiscard]] bool peek(T& out) const {
        if (pos + sizeof(T) > size) return false;
        std::memcpy(&out, data + pos, sizeof(T));
        return true;
    }

    /// Seek to absolute position.
    void seek(std::size_t abs_pos) { pos = abs_pos; }
};

/// Read an entire file into a vector. Returns empty on failure.
[[nodiscard]] std::vector<uint8_t> read_file(const std::filesystem::path& path);

} // namespace f4::models::detail

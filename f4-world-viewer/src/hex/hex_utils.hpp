// f4-world-viewer/src/hex/hex_utils.hpp
//
// Shared hex-formatting utilities used by both the hex inspector and
// the annotation decoders. Extracted from duplicated anonymous-namespace
// definitions in hex_inspector.cpp and decoders.cpp.

#pragma once

#include <cstdint>
#include <cctype>
#include <string>

namespace f4::viewer::hex {

/// Format a byte as two uppercase hex digits (e.g. 0xAB → "AB").
[[nodiscard]] inline std::string hex_byte(uint8_t b) {
    static const char* digits = "0123456789ABCDEF";
    std::string s(2, '0');
    s[0] = digits[b >> 4];
    s[1] = digits[b & 0x0F];
    return s;
}

/// Format a byte slice as a hex string (space-separated).
[[nodiscard]] inline std::string hex_dump(const uint8_t* data, std::size_t n) {
    std::string s;
    s.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0) s += ' ';
        s += hex_byte(data[i]);
    }
    return s;
}

/// Format a byte slice as an ASCII preview (non-printable → '.').
[[nodiscard]] inline std::string ascii_preview(const uint8_t* data, std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        s.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.');
    }
    return s;
}

} // namespace f4::viewer::hex

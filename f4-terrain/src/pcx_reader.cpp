// f4-terrain/src/pcx_reader.cpp
//
// ZSoft PCX (8-bit, single plane, RLE) decoder.
//
// Header layout (128 bytes):
//   [0]     0x0A signature
//   [1]     version (5 = supports 8-bit palette)
//   [2]     encoding (1 = RLE)
//   [3]     bits per pixel per plane (8 for our tiles)
//   [4..11] xmin, ymin, xmax, ymax (int16, inclusive)
//   [65]    number of planes (1)
//   [66..67] bytes per decoded scanline plane (>= width)
//   [68..69] palette info (2 = color)
//
// RLE per scanline plane: a byte < 0xC0 is a literal index; a byte
// >= 0xC0 encodes run length (b & 0x3F) with the next byte as the index.
// Runs never cross scanline boundaries.
//
// 256-color palette: last 769 bytes of the file — 0x0C marker followed
// by 256 RGB triples.

#include "pcx_reader.hpp"

#include <cstring>

namespace f4::terrain {

bool decode_pcx(const uint8_t* data, std::size_t size, PcxImage& out,
                std::string& err) {
    if (size < 128) { err = "pcx: smaller than header"; return false; }
    if (data[0] != 0x0A) { err = "pcx: bad signature"; return false; }
    if (data[2] != 1)    { err = "pcx: not RLE encoded"; return false; }
    if (data[3] != 8)    { err = "pcx: not 8-bit"; return false; }
    if (data[65] != 1)   { err = "pcx: multi-plane not supported"; return false; }

    auto rd_i16 = [&](std::size_t at) {
        return static_cast<int16_t>(data[at] | (data[at + 1] << 8));
    };
    const int xmin = rd_i16(4), ymin = rd_i16(6), xmax = rd_i16(8), ymax = rd_i16(10);
    const uint32_t bpl = static_cast<uint32_t>(data[66] | (data[67] << 8));

    const int32_t w = static_cast<int32_t>(xmax) - xmin + 1;
    const int32_t h = static_cast<int32_t>(ymax) - ymin + 1;
    if (w <= 0 || h <= 0 || bpl < static_cast<uint32_t>(w)) {
        err = "pcx: bad dimensions";
        return false;
    }

    // Palette (trailing 769 bytes). Missing palette degrades to gray.
    uint8_t pal[768];
    bool have_pal = false;
    if (size >= 769 && data[size - 769] == 0x0C) {
        std::memcpy(pal, data + size - 768, 768);
        have_pal = true;
    } else {
        for (int i = 0; i < 256; ++i) {
            const uint8_t g = static_cast<uint8_t>(i);
            pal[i * 3 + 0] = pal[i * 3 + 1] = pal[i * 3 + 2] = g;
        }
    }

    // RLE-decode row by row; skip bpl - w padding bytes per row.
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);

    std::size_t src = 128;
    for (int32_t row = 0; row < h; ++row) {
        std::vector<uint8_t> line(bpl);
        std::size_t dst = 0;
        while (dst < bpl) {
            if (src >= size) { err = "pcx: truncated pixel data"; return false; }
            const uint8_t b = data[src++];
            if (b >= 0xC0) {
                const std::size_t run = b & 0x3F;
                if (src >= size) { err = "pcx: truncated RLE run"; return false; }
                const uint8_t v = data[src++];
                for (std::size_t k = 0; k < run && dst < bpl; ++k) line[dst++] = v;
            } else {
                line[dst++] = b;
            }
        }
        const std::size_t row_base =
            static_cast<std::size_t>(row) * w * 4;
        for (int32_t x = 0; x < w; ++x) {
            const uint8_t pi = line[x];
            const std::size_t px = row_base + static_cast<std::size_t>(x) * 4;
            out.rgba[px + 0] = have_pal ? pal[pi * 3 + 0] : pi;
            out.rgba[px + 1] = have_pal ? pal[pi * 3 + 1] : pi;
            out.rgba[px + 2] = have_pal ? pal[pi * 3 + 2] : pi;
            out.rgba[px + 3] = 0xFF;
        }
    }
    return true;
}

} // namespace f4::terrain

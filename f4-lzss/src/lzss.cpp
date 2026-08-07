// f4-lzss/src/lzss.cpp
//
// LZSS decompression — FreeFalcon / Falcon 4.0 variant.
//
// References:
//   FreeFalcon: src/graphics/image/imagebuf.cpp (ImageMemClass::Expand)
//   FreeFalcon: src/ui/uicomp.cpp (Decompress)
//   BMS:        src/FalcLib/Compress.cpp
//
// The algorithm processes a stream of (flag_byte, 8 tokens) groups.
// Each bit in the flag byte selects literal (0) or match (1).

#include <f4/lzss/lzss.hpp>

namespace f4::lzss {

// ── Sliding window constants ───────────────────────────────────────────────
static constexpr std::size_t WINDOW_SIZE = 4096;   // 2^12
static constexpr unsigned    MIN_MATCH   = 3;       // minimum match length
static constexpr unsigned    MAX_MATCH   = 18;      // 3 + 15 (4-bit length field)

std::size_t decompress(
    const uint8_t* src, std::size_t src_size,
    uint8_t* dst, std::size_t dst_capacity)
{
    if (!src || !dst) return 0;

    std::size_t src_pos = 0;
    std::size_t dst_pos = 0;

    while (src_pos < src_size) {
        // Read flag byte — each bit controls the next token
        const uint8_t flag = src[src_pos++];

        for (unsigned bit = 0; bit < 8; ++bit) {
            if (src_pos >= src_size) break;

            if (flag & (1u << bit)) {
                // ── Match reference (2 bytes) ────────────────────────
                if (src_pos + 1 >= src_size) break;

                const uint8_t b0 = src[src_pos];
                const uint8_t b1 = src[src_pos + 1];
                src_pos += 2;

                // Decode: b0 high nibble = offset high 4 bits
                //         b0 low nibble  = length - 3
                //         b1             = offset low 8 bits
                const std::size_t offset = (static_cast<std::size_t>(b0 & 0xF0) << 4) | b1;
                const unsigned    length = static_cast<unsigned>(b0 & 0x0F) + MIN_MATCH;

                // Copy from sliding window
                for (unsigned k = 0; k < length; ++k) {
                    if (dst_pos >= dst_capacity) return dst_pos;
                    if (offset + k < dst_pos) {
                        dst[dst_pos++] = dst[offset + k];
                    } else {
                        // Offset references a position not yet written —
                        // this can happen with run-length patterns.
                        // FreeFalcon handles this by writing zeros.
                        dst[dst_pos++] = 0;
                    }
                }
            } else {
                // ── Literal byte ──────────────────────────────────────
                if (dst_pos >= dst_capacity) return dst_pos;
                dst[dst_pos++] = src[src_pos++];
            }
        }
    }

    return dst_pos;
}

std::vector<uint8_t> decompress(
    const uint8_t* src, std::size_t src_size,
    std::size_t expected_size)
{
    if (!src || src_size == 0) return {};

    // Pre-allocate: if we know the expected size, use it; otherwise
    // estimate ~2× compression ratio.
    const std::size_t cap = expected_size > 0 ? expected_size : src_size * 2;
    std::vector<uint8_t> result(cap);

    const std::size_t actual = decompress(src, src_size, result.data(), cap);
    if (actual == 0 && expected_size > 0) {
        // May have hit capacity — retry with larger buffer
        result.resize(expected_size * 2);
        const std::size_t retry = decompress(src, src_size, result.data(), result.size());
        if (retry == 0) return {};
        result.resize(retry);
        return result;
    }

    result.resize(actual);
    return result;
}

} // namespace f4::lzss

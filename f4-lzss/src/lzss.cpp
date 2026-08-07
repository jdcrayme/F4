// f4-lzss/src/lzss.cpp
//
// LZSS decompression — FreeFalcon / Falcon 4.0 variant.
//
// This is a faithful reimplementation of FreeFalcon's LZSS_Expand()
// from src/utils/lzss.cpp. The algorithm uses:
//   - 12-bit sliding window (4096 bytes), ring-buffered
//   - 4-bit match length (1..16 after BREAK_EVEN adjustment)
//   - Blocked I/O: stream of (flag_byte, 8 tokens) groups
//
// Match token encoding (2 bytes):
//   byte0: high nibble = length (0..15), low nibble = position bits [11:8]
//   byte1: position bits [7:0]
//   position = byte1 | ((byte0 & 0x0F) << 8)   → 12-bit index into window
//   length   = (byte0 >> 4) + BREAK_EVEN       → 1..16
//
// BREAK_EVEN = (1 + INDEX_BIT_COUNT + LENGTH_BIT_COUNT) / 9 = (1+12+4)/9 = 1
//
// Flag byte: bit N (0..7, LSB first) selects token type:
//   0 = match reference (2 bytes)
//   1 = literal byte (1 byte)
//
// CRITICAL: The window is a 4096-byte RING BUFFER (window[]), NOT the
// output buffer. current_position starts at 1 and wraps with
// MOD_WINDOW(pos) = pos & 4095. Match positions index into this ring
// buffer, not the output stream. Using the output buffer directly (as
// the previous implementation did) produces correct output for the
// first 4096 bytes but garbage afterwards — which is why 64×64
// textures (4096 bytes) appeared correct but 128×128 (16384 bytes)
// and 256×256 (65536 bytes) textures looked like static noise.
//
// References:
//   FreeFalcon: src/utils/lzss.cpp (LZSS_Expand, OutputPair, InputBit)
//   FreeFalcon: src/graphics/ddstuff/imagebuf.cpp (ImageMemClass::Expand)

#include <f4/lzss/lzss.hpp>

#include <cstring>

namespace f4::lzss {

// ── Sliding window constants (match FreeFalcon's lzss.cpp) ────────────────
static constexpr int      INDEX_BIT_COUNT = 12;
static constexpr int      LENGTH_BIT_COUNT = 4;
static constexpr std::size_t WINDOW_SIZE   = 1u << INDEX_BIT_COUNT;  // 4096
static constexpr int      BREAK_EVEN       = (1 + INDEX_BIT_COUNT + LENGTH_BIT_COUNT) / 9;  // 1

// MOD_WINDOW: wrap a window index to [0, WINDOW_SIZE)
static constexpr std::size_t mod_window(std::size_t a) {
    return a & (WINDOW_SIZE - 1);
}

std::size_t decompress(
    const uint8_t* src, std::size_t src_size,
    uint8_t* dst, std::size_t dst_capacity)
{
    if (!src || !dst) return 0;
    if (src_size == 0) return 0;

    // Ring buffer window — zero-initialized, same as FreeFalcon.
    // FreeFalcon uses `unsigned char window[WINDOW_SIZE]` which is
    // zero-initialized because it's a struct member.
    unsigned char window[WINDOW_SIZE];
    std::memset(window, 0, WINDOW_SIZE);

    // current_position starts at 1 (FreeFalcon: current_position = 1).
    // Position 0 is the END_OF_STREAM sentinel and is never written.
    std::size_t current_position = 1;

    std::size_t src_pos = 0;
    std::size_t dst_pos = 0;

    while (dst_pos < dst_capacity && src_pos < src_size) {
        // Read flag byte — 8 bits, LSB first.
        // Bit = 1 → literal, Bit = 0 → match (opposite of typical LZSS!
        // FreeFalcon's InputBit returns nonzero for literal.)
        const uint8_t flag = src[src_pos++];

        for (int bit = 0; bit < 8 && dst_pos < dst_capacity; ++bit) {
            if (src_pos >= src_size) break;

            const bool is_literal = (flag & (1u << bit)) != 0;

            if (is_literal) {
                // ── Literal byte ───────────────────────────────────
                if (src_pos >= src_size) break;
                const uint8_t c = src[src_pos++];
                dst[dst_pos++] = c;
                window[current_position] = c;
                current_position = mod_window(current_position + 1);
            } else {
                // ── Match reference (2 bytes) ─────────────────────
                if (src_pos + 1 >= src_size) break;

                const uint8_t b0 = src[src_pos];
                const uint8_t b1 = src[src_pos + 1];
                src_pos += 2;

                // FreeFalcon decode (LZSS_Expand):
                //   match_length = b0
                //   match_position = b1
                //   match_position |= (match_length & 0xf) << 8
                //   match_length >>= 4
                //   match_length += BREAK_EVEN
                //
                // So: position = b1 | ((b0 & 0x0F) << 8)  → 12-bit
                //     length   = (b0 >> 4) + BREAK_EVEN    → 1..16
                const std::size_t match_position =
                    static_cast<std::size_t>(b1) |
                    (static_cast<std::size_t>(b0 & 0x0F) << 8);
                const int match_length =
                    (b0 >> 4) + BREAK_EVEN;

                // Copy match_length bytes from the window ring buffer.
                // FreeFalcon iterates i = 0..match_length (inclusive),
                // so it copies match_length+1 bytes. But match_length
                // already includes BREAK_EVEN (=1), so the actual copy
                // count is (b0>>4) + 1 bytes... wait, let me re-check.
                //
                // Actually, looking at FreeFalcon again:
                //   for (i = 0; i <= match_length; i++)
                // That's match_length+1 iterations (0..match_length inclusive).
                // And match_length = (b0>>4) + BREAK_EVEN = (b0>>4) + 1.
                // So total bytes copied = (b0>>4) + 2.
                //
                // Hmm, but that doesn't match the compression side.
                // OutputPair stores (length << 4), so the stored length
                // is the raw 4-bit value (0..15). On expand, (b0>>4)
                // recovers 0..15, then +BREAK_EVEN(=1) gives 1..16.
                // The loop `i <= match_length` copies match_length+1
                // = 2..17 bytes.
                //
                // Wait, that seems off by one. Let me look at the
                // compression side more carefully...
                //
                // Actually, in LZSS_Compress, the match_length is
                // found by FindMatch, and it represents the number of
                // matching bytes minus 1 (because BREAK_EVEN accounts
                // for the fact that a match must be longer than the
                // 2-byte token to be worth encoding). The loop
                // `i <= match_length` copies match_length+1 bytes,
                // which is the actual match length.
                //
                // For our purposes, we just replicate the exact same
                // loop: i = 0..match_length (inclusive).
                for (int i = 0; i <= match_length; ++i) {
                    if (dst_pos >= dst_capacity) break;
                    const uint8_t c = window[mod_window(match_position + i)];
                    dst[dst_pos++] = c;
                    window[current_position] = c;
                    current_position = mod_window(current_position + 1);
                }
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

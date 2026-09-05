// f4-lzss/src/compress.cpp
//
// LZSS compression — the exact inverse of f4::lzss::decompress().
//
// The decoder (lzss.cpp) is the ground truth: this file produces a stream
// that the decoder reads back to the identical original bytes. See the
// format block in lzss.hpp for the byte-level contract.
//
// Match strategy: greedy longest-match via a hash chain over 3-byte
// prefixes (the zlib pattern, scaled to this codec's 4096-byte window and
// 17-byte max match). Min match = 3 (a 2-byte match saves no space over
// two literals). Max match = 17 (the 4-bit length field copies 2..17
// bytes via the decoder's `i <= match_length` loop). Position 0 is never
// emitted (FreeFalcon reserves it as the EOS sentinel).
//
// This is NOT byte-identical to FreeFalcon's compressor — the match
// heuristics differ — but any valid LZSS stream decompresses to the same
// bytes, so FreeFalcon's decoder and ours both read it back correctly.

#include <f4/lzss/lzss.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace f4::lzss {

namespace {

// ── Codec constants (must match lzss.cpp's decompressor exactly) ──────────
constexpr int      INDEX_BIT_COUNT = 12;
constexpr int      LENGTH_BIT_COUNT = 4;
constexpr std::size_t WINDOW_SIZE   = 1u << INDEX_BIT_COUNT;        // 4096
constexpr int      BREAK_EVEN       = (1 + INDEX_BIT_COUNT + LENGTH_BIT_COUNT) / 9;  // 1

// Match-length bounds, derived from the decoder's copy loop. The decoder
// computes match_length = (b0 >> 4) + BREAK_EVEN and copies
// `i = 0..match_length` (inclusive) = match_length + 1 bytes. So a stored
// length_raw = (b0 >> 4) ∈ [0, 15] copies length_raw + 2 bytes ∈ [2, 17].
// We only emit matches of length >= MIN_MATCH (=3); a 2-byte match saves
// nothing over two literals and is left as literals.
constexpr int MIN_MATCH = 3;
constexpr int MAX_MATCH = 17;   // length_raw 15 → copies 17

// Match-position encoding:
//   position = b1 | ((b0 & 0x0F) << 8)   → 12-bit window index ∈ [0, 4095]
// The decoder reads window[(position + i) & 4095]. The byte at input
// position j lives at window slot (1 + j) & 4095 (current_position starts
// at 1), so a match sourcing input[j..] is encoded with
// position = (1 + j) & 4095. Position 0 is reserved (EOS); a source index
// j ≡ 4095 (mod 4096) would map to position 0 and is skipped.
//
// Maximum back-reference distance: the window holds the last 4096 bytes,
// so j must satisfy i - j <= 4095 (j can be i - 4095 at the furthest).
// (j = i - 4096 would map to the same window slot as j = i, which holds
// stale/identity data — excluded by the distance bound.)
constexpr std::size_t MAX_DISTANCE = WINDOW_SIZE - 1;   // 4095

// Hash chain (zlib-style). HASH_SIZE buckets over 3-byte prefixes.
constexpr int HASH_BITS = 15;
constexpr std::size_t HASH_SIZE = 1u << HASH_BITS;
constexpr std::size_t HASH_MASK = HASH_SIZE - 1;

inline std::size_t hash3(const uint8_t* p) {
    // Mix the three bytes into a 15-bit bucket. A multiplicative hash
    // spreads 3-byte prefixes more evenly than a direct truncate, which
    // matters for the .cmp payload's long runs of NUL padding (many
    // prefixes hash to 0x0000xx under a naive shift).
    const uint32_t v =
        (static_cast<uint32_t>(p[0]) << 16) |
        (static_cast<uint32_t>(p[1]) << 8)  |
         static_cast<uint32_t>(p[2]);
    return (v * 2654435761u) >> (32 - HASH_BITS) & HASH_MASK;
}

// Cap on chain walks per position — bounds worst-case time. 128 gives
// near-best compression on the ~22 KB .cmp payload while keeping the
// encode of a full campaign save well under a second.
constexpr int MAX_CHAIN = 128;

// Emit a (flag, 8 tokens) group when full, or the partial final group at
// flush. Tokens are buffered; the flag byte's bit N (LSB first) is 1 for
// a literal, 0 for a match. Unused trailing bits in the final group are
// left 0 (match); the decoder breaks on src exhaustion, so they read no
// token bytes (verified by test_lzss.cpp's partial-group cases).
struct TokenBlock {
    uint8_t flag = 0;                 // bit N = 1 → literal
    int bit = 0;                      // number of tokens buffered [0..8]
    std::vector<uint8_t> bytes;       // token payload, in order
};

} // namespace

std::vector<uint8_t> compress(const uint8_t* src, std::size_t src_size) {
    std::vector<uint8_t> out;
    if (!src || src_size == 0) return out;
    out.reserve(src_size / 2 + 16);   // rough; most payloads compress ~2:1

    // Hash chains: head[h] = most recent input position with hash h (-1 = none);
    // prev[pos] = previous position with the same hash (-1 = none).
    std::vector<std::ptrdiff_t> head(HASH_SIZE, -1);
    std::vector<std::ptrdiff_t> prev(src_size, -1);

    TokenBlock block;
    auto flush_block = [&]() {
        if (block.bit == 0) return;
        out.push_back(block.flag);
        out.insert(out.end(), block.bytes.begin(), block.bytes.end());
        block.flag = 0;
        block.bit = 0;
        block.bytes.clear();
    };
    auto emit_literal = [&](uint8_t c) {
        block.flag |= static_cast<uint8_t>(1u << block.bit);
        block.bytes.push_back(c);
        if (++block.bit == 8) flush_block();
    };
    auto emit_match = [&](std::size_t src_pos, int length) {
        // position = (1 + src_pos) & 4095; skip if it would be the EOS
        // sentinel (slot 0). The caller filters these, but guard anyway.
        std::size_t position = (1u + src_pos) & (WINDOW_SIZE - 1);
        if (position == 0) {
            // Fall back to a single literal for this byte.
            emit_literal(src[src_pos]);
            return;
        }
        const int length_raw = length - 2;             // [1..15]
        const uint8_t b0 = static_cast<uint8_t>(
            (length_raw << 4) | ((position >> 8) & 0x0F));
        const uint8_t b1 = static_cast<uint8_t>(position & 0xFF);
        block.bytes.push_back(b0);
        block.bytes.push_back(b1);
        // flag bit stays 0 for a match
        if (++block.bit == 8) flush_block();
    };

    std::size_t i = 0;
    while (i < src_size) {
        // Insert the current position's 3-byte prefix into the hash chain
        // (only when at least 3 bytes remain).
        auto insert_hash = [&](std::size_t pos) {
            if (pos + MIN_MATCH > src_size) return;   // need 3 bytes
            const std::size_t h = hash3(src + pos);
            prev[pos] = head[h];
            head[h] = static_cast<std::ptrdiff_t>(pos);
        };

        int best_len = 0;
        std::ptrdiff_t best_src = -1;

        if (i + MIN_MATCH <= src_size) {
            const std::size_t h = hash3(src + i);
            const int max_len = static_cast<int>(
                std::min<std::size_t>(MAX_MATCH, src_size - i));

            std::ptrdiff_t cand = head[h];
            int chain = MAX_CHAIN;
            while (cand >= 0 && chain-- > 0) {
                const std::ptrdiff_t cand_pos = cand;
                const std::size_t distance = i - static_cast<std::size_t>(cand_pos);
                if (distance > MAX_DISTANCE) break;

                // Skip candidates whose window slot would be position 0
                // (the EOS sentinel) — emit a literal for that byte only
                // if it ends up the best; other candidates are preferred.
                const std::size_t position =
                    (1u + static_cast<std::size_t>(cand_pos)) & (WINDOW_SIZE - 1);
                if (position != 0) {
                    // Measure the match length. Overlapping matches
                    // (cand_pos + k >= i) are valid LZSS — the decoder
                    // copies byte-by-byte, reading bytes it just wrote.
                    int len = 0;
                    while (len < max_len &&
                           src[static_cast<std::size_t>(cand_pos) + len] == src[i + len]) {
                        ++len;
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_src = cand_pos;
                        if (best_len >= max_len) break;   // can't do better
                    }
                }
                cand = prev[static_cast<std::size_t>(cand_pos)];
            }
        }

        if (best_len >= MIN_MATCH) {
            emit_match(static_cast<std::size_t>(best_src), best_len);
            // Insert hash entries for every position inside the match so
            // future finds can reference them (greedy: insert all, not
            // just the endpoints). This is the zlib lazy-insert trade-off
            // done simply.
            const std::size_t end = i + static_cast<std::size_t>(best_len);
            for (std::size_t p = i; p < end && p + MIN_MATCH <= src_size; ++p) {
                insert_hash(p);
            }
            i = end;
        } else {
            emit_literal(src[i]);
            insert_hash(i);
            ++i;
        }
    }
    flush_block();   // emit the partial final group
    return out;
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& src) {
    return compress(src.data(), src.size());
}

} // namespace f4::lzss

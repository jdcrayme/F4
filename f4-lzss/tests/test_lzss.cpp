// test_lzss.cpp — Unit tests for f4::lzss::decompress().
//
// These tests exercise the canonical LZSS decompressor directly, covering:
//   - Basic contract (null, empty, single-byte)
//   - All-literal streams
//   - Match references (back-references into the ring buffer)
//   - Ring buffer wrapping (>4096 bytes output)
//   - Malformed/truncated input (bounds safety)
//   - The vector-returning overload
//   - Round-trip consistency with the f4-world-convert adapter

#include <gtest/gtest.h>
#include <f4/lzss/lzss.hpp>

#include <cstring>
#include <vector>

using namespace f4::lzss;

// ═══════════════════════════════════════════════════════════════════════════
// Contract / edge-case tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, NullSrcReturnsZero) {
    uint8_t dst[1];
    EXPECT_EQ(decompress(nullptr, 10, dst, sizeof(dst)), 0u);
}

TEST(LzssDecompress, NullDstReturnsZero) {
    const uint8_t src[] = {0xFF, 'A'};
    EXPECT_EQ(decompress(src, sizeof(src), nullptr, 10), 0u);
}

TEST(LzssDecompress, ZeroSrcSizeReturnsZero) {
    uint8_t dst[1];
    const uint8_t src[] = {0x00};
    EXPECT_EQ(decompress(src, 0, dst, sizeof(dst)), 0u);
}

TEST(LzssDecompress, ZeroDstCapacityProducesZero) {
    const uint8_t src[] = {0xFF, 'A'};
    uint8_t dst[1];
    EXPECT_EQ(decompress(src, sizeof(src), dst, 0), 0u);
}

TEST(LzssDecompress, VectorOverloadNullReturnsEmpty) {
    auto result = decompress(nullptr, 10);
    EXPECT_TRUE(result.empty());
}

TEST(LzssDecompress, VectorOverloadZeroSizeReturnsEmpty) {
    const uint8_t src[] = {0x00};
    auto result = decompress(src, 0);
    EXPECT_TRUE(result.empty());
}

// ═══════════════════════════════════════════════════════════════════════════
// All-literal streams
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, SingleLiteral) {
    // flag=0x01 → bit0=1 (literal), bits1-7=0 (matches, but window is zeroed)
    // However matches from zeroed window produce \0 bytes, so let's use all-literal.
    // flag=0xFF → all 8 bits are literals
    const uint8_t in[] = {0xFF, 'A'};
    uint8_t out[8] = {};
    // Only 1 literal token (flag has 8 bits set but only 1 byte of input follows)
    // The loop reads flag=0xFF, bit0=1 (literal), reads 'A', then bits1-7
    // are also 1 (literal) but src_pos >= src_size so the inner loop breaks.
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 1u);
    EXPECT_EQ(out[0], 'A');
}

TEST(LzssDecompress, EightLiterals) {
    // flag=0xFF → 8 literals
    const uint8_t in[] = {0xFF, 'A','B','C','D','E','F','G','H'};
    uint8_t out[8] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 8u);
    EXPECT_EQ(std::memcmp(out, "ABCDEFGH", 8), 0);
}

TEST(LzssDecompress, TwoFlagBytesAllLiterals) {
    // Two groups of 8 literals = 16 bytes output
    const uint8_t in[] = {
        0xFF, 'A','B','C','D','E','F','G','H',
        0xFF, 'I','J','K','L','M','N','O','P'
    };
    uint8_t out[16] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 16u);
    EXPECT_EQ(std::memcmp(out, "ABCDEFGHIJKLMNOP", 16), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Match references
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, LiteralThenSingleMatch) {
    // flag=0b00000001: bit0=1 (literal 'X'), bit1=0 (match)
    // Match token: position=1 (where 'X' was written), length=(0>>4)+1=1
    //   → copies window[1..2] which is 'X','X' (match loop is i=0..1 inclusive = 2 bytes)
    // Wait: match_length = (b0>>4) + BREAK_EVEN. b0=0x00, so match_length=0+1=1.
    // Loop: for (i=0; i<=1; i++) → copies 2 bytes.
    const uint8_t in[] = {
        0x01,               // flag: bit0=1 (lit), bits1-7=0 (match)
        'X',                // literal
        0x00, 0x01          // match: position=1, length=1 → copies 2 bytes ('X','X')
    };
    uint8_t out[4] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    // 1 literal + 2 match bytes = 3 output bytes
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'X');
    EXPECT_EQ(out[1], 'X');
    EXPECT_EQ(out[2], 'X');
}

TEST(LzssDecompress, RepeatedPatternViaMatches) {
    // Build a stream: write 4 literals "ABCD", then use matches to repeat them.
    // flag byte 0x0F: bits0-3=1 (4 literals), bits4-7=0 (4 matches)
    const uint8_t in[] = {
        0x0F,                               // flag: bits0-3 literal, bits4-7 match
        'A', 'B', 'C', 'D',                 // 4 literals
        // match 1: position=1, length=(0>>4)+1=1 → copies window[1..2] = 'A','B'
        0x00, 0x01,
        // match 2: position=3, length=(0>>4)+1=1 → copies window[3..4] = 'C','D'
        0x00, 0x03,
        // match 3: position=1, length=1 → 'A','B'
        0x00, 0x01,
        // match 4: position=3, length=1 → 'C','D'
        0x00, 0x03
    };
    uint8_t out[12] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    // 4 literals + 4 matches * 2 bytes = 12 output
    EXPECT_EQ(n, 12u);
    EXPECT_EQ(std::memcmp(out, "ABCDABCDABCD", 12), 0);
}

TEST(LzssDecompress, MatchFromZeroedWindow) {
    // flag=0x00: all 8 bits are matches. Zeroed window → all output is \0.
    const uint8_t in[] = {
        0x00,
        0x00, 0x01,   // match: pos=1, len=1 → 2 bytes of \0
        0x00, 0x01,   // match: pos=1, len=1 → 2 bytes of \0
        0x00, 0x01,   // match: pos=1, len=1 → 2 bytes of \0
        0x00, 0x01,   // match: pos=1, len=1 → 2 bytes of \0
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01
    };
    uint8_t out[16] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 16u);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], 0) << "at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Longer match lengths (using the 4-bit length field)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, MatchWithMaxLength) {
    // Write a literal 'Z', then a match with maximum length.
    // length field = (b0 >> 4) + BREAK_EVEN. Max b0>>4 = 15, so max length = 16.
    // The loop copies match_length+1 = 17 bytes.
    // b0 = (length_raw << 4) | (position >> 8). With length_raw=15, pos=1:
    //   b0 = (15 << 4) | 0 = 0xF0
    //   b1 = 0x01
    const uint8_t in[] = {
        0x01,               // flag: bit0=1 (lit), bit1=0 (match)
        'Z',                // literal
        0xF0, 0x01          // match: position=1, length_raw=15 → length=16 → copies 17 'Z's
    };
    uint8_t out[32] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 18u);  // 1 literal + 17 match bytes
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], 'Z') << "at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Ring buffer wrapping (output > 4096 bytes)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, OutputExceedsWindowSize) {
    // Generate 4200 bytes of output using all-literal streams.
    // This exceeds the 4096-byte ring buffer, testing the MOD_WINDOW wrap.
    // 4200 / 8 = 525 flag bytes, each followed by 8 literals.
    std::vector<uint8_t> in;
    in.reserve(525 * 9);
    for (int i = 0; i < 525; ++i) {
        in.push_back(0xFF);  // flag: 8 literals
        for (int j = 0; j < 8; ++j) {
            in.push_back(static_cast<uint8_t>((i * 8 + j) & 0xFF));
        }
    }
    std::vector<uint8_t> out(4200);
    auto n = decompress(in.data(), in.size(), out.data(), out.size());
    EXPECT_EQ(n, 4200u);
    // Verify the pattern
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], static_cast<uint8_t>(i & 0xFF)) << "at index " << i;
    }
}

TEST(LzssDecompress, MatchCrossesWindowBoundary) {
    // Write 4096+2 bytes using literals, then match back to near the boundary.
    // This tests that the ring buffer correctly wraps when reading matches
    // that cross the 4096-byte boundary.
    std::vector<uint8_t> in;
    // First: 512 groups of 8 literals = 4096 bytes of 'Q'
    for (int i = 0; i < 512; ++i) {
        in.push_back(0xFF);
        for (int j = 0; j < 8; ++j) in.push_back('Q');
    }
    // Now a group with 6 more literals + 1 match that references position 1
    // flag = 0b00111111 = 0x3F: bits0-5 literal, bit6 match, bit7 match
    // Actually let's do: 6 literals ('Q'), then 1 match back to position 1
    in.push_back(0x3F);  // bits0-5=1 (6 lits), bit6=0 (match), bit7=0 (match)
    for (int j = 0; j < 6; ++j) in.push_back('Q');
    // Match: position=1, length=1 → copies 2 'Q's
    in.push_back(0x00); in.push_back(0x01);
    // Second match: position=1, length=1 → copies 2 'Q's
    in.push_back(0x00); in.push_back(0x01);

    std::vector<uint8_t> out(4106);
    auto n = decompress(in.data(), in.size(), out.data(), out.size());
    EXPECT_EQ(n, 4106u);  // 4096 + 6 + 2 + 2
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], 'Q') << "at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Truncated / malformed input (bounds safety)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, TruncatedMidLiteralGroup) {
    // Flag byte says 8 literals but only 3 bytes follow
    const uint8_t in[] = {0xFF, 'A', 'B', 'C'};
    uint8_t out[8] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    // Should produce the 3 literals it could read, then stop
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'A');
    EXPECT_EQ(out[1], 'B');
    EXPECT_EQ(out[2], 'C');
}

TEST(LzssDecompress, TruncatedMidMatchToken) {
    // Flag byte says match, but only 1 byte of the 2-byte match token follows
    const uint8_t in[] = {0x00, 0x00};  // flag=0 (all matches), only 1 match byte
    uint8_t out[16] = {};
    // Should produce 0 output — can't read a complete match token
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    // The inner loop checks src_pos + 1 >= src_size, so it breaks before reading
    EXPECT_EQ(n, 0u);
}

TEST(LzssDecompress, DstCapacityLimitsOutput) {
    // 8 literals but dst_capacity = 3
    const uint8_t in[] = {0xFF, 'A','B','C','D','E','F','G','H'};
    uint8_t out[3] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'A');
    EXPECT_EQ(out[1], 'B');
    EXPECT_EQ(out[2], 'C');
}

TEST(LzssDecompress, EmptyCompressedDataProducesZeroOutput) {
    const uint8_t in[] = {0xFF};  // flag byte only, no token bytes
    uint8_t out[8] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    // Flag says 8 literals but src_pos >= src_size immediately
    EXPECT_EQ(n, 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Vector-returning overload
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, VectorOverloadEightLiterals) {
    const uint8_t in[] = {0xFF, 'A','B','C','D','E','F','G','H'};
    auto result = decompress(in, sizeof(in), 8);
    ASSERT_EQ(result.size(), 8u);
    EXPECT_EQ(std::memcmp(result.data(), "ABCDEFGH", 8), 0);
}

TEST(LzssDecompress, VectorOverloadWithExpectedSize) {
    const uint8_t in[] = {0xFF, 'A','B','C','D','E','F','G','H'};
    // Provide expected_size hint
    auto result = decompress(in, sizeof(in), 8);
    ASSERT_EQ(result.size(), 8u);
    EXPECT_EQ(std::memcmp(result.data(), "ABCDEFGH", 8), 0);
}

TEST(LzssDecompress, VectorOverloadWithoutExpectedSize) {
    const uint8_t in[] = {0xFF, 'A','B','C','D','E','F','G','H'};
    // expected_size=0 → estimate 2× compression ratio
    auto result = decompress(in, sizeof(in), 0);
    ASSERT_EQ(result.size(), 8u);
    EXPECT_EQ(std::memcmp(result.data(), "ABCDEFGH", 8), 0);
}

TEST(LzssDecompress, VectorOverloadMatchFromZeroedWindow) {
    const uint8_t in[] = {
        0x00,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x01
    };
    auto result = decompress(in, sizeof(in), 16);
    ASSERT_EQ(result.size(), 16u);
    for (auto b : result) EXPECT_EQ(b, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Specific position encoding tests (12-bit position field)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, MatchWithHighPosition) {
    // Write enough literals to fill position > 255, then match back.
    // First write 300 literals (38 groups of 8 = 304, but we only need 300).
    // Actually let's write exactly 256 literals (32 groups) then match to pos=256.
    std::vector<uint8_t> in;
    for (int i = 0; i < 32; ++i) {
        in.push_back(0xFF);
        for (int j = 0; j < 8; ++j) in.push_back('R');
    }
    // Now 1 match: position=256. Encoding: position=256=0x100
    //   b0 = (length_raw << 4) | (position >> 8) = (0 << 4) | 1 = 0x01
    //   b1 = position & 0xFF = 0x00
    // flag = 0x00 (all matches) but we only need 1 match, so:
    // flag = 0b11111110 = 0xFE: bits0-6=1 (7 dummy lits), bit7=0 (match)
    // Wait, that's wasteful. Let's use a group with just 1 match.
    // flag = 0b11111110 = 0xFE: bit0=0 (match), bits1-7=1 (7 lits that won't be read)
    // Actually the flag processes bits LSB first. bit0=0 means match first.
    in.push_back(0xFE);  // bit0=0 (match), bits1-7=1 (literals, but we'll truncate)
    in.push_back(0x01);  // b0: length_raw=0, position high nibble=1
    in.push_back(0x00);  // b1: position low byte=0 → position=0x100=256
    // The 7 remaining literal bits would need source data, but we'll just stop.
    // The decompressor will break when src_pos >= src_size.

    std::vector<uint8_t> out(260);
    auto n = decompress(in.data(), in.size(), out.data(), out.size());
    // 256 literals + 2 match bytes (from pos 256, length=1 → copies 2 bytes)
    EXPECT_EQ(n, 258u);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(out[i], 'R') << "at index " << i;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Consistency: buffer overload vs vector overload produce same output
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, BufferAndVectorOverloadsAgree) {
    const uint8_t in[] = {
        0x0F,                               // 4 lits, 4 matches
        'W', 'X', 'Y', 'Z',                 // 4 literals
        0x00, 0x01,                         // match pos=1 len=1 → 'W','X'
        0x00, 0x03,                         // match pos=3 len=1 → 'Y','Z'
        0x00, 0x01,                         // match pos=1 → 'W','X'
        0x00, 0x03                          // match pos=3 → 'Y','Z'
    };
    // Buffer overload
    uint8_t buf[12] = {};
    auto n = decompress(in, sizeof(in), buf, sizeof(buf));
    ASSERT_EQ(n, 12u);

    // Vector overload
    auto vec = decompress(in, sizeof(in), 12);
    ASSERT_EQ(vec.size(), 12u);

    EXPECT_EQ(std::memcmp(buf, vec.data(), 12), 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// FreeFalcon-specific: current_position starts at 1 (not 0)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssDecompress, MatchPositionOneReferencesFirstLiteral) {
    // In FreeFalcon's LZSS, current_position starts at 1.
    // The first literal is written to window[1]. A match at position=1
    // should reference that first literal.
    const uint8_t in[] = {
        0x01,               // flag: bit0=1 (lit), bit1=0 (match)
        'M',                // literal → written to window[1]
        0x00, 0x01          // match: position=1, length=1 → copies window[1..2]
    };
    uint8_t out[4] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'M');
    EXPECT_EQ(out[1], 'M');
    EXPECT_EQ(out[2], 'M');
}

TEST(LzssDecompress, MatchPositionZeroReferencesEndOfStreamSentinel) {
    // Position 0 is the END_OF_STREAM sentinel in FreeFalcon's LZSS.
    // The window at position 0 is zero-initialized and never written.
    // A match at position=0 should copy zero bytes.
    const uint8_t in[] = {
        0x01,               // flag: bit0=1 (lit), bit1=0 (match)
        'Z',                // literal
        0x00, 0x00          // match: position=0, length=1 → copies window[0..1]
    };
    uint8_t out[4] = {};
    auto n = decompress(in, sizeof(in), out, sizeof(out));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 'Z');
    // window[0] = 0 (sentinel), window[1] = 'Z'
    EXPECT_EQ(out[1], 0);   // copied from window[0]
    EXPECT_EQ(out[2], 'Z'); // copied from window[1]
}

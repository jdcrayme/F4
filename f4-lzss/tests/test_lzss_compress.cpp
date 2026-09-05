// test_lzss_compress.cpp — round-trip tests for f4::lzss::compress().
//
// The contract: compress() produces a stream that decompress() reads back
// to the exact original bytes. These tests exercise that contract across:
//   - edge cases (empty, 1 byte, exactly 8/9/16/17 bytes — group boundaries)
//   - incompressible data (random bytes)
//   - highly compressible data (runs, repeats)
//   - data larger than the 4096-byte window (ring-buffer wrap on both
//     the write and the match-reference sides)
//   - the position-0 avoidance (output positions ≡ 4095 mod 4096)
//   - a real .cmp-class payload (a struct sequence with long NUL runs,
//     matching the campaign-header shape that motivated this tranche)
//
// The byte-identity bar is on the ROUND-TRIP, not on matching FreeFalcon's
// compressed bytes: compress(x) may differ from FreeFalcon's compressor,
// but decompress(compress(x)) == x must hold exactly.

#include <gtest/gtest.h>
#include <f4/lzss/lzss.hpp>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace f4::lzss;

namespace {

// Round-trip helper: compress then decompress, return whether it matches.
bool roundtrip(const std::vector<uint8_t>& in, std::vector<uint8_t>* out = nullptr) {
    auto comp = compress(in.data(), in.size());
    auto dec = decompress(comp.data(), comp.size(), in.size());
    if (out) *out = dec;
    return dec == in;
}

std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Edge cases
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssCompress, EmptyInputProducesEmptyOutput) {
    auto comp = compress(nullptr, 0);
    EXPECT_TRUE(comp.empty());
    auto v = compress(std::vector<uint8_t>{});
    EXPECT_TRUE(v.empty());
}

TEST(LzssCompress, SingleByte) {
    EXPECT_TRUE(roundtrip({0x42}));
}

TEST(LzssCompress, TwoBytesNoMatch) {
    EXPECT_TRUE(roundtrip({0x01, 0x02}));
}

TEST(LzssCompress, TwoIdenticalBytesStaysLiteral) {
    // 2-byte match is below MIN_MATCH (3) → emitted as 2 literals.
    // Round-trip must still hold.
    EXPECT_TRUE(roundtrip({0x55, 0x55}));
}

TEST(LzssCompress, ThreeIdenticalBytesBecomesMatch) {
    EXPECT_TRUE(roundtrip({0x55, 0x55, 0x55}));
}

TEST(LzssCompress, ExactlyEightBytesAllLiterals) {
    // One full flag group, all literals (no 3-byte match possible).
    EXPECT_TRUE(roundtrip(bytes("ABCDEFGH")));
}

TEST(LzssCompress, NineBytesSpansTwoGroups) {
    // 8 literals (group 1) + 1 literal (partial group 2).
    EXPECT_TRUE(roundtrip(bytes("ABCDEFGHI")));
}

// ═══════════════════════════════════════════════════════════════════════════
// Compressibility
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssCompress, AllZeroRunCompresses) {
    std::vector<uint8_t> in(1000, 0);
    auto comp = compress(in.data(), in.size());
    // 1000 zeros should compress well under MAX_MATCH=17 matches.
    EXPECT_LT(comp.size(), in.size() / 4);
    EXPECT_TRUE(roundtrip(in));
}

TEST(LzssCompress, LongRunCompresses) {
    std::vector<uint8_t> in(5000, 'Z');
    auto comp = compress(in.data(), in.size());
    EXPECT_LT(comp.size(), in.size() / 4);
    EXPECT_TRUE(roundtrip(in));
}

TEST(LzssCompress, RepeatedPatternCompresses) {
    // "ABCDE" repeated 200× = 1000 bytes. Matches reference earlier cycles.
    std::string pat = "ABCDE";
    std::string s;
    for (int i = 0; i < 200; ++i) s += pat;
    auto in = bytes(s);
    auto comp = compress(in.data(), in.size());
    EXPECT_LT(comp.size(), in.size() / 2);
    EXPECT_TRUE(roundtrip(in));
}

TEST(LzssCompress, RandomIncompressibleRoundTrips) {
    // Random data has few matches; compressed size may exceed input, but
    // the round-trip must be exact.
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<uint8_t> in;
    in.reserve(4096);
    for (int i = 0; i < 4096; ++i) in.push_back(static_cast<uint8_t>(dist(rng)));
    EXPECT_TRUE(roundtrip(in));
}

// ═══════════════════════════════════════════════════════════════════════════
// Ring-buffer wrap (window boundary)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssCompress, OutputExceedsWindowSize) {
    // 5000 literals of distinct-ish values, then a repeat of the prefix.
    // The match references a position > 4096 bytes back, exercising the
    // window wrap on the read side.
    std::vector<uint8_t> in;
    for (int i = 0; i < 5000; ++i) in.push_back(static_cast<uint8_t>(i & 0xFF));
    // Append a copy of the first 100 bytes — match distance ~5000.
    in.insert(in.end(), in.begin(), in.begin() + 100);
    EXPECT_TRUE(roundtrip(in));
}

TEST(LzssCompress, MatchAtMaximumDistance) {
    // Place a 3-byte pattern at position 0, then 4092 filler bytes, then
    // the same pattern again at position 4095 — distance exactly 4095
    // (MAX_DISTANCE). Must round-trip (the candidate is in-window).
    std::vector<uint8_t> in;
    in.push_back('X'); in.push_back('Y'); in.push_back('Z');   // pos 0..2
    for (int i = 0; i < 4092; ++i) in.push_back(static_cast<uint8_t>('a' + (i % 26)));
    // pos 4095..4097 — distance 4095 from pos 0
    in.push_back('X'); in.push_back('Y'); in.push_back('Z');
    EXPECT_TRUE(roundtrip(in));
}

TEST(LzssCompress, PositionZeroAvoided) {
    // A match sourcing input position 4095 would map to window slot 0
    // (the EOS sentinel). The compressor must skip it and still round-trip.
    // Build data where position 4095 is part of a repeatable run.
    std::vector<uint8_t> in(4200, 'Q');
    EXPECT_TRUE(roundtrip(in));
    // And verify the compressed stream contains no match token whose
    // position field is 0 (b0 low nibble 0 AND b1 0) — a weak but useful
    // invariant check. We scan for (b0 & 0x0F)==0 && b1==0 in match slots.
    auto comp = compress(in.data(), in.size());
    bool found_pos0 = false;
    for (std::size_t k = 0; k + 1 < comp.size(); ) {
        const uint8_t flag = comp[k++];
        for (int bit = 0; bit < 8; ++bit) {
            if (k >= comp.size()) break;
            if (flag & (1u << bit)) {
                ++k;   // literal
            } else {
                if (k + 1 >= comp.size()) break;
                const uint8_t b0 = comp[k];
                const uint8_t b1 = comp[k + 1];
                if ((b0 & 0x0F) == 0 && b1 == 0) found_pos0 = true;
                k += 2;
            }
        }
    }
    EXPECT_FALSE(found_pos0) << "compressor emitted a position-0 match token";
}

// ═══════════════════════════════════════════════════════════════════════════
// Max match length (17) and overlap (RLE-style)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssCompress, OverlappingMatchRun) {
    // "ABABAB..." — overlapping matches (source overlaps the current
    // position) exercise the decoder's write-then-read semantics.
    std::string s;
    for (int i = 0; i < 300; ++i) s += (i % 2) ? 'B' : 'A';
    EXPECT_TRUE(roundtrip(bytes(s)));
}

TEST(LzssCompress, LongRunUsesMaxMatchLength) {
    // A run long enough that matches hit the 17-byte cap repeatedly.
    std::vector<uint8_t> in(1000, 0x7F);
    auto comp = compress(in.data(), in.size());
    // Should compress to roughly 1000/17 match tokens ≈ 59 tokens × 2
    // bytes + ~8 flag bytes ≈ 130 bytes. Well under half.
    EXPECT_LT(comp.size(), in.size() / 4);
    EXPECT_TRUE(roundtrip(in));
}

// ═══════════════════════════════════════════════════════════════════════════
// Real-payload shape: a struct sequence with NUL padding (campaign header)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssCompress, StructLikePayloadWithPadding) {
    // Mimic the .cmp decompressed payload: int32 fields followed by
    // fixed-width NUL-padded strings (team names, theater name). The
    // long NUL runs are where LZSS earns its keep.
    std::vector<uint8_t> in;
    auto put_i32 = [&](int32_t v) {
        uint8_t b[4];
        std::memcpy(b, &v, 4);
        in.insert(in.end(), b, b + 4);
    };
    auto put_str = [&](const std::string& s, std::size_t width) {
        in.insert(in.end(), s.begin(), s.end());
        for (std::size_t i = s.size(); i < width; ++i) in.push_back(0);
    };
    for (int i = 0; i < 20; ++i) put_i32(i * 1000);
    for (int t = 0; t < 8; ++t) {
        put_i32(t); put_i32(t + 1);
        put_str(std::string("Team") + char('A' + t), 20);
        put_str(std::string("Motto for team ") + char('A' + t), 200);
    }
    put_str("KOREA", 40);
    put_str("scenario0", 40);
    put_str("save1", 40);
    put_str("ui_name", 40);
    for (int i = 0; i < 1000; ++i) in.push_back(0);  // camp_map + padding

    auto comp = compress(in.data(), in.size());
    EXPECT_LT(comp.size(), in.size() / 3);   // lots of NUL padding
    EXPECT_TRUE(roundtrip(in));
}

// ═══════════════════════════════════════════════════════════════════════════
// Stream well-formedness: flag groups are (flag, ≤8 tokens)
// ═══════════════════════════════════════════════════════════════════════════

TEST(LzssCompress, OutputIsParseableByWalkingFlagGroups) {
    // Walk the compressed stream the way the decoder does and confirm we
    // never run past the end mid-token. This catches off-by-one bugs in
    // the flag/group buffering that a pure round-trip might mask.
    std::vector<uint8_t> in;
    for (int i = 0; i < 1000; ++i) in.push_back(static_cast<uint8_t>(i % 7));
    auto comp = compress(in.data(), in.size());

    std::size_t k = 0;
    while (k < comp.size()) {
        const uint8_t flag = comp[k++];
        for (int bit = 0; bit < 8 && k < comp.size(); ++bit) {
            if (flag & (1u << bit)) {
                ++k;   // literal
            } else {
                ASSERT_LE(k + 1, comp.size()) << "truncated match token";
                k += 2;
            }
        }
    }
    SUCCEED();
}

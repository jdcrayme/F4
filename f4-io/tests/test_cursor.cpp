// test_cursor.cpp — unit tests for f4::io::Cursor.
//
// Coverage:
//   * Each typed reader returns the correct little-endian value.
//   * OOB read sets the sticky `error` flag and does not throw.
//   * Once `error` is set, further reads are no-ops.
//   * fixed_string() trims trailing NULs and respects the fixed width.
//   * remaining() / eof() reflect cursor position.
//   * Both constructors (pointer pair + vector) produce equivalent cursors.
//   * skip() advances p; skip() past end sets error.
//   * read_bytes() is a working alias for read().
//   * i8/i16/i32 are aliases for s8/s16/s32.

#include <gtest/gtest.h>

#include <f4/io/cursor.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using f4::io::Cursor;

namespace {

// Build a cursor over a small known byte pattern.
//   bytes 0..3   = 0x01 0x02 0x03 0x04  -> u32 LE = 0x04030201
//   bytes 4..5   = 0xFF 0x7F            -> u16 LE = 0x7FFF
//   bytes 6..7   = 0x80 0x00            -> u16 LE = 0x0080 (s16 = +128)
//   byte  8      = 0xAB                 -> u8 = 0xAB (s8 = -85)
//   bytes 9..12  = 0x00 0x00 0x80 0x3F  -> f32 LE = 1.0f
std::vector<uint8_t> make_pattern() {
    return {0x01, 0x02, 0x03, 0x04,
            0xFF, 0x7F,
            0x80, 0x00,
            0xAB,
            0x00, 0x00, 0x80, 0x3F};
}

} // namespace

TEST(Cursor, U32ReadsLittleEndian) {
    auto buf = make_pattern();
    Cursor c(buf);
    EXPECT_EQ(c.u32(), 0x04030201u);
    EXPECT_FALSE(c.error);
}

TEST(Cursor, S32ReadsLittleEndian) {
    // Bytes 0x01 0x02 0x03 0x84 -> u32 LE = 0x84030201, s32 LE = negative.
    std::vector<uint8_t> buf = {0x01, 0x02, 0x03, 0x84};
    Cursor c1(buf);
    EXPECT_EQ(c1.s32(), static_cast<int32_t>(0x84030201u));
    Cursor c2(buf);
    EXPECT_EQ(c2.u32(), 0x84030201u);
}

TEST(Cursor, U16ReadsLittleEndian) {
    std::vector<uint8_t> buf = {0xFF, 0x7F, 0x80, 0x00};
    Cursor c(buf);
    EXPECT_EQ(c.u16(), 0x7FFFu);  // bytes 0..1
    EXPECT_EQ(c.u16(), 0x0080u);  // bytes 2..3
    EXPECT_FALSE(c.error);
}

TEST(Cursor, S16ReadsLittleEndian) {
    // 0x80 0x00 -> s16 LE = +128
    // 0x00 0x80 -> s16 LE = -32768
    std::vector<uint8_t> buf = {0x80, 0x00, 0x00, 0x80};
    Cursor c(buf);
    EXPECT_EQ(c.s16(), 128);
    EXPECT_EQ(c.s16(), -32768);
}

TEST(Cursor, U8AndS8ReadSingleByte) {
    std::vector<uint8_t> buf = {0xAB, 0x7F};
    Cursor c(buf);
    EXPECT_EQ(c.u8(), 0xABu);
    EXPECT_EQ(c.s8(), 127);   // 0x7F as int8_t
    EXPECT_FALSE(c.error);
}

TEST(Cursor, F32ReadsLittleEndianOne) {
    auto buf = make_pattern();
    Cursor c(buf);
    c.skip(9);  // bytes 9..12 = 1.0f LE
    EXPECT_FLOAT_EQ(c.f32(), 1.0f);
    EXPECT_FALSE(c.error);
}

TEST(Cursor, I16AndS16AreAliases) {
    auto buf = make_pattern();
    Cursor c1(buf);
    Cursor c2(buf);
    EXPECT_EQ(c1.i16(), c2.s16());
}

TEST(Cursor, I32AndS32AreAliases) {
    auto buf = make_pattern();
    Cursor c1(buf);
    Cursor c2(buf);
    EXPECT_EQ(c1.i32(), c2.s32());
}

TEST(Cursor, I8AndS8AreAliases) {
    auto buf = make_pattern();
    Cursor c1(buf);
    Cursor c2(buf);
    EXPECT_EQ(c1.i8(), c2.s8());
}

TEST(Cursor, OobSetsErrorAndDoesNotThrow) {
    std::vector<uint8_t> buf = {1, 2, 3};
    Cursor c(buf);
    EXPECT_NO_THROW({
        uint8_t v = c.u8();
        EXPECT_EQ(v, 1);
    });
    EXPECT_NO_THROW({
        uint8_t v = c.u8();
        EXPECT_EQ(v, 2);
    });
    EXPECT_NO_THROW({
        uint8_t v = c.u8();
        EXPECT_EQ(v, 3);
    });
    EXPECT_FALSE(c.error);
    // 4th read: OOB.
    EXPECT_NO_THROW({
        uint8_t v = c.u8();
        EXPECT_EQ(v, 0);   // no-op returns 0
    });
    EXPECT_TRUE(c.error);
}

TEST(Cursor, StickyErrorMakesSubsequentReadsNoOps) {
    auto buf = make_pattern();
    Cursor c(buf);
    c.skip(buf.size() + 1);  // sets error, doesn't advance p
    EXPECT_TRUE(c.error);
    EXPECT_EQ(c.p, buf.data());  // p unchanged
    // Subsequent reads are no-ops, returning 0.
    EXPECT_EQ(c.u8(), 0);
    EXPECT_EQ(c.u32(), 0u);
    EXPECT_FLOAT_EQ(c.f32(), 0.0f);
    // p still unchanged.
    EXPECT_EQ(c.p, buf.data());
}

TEST(Cursor, RemainingAndEof) {
    auto buf = make_pattern();
    Cursor c(buf);
    EXPECT_EQ(c.remaining(), buf.size());
    EXPECT_FALSE(c.eof());
    c.skip(buf.size());
    EXPECT_EQ(c.remaining(), 0u);
    EXPECT_TRUE(c.eof());
    EXPECT_FALSE(c.error);  // skip-to-end is not OOB
}

TEST(Cursor, SkipOobSetsError) {
    auto buf = make_pattern();
    Cursor c(buf);
    c.skip(buf.size() + 1);
    EXPECT_TRUE(c.error);
}

TEST(Cursor, FixedStringTrimsTrailingNuls) {
    // 8-byte field: "Hi\0\0\0\0\0\0" -> should return "Hi"
    std::vector<uint8_t> buf = {'H', 'i', 0, 0, 0, 0, 0, 0};
    Cursor c(buf);
    std::string s = c.fixed_string(8);
    EXPECT_EQ(s, "Hi");
    EXPECT_EQ(c.remaining(), 0u);
    EXPECT_FALSE(c.error);
}

TEST(Cursor, FixedStringFullWidthWhenNoNul) {
    std::vector<uint8_t> buf = {'A', 'B', 'C'};
    Cursor c(buf);
    EXPECT_EQ(c.fixed_string(3), "ABC");
    EXPECT_FALSE(c.error);
}

TEST(Cursor, FixedStringOobSetsError) {
    auto buf = make_pattern();
    Cursor c(buf);
    c.fixed_string(buf.size() + 1);
    EXPECT_TRUE(c.error);
}

TEST(Cursor, ReadBytesIsAliasForRead) {
    auto buf = make_pattern();
    Cursor c(buf);
    uint8_t dst[4] = {0, 0, 0, 0};
    c.read_bytes(dst, 4);
    EXPECT_EQ(dst[0], 0x01);
    EXPECT_EQ(dst[1], 0x02);
    EXPECT_EQ(dst[2], 0x03);
    EXPECT_EQ(dst[3], 0x04);
    EXPECT_FALSE(c.error);
    EXPECT_EQ(c.remaining(), buf.size() - 4);
}

TEST(Cursor, ReadOobSetsErrorWithoutWriting) {
    auto buf = make_pattern();
    Cursor c(buf);
    uint8_t dst[100];
    std::memset(dst, 0xFF, sizeof(dst));
    c.read_bytes(dst, buf.size() + 1);
    EXPECT_TRUE(c.error);
    // dst should NOT have been written to.
    EXPECT_EQ(dst[0], 0xFF);
}

TEST(Cursor, PointerPairConstructorMatchesVectorConstructor) {
    auto buf = make_pattern();
    Cursor c_vec(buf);
    Cursor c_ptr(buf.data(), buf.data() + buf.size());
    EXPECT_EQ(c_vec.p, c_ptr.p);
    EXPECT_EQ(c_vec.end, c_ptr.end);
    EXPECT_FALSE(c_vec.error);
    EXPECT_FALSE(c_ptr.error);
    // Both should read the same first byte.
    EXPECT_EQ(c_vec.u8(), c_ptr.u8());
}

TEST(Cursor, CopyConstructorPreservesState) {
    auto buf = make_pattern();
    Cursor c(buf);
    c.skip(3);
    Cursor copy = c;
    EXPECT_EQ(copy.p, c.p);
    EXPECT_EQ(copy.end, c.end);
    EXPECT_FALSE(copy.error);
    // Advance the copy; the original is unchanged.
    copy.u8();
    EXPECT_NE(copy.p, c.p);

    // Now error path: copy an errored cursor.
    c.skip(buf.size() + 1);
    EXPECT_TRUE(c.error);
    Cursor err_copy = c;
    EXPECT_TRUE(err_copy.error);
}

TEST(Cursor, DefaultConstructorIsEmpty) {
    Cursor c;
    EXPECT_EQ(c.p, nullptr);
    EXPECT_EQ(c.end, nullptr);
    EXPECT_FALSE(c.error);
    EXPECT_TRUE(c.eof());
    EXPECT_EQ(c.remaining(), 0u);
}

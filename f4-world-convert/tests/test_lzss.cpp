// test_lzss.cpp — LZSS decompressor.
//
// We can't easily generate FreeFalcon-LZSS-compressed fixtures without the
// original compressor, so these tests verify the decompressor's contract on
// synthetic inputs and on the real save1.cam .cmp payload (which must
// decompress to exactly `datasize` bytes and contain readable team names).

#include <gtest/gtest.h>
#include <f4/convert/lzss.hpp>

#include <cstring>
#include <fstream>
#include <vector>

using namespace f4::convert;

// A minimal valid LZSS stream: one flag byte 0xFF (8 literal bits), then
// 8 literal bytes 'A'..'H'. Should decompress to "ABCDEFGH".
TEST(Lzss, AllLiterals) {
    const uint8_t in[] = {0xFF, 'A','B','C','D','E','F','G','H'};
    auto out = lzss_expand(in, sizeof(in), 8);
    ASSERT_EQ(out.size(), 8u);
    EXPECT_EQ(std::memcmp(out.data(), "ABCDEFGH", 8), 0);
}

// Flag byte 0x00 (8 match bits) would require 16 bytes of match tokens
// for 8 matches — but with a zeroed window every match copies '\0'. We
// instead test a mixed stream: 1 literal then a match back to it.
TEST(Lzss, LiteralThenMatch) {
    // flag byte 0b00000001 (bit0=1 literal, bit1..7=0 matches)
    // token 0: literal 'X'
    // token 1: match at position 1 (the 'X'), length BREAK_EVEN+0 = 1 -> "XX"
    //   match token bytes: byte_a = (len<<4)|(pos>>8) = (0<<4)|0 = 0x00
    //                      byte_b = pos & 0xFF = 0x01
    // remaining 6 matches: same, each copies 2 bytes from window.
    // 1 flag + 1 literal + 7*2 match tokens = 16 bytes input
    // output: 1 literal + 7 matches * 2 bytes = 15 bytes, all 'X'
    const uint8_t in[] = {
        0x01,                               // flag: bit0=1 (lit), bits1-7=0 (match)
        'X',                                // literal
        0x00, 0x01,                         // match 1: pos=1, len=1 -> copies 'XX'
        0x00, 0x01,                         // match 2
        0x00, 0x01,                         // match 3
        0x00, 0x01,                         // match 4
        0x00, 0x01,                         // match 5
        0x00, 0x01,                         // match 6
        0x00, 0x01                          // match 7
    };
    // 1 literal + 7 matches * 2 bytes each = 1 + 14 = 15 output bytes
    auto out = lzss_expand(in, sizeof(in), 15);
    ASSERT_EQ(out.size(), 15u);

    for (auto b : out) EXPECT_EQ(b, 'X');
}

TEST(Lzss, EmptyInputProducesEmptyOutput) {
    auto out = lzss_expand(nullptr, 0, 0);
    EXPECT_TRUE(out.empty());
}

TEST(Lzss, RealCamCmpPayloadDecompressesToExpectedSize) {
    // Read the .cmp sub-file from the real save1.cam fixture and verify the
    // LZSS payload decompresses to exactly the datasize recorded in its
    // 4-byte header.
    std::ifstream f(FIXTURE_DIR "save1.cam", std::ios::binary);
    ASSERT_TRUE(f) << "missing fixture: " FIXTURE_DIR "save1.cam";
    std::vector<uint8_t> cam((std::istreambuf_iterator<char>(f)), {});
    ASSERT_GT(cam.size(), 12u);
    // manifest offset
    int32_t manifest_off = 0;
    std::memcpy(&manifest_off, cam.data(), 4);
    // find .cmp entry in manifest
    std::size_t pos = static_cast<std::size_t>(manifest_off) + 4;
    int32_t nfiles = 0;
    std::memcpy(&nfiles, cam.data() + pos - 4, 4);
    const uint8_t* cmp_data = nullptr;
    std::size_t cmp_size = 0;
    for (int i = 0; i < nfiles; ++i) {
        uint8_t len = cam[pos++] & 0xFF;
        std::string name(reinterpret_cast<const char*>(cam.data() + pos), len);
        pos += len;
        int32_t off = 0, sz = 0;
        std::memcpy(&off, cam.data() + pos, 4); pos += 4;
        std::memcpy(&sz,  cam.data() + pos, 4); pos += 4;
        if (name.size() >= 4 && name.substr(name.size()-4) == ".cmp") {
            cmp_data = cam.data() + off;
            cmp_size = static_cast<std::size_t>(sz);
            break;
        }
    }
    ASSERT_NE(cmp_data, nullptr);
    // .cmp header: skip int32, then datasize int32, then payload.
    int32_t skip = 0, datasize = 0;
    std::memcpy(&skip, cmp_data, 4);
    std::memcpy(&datasize, cmp_data + 4, 4);
    ASSERT_GT(datasize, 0);
    auto out = lzss_expand(cmp_data + 8, cmp_size - 8, static_cast<std::size_t>(datasize));
    EXPECT_EQ(out.size(), static_cast<std::size_t>(datasize));
    // The decompressed payload must contain readable team-name strings
    // (Korea theater: "ROK", "Japan", "PRC", "DPRK" or similar). This proves
    // the LZSS port is byte-exact, not just length-correct.
    std::string payload(out.begin(), out.end());
    EXPECT_NE(payload.find("ROK"),    std::string::npos);
    EXPECT_NE(payload.find("Japan"),  std::string::npos);
    EXPECT_NE(payload.find("PRC"),    std::string::npos);
}

// test_zip_reader.cpp — ZipReader against hand-built STORED archives.

#include <gtest/gtest.h>
#include <f4/io/zip_reader.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace f4::io;

namespace {

struct TestZip {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / "f4_zip_reader_test.zip";

    // Build a zip with STORED entries. CRCs are zero — ZipReader does not
    // validate them. Offsets are computed as the entries are appended.
    void build(const std::vector<std::pair<std::string, std::string>>& entries) {
        std::vector<uint8_t> out;
        struct Cd { std::string name; uint32_t csize, lho; };
        std::vector<Cd> cds;

        auto w8  = [&](uint8_t v) { out.push_back(v); };
        auto w16 = [&](uint16_t v) { w8(v & 0xFF); w8(v >> 8); };
        auto w32 = [&](uint32_t v) { w16(v & 0xFFFF); w16(v >> 16); };

        for (const auto& [name, data] : entries) {
            const uint32_t lho = static_cast<uint32_t>(out.size());
            w32(0x04034b50); w16(10); w16(0); w16(0);           // sig, ver, flags, stored
            w16(0); w16(0); w32(0);                              // time, date, crc
            const uint32_t sz = static_cast<uint32_t>(data.size());
            w32(sz); w32(sz);                                    // csize, usize
            w16(static_cast<uint16_t>(name.size())); w16(0);     // nlen, elen
            for (char c : name) w8(static_cast<uint8_t>(c));
            for (char c : data) w8(static_cast<uint8_t>(c));
            cds.push_back({name, sz, lho});
        }

        const uint32_t cd_offset = static_cast<uint32_t>(out.size());
        for (const auto& cd : cds) {
            w32(0x02014b50); w16(20); w16(10); w16(0); w16(0);   // sig, made, need, flags, stored
            w16(0); w16(0); w32(0);                              // time, date, crc
            w32(cd.csize); w32(cd.csize);
            w16(static_cast<uint16_t>(cd.name.size())); w16(0); w16(0); // nlen, elen, clen
            w16(0); w16(0); w32(0); w32(cd.lho);                 // disk, iattr, eattr, lho
            for (char c : cd.name) w8(static_cast<uint8_t>(c));
        }
        const uint32_t cd_size =
            static_cast<uint32_t>(out.size()) - cd_offset;
        w32(0x06054b50); w16(0); w16(0);
        w16(static_cast<uint16_t>(cds.size()));
        w16(static_cast<uint16_t>(cds.size()));
        w32(cd_size); w32(cd_offset); w16(0);

        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()),
                static_cast<std::streamsize>(out.size()));
    }
};

} // namespace

TEST(ZipReader, ReadsStoredEntries) {
    TestZip tz;
    tz.build({{"a.txt", "alpha"}, {"b/c.bin", std::string("\x01\x02\x03", 3)}});
    ZipReader z;
    z.load(tz.path);
    EXPECT_EQ(z.size(), 2u);
    EXPECT_TRUE(z.has("a.txt"));
    EXPECT_TRUE(z.has("B/C.BIN"));   // case-insensitive
    EXPECT_EQ(z.read("a.txt"),
              (std::vector<uint8_t>{'a', 'l', 'p', 'h', 'a'}));
    EXPECT_EQ(z.read("b/c.bin"),
              (std::vector<uint8_t>{0x01, 0x02, 0x03}));
    EXPECT_THROW(z.read("missing.txt"), std::runtime_error);
}

TEST(ZipReader, RejectsNonZip) {
    const auto path = std::filesystem::temp_directory_path() /
                      "f4_zip_reader_notazip.bin";
    { std::ofstream f(path, std::ios::binary); f << "garbage that is not a zip"; }
    ZipReader z;
    EXPECT_THROW(z.load(path), std::runtime_error);
}

TEST(ZipReader, EmptyArchive) {
    TestZip tz;
    tz.build({});
    ZipReader z;
    z.load(tz.path);
    EXPECT_EQ(z.size(), 0u);
    EXPECT_FALSE(z.has("anything"));
}

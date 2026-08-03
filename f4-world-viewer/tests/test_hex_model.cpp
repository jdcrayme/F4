// f4-world-viewer/tests/test_hex_model.cpp
//
// Unit tests for the Hex Inspector's data model + decoders. We test:
//   - ByteRange semantics (contains, overlaps, equality)
//   - HexModel::load_bytes + read_le + read_fixed_string + slice
//   - HexModel::entropy (known-value check)
//   - HexModel::find_ascii_strings
//   - identify_file (extension + magic bytes)
//   - decoders: cam_manifest, cmp_header, theater_map, falcon4_ct, generic
//
// The decoders are pure functions that operate on the model — no raylib,
// no ImGui — so we can test them in isolation without spinning up a window.

#include <gtest/gtest.h>

#include <f4/viewer/hex_model.hpp>
#include <f4/viewer/decoders.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace f4::viewer;

// ===========================================================================
// ByteRange
// ===========================================================================

TEST(ByteRange, EmptyByDefault) {
    ByteRange r;
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.offset, 0u);
    EXPECT_EQ(r.length, 0u);
}

TEST(ByteRange, ContainsAndEnd) {
    ByteRange r{10, 5};
    EXPECT_EQ(r.end(), 15u);
    EXPECT_TRUE(r.contains(10));
    EXPECT_TRUE(r.contains(14));
    EXPECT_FALSE(r.contains(9));
    EXPECT_FALSE(r.contains(15));  // half-open
}

TEST(ByteRange, Overlaps) {
    ByteRange a{0, 10};
    ByteRange b{5, 10};
    ByteRange c{10, 5};   // adjacent to a (touching at boundary, not overlapping)
    ByteRange d{20, 5};
    EXPECT_TRUE(a.overlaps(b));
    EXPECT_FALSE(a.overlaps(c));  // adjacent at boundary — not overlapping
    EXPECT_FALSE(a.overlaps(d));
}

TEST(ByteRange, Equality) {
    ByteRange a{10, 5};
    ByteRange b{10, 5};
    ByteRange c{10, 6};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ===========================================================================
// HexModel — loading + accessors
// ===========================================================================

TEST(HexModel, LoadBytesSetsPathAndBytes) {
    HexModel m;
    m.load_bytes("test.bin", {0x41, 0x42, 0x43, 0x44});
    EXPECT_EQ(m.path(), std::filesystem::path("test.bin"));
    EXPECT_EQ(m.size(), 4u);
    EXPECT_TRUE(m.loaded());
    EXPECT_EQ(m.byte_at(0), 0x41);
    EXPECT_EQ(m.byte_at(3), 0x44);
    EXPECT_EQ(m.byte_at(4), 0);  // out of range
}

TEST(HexModel, ReadLeLittleEndian) {
    HexModel m;
    m.load_bytes("test", {0x78, 0x56, 0x34, 0x12, 0xEF, 0xCD, 0xAB, 0x90});
    EXPECT_EQ(m.read_le(0, 1), 0x78u);
    EXPECT_EQ(m.read_le(0, 2), 0x5678u);
    EXPECT_EQ(m.read_le(0, 4), 0x12345678u);
    EXPECT_EQ(m.read_le(0, 8), 0x90ABCDEF12345678ull);
    EXPECT_EQ(m.read_le(4, 4), 0x90ABCDEFu);
}

TEST(HexModel, ReadLeOutOfRangeReturnsZero) {
    HexModel m;
    m.load_bytes("test", {0x01, 0x02});
    EXPECT_EQ(m.read_le(0, 4), 0u);  // would read past end
    EXPECT_EQ(m.read_le(2, 1), 0u);
    EXPECT_EQ(m.read_le(0, 0), 0u);  // length 0
    EXPECT_EQ(m.read_le(0, 9), 0u);  // length > 8
}

TEST(HexModel, ReadFixedStringNullTerminated) {
    HexModel m;
    // "Hello\0xxxx" — string stops at null, returns "Hello"
    m.load_bytes("test", {'H', 'e', 'l', 'l', 'o', 0, 'X', 'Y', 'Z'});
    EXPECT_EQ(m.read_fixed_string(0, 9), "Hello");
    EXPECT_EQ(m.read_fixed_string(0, 5), "Hello");  // no null in range
    EXPECT_EQ(m.read_fixed_string(0, 4), "Hell");
}

TEST(HexModel, ReadFixedStringEmptyWhenStartsWithNull) {
    HexModel m;
    m.load_bytes("test", {0, 'A', 'B'});
    EXPECT_EQ(m.read_fixed_string(0, 3), "");
}

TEST(HexModel, SliceBoundsClamped) {
    HexModel m;
    m.load_bytes("test", {0, 1, 2, 3, 4, 5, 6, 7});
    EXPECT_EQ(m.slice(2, 3), (std::vector<uint8_t>{2, 3, 4}));
    EXPECT_EQ(m.slice(0, 8), (std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7}));
    EXPECT_EQ(m.slice(6, 10), (std::vector<uint8_t>{6, 7}));  // clamped
    EXPECT_TRUE(m.slice(10, 4).empty());  // past end
}

TEST(HexModel, LoadBytesClearsAnnotationsAndSelection) {
    HexModel m;
    m.load_bytes("a.cam", {0x01, 0x02});
    m.set_selection({0, 2});
    m.set_annotations({{{0, 1}, "x", "y", "z", "field"}});
    EXPECT_FALSE(m.selection().empty());
    EXPECT_EQ(m.annotations().size(), 1u);

    // Loading new bytes should reset state.
    m.load_bytes("b.bin", {0x03, 0x04, 0x05});
    EXPECT_TRUE(m.selection().empty());
    EXPECT_TRUE(m.annotations().empty());
}

// ===========================================================================
// HexModel — entropy
// ===========================================================================

TEST(HexModel, EntropyZeroForSingleByte) {
    HexModel m;
    m.load_bytes("test", std::vector<uint8_t>(100, 0x00));  // all zeros
    EXPECT_NEAR(m.entropy(), 0.0, 1e-9);
}

TEST(HexModel, EntropyMaxForUniformRandom) {
    // Each byte value appears exactly once → entropy = log2(256) = 8.0
    HexModel m;
    std::vector<uint8_t> bytes(256);
    for (int i = 0; i < 256; ++i) bytes[i] = static_cast<uint8_t>(i);
    m.load_bytes("test", bytes);
    EXPECT_NEAR(m.entropy(), 8.0, 1e-9);
}

TEST(HexModel, EntropyMediumForTwoValues) {
    // Half 0x00, half 0xFF → entropy = 1.0 bit
    HexModel m;
    std::vector<uint8_t> bytes(100, 0x00);
    bytes.insert(bytes.end(), 100, 0xFF);
    m.load_bytes("test", bytes);
    EXPECT_NEAR(m.entropy(), 1.0, 1e-9);
}

// ===========================================================================
// HexModel — find_ascii_strings
// ===========================================================================

TEST(HexModel, FindAsciiStringsExtractsPrintableRuns) {
    HexModel m;
    m.load_bytes("test", {
        'H', 'e', 'l', 'l', 'o',  // 5-char string at offset 0
        0x00, 0x01, 0xFF,          // binary noise
        'W', 'o', 'r', 'l', 'd'    // 5-char string at offset 8
    });
    auto strings = m.find_ascii_strings(4);
    ASSERT_EQ(strings.size(), 2u);
    EXPECT_EQ(strings[0].offset, 0u);
    EXPECT_EQ(strings[0].length, 5u);
    EXPECT_EQ(strings[1].offset, 8u);
    EXPECT_EQ(strings[1].length, 5u);
}

TEST(HexModel, FindAsciiStringsRespectsMinLength) {
    HexModel m;
    m.load_bytes("test", {'A', 'B', 'C', 0x00, 'D', 'E', 'F', 'G', 0x00});
    EXPECT_EQ(m.find_ascii_strings(3).size(), 2u);  // "ABC" + "DEFG"
    EXPECT_EQ(m.find_ascii_strings(4).size(), 1u);  // only "DEFG"
    EXPECT_EQ(m.find_ascii_strings(5).size(), 0u);
}

TEST(HexModel, FindAsciiStringsHandlesNewlines) {
    // Multi-line text should appear as one string (newlines are part of
    // printable ASCII for this purpose).
    HexModel m;
    m.load_bytes("test", {'L', 'i', 'n', 'e', '1', '\n', 'L', 'i', 'n', 'e', '2'});
    auto strings = m.find_ascii_strings(4);
    ASSERT_EQ(strings.size(), 1u);
    EXPECT_EQ(strings[0].length, 11u);
}

// ===========================================================================
// identify_file
// ===========================================================================

TEST(IdentifyFile, ExtensionBased) {
    EXPECT_EQ(identify_file_by_extension("save1.cam"), FileType::CamArchive);
    EXPECT_EQ(identify_file_by_extension("save1.CAM"), FileType::CamArchive);
    EXPECT_EQ(identify_file_by_extension("save1.cmp"), FileType::CmpSubfile);
    EXPECT_EQ(identify_file_by_extension("FALCON4.ct"), FileType::Falcon4Ct);
    EXPECT_EQ(identify_file_by_extension("falcon4.CT"), FileType::Falcon4Ct);
    EXPECT_EQ(identify_file_by_extension("THEATER.MAP"), FileType::TheaterMap);
    EXPECT_EQ(identify_file_by_extension("theater.mea"), FileType::TheaterMea);
    EXPECT_EQ(identify_file_by_extension("THEATER.O2"), FileType::TheaterO2);
    EXPECT_EQ(identify_file_by_extension("f16.dat"), FileType::Dat);
    EXPECT_EQ(identify_file_by_extension("save1.ver"), FileType::Ver);
    EXPECT_EQ(identify_file_by_extension("script.lua"), FileType::Lua);
    EXPECT_EQ(identify_file_by_extension("notes.txt"), FileType::Text);
    EXPECT_EQ(identify_file_by_extension("config.ini"), FileType::Text);
    EXPECT_EQ(identify_file_by_extension("unknown.xyz"), FileType::Unknown);
}

TEST(IdentifyFile, MagicBytesOverrideExtension) {
    // THEATER.MAP magic 0x444CFFAE in little-endian on disk:
    // [AE, FF, 4C, 44, ...]
    std::vector<uint8_t> bytes = {0xAE, 0xFF, 0x4C, 0x44, 0, 0, 0, 0};
    // Even with no extension / unknown extension, magic identifies it.
    EXPECT_EQ(identify_file("mystery_file", bytes.data(), bytes.size()),
              FileType::TheaterMap);
}

TEST(IdentifyFile, TextHeuristicWhenNoExtension) {
    const std::string s = "Hello, this is a text file with words.\nLine 2.\n";
    std::vector<uint8_t> text(s.begin(), s.end());
    EXPECT_EQ(identify_file("README", text.data(), text.size()),
              FileType::Text);
}

TEST(IdentifyFile, BinaryWhenHighNonPrintableRatio) {
    std::vector<uint8_t> bin = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD, 0xFC, 0xFB};
    EXPECT_EQ(identify_file("data", bin.data(), bin.size()),
              FileType::Binary);
}

// ===========================================================================
// decode_cam_manifest — uses the real save1.cam fixture
// ===========================================================================

TEST(DecodeCamManifest, LoadsRealFixture) {
    HexModel m;
    m.load_file(FIXTURE_DIR "save1.cam");
    m.apply_decoder(FileType::CamArchive);
    // Should produce: 1 manifest_offset + 10 subfiles + 1 manifest_directory
    // = 12 annotations (the actual count may vary slightly with the
    // decoder implementation, but should be in this ballpark).
    EXPECT_GE(m.annotations().size(), 10u);
    // The first annotation should be the manifest_offset.
    EXPECT_EQ(m.annotations()[0].label, "manifest_offset");
    EXPECT_EQ(m.annotations()[0].range.offset, 0u);
    EXPECT_EQ(m.annotations()[0].range.length, 4u);
    // At least one annotation should mention a sub-file.
    bool has_subfile = false;
    for (const auto& a : m.annotations()) {
        if (a.label.find("subfile:") != std::string::npos) {
            has_subfile = true;
            break;
        }
    }
    EXPECT_TRUE(has_subfile);
}

// ===========================================================================
// decode_cmp_header
// ===========================================================================

TEST(DecodeCmpHeader, ParsesHeaderFields) {
    // Minimal valid .cmp: 8-byte header + a few bytes of "compressed" payload.
    // reserved_skip = 0, decompressed_size = 1000.
    std::vector<uint8_t> bytes = {
        0x00, 0x00, 0x00, 0x00,   // reserved_skip = 0
        0xE8, 0x03, 0x00, 0x00,   // decompressed_size = 1000
        0xAA, 0xBB, 0xCC, 0xDD    // payload (4 bytes)
    };
    HexModel m;
    m.load_bytes("save1.cmp", bytes);
    m.apply_decoder(FileType::CmpSubfile);
    ASSERT_EQ(m.annotations().size(), 3u);
    EXPECT_EQ(m.annotations()[0].label, "reserved_skip");
    EXPECT_EQ(m.annotations()[1].label, "decompressed_size");
    EXPECT_EQ(m.annotations()[2].label, "lzss_payload");
    // The decompressed_size value should be parsed correctly.
    EXPECT_NE(m.annotations()[1].value.find("1000"), std::string::npos);
}

// ===========================================================================
// decode_theater_map
// ===========================================================================

TEST(DecodeTheaterMap, ParsesHeaderFields) {
    // THEATER.MAP magic = 0x444CFFAE → little-endian [AE, FF, 4C, 44]
    std::vector<uint8_t> bytes = {
        0xAE, 0xFF, 0x4C, 0x44,   // magic = 0x444CFFAE
        0x80, 0x00, 0x00, 0x00,   // width = 128
        0x80, 0x00, 0x00, 0x00,   // height = 128
        0x10, 0x00, 0x00, 0x00,   // ft_to_cell = 16
        // 4 palette entries × 4 bytes RGBA
        0x00, 0x00, 0xFF, 0xFF,   // blue
        0x00, 0xFF, 0x00, 0xFF,   // green
        0xFF, 0x00, 0x00, 0xFF,   // red
        0xFF, 0xFF, 0xFF, 0xFF    // white
    };
    HexModel m;
    m.load_bytes("THEATER.MAP", bytes);
    m.apply_decoder(FileType::TheaterMap);
    // 4 header fields + 4 palette entries
    ASSERT_EQ(m.annotations().size(), 8u);
    EXPECT_EQ(m.annotations()[0].label, "magic");
    EXPECT_EQ(m.annotations()[0].value, "0x444cffae");
    EXPECT_EQ(m.annotations()[1].label, "width");
    EXPECT_NE(m.annotations()[1].value.find("128"), std::string::npos);
    EXPECT_EQ(m.annotations()[2].label, "height");
    EXPECT_EQ(m.annotations()[3].label, "ft_to_mea_cell");
    EXPECT_EQ(m.annotations()[4].label, "palette[0]");
    EXPECT_NE(m.annotations()[4].value.find("RGBA(0,0,255,255)"), std::string::npos);
}

// ===========================================================================
// decode_falcon4_ct — uses the real FALCON4.ct fixture
// ===========================================================================

TEST(DecodeFalcon4Ct, LoadsRealFixture) {
    HexModel m;
    m.load_file(FIXTURE_DIR "FALCON4.ct");
    m.apply_decoder(FileType::Falcon4Ct);
    // The fixture has 2135 entries (verified by f4-world-convert tests).
    // We should get: 1 num_entities + min(16, 2135) entry annotations +
    // possibly 1 "remaining entries" annotation.
    EXPECT_GE(m.annotations().size(), 10u);
    // First annotation is num_entities.
    EXPECT_EQ(m.annotations()[0].label, "num_entities");
    EXPECT_EQ(m.annotations()[0].value, "2135");
}

// ===========================================================================
// decode_generic
// ===========================================================================

TEST(DecodeGeneric, EmitsFileAndMagicAndEntropyAnnotations) {
    std::vector<uint8_t> bytes = {0xDE, 0xAD, 0xBE, 0xEF, 'H', 'i', 0x00};
    HexModel m;
    m.load_bytes("test.bin", bytes);
    m.apply_decoder(FileType::Binary);  // force generic
    // Should have at least: file_size, magic, entropy
    ASSERT_GE(m.annotations().size(), 3u);
    bool has_file_size = false, has_magic = false, has_entropy = false;
    for (const auto& a : m.annotations()) {
        if (a.label == "file_size") has_file_size = true;
        if (a.label == "magic")     has_magic = true;
        if (a.label == "entropy")   has_entropy = true;
    }
    EXPECT_TRUE(has_file_size);
    EXPECT_TRUE(has_magic);
    EXPECT_TRUE(has_entropy);
}

TEST(DecodeGeneric, ExtractsAsciiStrings) {
    // Mix of binary + ASCII runs.
    std::vector<uint8_t> bytes = {
        0x00, 0x01,
        'H', 'e', 'l', 'l', 'o',
        0xFF, 0xFE,
        'W', 'o', 'r', 'l', 'd',
        0x00
    };
    HexModel m;
    m.load_bytes("test.bin", bytes);
    m.apply_decoder(FileType::Binary);
    bool found_hello = false, found_world = false;
    for (const auto& a : m.annotations()) {
        if (a.value.find("Hello") != std::string::npos) found_hello = true;
        if (a.value.find("World") != std::string::npos) found_world = true;
    }
    EXPECT_TRUE(found_hello);
    EXPECT_TRUE(found_world);
}

// ===========================================================================
// HexModel::annotation_at
// ===========================================================================

TEST(HexModel, AnnotationAtFindsContainingRange) {
    HexModel m;
    m.load_bytes("test", std::vector<uint8_t>(20, 0));
    m.set_annotations({
        {{0, 4},  "magic",   "0x12345678", "Magic bytes", "header"},
        {{8, 12}, "payload", "12 bytes",   "Body",        "field"}
    });
    ASSERT_NE(m.annotation_at(2), nullptr);
    EXPECT_EQ(m.annotation_at(2)->label, "magic");
    ASSERT_NE(m.annotation_at(10), nullptr);
    EXPECT_EQ(m.annotation_at(10)->label, "payload");
    EXPECT_EQ(m.annotation_at(5), nullptr);   // gap between ranges
    EXPECT_EQ(m.annotation_at(20), nullptr);  // past end
}

// ===========================================================================
// apply_decoder auto-detection
// ===========================================================================

TEST(ApplyDecoder, AutoDetectsCamByExtension) {
    HexModel m;
    m.load_file(FIXTURE_DIR "save1.cam");
    m.apply_decoder();  // auto — should pick CamArchive
    EXPECT_EQ(m.file_type(), FileType::CamArchive);
    // Same annotations as explicitly calling apply_decoder(CamArchive).
    EXPECT_GE(m.annotations().size(), 10u);
}

TEST(ApplyDecoder, AutoDetectsTheaterMapByExtension) {
    HexModel m;
    m.load_file(TERRAIN_FIXTURE_DIR "THEATER.MAP");
    m.apply_decoder();
    EXPECT_EQ(m.file_type(), FileType::TheaterMap);
    EXPECT_GE(m.annotations().size(), 4u);  // 4 header fields minimum
}

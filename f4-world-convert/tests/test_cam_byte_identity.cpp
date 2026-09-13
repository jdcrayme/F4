// test_cam_byte_identity.cpp — the byte-identity closure of the save-write
// encoders, pinned against both committed .cam fixtures.
//
// Contract: for every LZSS-structured sub-file (.cmp/.obj/.obd/.tea/.uni),
//   decode(original) -> encode == original, BYTE-FOR-BYTE,
// and the whole archive reassembles to the identical .cam. The sub-file
// encoders are struct-faithful AND byte-faithful because
//   (a) f4::lzss::compress is a faithful port of FreeFalcon's
//       LZSS_Compress (see f4-lzss/src/compress.cpp), and
//   (b) the decoders capture every field the semantic projection drops —
//       fixed-width-string padding garbage, alignment padding, the
//       skipped stores/schedule/rating and loadout regions — so the
//       encoder reproduces the original bytes exactly.
//
// The passthrough sub-files (.evt/.plt/.pst/.wth/.pol/.ver) carry no
// decode structs yet; they ride verbatim through CamWriter (pinned by
// CamWriter.RoundTripIsByteIdenticalForStandardLayout).

#include <gtest/gtest.h>

#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/objective_encoder.hpp>
#include <f4/world_convert/team_decoder.hpp>
#include <f4/world_convert/team_encoder.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/unit_encoder.hpp>
#include <f4/world_convert/cam_writer.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace f4::world_convert;

namespace {

struct Fixture {
    const char* path;
    int version;
};

// Both committed fixtures: save1 (v63, embedded .obj + empty 10-byte .obd)
// and TestCamp (v71 mid-campaign, .obd-only with 14 deltas).
std::vector<Fixture> fixtures() {
    std::vector<Fixture> f;
    f.push_back({FIXTURE_DIR "save1.cam", 63});
    if (std::filesystem::exists(std::filesystem::path(REPO_ROOT "TestCamp.cam")))
        f.push_back({REPO_ROOT "TestCamp.cam", 71});
    return f;
}

// Re-encode one sub-file by extension; nullopt for passthrough types.
std::optional<std::vector<uint8_t>> reencode_subfile(const SubFile& sf,
                                                     int ver) {
    const std::string ext = sf.ext();
    if (ext == "cmp") {
        auto h = decode_cmp(sf.data.data(), sf.data.size(), ver);
        return encode_cmp(h, ver);
    }
    if (ext == "obj") {
        auto d = decode_obj(sf.data.data(), sf.data.size(), ver);
        return encode_obj(d, ver);
    }
    if (ext == "obd") {
        auto d = decode_obd(sf.data.data(), sf.data.size(), ver);
        return encode_obd(d);
    }
    if (ext == "tea") {
        auto d = decode_tea(sf.data.data(), sf.data.size(), ver);
        return encode_tea(d, ver);
    }
    if (ext == "uni") {
        auto d = decode_uni(sf.data.data(), sf.data.size(),
                            UnitDecodeOptions{ver, nullptr});
        return encode_uni(d, ver);
    }
    return std::nullopt;   // passthrough (.evt/.plt/.pst/.wth/.pol/.ver)
}

// Decode every sub-file, re-encode, and compare against the original
// bytes. `loaded` reports whether the archive opened; `reencoded` the
// number of structurally re-encoded sub-files.
void check_subfiles_byte_identical(const char* path, int ver,
                                   const char* label, bool* loaded,
                                   int* reencoded) {
    *loaded = false;
    *reencoded = 0;
    CamArchive cam;
    ASSERT_NO_THROW(cam.load(path));
    ASSERT_FALSE(cam.subfiles().empty());
    *loaded = true;

    const int file_ver = [&] {
        if (const auto* v = cam.find("ver"))
            return read_version(v->data.data(), v->data.size());
        return ver;
    }();
    EXPECT_EQ(file_ver, ver);

    for (const auto& sf : cam.subfiles()) {
        auto re = reencode_subfile(sf, file_ver);
        if (!re) continue;   // passthrough sub-file
        SCOPED_TRACE(sf.name);
        ++*reencoded;
        EXPECT_EQ(*re, sf.data)
            << label << ": sub-file " << sf.name
            << " does not re-encode byte-identically";
    }
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Per-sub-file byte identity
// ═══════════════════════════════════════════════════════════════════════════

TEST(CamByteIdentity, Save1EverySubfileReencodesByteIdentical) {
    bool loaded = false;
    int reencoded = 0;
    check_subfiles_byte_identical(FIXTURE_DIR "save1.cam", 63, "save1",
                                  &loaded, &reencoded);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(reencoded, 5);   // cmp, obj, obd, tea, uni
}

TEST(CamByteIdentity, TestCampEverySubfileReencodesByteIdentical) {
    if (!std::filesystem::exists(std::filesystem::path(REPO_ROOT "TestCamp.cam")))
        GTEST_SKIP() << "TestCamp.cam not present";
    bool loaded = false;
    int reencoded = 0;
    check_subfiles_byte_identical(REPO_ROOT "TestCamp.cam", 71, "TestCamp",
                                  &loaded, &reencoded);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(reencoded, 4);   // cmp, obd, tea, uni (no embedded .obj)
}

// ═══════════════════════════════════════════════════════════════════════════
// Whole-archive round-trip: load → decode → re-encode → assemble → identical
// ═══════════════════════════════════════════════════════════════════════════

void check_cam_level_byte_identical(const char* path, int ver) {
    CamArchive cam;
    ASSERT_NO_THROW(cam.load(path));
    ASSERT_FALSE(cam.subfiles().empty());

    int ver_found = ver;
    if (const auto* v = cam.find("ver"))
        ver_found = read_version(v->data.data(), v->data.size());
    ASSERT_EQ(ver_found, ver);

    CamWriter writer;
    for (const auto& sf : cam.subfiles()) {
        auto re = reencode_subfile(sf, ver_found);
        if (re)
            writer.add(sf.name, *re);
        else
            writer.add(sf.name, sf.data);   // passthrough
    }
    const auto rebuilt = writer.build();
    EXPECT_EQ(rebuilt, cam.raw_bytes())
        << path << ": the decode → re-encode → assemble loop is not "
                   "byte-identical to the original .cam";
}

TEST(CamByteIdentity, Save1CamLevelRoundTripIsByteIdentical) {
    check_cam_level_byte_identical(FIXTURE_DIR "save1.cam", 63);
}

TEST(CamByteIdentity, TestCampCamLevelRoundTripIsByteIdentical) {
    if (!std::filesystem::exists(std::filesystem::path(REPO_ROOT "TestCamp.cam")))
        GTEST_SKIP() << "TestCamp.cam not present";
    check_cam_level_byte_identical(REPO_ROOT "TestCamp.cam", 71);
}

// ═══════════════════════════════════════════════════════════════════════════
// The empty-.obd edge (save1 carries a 10-byte [i32 6][i16 0][i32 0])
// ═══════════════════════════════════════════════════════════════════════════

TEST(CamByteIdentity, EmptyObdDecodesAndReencodesToTheSame10Bytes) {
    CamArchive cam;
    ASSERT_NO_THROW(cam.load(FIXTURE_DIR "save1.cam"));
    const auto* obd = cam.find("obd");
    ASSERT_NE(obd, nullptr);
    ASSERT_EQ(obd->data.size(), 10u);

    DecodedObjectiveDeltas dec;
    ASSERT_NO_THROW(dec = decode_obd(obd->data.data(), obd->data.size(), 63));
    EXPECT_EQ(dec.count, 0);
    EXPECT_TRUE(dec.deltas.empty());

    auto re = encode_obd(dec);
    EXPECT_EQ(re, obd->data);
}

// ═══════════════════════════════════════════════════════════════════════════
// The v71 .obd deltas: decode → re-encode is byte-identical, and the
// decoded delta fields match the .obj-of-record semantics (owner flips,
// fstatus damage bitmaps).
// ═══════════════════════════════════════════════════════════════════════════

TEST(CamByteIdentity, TestCampObdDeltasCarryMutationState) {
    if (!std::filesystem::exists(std::filesystem::path(REPO_ROOT "TestCamp.cam")))
        GTEST_SKIP() << "TestCamp.cam not present";
    CamArchive cam;
    ASSERT_NO_THROW(cam.load(REPO_ROOT "TestCamp.cam"));
    const auto* obd = cam.find("obd");
    ASSERT_NE(obd, nullptr);

    auto dec = decode_obd(obd->data.data(), obd->data.size(), 71);
    EXPECT_EQ(dec.count, 14);
    EXPECT_EQ(dec.deltas.size(), 14u);
    EXPECT_EQ(dec.bytes_consumed, dec.inner_size);

    // Every delta carries a real VU_ID and a damage bitmap; most are
    // owner flips or supply/fuel drains.
    int with_fstatus = 0;
    for (const auto& d : dec.deltas) {
        EXPECT_NE(d.id_num, 0u);
        if (!d.fstatus.empty()) ++with_fstatus;
    }
    EXPECT_GT(with_fstatus, 0);

    auto re = encode_obd(dec);
    EXPECT_EQ(re, obd->data);
}

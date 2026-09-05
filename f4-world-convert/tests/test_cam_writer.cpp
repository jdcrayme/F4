// test_cam_writer.cpp — CamWriter round-trip + the json2cam end-to-end path.
//
// Two round-trips, both against the real save1.cam fixture:
//
//  1. Container round-trip (CamWriter, no JSON):
//       load save1.cam  ->  subfiles
//       CamWriter.build()  ->  bytes
//       load bytes  ->  subfiles'
//       subfiles == subfiles'   (name + data, every sub-file)
//
//  2. JSON round-trip (the json2cam path, via the shared library core):
//       to_world_json(cam, preserve_all_subfiles=true)  ->  json
//       cam_from_world_json(json)  ->  bytes
//       load bytes  ->  subfiles''
//       subfiles == subfiles''   (the save-write tranche's closure)
//
// The container round-trip should also be byte-identical to the original
// .cam when the original uses the standard packed layout (sub-files
// contiguous from offset 4 in manifest order); we assert that as an
// EXPECT alongside the structural equality.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/cam_writer.hpp>
#include <f4/world_convert/world_json.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace f4::world_convert;

namespace fs = std::filesystem;

namespace {

// Write `bytes` to a temp file and load it as a CamArchive. Returns the
// loaded archive (subfiles populated). CamArchive::load takes a path, so
// the round-trip goes through disk. The temp filename is unique within
// the test binary (gtest runs tests sequentially in one process).
CamArchive load_from_bytes(const std::vector<uint8_t>& bytes) {
    static int seq = 0;
    fs::path tmp = fs::temp_directory_path() /
                   ("f4_test_cam_" + std::to_string(seq++) + ".cam");
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    CamArchive cam;
    cam.load(tmp);
    std::error_code ec;
    fs::remove(tmp, ec);
    return cam;
}

// Compare two sub-file lists: same count, and each (name, data) matches
// in order.
::testing::AssertionResult subfiles_equal(const std::vector<SubFile>& a,
                                          const std::vector<SubFile>& b) {
    if (a.size() != b.size()) {
        return ::testing::AssertionFailure()
               << "subfile count differs: " << a.size() << " vs " << b.size();
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name) {
            return ::testing::AssertionFailure()
                   << "subfile " << i << " name: " << a[i].name << " vs " << b[i].name;
        }
        if (a[i].data != b[i].data) {
            return ::testing::AssertionFailure()
                   << "subfile " << i << " (" << a[i].name << ") data differs ("
                   << a[i].data.size() << " vs " << b[i].data.size() << " bytes)";
        }
    }
    return ::testing::AssertionSuccess();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1. Container round-trip (CamWriter)
// ═══════════════════════════════════════════════════════════════════════════

TEST(CamWriter, RoundTripPreservesAllSubfiles) {
    CamArchive orig;
    orig.load(FIXTURE_DIR "save1.cam");

    CamWriter w;
    for (const auto& sf : orig.subfiles()) {
        w.add(sf.name, sf.data);   // preserve manifest order
    }
    auto bytes = w.build();
    ASSERT_FALSE(bytes.empty());

    CamArchive roundtrip = load_from_bytes(bytes);
    EXPECT_TRUE(subfiles_equal(orig.subfiles(), roundtrip.subfiles()));
}

TEST(CamWriter, RoundTripIsByteIdenticalForStandardLayout) {
    // The standard FreeFalcon .cam layout packs sub-files contiguously
    // from offset 4 in manifest order, with the manifest immediately
    // after the data. CamWriter reproduces that layout, so a round-trip
    // of save1.cam should be byte-identical. (If a future fixture uses
    // a non-standard layout, this EXPECT would fail while the structural
    // test above still passes — the difference is informational.)
    CamArchive orig;
    orig.load(FIXTURE_DIR "save1.cam");

    CamWriter w;
    for (const auto& sf : orig.subfiles()) {
        w.add(sf.name, sf.data);
    }
    auto bytes = w.build();
    EXPECT_EQ(bytes, orig.raw_bytes())
        << "CamWriter output is not byte-identical to the original .cam "
           "(structural equality is checked in RoundTripPreservesAllSubfiles)";
}

TEST(CamWriter, EmptyArchiveRejectsLoad) {
    // An empty CamWriter produces a 4-byte file (just the manifest_offset
    // placeholder = 4, pointing at an empty manifest). CamArchive::load
    // rejects files < 8 bytes — the writer's output for zero sub-files is
    // 8 bytes (4-byte offset + 4-byte count=0), which loads to zero
    // sub-files. Confirm that boundary.
    CamWriter w;
    auto bytes = w.build();
    EXPECT_EQ(bytes.size(), 8u);
    CamArchive cam = load_from_bytes(bytes);
    EXPECT_EQ(cam.subfiles().size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. JSON round-trip (the json2cam path, via cam_from_world_json)
// ═══════════════════════════════════════════════════════════════════════════

TEST(CamFromWorldJson, RoundTripsAllSubfilesViaJson) {
    // The save-write tranche's closure: cam2json --preserve-subfiles
    // produces a world JSON whose "subfiles_b64" block carries every
    // sub-file; cam_from_world_json reads it back into a .cam. The result
    // must decode to the identical sub-files.
    CamArchive orig;
    orig.load(FIXTURE_DIR "save1.cam");

    WorldJsonOptions opts;
    opts.preserve_all_subfiles = true;
    std::string json = to_world_json(orig, opts);
    ASSERT_NE(json.find("\"subfiles_b64\""), std::string::npos)
        << "to_world_json did not emit the subfiles_b64 block";

    auto bytes = cam_from_world_json(json);
    ASSERT_FALSE(bytes.empty());

    CamArchive roundtrip = load_from_bytes(bytes);
    EXPECT_TRUE(subfiles_equal(orig.subfiles(), roundtrip.subfiles()));
}

TEST(CamFromWorldJson, ThrowsWhenSubfilesBlockAbsent) {
    // A world JSON without --preserve-subfiles has no subfiles_b64 block;
    // cam_from_world_json must refuse rather than write an empty .cam.
    CamArchive orig;
    orig.load(FIXTURE_DIR "save1.cam");
    std::string json = to_world_json(orig);   // preserve_all_subfiles=false
    EXPECT_THROW(cam_from_world_json(json), std::runtime_error);
}

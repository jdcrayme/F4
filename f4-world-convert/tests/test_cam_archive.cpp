// test_cam_archive.cpp — .cam container parser against the real fixture.

#include <gtest/gtest.h>
#include <f4/convert/cam_archive.hpp>
#include <f4/convert/campaign_decoder.hpp>

#include <algorithm>

using namespace f4::convert;

namespace {
CamArchive load_fixture() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    return cam;
}
}

TEST(CamArchive, ParsesAllTenSubfiles) {
    auto cam = load_fixture();
    EXPECT_EQ(cam.subfiles().size(), 10u);
}

TEST(CamArchive, ContainsExpectedSubfileTypes) {
    auto cam = load_fixture();
    const std::vector<std::string> want = {"cmp","obd","obj","tea","uni",
                                           "evt","plt","pst","wth","ver"};
    for (const auto& ext : want) {
        EXPECT_NE(cam.find(ext), nullptr) << "missing sub-file type ." << ext;
    }
}

TEST(CamArchive, SubfileDataMatchesRecordedSize) {
    auto cam = load_fixture();
    for (const auto& sf : cam.subfiles()) {
        EXPECT_EQ(sf.data.size(), static_cast<std::size_t>(sf.size))
            << "size mismatch for " << sf.name;
    }
}

TEST(CamArchive, VerSubfileIsTextVersionNumber) {
    auto cam = load_fixture();
    const SubFile* ver = cam.find("ver");
    ASSERT_NE(ver, nullptr);
    // save1.ver is "63" (text). read_version returns the parsed int.
    EXPECT_EQ(read_version(ver->data.data(), ver->data.size()), 63);
}

TEST(CamArchive, CmpSubfileIsAtExpectedOffsetAndSize) {
    auto cam = load_fixture();
    const SubFile* cmp = cam.find("cmp");
    ASSERT_NE(cmp, nullptr);
    // From our manual decode: save1.cmp at offset 4, size 4420.
    EXPECT_EQ(cmp->offset, 4);
    EXPECT_EQ(cmp->size, 4420);
    EXPECT_EQ(cmp->name, "save1.cmp");
    EXPECT_EQ(cmp->stem(), "save1");
}

TEST(CamArchive, ThrowsOnNonexistentFile) {
    CamArchive cam;
    EXPECT_THROW(cam.load("/nonexistent/path/x.cam"), std::runtime_error);
}

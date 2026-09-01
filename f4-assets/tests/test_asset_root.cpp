// f4-assets/tests/test_asset_root.cpp

#include <f4/assets/f4_assets.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::assets;

namespace {

std::filesystem::path make_data_dir(const std::string& suffix) {
    auto base = std::filesystem::temp_directory_path() / "f4_assets_root_test";
    auto p = base / suffix;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

void write_file(const std::filesystem::path& p, const std::string& contents) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

const char* kMinimalManifest = R"({
  "f4": { "v": 1, "generator": "test" },
  "data_dir": "Data/",
  "assets": [
    {
      "id": "theater:korea",
      "path": "Theater/korea/theater.json",
      "format_version": 1,
      "capabilities": [
        { "name": "map", "status": "present" },
        { "name": "tiles_far", "status": "unknown" }
      ],
      "sources": []
    }
  ]
}
)";

} // namespace

TEST(AssetRoot, DiscoverFindsExplicitDir) {
    auto p = make_data_dir("discover_explicit");
    write_file(p / "manifest.json", kMinimalManifest);
    write_file(p / "Theater/korea/theater.json", R"({"theater":"korea"})");

    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(root->valid());
    EXPECT_EQ(root->manifest().assets.size(), 1u);
    auto tp = root->resolve_existing(AssetId{AssetFamily::theater, "korea"});
    EXPECT_FALSE(tp.empty());
    EXPECT_TRUE(std::filesystem::exists(tp));
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, DiscoverViaEnvVar) {
    auto p = make_data_dir("discover_env");
    write_file(p / "manifest.json", kMinimalManifest);
    write_file(p / "Theater/korea/theater.json", "{}");

    const int rc = ::setenv("F4_DATA_DIR", p.string().c_str(), 1);
    ASSERT_EQ(rc, 0);
    auto root = AssetRoot::discover();
    ::unsetenv("F4_DATA_DIR");
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(root->valid());
    EXPECT_EQ(root->manifest().assets.size(), 1u);
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, AtRejectsMissingDir) {
    auto root = AssetRoot::at("/no/such/dir/here");
    EXPECT_FALSE(root.has_value());
}

TEST(AssetRoot, MissingManifestIsNotAnError) {
    auto p = make_data_dir("empty");
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    EXPECT_TRUE(root->valid());
    EXPECT_TRUE(root->manifest().assets.empty());
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CorruptManifestIsAnError) {
    auto p = make_data_dir("corrupt");
    write_file(p / "manifest.json", R"({ not valid json })");
    auto root = AssetRoot::at(p);
    EXPECT_FALSE(root.has_value());
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, ResolveAssetPathForUnknownReturnsEmpty) {
    auto p = make_data_dir("unknown_id");
    write_file(p / "manifest.json", kMinimalManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto path = root->resolve_asset_path(AssetId{AssetFamily::koreaobj, "99999"});
    EXPECT_TRUE(path.empty());
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CheckReportsOkForPresentCapability) {
    auto p = make_data_dir("check_ok");
    write_file(p / "manifest.json", kMinimalManifest);
    write_file(p / "Theater/korea/theater.json", "{}");
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {
        RequiredAsset::with_capability(
            AssetId{AssetFamily::theater, "korea"}, "map")
    });
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::ok) << reports[0].detail;
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CheckReportsUnknownCapability) {
    auto p = make_data_dir("check_unknown");
    write_file(p / "manifest.json", kMinimalManifest);
    write_file(p / "Theater/korea/theater.json", "{}");
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {
        RequiredAsset::with_capability(
            AssetId{AssetFamily::theater, "korea"}, "tiles_far")
    });
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::unknown_capability)
        << "tiles_far is unknown in the manifest";
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CheckReportsMissingForUnknownAsset) {
    auto p = make_data_dir("check_missing");
    write_file(p / "manifest.json", kMinimalManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {
        RequiredAsset::model(AssetId{AssetFamily::koreaobj, "00001"})
    });
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::missing);
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CheckReportsStaleWhenFileMissing) {
    auto p = make_data_dir("check_stale");
    write_file(p / "manifest.json", kMinimalManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {
        RequiredAsset::with_capability(
            AssetId{AssetFamily::theater, "korea"}, "map")
    });
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::missing)
        << "capability is present but file missing on disk";
    std::filesystem::remove_all(p);
}

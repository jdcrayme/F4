// f4-assets/tests/test_asset_root.cpp

#include <f4/assets/f4_assets.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::assets;

namespace {

// POSIX setenv/unsetenv have no MSVC equivalents; _putenv_s(name, "")
// is the Windows idiom for unsetting. Returns 0 on success (both
// platforms' semantics) so callers can ASSERT on it.
int set_env(const char* name, const char* value) {
#ifdef _WIN32
    return ::_putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}
void unset_env(const char* name) {
#ifdef _WIN32
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

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

    const int rc = set_env("F4_DATA_DIR", p.string().c_str());
    ASSERT_EQ(rc, 0);
    auto root = AssetRoot::discover();
    unset_env("F4_DATA_DIR");
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

// ============================================================================
// Task 58 (NO_BINARY_RUNTIME_PLAN Tranche 0e): P7 staleness detection +
// @asset: reference resolution. "Existence alone is never evidence of
// freshness" — check() now hash-verifies entries that carry fingerprints,
// and resolve_ref() is the consumer-side @asset: → path resolver.
// ============================================================================

namespace {

const char* kFingerprintManifest = R"({
  "f4": { "v": 1, "generator": "test" },
  "data_dir": "Data/",
  "assets": [
    { "id": "aircraft:f16", "path": "Aircraft/f16.json", "size_bytes": 6,
      "sha256": "sha-of-abc-def-ignored", "fnv1a_64": "0123456789abcdef" }
  ]
}
)";

} // namespace

TEST(AssetRoot, CheckReportsStaleOnHashMismatch) {
    auto p = make_data_dir("stale_hash");
    write_file(p / "manifest.json", kFingerprintManifest);
    write_file(p / "Aircraft/f16.json", "abcdef");  // size matches, content differs
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());

    // Both fingerprints in the fixture are wrong for this content — either
    // mismatch alone must report stale with a pointed detail.
    auto reports = check(*root, {RequiredAsset::model(AssetId{AssetFamily::aircraft, "f16"})});
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::stale);
    EXPECT_NE(reports[0].detail.find("mismatch"), std::string::npos) << reports[0].detail;
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CheckReportsStaleOnSizeMismatch) {
    auto p = make_data_dir("stale_size");
    write_file(p / "manifest.json", kFingerprintManifest);
    write_file(p / "Aircraft/f16.json", "abc");  // wrong size — the fast path
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {RequiredAsset::model(AssetId{AssetFamily::aircraft, "f16"})});
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::stale);
    EXPECT_NE(reports[0].detail.find("size mismatch"), std::string::npos) << reports[0].detail;
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, CheckReportsOkWhenFingerprintsMatch) {
    const std::string content = "abcdef";
    auto p = make_data_dir("fresh_hash");
    write_file(p / "Aircraft/f16.json", content);
    // Record the TRUE fingerprints (computed with the same code the runtime
    // verifies with) so the manifest self-consistently verifies.
    std::string manifest = std::string(R"json({
  "f4": { "v": 1, "generator": "test" },
  "data_dir": "Data/",
  "assets": [
    { "id": "aircraft:f16", "path": "Aircraft/f16.json", "size_bytes": 6,
      "sha256": ")json") + sha256_hex(content) + R"json(",
      "fnv1a_64": ")json" + fnv1a_64_hex(content) + R"json(" }
  ]
}
)json";
    write_file(p / "manifest.json", manifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {RequiredAsset::model(AssetId{AssetFamily::aircraft, "f16"})});
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::ok) << reports[0].detail;
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, EntriesWithoutFingerprintsNeverReportStale) {
    auto p = make_data_dir("no_fingerprint");
    write_file(p / "manifest.json", kMinimalManifest);  // no fingerprints
    write_file(p / "Theater/korea/theater.json", "{}");
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    auto reports = check(*root, {RequiredAsset::model(AssetId{AssetFamily::theater, "korea"})});
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, AssetStatus::ok);
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, ResolveRefPassesNonRefsThrough) {
    auto p = make_data_dir("resolve_passthrough");
    write_file(p / "manifest.json", kMinimalManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    const auto rr = resolve_ref(*root, "scenarios/foo.json");
    EXPECT_TRUE(rr.ok);
    EXPECT_EQ(rr.path, std::filesystem::path("scenarios/foo.json"));
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, ResolveRefResolvesIdThroughManifest) {
    auto p = make_data_dir("resolve_id");
    write_file(p / "manifest.json", kMinimalManifest);
    write_file(p / "Theater/korea/theater.json", "{}");
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    const auto rr = resolve_ref(*root, "@asset:theater:korea");
    ASSERT_TRUE(rr.ok) << rr.error;
    EXPECT_TRUE(std::filesystem::exists(rr.path));
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, ResolveRefFailsLoudOnDanglingId) {
    auto p = make_data_dir("resolve_dangling");
    write_file(p / "manifest.json", kMinimalManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    const auto rr = resolve_ref(*root, "@asset:aircraft:f16");
    EXPECT_FALSE(rr.ok);
    EXPECT_NE(rr.error.find("not in the manifest"), std::string::npos) << rr.error;
    std::filesystem::remove_all(p);
}

TEST(AssetRoot, ResolveRefFailsOnFileMissingOnDisk) {
    auto p = make_data_dir("resolve_missing_file");
    write_file(p / "manifest.json", kMinimalManifest);  // entry lists the file, disk lacks it
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());
    const auto rr = resolve_ref(*root, "@asset:theater:korea");
    EXPECT_FALSE(rr.ok);
    EXPECT_NE(rr.error.find("file missing on disk"), std::string::npos) << rr.error;
    std::filesystem::remove_all(p);
}

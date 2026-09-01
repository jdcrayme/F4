// f4-install/tests/test_file_finder.cpp
//
// Tests for the canonical case-insensitive file finders. These cover
// the consolidated helpers that f4-terrain, f4-world-convert, and
// f4-models delegate to (Stage 2 of the asset pipeline).

#include <f4/install/file_finder.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace f4::install;
namespace fs = std::filesystem;

namespace {

// Make a unique temp dir + populate with named files. Returns the dir.
fs::path make_test_dir(const std::string& suffix,
                       const std::vector<std::string>& filenames) {
    auto p = fs::temp_directory_path() / "f4_install_finder_test" / suffix;
    fs::remove_all(p);
    fs::create_directories(p);
    for (const auto& f : filenames) {
        std::ofstream(p / f) << "stub";
    }
    return p;
}

void touch_subdir(const fs::path& parent, const std::string& name) {
    fs::create_directories(parent / name);
}

} // namespace

TEST(FileFinder, FindFileCiMatchesExactName) {
    auto d = make_test_dir("exact", {"THEATER.MAP"});
    auto p = find_file_ci(d, "THEATER.MAP");
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), "THEATER.MAP");
    fs::remove_all(d);
}

TEST(FileFinder, FindFileCiMatchesCaseInsensitive) {
    // The file ships as "FArtILES.PAL" (mixed case). A caller asking for
    // "fartiles.pal" should still find it.
    auto d = make_test_dir("ci", {"FArtILES.PAL"});
    auto p = find_file_ci(d, "fartiles.pal");
    EXPECT_FALSE(p.empty());
    // The returned path preserves the on-disk case (the caller can use
    // it directly even on case-sensitive filesystems).
    EXPECT_EQ(p.filename(), "FArtILES.PAL");
    fs::remove_all(d);
}

TEST(FileFinder, FindFileCiReturnsEmptyOnMissing) {
    auto d = make_test_dir("missing", {"THEATER.MAP"});
    auto p = find_file_ci(d, "TEXTURE.BIN");
    EXPECT_TRUE(p.empty());
    fs::remove_all(d);
}

TEST(FileFinder, FindFileCiReturnsEmptyOnMissingDir) {
    auto p = find_file_ci("/no/such/dir/here", "FALCON4.ct");
    EXPECT_TRUE(p.empty());
}

TEST(FileFinder, FindFileCiWithVariantsTriesEachInOrder) {
    auto d = make_test_dir("variants", {"KoreaObj.DXH"});
    // The caller prefers HDR but accepts DXH; HDR is absent, so the
    // second variant (DXH) should be returned.
    auto p = find_file_ci_with_variants(d, {"KoreaObj.HDR", "KoreaObj.DXH"});
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), "KoreaObj.DXH");
    fs::remove_all(d);
}

TEST(FileFinder, FindFileCiWithVariantsReturnsFirstMatch) {
    auto d = make_test_dir("variants_first", {"KoreaObj.HDR", "KoreaObj.DXH"});
    // Both exist — the first variant wins.
    auto p = find_file_ci_with_variants(d, {"KoreaObj.HDR", "KoreaObj.DXH"});
    EXPECT_EQ(p.filename(), "KoreaObj.HDR");
    fs::remove_all(d);
}

TEST(FileFinder, FindSubdirCiMatchesCaseInsensitive) {
    auto d = make_test_dir("subdir_ci", {});
    touch_subdir(d, "terrdata");
    auto p = find_subdir_ci(d, "TERRDATA");
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), "terrdata");
    fs::remove_all(d);
}

TEST(FileFinder, FindFileCiInDirsSearchesInOrder) {
    auto d1 = make_test_dir("in_dirs_1", {});
    auto d2 = make_test_dir("in_dirs_2", {"FALCON4.ct"});
    auto p = find_file_ci_in_dirs({d1, d2}, "FALCON4.ct");
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.parent_path(), d2);
    fs::remove_all(d1.parent_path());
}

TEST(FileFinder, FindFileCiInDirsReturnsEmptyWhenNotFoundInAny) {
    auto d1 = make_test_dir("in_dirs_empty_1", {});
    auto d2 = make_test_dir("in_dirs_empty_2", {});
    auto p = find_file_ci_in_dirs({d1, d2}, "FALCON4.ct");
    EXPECT_TRUE(p.empty());
    fs::remove_all(d1.parent_path());
}

TEST(FileFinder, FindFileCiInDirsWithVariantsSearchesAllCombinations) {
    auto d1 = make_test_dir("in_dirs_var_1", {});
    auto d2 = make_test_dir("in_dirs_var_2", {"KoreaObj.DXL"});
    auto p = find_file_ci_in_dirs_with_variants(
        {d1, d2}, {"KoreaObj.LOD", "KoreaObj.DXL"});
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), "KoreaObj.DXL");
    EXPECT_EQ(p.parent_path(), d2);
    fs::remove_all(d1.parent_path());
}

TEST(FileFinder, FindFileByExtensionCiMatchesExactExtension) {
    auto d = make_test_dir("ext_exact", {"Falcon4.OCD"});
    auto base = d / "Falcon4";  // base_path WITHOUT the extension
    auto p = find_file_by_extension_ci(base, "OCD");
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), "Falcon4.OCD");
    fs::remove_all(d);
}

TEST(FileFinder, FindFileByExtensionCiMatchesCaseInsensitive) {
    // The file ships as "Falcon4.ocd" (lowercase); the caller asks for
    // extension "OCD" (uppercase). The case-insensitive scan in the
    // parent dir should find it.
    auto d = make_test_dir("ext_ci", {"Falcon4.ocd"});
    auto base = d / "Falcon4";
    auto p = find_file_by_extension_ci(base, "OCD");
    EXPECT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), "Falcon4.ocd");
    fs::remove_all(d);
}

TEST(FileFinder, FindFileByExtensionCiReturnsEmptyOnMissing) {
    auto d = make_test_dir("ext_missing", {});
    auto base = d / "Falcon4";
    auto p = find_file_by_extension_ci(base, "OCD");
    EXPECT_TRUE(p.empty());
    fs::remove_all(d);
}

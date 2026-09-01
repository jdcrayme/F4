// f4-world/tests/test_world_assets.cpp
//
// Tests for the asset-pipeline load_terrain_via_assets() overload.

#include <f4/world/detail/world_state.hpp>
#include <f4/assets/asset_root.hpp>
#include <f4/assets/manifest.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::world;
using namespace f4::assets;
namespace fs = std::filesystem;

namespace {

fs::path make_data_dir(const std::string& suffix) {
    auto p = fs::temp_directory_path() / "f4_world_assets_test" / suffix;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

const char* kManifest = R"({
  "f4": { "v": 1, "generator": "test" },
  "data_dir": "Data/",
  "assets": [
    {
      "id": "theater:korea",
      "path": "Theater/korea/terrain.json",
      "format_version": 1,
      "capabilities": [
        { "name": "map", "status": "present" }
      ],
      "sources": []
    }
  ]
})";

const char* kTerrainJson = R"({
  "theater": "korea",
  "width": 4,
  "height": 4,
  "tile_types": [0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 0, 1, 2, 3, 4]
})";

} // namespace

TEST(WorldAssets, LoadTerrainViaAssetsResolvesManifest) {
    auto p = make_data_dir("resolve");
    write_file(p / "manifest.json", kManifest);
    write_file(p / "Theater/korea/terrain.json", kTerrainJson);

    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());

    WorldState ws;
    ws.terrain_file = "@asset:theater:korea";
    EXPECT_NO_THROW(ws.load_terrain_via_assets(*root));
    EXPECT_TRUE(ws.terrain_loaded);

    fs::remove_all(p);
}

TEST(WorldAssets, LoadTerrainViaAssetsThrowsOnLegacyForm) {
    auto p = make_data_dir("legacy_form");
    write_file(p / "manifest.json", kManifest);
    write_file(p / "Theater/korea/terrain.json", kTerrainJson);

    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());

    WorldState ws;
    ws.terrain_file = "korea.terrain.json";
    EXPECT_THROW(ws.load_terrain_via_assets(*root), std::runtime_error);

    fs::remove_all(p);
}

TEST(WorldAssets, LoadTerrainViaAssetsThrowsWhenAssetNotInManifest) {
    auto p = make_data_dir("missing_asset");
    write_file(p / "manifest.json", kManifest);
    write_file(p / "Theater/korea/terrain.json", kTerrainJson);

    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());

    WorldState ws;
    ws.terrain_file = "@asset:theater:nope";
    EXPECT_THROW(ws.load_terrain_via_assets(*root), std::runtime_error);

    fs::remove_all(p);
}

TEST(WorldAssets, LoadTerrainViaAssetsThrowsWhenFileMissing) {
    auto p = make_data_dir("missing_file");
    write_file(p / "manifest.json", kManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());

    WorldState ws;
    ws.terrain_file = "@asset:theater:korea";
    EXPECT_THROW(ws.load_terrain_via_assets(*root), std::runtime_error);

    fs::remove_all(p);
}

TEST(WorldAssets, LoadTerrainViaAssetsThrowsWhenNoTerrainFile) {
    auto p = make_data_dir("no_ref");
    write_file(p / "manifest.json", kManifest);
    auto root = AssetRoot::at(p);
    ASSERT_TRUE(root.has_value());

    WorldState ws;
    ws.terrain_file.clear();
    EXPECT_THROW(ws.load_terrain_via_assets(*root), std::runtime_error);

    fs::remove_all(p);
}

TEST(WorldAssets, LegacyLoadTerrainStillWorks) {
    auto p = make_data_dir("legacy");
    write_file(p / "korea.terrain.json", kTerrainJson);

    WorldState ws;
    ws.terrain_file = "korea.terrain.json";
    EXPECT_NO_THROW(ws.load_terrain(p));
    EXPECT_TRUE(ws.terrain_loaded);

    fs::remove_all(p);
}

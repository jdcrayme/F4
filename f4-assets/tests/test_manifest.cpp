// f4-assets/tests/test_manifest.cpp

#include <f4/assets/manifest.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace f4::assets;

namespace {

const char* kSampleManifest = R"({
  "f4": { "v": 1, "generator": "f4import 0.4.0" },
  "data_dir": "Data/",
  "assets": [
    {
      "id": "koreaobj:00002",
      "path": "Models/koreaobj/00002.gltf",
      "format_version": 1,
      "capabilities": [
        { "name": "dofs", "status": "present", "count": 9 },
        { "name": "anchors", "status": "unknown" },
        { "name": "slots", "status": "none" }
      ],
      "sources": [
        { "path": "terrdata/objects/KoreaObj.HDR", "role": "art", "sha256": "abc123" },
        { "path": "FALCON4.ct", "role": "classes", "sha256": "deadbeef" }
      ]
    },
    {
      "id": "theater:korea",
      "path": "Theater/korea/theater.json",
      "format_version": 1,
      "capabilities": [],
      "sources": [ ]
    }
  ]
}
)";

} // namespace

TEST(Manifest, ReadsSchemaVersionAndEnvelope) {
    Manifest m = read_manifest(kSampleManifest);
    EXPECT_EQ(m.format_version, kManifestFormatVersion);
    EXPECT_EQ(m.data_dir, "Data/");
    EXPECT_EQ(m.assets.size(), 2u);
}

TEST(Manifest, ReadsAssetFields) {
    Manifest m = read_manifest(kSampleManifest);
    const AssetEntry* e = m.find(AssetId{AssetFamily::koreaobj, "00002"});
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->path, "Models/koreaobj/00002.gltf");
    EXPECT_EQ(e->format_version, 1);
    EXPECT_EQ(e->sources.size(), 2u);
    EXPECT_EQ(e->sources[0].path, "terrdata/objects/KoreaObj.HDR");
    EXPECT_EQ(e->sources[0].role, "art");
    EXPECT_EQ(e->sources[0].sha256, "abc123");
}

TEST(Manifest, ReadsCapabilities) {
    Manifest m = read_manifest(kSampleManifest);
    const AssetEntry* e = m.find(AssetId{AssetFamily::koreaobj, "00002"});
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(e->capabilities.size(), 3u);
    const Capability* dofs = e->find_capability("dofs");
    ASSERT_NE(dofs, nullptr);
    EXPECT_TRUE(dofs->is_present());
    EXPECT_EQ(dofs->count.value_or(-1), 9);
    const Capability* anchors = e->find_capability("anchors");
    ASSERT_NE(anchors, nullptr);
    EXPECT_TRUE(anchors->is_unknown());
    EXPECT_FALSE(anchors->count.has_value());
    const Capability* slots = e->find_capability("slots");
    ASSERT_NE(slots, nullptr);
    EXPECT_TRUE(slots->is_none());
}

TEST(Manifest, CapabilityLookupDistinguishesUnknownAsset) {
    Manifest m = read_manifest(kSampleManifest);
    CapabilityLookup cl = m.capability_for(
        AssetId{AssetFamily::koreaobj, "99999"}, "dofs");
    EXPECT_FALSE(cl.asset_known);
    EXPECT_EQ(cl.cap, nullptr);
}

TEST(Manifest, CapabilityLookupDistinguishesUnknownCapability) {
    Manifest m = read_manifest(kSampleManifest);
    CapabilityLookup cl = m.capability_for(
        AssetId{AssetFamily::koreaobj, "00002"}, "rocket-launcher");
    EXPECT_TRUE(cl.asset_known);
    EXPECT_EQ(cl.cap, nullptr);
}

TEST(Manifest, CapabilityLookupResolvesPresent) {
    Manifest m = read_manifest(kSampleManifest);
    CapabilityLookup cl = m.capability_for(
        AssetId{AssetFamily::koreaobj, "00002"}, "dofs");
    EXPECT_TRUE(cl.asset_known);
    ASSERT_NE(cl.cap, nullptr);
    EXPECT_TRUE(cl.cap->is_present());
}

TEST(Manifest, RoundTripPreservesEverything) {
    Manifest m = read_manifest(kSampleManifest);
    std::string emitted = write_manifest(m);
    Manifest m2 = read_manifest(emitted);
    EXPECT_EQ(m2.format_version, m.format_version);
    EXPECT_EQ(m2.data_dir, m.data_dir);
    ASSERT_EQ(m2.assets.size(), m.assets.size());
    EXPECT_EQ(m2.assets[0].id, m.assets[0].id);
    EXPECT_EQ(m2.assets[0].path, m.assets[0].path);
    EXPECT_EQ(m2.assets[0].format_version, m.assets[0].format_version);
    ASSERT_EQ(m2.assets[0].capabilities.size(), m.assets[0].capabilities.size());
    EXPECT_EQ(m2.assets[0].capabilities[0].name, m.assets[0].capabilities[0].name);
    EXPECT_EQ(m2.assets[0].capabilities[0].status, m.assets[0].capabilities[0].status);
    EXPECT_EQ(m2.assets[0].capabilities[0].count, m.assets[0].capabilities[0].count);
    EXPECT_EQ(m2.assets[0].sources.size(), m.assets[0].sources.size());
    EXPECT_EQ(m2.assets[0].sources[0].sha256, m.assets[0].sources[0].sha256);
}

TEST(Manifest, RejectsMissingEnvelope) {
    EXPECT_THROW(read_manifest(R"({ "data_dir": "Data/" })"), std::runtime_error);
}

TEST(Manifest, RejectsWrongSchemaVersion) {
    const std::string bad = R"({ "f4": { "v": 99 }, "data_dir": "Data/", "assets": [] })";
    EXPECT_THROW(read_manifest(bad), std::runtime_error);
}

TEST(Manifest, AcceptsUnknownTopLevelFields) {
    const std::string ok = R"({ "f4": { "v": 1 }, "data_dir": "Data/", "assets": [], "future_field": "ignored" })";
    EXPECT_NO_THROW((void)read_manifest(ok));
}

TEST(Manifest, EmptyAssetsArray) {
    const std::string empty = R"({ "f4": { "v": 1 }, "data_dir": "Data/", "assets": [] })";
    Manifest m = read_manifest(empty);
    EXPECT_TRUE(m.assets.empty());
}

TEST(Manifest, CapabilityStatusRoundTrip) {
    for (CapabilityStatus s : {CapabilityStatus::present,
                              CapabilityStatus::none,
                              CapabilityStatus::unknown}) {
        std::string_view name = capability_status_to_string(s);
        EXPECT_EQ(capability_status_from_string(name), s)
            << "round-trip failed for " << name;
    }
    EXPECT_EQ(capability_status_from_string("future-state"), CapabilityStatus::unknown);
}

TEST(Manifest, FileWriteReadRoundTrip) {
    Manifest m = read_manifest(kSampleManifest);
    auto tmp = std::filesystem::temp_directory_path() / "f4_assets_manifest_test";
    std::filesystem::create_directories(tmp);
    auto path = (tmp / "manifest.json").string();
    EXPECT_NO_THROW(write_manifest_file(path, m));
    Manifest m2 = read_manifest_file(path);
    EXPECT_EQ(m2.assets.size(), m.assets.size());
    std::filesystem::remove_all(tmp);
}

TEST(Manifest, AtomicWriteDoesNotLeaveTempOnSuccess) {
    Manifest m = read_manifest(kSampleManifest);
    auto tmp = std::filesystem::temp_directory_path() / "f4_assets_atomic_test";
    std::filesystem::create_directories(tmp);
    auto path = (tmp / "manifest.json").string();
    EXPECT_NO_THROW(write_manifest_file(path, m));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"))
        << "atomic rename should leave no .tmp file behind";
    std::filesystem::remove_all(tmp);
}

// ============================================================================
// Task 58 (NO_BINARY_RUNTIME_PLAN Tranche 0e): legacy fingerprint schema.
//
// The Tranche 0a manifests (generate_manifest.py ≤ 0a) carry
// {path, size_bytes, sha256, fnv1a_64} with NO id. The reader derives ids
// from paths (the convention mirrored by the upgraded generator) so the
// committed manifest resolves without regeneration. Explicit ids are kept
// as-is. Fingerprints round-trip through the writer.
// ============================================================================

namespace {
const char* kLegacyFingerprintManifest = R"({
  "f4": { "v": 1 },
  "format": "f4-data-manifest",
  "version": 1,
  "generated_at": "2026-09-05T12:16:43Z",
  "source_install": "D:\\SteamLibrary\\steamapps\\common\\Falcon 4.0",
  "theater": "korea",
  "save": "save1",
  "data_dir": "Data/",
  "excluded_dirs": ["Models"],
  "assets": [
    { "path": "Aircraft/f16.json", "size_bytes": 11413,
      "sha256": "aa11", "fnv1a_64": "bb22" },
    { "path": "Aircraft/Mig19.json", "size_bytes": 1, "sha256": "cc33" },
    { "path": "Classes/falcon4.ct.json", "size_bytes": 305215 },
    { "path": "SimData/braindata.json", "size_bytes": 900 },
    { "path": "SimData/visualdata.json", "size_bytes": 901 },
    { "path": "Theater/korea/terrain.json", "size_bytes": 85818 },
    { "path": "World/korea.world.json", "size_bytes": 11846124 },
    { "path": "Models/koreaobj/00042.gltf", "size_bytes": 555 },
    { "path": "Misc/unmapped.txt", "size_bytes": 1 }
  ]
})";
} // namespace

TEST(Manifest, LegacyFingerprintEntriesDeriveIdsFromPaths) {
    const Manifest m = read_manifest(kLegacyFingerprintManifest);
    ASSERT_EQ(m.assets.size(), 9u);
    EXPECT_EQ(m.theater, "korea");
    EXPECT_EQ(m.save, "save1");

    EXPECT_EQ(m.find(AssetId{AssetFamily::aircraft, "f16"})->path, "Aircraft/f16.json");
    EXPECT_EQ(m.find(AssetId{AssetFamily::aircraft, "mig19"})->path, "Aircraft/Mig19.json")
        << "uppercase stems lowercase into the local-id";
    EXPECT_EQ(m.find(AssetId{AssetFamily::class_, "falcon4.ct"})->path,
              "Classes/falcon4.ct.json");
    EXPECT_EQ(m.find(AssetId{AssetFamily::simdata, "braindata"})->path,
              "SimData/braindata.json");
    EXPECT_EQ(m.find(AssetId{AssetFamily::theater, "korea"})->path,
              "Theater/korea/terrain.json");
    EXPECT_EQ(m.find(AssetId{AssetFamily::campaign, "save1"})->path,
              "World/korea.world.json")
        << "World/*.world.json takes the campaign id from the manifest's save field";
    EXPECT_EQ(m.find(AssetId{AssetFamily::koreaobj, "00042"})->path,
              "Models/koreaobj/00042.gltf");
    // No convention match — the entry stays listed but unaddressable.
    EXPECT_EQ(m.find(AssetId{AssetFamily::tileset, "unmapped"}), nullptr);
    EXPECT_FALSE(m.assets[8].id.valid());
}

TEST(Manifest, LegacyFingerprintFieldsRoundTrip) {
    const Manifest m = read_manifest(kLegacyFingerprintManifest);
    const AssetEntry& f16 = *m.find(AssetId{AssetFamily::aircraft, "f16"});
    ASSERT_TRUE(f16.has_fingerprints());
    EXPECT_EQ(*f16.size_bytes, 11413u);
    EXPECT_EQ(*f16.sha256, "aa11");
    EXPECT_EQ(*f16.fnv1a_64, "bb22");
    // The entry without sha/fnv still counts (size alone is a fingerprint).
    const AssetEntry& ct = *m.find(AssetId{AssetFamily::class_, "falcon4.ct"});
    ASSERT_TRUE(ct.has_fingerprints());
    EXPECT_FALSE(ct.sha256.has_value());

    const std::string json = write_manifest(m);
    const Manifest m2 = read_manifest(json);
    ASSERT_EQ(m2.assets.size(), 9u);
    const AssetEntry& f16b = *m2.find(AssetId{AssetFamily::aircraft, "f16"});
    EXPECT_EQ(f16b.path, f16.path);
    EXPECT_EQ(*f16b.size_bytes, 11413u);
    EXPECT_EQ(*f16b.sha256, "aa11");
    EXPECT_EQ(*f16b.fnv1a_64, "bb22");
    EXPECT_EQ(m2.theater, "korea");
    EXPECT_EQ(m2.save, "save1");
}

TEST(Manifest, ExplicitIdEntriesAreKeptAsIs) {
    // The new-schema sample already carries ids; derivation must NOT
    // rewrite them (id takes precedence over path-derived guesses).
    const Manifest m = read_manifest(kSampleManifest);
    const AssetEntry* korea = m.find(AssetId{AssetFamily::theater, "korea"});
    ASSERT_NE(korea, nullptr);
    EXPECT_EQ(korea->path, "Theater/korea/theater.json");
}

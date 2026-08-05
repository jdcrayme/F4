// f4-models/tests/test_models.cpp
//
// Unit tests for f4-models library.
// Uses the KoreaObj.HDR fixture from the temp directory.

#include <f4/models/f4_models.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;

namespace {

/// Get the fixture directory (set by CMake).
std::string fixture_dir() {
    return MODELS_FIXTURE_DIR;
}

/// Check if the HDR fixture exists (may not be available in CI).
bool has_hdr_fixture() {
    auto dir = fixture_dir();
    return fs::exists(dir + "KoreaObj.HDR");
}

/// Get HDR fixture path.
fs::path hdr_path() { return fixture_dir() + "KoreaObj.HDR"; }
/// Get LOD fixture path.
fs::path lod_path() { return fixture_dir() + "KoreaObj.LOD"; }

} // anonymous namespace

// ── Type Tests ─────────────────────────────────────────────────────────────

TEST(ModelsTypes, BspNodeTypeName) {
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::BRoot), "BRoot");
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::BSlotNode), "BSlotNode");
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::BSplitterNode), "BSplitterNode");
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::Unknown), "Unknown");
}

TEST(ModelsTypes, PolyTypeName) {
    EXPECT_STREQ(f4::models::poly_type_name(f4::models::PolyType::Tex), "Tex");
    EXPECT_STREQ(f4::models::poly_type_name(f4::models::PolyType::ATexGL), "ATexGL");
}

TEST(ModelsTypes, BoundingBox) {
    f4::models::BoundingBox bb{-10, 10, -5, 5, 0, 20};
    EXPECT_FLOAT_EQ(bb.center_x(), 0.0f);
    EXPECT_FLOAT_EQ(bb.center_y(), 0.0f);
    EXPECT_FLOAT_EQ(bb.center_z(), 10.0f);
    EXPECT_FLOAT_EQ(bb.extent_x(), 10.0f);
    EXPECT_FLOAT_EQ(bb.extent_y(), 5.0f);
    EXPECT_FLOAT_EQ(bb.extent_z(), 10.0f);
}

TEST(ModelsTypes, Vec3Equality) {
    f4::models::Vec3 a{1, 2, 3};
    f4::models::Vec3 b{1, 2, 3};
    f4::models::Vec3 c{1, 2, 4};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ── ModelRecord Tests ─────────────────────────────────────────────────────

TEST(ModelRecord, VisualClass) {
    f4::models::ModelRecord air;
    air.n_slots = 9;
    air.n_dof = 11;  // legacy count (n_dofs=0 in vanilla HDR)
    EXPECT_EQ(air.visual_class(), "air");
    EXPECT_EQ(air.effective_dofs(), 11);

    f4::models::ModelRecord ground;
    ground.n_slots = 2;
    ground.n_dof = 3;
    EXPECT_EQ(ground.visual_class(), "ground");

    f4::models::ModelRecord feature;
    feature.n_slots = 0;
    feature.n_dof = 0;
    EXPECT_EQ(feature.visual_class(), "feature");
}

// ── TextureEntry Tests ────────────────────────────────────────────────────

TEST(TextureEntry, Filename) {
    f4::models::TextureEntry te;
    strncpy(te.raw.data(), "runway1.jpg", 12);
    EXPECT_EQ(te.filename(), "runway1.jpg");
}

// ── ModelDatabase Tests (require fixture) ─────────────────────────────────

class ModelsFixtureTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!has_hdr_fixture()) GTEST_SKIP() << "HDR fixture not available";
    }
};

TEST_F(ModelsFixtureTest, LoadHdrOnly) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << "HDR load error: " << err;
    EXPECT_TRUE(db.valid());
    EXPECT_EQ(db.n_models(), 1342);
    EXPECT_GT(db.n_lod_entries(), 0);
    EXPECT_GT(db.n_textures(), 0);
}

TEST_F(ModelsFixtureTest, LoadHdrAndLod) {
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << "Load error: " << err;
    EXPECT_TRUE(db.valid());
    EXPECT_EQ(db.n_models(), 1342);
}

TEST_F(ModelsFixtureTest, ModelAccess) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    // First model
    auto* m0 = db.model(0);
    ASSERT_NE(m0, nullptr);
    EXPECT_EQ(m0->index, 0);
    EXPECT_GE(m0->radius, 0);

    // Out of range
    EXPECT_EQ(db.model(-1), nullptr);
    EXPECT_EQ(db.model(db.n_models()), nullptr);
}

TEST_F(ModelsFixtureTest, ModelDataIntegrity) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    // Model 1 should be an aircraft-like model (F-16)
    auto* m1 = db.model(1);
    ASSERT_NE(m1, nullptr);
    EXPECT_GT(m1->radius, 0);
    EXPECT_GT(m1->n_slots, 0);
    EXPECT_GT(m1->effective_dofs(), 0);  // uses max(legacy, extended)
    EXPECT_EQ(m1->visual_class(), "air");
}

TEST_F(ModelsFixtureTest, QueryBySlots) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto air_models = db.find_by_slots(4, 100);
    EXPECT_GT(air_models.size(), 0);

    auto feature_models = db.find_by_slots(0, 0);
    EXPECT_GT(feature_models.size(), 0);
}

TEST_F(ModelsFixtureTest, QueryByRadius) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto large = db.find_by_radius(50.0f, 1000.0f);
    EXPECT_GT(large.size(), 0);
}

TEST_F(ModelsFixtureTest, QueryByClass) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto air = db.find_by_class("air");
    auto ground = db.find_by_class("ground");
    auto feature = db.find_by_class("feature");

    EXPECT_GT(air.size(), 0);
    EXPECT_GT(ground.size(), 0);
    EXPECT_GT(feature.size(), 0);

    // All models should be accounted for
    EXPECT_EQ(air.size() + ground.size() + feature.size(),
              static_cast<std::size_t>(db.n_models()));
}

TEST_F(ModelsFixtureTest, ParseModelLod) {
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    // Parse model 1 (should have BSP geometry)
    auto parse_err = db.parse_model(1);
    EXPECT_TRUE(parse_err.empty()) << "Parse error: " << parse_err;
}

TEST_F(ModelsFixtureTest, Version) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_EQ(db.version(), 0x03087000u);
}

TEST_F(ModelsFixtureTest, LodNames) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;
    // Vanilla F4 KoreaObj.HDR should not have LOD names
    EXPECT_FALSE(db.has_lod_names());
}

// ── JSON Export Tests ──────────────────────────────────────────────────────

TEST_F(ModelsFixtureTest, ModelListJson) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto json = f4::models::model_list_json(db);
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("\"command\""), std::string::npos);
    EXPECT_NE(json.find("\"n_models\""), std::string::npos);
    EXPECT_NE(json.find("1342"), std::string::npos);
}

TEST_F(ModelsFixtureTest, ModelRecordJson) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto* m = db.model(1);
    ASSERT_NE(m, nullptr);
    auto json = f4::models::model_record_json(*m);
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("\"radius\""), std::string::npos);
}

// ── File Finder Tests ──────────────────────────────────────────────────────

TEST(FileFinder, FindInTempDir) {
    // Try to find the fixture files
    fs::path temp_dir = fixture_dir();
    auto [hdr, lod] = f4::models::ModelDatabase::find_koreaobj_files(temp_dir);

    if (fs::exists(temp_dir / "KoreaObj.HDR")) {
        EXPECT_FALSE(hdr.empty());
        EXPECT_FALSE(lod.empty());
    }
}

// ── DX Format Detection ───────────────────────────────────────────────────

TEST(DxDetection, Checksum) {
    // Valid DX header: lower 16 bits == complement of upper 16 bits
    // Example: 0x1234EDCB -> (0x1234 & 0xFFFF) should equal (~0xEDCB >> 0 & 0xFFFF)
    uint32_t valid_dx = 0x0000FFFF; // (0x0000) == (~0xFFFF >> 16) & 0xFFFF = 0x0000... no
    // Let's compute a valid one: version=0x1234, checksum=~0x1234=0xEDCB
    // Packed: 0xEDCB1234
    uint32_t dx = 0xEDCB1234u;
    EXPECT_TRUE((dx & 0xFFFF) == ((~dx >> 16) & 0xFFFF));

    // A BSP tag count (e.g. 308) should NOT match DX format
    uint32_t bsp = 308;
    EXPECT_FALSE((bsp & 0xFFFF) == ((~bsp >> 16) & 0xFFFF));
}

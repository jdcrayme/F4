// f4-renderer/tests/test_feature_mesh.cpp
//
// Unit tests for f4::renderer::draw_feature_mesh.
//
// Tests two layers:
//   1. Pure-config tests: draw_feature_mesh with null resources returns
//      zeroed DrawStats without crashing (no GL state touched).
//   2. GPU-context test: loads the bundled KoreaObj fixture (the same
//      one used by f4-models-viewer and the class_table_browser tests),
//      loads the bundled FALCON4.ct, calls draw_feature_mesh inside a
//      BeginMode3D/EndMode3D block, and asserts that DrawStats reports
//      at least one mesh drawn for a known-good entity_type.
//
// The GPU-context test is skipped (via GTEST_SKIP) if the fixture files
// can't be found — this happens in CI environments without the F4
// submodules checked out.

#include <f4/renderer/feature_mesh.hpp>

#include <f4/models/model_database.hpp>
#include <f4/world_convert/class_table.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

#include <filesystem>
#include <string>

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::filesystem::path find_fixture(const char* name) {
    // Search a few well-known locations relative to the test executable
    // and the source tree.
    namespace fs = std::filesystem;
    const char* candidates[] = {
        // Relative to test exe (typical CTest wd = build dir)
        "f4-models/tests/fixtures",
        "f4-world-convert/tests/fixtures",
        // Relative to source root
        "../F4/f4-models/tests/fixtures",
        "../F4/f4-world-convert/tests/fixtures",
        // Absolute fallbacks (CI paths)
        "/home/z/my-project/F4/f4-models/tests/fixtures",
        "/home/z/my-project/F4/f4-world-convert/tests/fixtures",
    };
    for (const char* c : candidates) {
        fs::path p = fs::path(c) / name;
        if (fs::exists(p)) return p;
    }
    return {};
}

// ── Layer 1: pure-config (no GL state) ──────────────────────────────────────

TEST(FeatureMeshTest, NullResources_ReturnsZeroStats) {
    f4::renderer::FeatureMeshResources res;  // all pointers null
    auto stats = f4::renderer::draw_feature_mesh(
        res, /*entity_type=*/200,
        /*enu_x=*/0.0f, /*enu_y=*/0.0f, /*enu_z=*/0.0f,
        /*facing_deg=*/0.0f);
    EXPECT_EQ(stats.draw_calls, 0);
    EXPECT_EQ(stats.meshes_drawn, 0);
    EXPECT_EQ(stats.vertices_drawn, 0u);
}

TEST(FeatureMeshTest, NullMeshCache_BuildFeatureMeshIsNoOp) {
    f4::renderer::FeatureMeshResources res;  // all pointers null
    // Should not crash even with null mesh_cache.
    f4::renderer::build_feature_mesh(res, /*vis_type=*/100);
    SUCCEED();
}

// ── Layer 2: GPU-context (requires fixture files + GL) ──────────────────────

class FeatureMeshGpuTest : public ::testing::Test {
protected:
    static bool initialized_;

    void SetUp() override {
        if (!initialized_) {
            SetConfigFlags(FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNDECORATED);
            InitWindow(256, 256, "test");
            initialized_ = true;
        }
    }

    static void TearDownTestSuite() {
        if (initialized_) {
            CloseWindow();
            initialized_ = false;
        }
    }
};

bool FeatureMeshGpuTest::initialized_ = false;

TEST_F(FeatureMeshGpuTest, DrawFeatureMesh_KnownGoodEntityType_DrawsAtLeastOneMesh) {
    namespace fs = std::filesystem;

    // Locate fixtures.
    const auto hdr = find_fixture("KoreaObj.HDR");
    const auto lod = find_fixture("KoreaObj.LOD");
    const auto tex = find_fixture("KoreaObj.TEX");
    const auto ct  = find_fixture("FALCON4.ct");
    if (hdr.empty() || lod.empty() || ct.empty()) {
        GTEST_SKIP() << "KoreaObj or FALCON4.ct fixture not found — skipping GPU test";
    }

    // Load model DB.
    f4::models::ModelDatabase db;
    auto err = db.load(hdr, lod);
    ASSERT_TRUE(err.empty()) << "ModelDatabase::load failed: " << err;
    if (!tex.empty()) {
        auto tex_err = db.load_tex(tex);
        // Not fatal if TEX is missing — meshes will just be untextured.
        (void)tex_err;
    }
    ASSERT_TRUE(db.valid());

    // Load class table.
    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(ct));
    ASSERT_TRUE(class_table.loaded());

    // Find an entity_type whose vis_type[0] is a real model.
    // We scan the class table for the first CLASS_FEATURE entry with a
    // non-zero vis_type[0]. If none is found, skip (some fixture subsets
    // might not include feature-class entries).
    uint16_t found_entity_type = 0;
    int16_t found_vis_type = 0;
    const std::size_t n_entries = class_table.size();
    for (std::size_t i = 0; i < n_entries; ++i) {
        const uint16_t entity_type = static_cast<uint16_t>(
            f4::world_convert::VU_LAST_ENTITY_TYPE + i);
        const auto* entry = class_table.lookup(entity_type);
        if (!entry) continue;
        // Prefer features (classInfo_[1] == CLASS_FEATURE), but accept
        // any entry with a non-zero vis_type[0] that resolves to a real
        // model in the DB.
        const int16_t vis0 = class_table.vis_type_for(entity_type, 0);
        if (vis0 <= 0) continue;
        if (!db.model(vis0)) continue;
        found_entity_type = entity_type;
        found_vis_type = vis0;
        if (entry->cls == f4::world_convert::CLASS_FEATURE) break;
    }
    if (found_entity_type == 0) {
        GTEST_SKIP() << "No class-table entry with a resolvable vis_type[0] found";
    }

    // Build resources.
    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    std::unordered_map<int, f4::renderer::FeatureMeshCacheEntry> mesh_cache;
    ::Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

    f4::renderer::FeatureMeshResources res{};
    res.model_db = &db;
    res.class_table = &class_table;
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
    res.mesh_cache = &mesh_cache;
    res.default_material = &default_mat;

    // Draw inside BeginMode3D.
    Camera3D cam = {};
    cam.position = {0.0f, 100.0f, 0.0f};
    cam.target   = {0.0f, 0.0f, 0.0f};
    cam.up       = {0.0f, 0.0f, -1.0f};
    cam.fovy     = 200.0f;
    cam.projection = CAMERA_ORTHOGRAPHIC;

    BeginDrawing();
    ClearBackground(BLACK);
    BeginMode3D(cam);
    auto stats = f4::renderer::draw_feature_mesh(
        res, found_entity_type,
        /*enu_x=*/0.0f, /*enu_y=*/0.0f, /*enu_z=*/0.0f,
        /*facing_deg=*/0.0f);
    EndMode3D();
    EndDrawing();

    EXPECT_GT(stats.meshes_drawn, 0)
        << "Expected at least one mesh to be drawn for entity_type="
        << found_entity_type << " (vis_type=" << found_vis_type << ")";
    EXPECT_GT(stats.vertices_drawn, 0u);

    // Cleanup.
    tex_cache.unload_all();
    for (auto& [_, entry] : mesh_cache) {
        for (auto& me : entry.meshes) {
            UnloadMesh(me.mesh);
        }
    }
    UnloadMaterial(default_mat);
}

TEST_F(FeatureMeshGpuTest, DrawFeatureMesh_UnknownEntityType_ReturnsZeroStats) {
    namespace fs = std::filesystem;
    const auto hdr = find_fixture("KoreaObj.HDR");
    const auto lod = find_fixture("KoreaObj.LOD");
    const auto ct  = find_fixture("FALCON4.ct");
    if (hdr.empty() || lod.empty() || ct.empty()) {
        GTEST_SKIP() << "Fixtures not found";
    }

    f4::models::ModelDatabase db;
    ASSERT_TRUE(db.load(hdr, lod).empty());
    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(ct));

    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    std::unordered_map<int, f4::renderer::FeatureMeshCacheEntry> mesh_cache;
    ::Material default_mat = LoadMaterialDefault();

    f4::renderer::FeatureMeshResources res{};
    res.model_db = &db;
    res.class_table = &class_table;
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
    res.mesh_cache = &mesh_cache;
    res.default_material = &default_mat;

    // 65535 is way past any real entity_type — should yield no vis_type.
    Camera3D cam = {};
    cam.position = {0.0f, 100.0f, 0.0f};
    cam.target   = {0.0f, 0.0f, 0.0f};
    cam.up       = {0.0f, 0.0f, -1.0f};
    cam.fovy     = 200.0f;
    cam.projection = CAMERA_ORTHOGRAPHIC;

    BeginDrawing();
    ClearBackground(BLACK);
    BeginMode3D(cam);
    auto stats = f4::renderer::draw_feature_mesh(
        res, /*entity_type=*/65535,
        0.0f, 0.0f, 0.0f, 0.0f);
    EndMode3D();
    EndDrawing();

    EXPECT_EQ(stats.draw_calls, 0);
    EXPECT_EQ(stats.meshes_drawn, 0);
    EXPECT_EQ(stats.vertices_drawn, 0u);

    tex_cache.unload_all();
    for (auto& [_, entry] : mesh_cache) {
        for (auto& me : entry.meshes) {
            UnloadMesh(me.mesh);
        }
    }
    UnloadMaterial(default_mat);
}

// V-3DLIVE: the vis-type-DIRECT entry point — the live session pass.
// (1) A known-good vis_type draws the same meshes draw_feature_mesh
//     would (the shared path), WITHOUT needing the class table.
// (2) vis_type 0 ("never resolved") draws nothing.
TEST_F(FeatureMeshGpuTest, DrawVisTypeMesh_DirectAndZero) {
    namespace fs = std::filesystem;
    const auto hdr = find_fixture("KoreaObj.HDR");
    const auto lod = find_fixture("KoreaObj.LOD");
    const auto ct  = find_fixture("FALCON4.ct");
    if (hdr.empty() || lod.empty() || ct.empty()) {
        GTEST_SKIP() << "Fixtures not found";
    }

    f4::models::ModelDatabase db;
    ASSERT_TRUE(db.load(hdr, lod).empty());
    f4::world_convert::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load(ct));

    // Find an entity_type whose vis_type[0] resolves a real model —
    // the SAME scan the known-good feature test uses (prefer
    // CLASS_FEATURE entries; the fixture subset models those).
    uint16_t found_entity_type = 0;
    int16_t found_vis_type = 0;
    const std::size_t n_entries = class_table.size();
    for (std::size_t i = 0; i < n_entries; ++i) {
        const uint16_t entity_type = static_cast<uint16_t>(
            f4::world_convert::VU_LAST_ENTITY_TYPE + i);
        const auto* entry = class_table.lookup(entity_type);
        if (!entry) continue;
        const int16_t vis0 = class_table.vis_type_for(entity_type, 0);
        if (vis0 <= 0) continue;
        if (!db.model(vis0)) continue;
        found_entity_type = entity_type;
        found_vis_type = vis0;
        if (entry->cls == f4::world_convert::CLASS_FEATURE) break;
    }
    if (found_vis_type <= 0) {
        GTEST_SKIP() << "No resolvable vis_type in the fixture subset";
    }

    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    std::unordered_map<int, f4::renderer::FeatureMeshCacheEntry> mesh_cache;
    ::Material default_mat = LoadMaterialDefault();

    f4::renderer::FeatureMeshResources res{};
    res.model_db = &db;
    // NOTE: no class_table — the direct path must not need one.
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
    res.mesh_cache = &mesh_cache;
    res.default_material = &default_mat;

    Camera3D cam = {};
    cam.position = {0.0f, 100.0f, 0.0f};
    cam.target   = {0.0f, 0.0f, 0.0f};
    cam.up       = {0.0f, 0.0f, -1.0f};
    cam.fovy     = 200.0f;
    cam.projection = CAMERA_ORTHOGRAPHIC;

    BeginDrawing();
    ClearBackground(BLACK);
    BeginMode3D(cam);
    const auto stats = f4::renderer::draw_vis_type_mesh(
        res, found_vis_type, 100.0f, 200.0f, 0.0f, /*facing=*/45.0f);
    const auto zero = f4::renderer::draw_vis_type_mesh(
        res, /*vis_type=*/0, 0.0f, 0.0f, 0.0f, 0.0f);
    EndMode3D();
    EndDrawing();

    EXPECT_GT(stats.meshes_drawn, 0)
        << "vis_type " << found_vis_type << " drew nothing";
    EXPECT_GT(stats.vertices_drawn, 0u);
    EXPECT_EQ(zero.meshes_drawn, 0);
    EXPECT_EQ(zero.vertices_drawn, 0u);

    tex_cache.unload_all();
    for (auto& [_, entry] : mesh_cache) {
        for (auto& me : entry.meshes) {
            UnloadMesh(me.mesh);
        }
    }
    UnloadMaterial(default_mat);
}

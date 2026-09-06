// f4-renderer/tests/test_feature_mesh.cpp
//
// Unit tests for f4::renderer::draw_feature_mesh.
//
// Tests two layers:
//   1. Pure-config tests: draw_feature_mesh with null resources returns
//      zeroed DrawStats without crashing (no GL state touched).
//   2. GPU-context test: loads the committed glTF fixture
//      (f4-import/tests/fixtures/clean_data/Models/koreaobj — the same
//      tree the f4import round-trip tests use) + the committed JSON
//      class table (Data/Classes/falcon4.ct.json), calls
//      draw_feature_mesh inside a BeginMode3D/EndMode3D block, and
//      asserts that DrawStats reports at least one mesh drawn for a
//      known-good entity_type.
//
// Tranche 0d: the model source is the glTF export (f4import models) —
// no KoreaObj binary, no f4-models link. The GPU-context tests skip
// (GTEST_SKIP) when the fixture tree isn't available.

#include <f4/renderer/feature_mesh.hpp>

#include <f4/world_types/class_table.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

#include <cstdio>
#include <filesystem>
#include <string>

#ifndef F4_KOREAOBJ_DATA_DIR
#define F4_KOREAOBJ_DATA_DIR ""
#endif
#ifndef F4_CLASS_TABLE_JSON
#define F4_CLASS_TABLE_JSON ""
#endif

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

TEST(FeatureMeshTest, NullModelCache_BuildFeatureMeshIsNoOp) {
    f4::renderer::FeatureMeshResources res;  // all pointers null
    // Should not crash even with a null model_cache.
    f4::renderer::build_feature_mesh(res, /*vis_type=*/100);
    SUCCEED();
}

// ── Layer 2: GPU-context (requires glTF fixture files + GL) ────────────────

namespace {

std::filesystem::path koreaobj_data_dir() {
    return std::filesystem::path(F4_KOREAOBJ_DATA_DIR);
}

std::filesystem::path class_table_json() {
    return std::filesystem::path(F4_CLASS_TABLE_JSON);
}

bool gltf_model_exists(const std::filesystem::path& data_dir, int vis_type) {
    char name[16];
    std::snprintf(name, sizeof(name), "%05d", vis_type);
    return std::filesystem::exists(data_dir / "Models" / "koreaobj" /
                                   (std::string(name) + ".gltf"));
}

}  // namespace

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
    const auto data_dir = koreaobj_data_dir();
    const auto ct_path = class_table_json();
    if (data_dir.empty() ||
        !std::filesystem::exists(data_dir / "Models" / "koreaobj")) {
        GTEST_SKIP() << "glTF koreaobj fixture not found — skipping GPU test";
    }
    if (ct_path.empty() || !std::filesystem::exists(ct_path)) {
        GTEST_SKIP() << "JSON class table not found — skipping GPU test";
    }

    // Load the runtime class table (JSON — the binary decoder is not
    // linked into the runtime anymore).
    f4::world_types::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load_auto(ct_path.string()));
    ASSERT_TRUE(class_table.loaded());

    // Find an entity_type whose vis_type[0] has a glTF export in the
    // fixture tree. Prefer CLASS_FEATURE entries (the fixture trees
    // model those), but accept any entry whose model file exists.
    uint16_t found_entity_type = 0;
    int16_t found_vis_type = 0;
    const std::size_t n_entries = class_table.size();
    for (std::size_t i = 0; i < n_entries; ++i) {
        const uint16_t entity_type = static_cast<uint16_t>(
            f4::world_types::VU_LAST_ENTITY_TYPE + i);
        const auto* entry = class_table.lookup(entity_type);
        if (!entry) continue;
        const int16_t vis0 = class_table.vis_type_for(entity_type, 0);
        if (vis0 <= 0) continue;
        if (!gltf_model_exists(data_dir, vis0)) continue;
        found_entity_type = entity_type;
        found_vis_type = vis0;
        if (entry->cls == f4::world_types::CLASS_FEATURE) break;
    }
    if (found_entity_type == 0) {
        GTEST_SKIP() << "No class-table entry with a glTF fixture found";
    }

    // Build resources.
    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    f4::renderer::RuntimeModelCache model_cache;
    model_cache.set_data_dir(data_dir);
    ::Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;

    f4::renderer::FeatureMeshResources res{};
    res.model_cache = &model_cache;
    res.class_table = &class_table;
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
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
    model_cache.unload_all();
    UnloadMaterial(default_mat);
}

TEST_F(FeatureMeshGpuTest, DrawFeatureMesh_UnknownEntityType_ReturnsZeroStats) {
    const auto data_dir = koreaobj_data_dir();
    const auto ct_path = class_table_json();
    if (data_dir.empty() ||
        !std::filesystem::exists(data_dir / "Models" / "koreaobj")) {
        GTEST_SKIP() << "glTF koreaobj fixture not found";
    }
    if (ct_path.empty() || !std::filesystem::exists(ct_path)) {
        GTEST_SKIP() << "JSON class table not found";
    }

    f4::world_types::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load_auto(ct_path.string()));

    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    f4::renderer::RuntimeModelCache model_cache;
    model_cache.set_data_dir(data_dir);
    ::Material default_mat = LoadMaterialDefault();

    f4::renderer::FeatureMeshResources res{};
    res.model_cache = &model_cache;
    res.class_table = &class_table;
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
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
    model_cache.unload_all();
    UnloadMaterial(default_mat);
}

// V-3DLIVE: the vis-type-DIRECT entry point — the live session pass.
// (1) A known-good vis_type draws the same meshes draw_feature_mesh
//     would (the shared path), WITHOUT needing the class table.
// (2) vis_type 0 ("never resolved") draws nothing.
TEST_F(FeatureMeshGpuTest, DrawVisTypeMesh_DirectAndZero) {
    const auto data_dir = koreaobj_data_dir();
    const auto ct_path = class_table_json();
    if (data_dir.empty() ||
        !std::filesystem::exists(data_dir / "Models" / "koreaobj")) {
        GTEST_SKIP() << "glTF koreaobj fixture not found";
    }
    if (ct_path.empty() || !std::filesystem::exists(ct_path)) {
        GTEST_SKIP() << "JSON class table not found";
    }

    f4::world_types::ClassTable class_table;
    ASSERT_NO_THROW(class_table.load_auto(ct_path.string()));

    // Find a vis_type that has a glTF fixture — the SAME scan the
    // known-good feature test uses (prefer CLASS_FEATURE entries).
    int16_t found_vis_type = 0;
    const std::size_t n_entries = class_table.size();
    for (std::size_t i = 0; i < n_entries; ++i) {
        const uint16_t entity_type = static_cast<uint16_t>(
            f4::world_types::VU_LAST_ENTITY_TYPE + i);
        const auto* entry = class_table.lookup(entity_type);
        if (!entry) continue;
        const int16_t vis0 = class_table.vis_type_for(entity_type, 0);
        if (vis0 <= 0) continue;
        if (!gltf_model_exists(data_dir, vis0)) continue;
        found_vis_type = vis0;
        if (entry->cls == f4::world_types::CLASS_FEATURE) break;
    }
    if (found_vis_type <= 0) {
        GTEST_SKIP() << "No glTF-backed vis_type in the fixture subset";
    }

    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    f4::renderer::RuntimeModelCache model_cache;
    model_cache.set_data_dir(data_dir);
    ::Material default_mat = LoadMaterialDefault();

    f4::renderer::FeatureMeshResources res{};
    res.model_cache = &model_cache;
    // NOTE: no class_table — the direct path must not need one.
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
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
    model_cache.unload_all();
    UnloadMaterial(default_mat);
}

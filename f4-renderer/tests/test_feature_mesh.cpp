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
#include <f4/renderer/runtime_model_cache.hpp>
#include <f4/renderer/texture_cache.hpp>

#include <f4/world_types/class_table.hpp>

#include <gtest/gtest.h>
#include <raylib.h>
#include <raymath.h>

#include "display_guard.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
            if (!f4::testing::init_window_if_display(256, 256, "test", FLAG_WINDOW_HIDDEN | FLAG_WINDOW_UNDECORATED)) {
                GTEST_SKIP() << "no display available — GPU-context test skipped";
            }
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

// ── RuntimeModelCache classification (the animated-vs-static regression) ───
//
// The f4import legacy ("flat") emitter writes dof/sw/slot stub nodes as
// ORPHANS beside the geometry, and attaches the LOD mesh DIRECTLY to the
// lod:N node. A build path that classifies such documents as animated
// (has_animation_tags saw the stubs) extracted zero parts AND skipped
// the flat extraction — the model drew nothing. These tests pin the
// runtime classification for both on-disk layouts, with real GL uploads
// (build_gltf_mesh needs a context — hence the GPU fixture).

namespace {

// One-triangle glTF document (positions + uint32 indices in an external
// .bin) — the minimum the extractor accepts. `nodes_json` spliced in
// verbatim picks the layout: flat-with-stubs vs hierarchy chain.
std::filesystem::path write_model_fixture(const std::filesystem::path& dir,
                                          const char* file_stem,
                                          const std::string& nodes_json,
                                          const std::string& scene_nodes) {
    const auto model_dir = dir / "Models" / "koreaobj";
    std::filesystem::create_directories(model_dir);

    const float positions[9] = {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
    const uint32_t indices[3] = {0, 1, 2};

    const auto bin_path = model_dir / (std::string(file_stem) + ".bin");
    {
        std::ofstream bin(bin_path, std::ios::binary);
        bin.write(reinterpret_cast<const char*>(positions), sizeof(positions));
        bin.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    }

    std::string json = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [)" + scene_nodes + R"(] } ],
  "nodes": [)" + nodes_json + R"(],
  "meshes": [ { "name": "LOD_0", "primitives": [
      { "attributes": { "POSITION": 0 }, "indices": 1, "mode": 4 } ] } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR" }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 12, "target": 34963 }
  ],
  "buffers": [ { "byteLength": 48, "uri": ")" +
               std::string(file_stem) + R"(.bin" } ]
})";

    const auto gltf_path = model_dir / (std::string(file_stem) + ".gltf");
    std::ofstream out(gltf_path);
    out << json;
    return dir;
}

}  // namespace

TEST_F(FeatureMeshGpuTest, RuntimeModelCache_FlatDocWithStubTags_FallsBackToFlat) {
    if (!f4::testing::init_window_if_display(64, 64, "t", FLAG_WINDOW_HIDDEN))
        GTEST_SKIP() << "no display available";

    const auto dir = write_model_fixture(
        std::filesystem::temp_directory_path() / "f4_hybrid_model_test",
        "00099",
        // root → lod:0 (mesh DIRECT on the lod node) + orphan stubs.
        R"(    { "name": "root", "children": [1] },
    { "name": "lod:0", "mesh": 0,
      "extras": { "f4": { "v": 1, "kind": "lod", "id": "0", "level": 0 } } },
    { "name": "dof:unknown.0",
      "extras": { "f4": { "v": 1, "kind": "dof", "id": "unknown.0", "index": 0 } } },
    { "name": "sw:unknown.0",
      "extras": { "f4": { "v": 1, "kind": "sw", "id": "unknown.0", "index": 0 } } })",
        "0");

    f4::renderer::TextureCache tex_cache;
    f4::renderer::RuntimeModelCache model_cache;
    model_cache.set_data_dir(dir);
    model_cache.build_model(99, tex_cache);
    const auto* model = model_cache.lookup(99);
    ASSERT_NE(model, nullptr);
    ASSERT_TRUE(model->built);

    // The regression: these documents must classify STATIC and keep
    // their geometry in lod0_meshes. The broken classification left
    // both surfaces empty → aircraft vanished from the 3D view.
    EXPECT_FALSE(model->animated);
    EXPECT_FALSE(model->lod0_meshes.empty());
    EXPECT_TRUE(model->lod0_parts.empty());

    // Draw it the way the live views do — a built model must render,
    // not just sit in the cache.
    {
        f4::renderer::LitShader lit_shader;
        ::Material default_mat = LoadMaterialDefault();
        default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        f4::renderer::FeatureMeshResources res{};
        res.model_cache = &model_cache;
        res.texture_cache = &tex_cache;
        res.lit_shader = &lit_shader;
        res.default_material = &default_mat;

        Camera3D cam = {};
        cam.position = {0.0f, 100.0f, 0.0f};
        cam.target = {0.0f, 0.0f, 0.0f};
        cam.up = {0.0f, 0.0f, -1.0f};
        cam.fovy = 200.0f;
        cam.projection = CAMERA_ORTHOGRAPHIC;
        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode3D(cam);
        const auto st = f4::renderer::draw_vis_type_mesh(
            res, 99, 0.0f, 0.0f, 0.0f, 0.0f);
        EndMode3D();
        EndDrawing();
        EXPECT_GT(st.meshes_drawn, 0)
            << "flat-with-stubs model drew nothing through the static path";

        UnloadMaterial(default_mat);
    }

    model_cache.unload_all();
    tex_cache.unload_all();
    std::filesystem::remove_all(dir);
}

TEST_F(FeatureMeshGpuTest, RuntimeModelCache_HierarchyDoc_BuildsParts) {
    if (!f4::testing::init_window_if_display(64, 64, "t", FLAG_WINDOW_HIDDEN))
        GTEST_SKIP() << "no display available";

    const auto dir = write_model_fixture(
        std::filesystem::temp_directory_path() / "f4_hier_model_test",
        "00098",
        R"(    { "name": "root", "children": [1] },
    { "name": "lod:0",
      "extras": { "f4": { "v": 1, "kind": "lod", "id": "0", "level": 0 } },
      "children": [2] },
    { "name": "dof:gear_leg.0",
      "extras": { "f4": { "v": 1, "kind": "dof", "id": "gear_leg.0",
                          "index": 19, "channel": "gear_leg_pos.0" } },
      "children": [3] },
    { "name": "part", "mesh": 0 })",
        "0");

    f4::renderer::TextureCache tex_cache;
    f4::renderer::RuntimeModelCache model_cache;
    model_cache.set_data_dir(dir);
    model_cache.build_model(98, tex_cache);
    const auto* model = model_cache.lookup(98);
    ASSERT_NE(model, nullptr);
    ASSERT_TRUE(model->built);

    // A genuine --hierarchy document goes through the parts path.
    EXPECT_TRUE(model->animated);
    EXPECT_TRUE(model->lod0_meshes.empty());
    ASSERT_EQ(model->lod0_parts.size(), 1u);
    EXPECT_EQ(model->lod0_parts[0].entry.mesh.triangleCount, 1);
    ASSERT_EQ(model->lod0_parts[0].node_chain.size(), 2u);  // dof + mesh node
    EXPECT_FALSE(model->anim_map.empty());
    // The document must stay alive on the model: the animated draw path
    // evaluates node chains against it every frame. (A build path that
    // dropped it produced a null doc → null deref in eval_tagged_local
    // the first time an animated model was actually drawn.)
    ASSERT_NE(model->doc, nullptr);

    // DRAW it — the coverage gap that hid the null-doc bug: building an
    // animated model never touches doc, drawing it does.
    {
        f4::renderer::LitShader lit_shader;
        ::Material default_mat = LoadMaterialDefault();
        default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        f4::renderer::FeatureMeshResources res{};
        res.model_cache = &model_cache;
        res.texture_cache = &tex_cache;
        res.lit_shader = &lit_shader;
        res.default_material = &default_mat;

        Camera3D cam = {};
        cam.position = {0.0f, 100.0f, 0.0f};
        cam.target = {0.0f, 0.0f, 0.0f};
        cam.up = {0.0f, 0.0f, -1.0f};
        cam.fovy = 200.0f;
        cam.projection = CAMERA_ORTHOGRAPHIC;
        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode3D(cam);
        // anim = null → the staged-parked preset drives the channels.
        const auto st = f4::renderer::draw_vis_type_mesh(
            res, 98, 0.0f, 0.0f, 0.0f, 0.0f);
        EndMode3D();
        EndDrawing();
        EXPECT_GT(st.meshes_drawn, 0)
            << "hierarchy model drew nothing through the animated path";

        UnloadMaterial(default_mat);
    }

    model_cache.unload_all();
    tex_cache.unload_all();
    std::filesystem::remove_all(dir);
}

// TEMPORARY diagnostic: render model 1052's rest pose through the real
// runtime (RuntimeModelCache + animated draw path) and save screenshots
// from two angles. Visual ground truth for the DOF-placement reports.
TEST_F(FeatureMeshGpuTest, DIAG_Model1052_RenderRestPose) {
    const std::filesystem::path data_root = F4_DATA_ROOT;
    if (!std::filesystem::exists(data_root / "Models" / "koreaobj" /
                                 "01052.gltf"))
        GTEST_SKIP() << "model 1052 not found";

    f4::renderer::TextureCache tex_cache;
    f4::renderer::LitShader lit_shader;
    f4::renderer::RuntimeModelCache model_cache;
    model_cache.set_data_dir(data_root);
    model_cache.build_model(1052, tex_cache);
    const auto* model = model_cache.lookup(1052);
    ASSERT_NE(model, nullptr);
    ASSERT_TRUE(model->built);
    ASSERT_NE(model->doc, nullptr);

    ::Material default_mat = LoadMaterialDefault();
    default_mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    f4::renderer::FeatureMeshResources res{};
    res.model_cache = &model_cache;
    res.texture_cache = &tex_cache;
    res.lit_shader = &lit_shader;
    res.default_material = &default_mat;

    // Draw into a RenderTexture and export it (rear-quarter view).
    const auto rt = LoadRenderTexture(800, 600);
    BeginTextureMode(rt);
    ClearBackground(BLACK);
    Camera3D cam = {};
    cam.position = {0.0f, 30.0f, -120.0f};   // behind-left, tail toward +?
    cam.target = {0.0f, 0.0f, 0.0f};
    cam.up = {0.0f, 1.0f, 0.0f};
    cam.fovy = 25.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    BeginMode3D(cam);
    if (lit_shader.ensure()) {
        lit_shader.set_lighting({0.4f, -0.8f, 0.45f}, {255, 250, 235, 255},
                                1.2f, {110, 110, 120, 255});
    }
    f4::anim::AnimValues parked2;
    parked2.set_parked_defaults();
    f4::renderer::draw_vis_type_mesh(res, 1052, 0.0f, 0.0f, 0.0f, 0.0f,
                                    &parked2);
    EndMode3D();
    EndTextureMode();

    Image shot = LoadImageFromTexture(rt.texture);
    const auto out = std::filesystem::temp_directory_path() / "f4_1052_view.png";
    ExportImage(shot, out.string().c_str());
    UnloadImage(shot);
    UnloadRenderTexture(rt);
    std::printf("[1052-VIEW] saved %s\n", out.string().c_str());

    model_cache.unload_all();
    tex_cache.unload_all();
    UnloadMaterial(default_mat);
}

// TEMPORARY diagnostic #2: per-part raw vs assembled bbox centers for
// model 1052, computed with the runtime's own math (raylib fold).
TEST_F(FeatureMeshGpuTest, DIAG_Model1052_PartPositions) {
    const std::filesystem::path data_root = F4_DATA_ROOT;
    f4::gltf::GltfDocument doc;
    doc.load(data_root / "Models" / "koreaobj" / "01052.gltf");

    // Find lod:0, DFS parts with chains (mirror extract_gltf_lod_parts).
    const f4::gltf::Node* lod = nullptr;
    for (const auto& n : doc.nodes) {
        if (n.has_f4 && n.f4.kind == "lod" &&
            n.f4.lod_level.value_or(-1) == 0) {
            lod = &n;
            break;
        }
    }
    ASSERT_NE(lod, nullptr);

    struct Frame { std::size_t node; std::size_t depth; };
    std::vector<Frame> stack;
    for (auto it = lod->children.rbegin(); it != lod->children.rend(); ++it)
        stack.push_back({*it, 0});
    std::vector<std::size_t> path;

    auto fold_node = [&](std::size_t ni, Vector3& p) {
        double t[3], q[4], s[3];
        f4::gltf::eval_tagged_local(doc, ni, 0.0f, t, q, s);
        const Matrix rot = QuaternionToMatrix(Quaternion{
            static_cast<float>(q[0]), static_cast<float>(q[1]),
            static_cast<float>(q[2]), static_cast<float>(q[3])});
        const Matrix sc = MatrixScale(static_cast<float>(s[0]),
                                      static_cast<float>(s[1]),
                                      static_cast<float>(s[2]));
        const Matrix tr = MatrixTranslate(static_cast<float>(t[0]),
                                          static_cast<float>(t[1]),
                                          static_cast<float>(t[2]));
        const Matrix m = MatrixMultiply(tr, MatrixMultiply(rot, sc));
        const Vector3 out = Vector3Transform(p, m);
        p = out;
    };

    int reported = 0;
    while (!stack.empty() && reported < 10) {
        const auto fr = stack.back();
        stack.pop_back();
        if (fr.node >= doc.nodes.size()) continue;
        path.resize(fr.depth);
        path.push_back(fr.node);
        const auto& node = doc.nodes[fr.node];
        if (node.mesh.has_value() && *node.mesh < doc.meshes.size()) {
            bool has_dof = false;
            std::string chain;
            for (const auto ni : path) {
                chain += doc.nodes[ni].name + ">";
                if (doc.nodes[ni].has_f4 && doc.nodes[ni].f4.kind == "dof")
                    has_dof = true;
            }
            if (!has_dof) {
                for (const auto c : node.children)
                    stack.push_back({c, fr.depth + 1});
                continue;
            }
            // Raw vs assembled bbox center over the first primitive.
            const auto& prim = doc.meshes[*node.mesh].primitives[0];
            const auto& acc = doc.accessors[*prim.positions];
            Vector3 raw_c{}, asm_c{};
            for (std::size_t i = 0; i < acc.count; ++i) {
                auto v = doc.read_vec3_float(*prim.positions, i);
                if (!v) continue;
                Vector3 p{(*v)[0], (*v)[1], (*v)[2]};
                raw_c = Vector3Add(raw_c, Vector3Scale(p, 1.0f / acc.count));
                Vector3 q2 = p;
                for (const auto ni : path) fold_node(ni, q2);
                asm_c = Vector3Add(asm_c, Vector3Scale(q2, 1.0f / acc.count));
            }
            std::printf("[1052-PART] chain=%s raw=(%.2f, %.2f, %.2f) "
                        "assembled=(%.2f, %.2f, %.2f)\n",
                        chain.c_str(), raw_c.x, raw_c.y, raw_c.z,
                        asm_c.x, asm_c.y, asm_c.z);
            ++reported;
        }
        for (const auto c : node.children) stack.push_back({c, fr.depth + 1});
    }
    SUCCEED();
}

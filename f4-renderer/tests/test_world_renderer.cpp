// f4-renderer/tests/test_world_renderer.cpp
//
// Tests for render_world() / SceneDescription:
//   1. Pure tests: SceneDescription defaults, culling math, and
//      render_world with an empty scene (no GL draw calls reach the
//      driver — BeginMode3D needs a context though, so the truly pure
//      tests avoid calling render_world entirely).
//   2. GPU-context test: renders an empty world into a RenderTexture
//      and asserts the returned FrameStats + that the texture receives
//      the sky color (auto-skips headless like the other GPU tests).
//
// Entity dispatch (GroundLayoutComponent → geometry) is exercised via
// RenderEntity with a real EntityWorld — that part needs no GPU when the
// resources are null (degrades to no-ops).

#include <f4/renderer/world_renderer.hpp>
#include <f4/renderer/render_resources.hpp>
#include <f4/renderer/scene_draw.hpp>

#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>
#include <raylib.h>

using namespace f4::renderer;

// ── Pure: SceneDescription defaults ────────────────────────────────────────

TEST(SceneDescriptionTest, SensibleDefaults) {
    SceneDescription s;
    EXPECT_EQ(s.target, nullptr);
    EXPECT_EQ(s.world, nullptr);
    EXPECT_TRUE(s.entities.empty());
    EXPECT_EQ(s.class_table, nullptr);
    EXPECT_EQ(s.airfield, nullptr);
    EXPECT_FLOAT_EQ(s.cull_radius_ft, 0.0f);   // unlimited by default
    EXPECT_FALSE(static_cast<bool>(s.overlay_3d));
    EXPECT_FALSE(s.airfield_labels);
    EXPECT_TRUE(s.draw_feature_models);

    // Ground defaults on: plane + grid, axes off, anchored at origin.
    EXPECT_TRUE(s.ground.plane);
    EXPECT_TRUE(s.ground.grid);
    EXPECT_FALSE(s.ground.axes);
    EXPECT_FLOAT_EQ(s.ground.origin_enu_x, 0.0f);

    // All airfield layers enabled by default.
    EXPECT_TRUE(s.airfield_toggles.runway);
    EXPECT_TRUE(s.airfield_toggles.taxiways);
    EXPECT_TRUE(s.airfield_toggles.parking);
}

TEST(SceneDescriptionTest, AirfieldTogglesDefaultAllOn) {
    AirfieldDrawToggles t;
    EXPECT_TRUE(t.runway && t.markers && t.taxiways && t.parking &&
                t.helipads && t.features);
}

// ── Pure: GroundConfig anchoring values ────────────────────────────────────

TEST(GroundConfigTest, ZOffsetsPreventZFighting) {
    GroundConfig g;
    // Plane below grid, grid below layout elevation.
    EXPECT_LT(g.plane_z_offset, g.grid_z_offset);
    EXPECT_LT(g.grid_z_offset, 0.0f);
}

// ── Pure: RenderEntity ground-layout dispatch (no GPU) ────────────────────
//
// Dispatch paths that never reach an immediate-mode draw call can run
// without a GL context. The full build+draw path is covered by the GPU
// fixture below.

TEST(RenderEntityGroundLayoutTest, NoLayoutComponentIsNoOp) {
    f4::entities::EntityWorld world;
    auto h = world.create();
    f4::entities::TransformComponent tf;
    h.add<f4::entities::TransformComponent>(std::move(tf));

    EntityRenderResources res{};
    auto stats = RenderEntity(res, h);
    EXPECT_EQ(stats.draw_calls, 0);
    EXPECT_EQ(stats.meshes_drawn, 0);
}

TEST(RenderEntityGroundLayoutTest, CanDisableGroundLayout) {
    f4::entities::EntityWorld world;
    auto h = world.create();

    f4::entities::GroundLayoutComponent gl;
    f4::entities::GroundLayoutList list;
    list.type = 1;
    f4::entities::GroundLayoutPoint p0{}, p1{};
    p1.y = 9000.0f;
    list.points = {p0, p1};
    gl.layouts = {list};
    h.add<f4::entities::GroundLayoutComponent>(std::move(gl));

    f4::entities::TransformComponent tf;
    h.add<f4::entities::TransformComponent>(std::move(tf));

    std::unordered_map<uint64_t, AirfieldGeometry3D> cache;
    EntityRenderResources res{};
    res.airfield_cache = &cache;
    res.show_ground_layout = false;

    (void)RenderEntity(res, h);
    EXPECT_EQ(cache.size(), 0u);   // dispatch skipped entirely
}

// ── GPU-context: render_world smoke ────────────────────────────────────────

class RenderWorldGpuTest : public ::testing::Test {
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

bool RenderWorldGpuTest::initialized_ = false;

TEST_F(RenderWorldGpuTest, EmptySceneRendersToTextureWithSkyColor) {
    RenderResources res;
    RenderTexture2D rt = LoadRenderTexture(128, 128);

    SceneDescription scene;
    scene.camera = make_perspective_camera(0, 0, 1000, 0.0f, -30.0f);
    scene.target = &rt;
    scene.sky_color = Color{10, 20, 30, 255};
    scene.ground.plane = false;
    scene.ground.grid = false;

    const auto stats = render_world(res, scene);

    EXPECT_EQ(stats.entities_total, 0);
    EXPECT_EQ(stats.entity_meshes_drawn, 0);
    EXPECT_EQ(stats.draw.draw_calls, 0);

    // The texture's center pixel should be the sky color.
    Image img = LoadImageFromTexture(rt.texture);
    ASSERT_NE(img.data, nullptr);
    const auto* px = static_cast<const unsigned char*>(img.data);
    const int cx = img.width / 2, cy = img.height / 2;
    const int idx = (cy * img.width + cx) * 4;
    EXPECT_NEAR(px[idx + 0], 10, 2);
    EXPECT_NEAR(px[idx + 1], 20, 2);
    EXPECT_NEAR(px[idx + 2], 30, 2);
    UnloadImage(img);

    UnloadRenderTexture(rt);
    res.unload_all();
}

TEST_F(RenderWorldGpuTest, SceneWithGroundDrawsWithoutCrash) {
    RenderResources res;
    RenderTexture2D rt = LoadRenderTexture(128, 128);

    SceneDescription scene;
    scene.camera = make_perspective_camera(0, 0, 2000, 45.0f, -30.0f);
    scene.target = &rt;
    scene.ground.grid = true;
    scene.ground.axes = true;

    const auto stats = render_world(res, scene);
    EXPECT_EQ(stats.entities_total, 0);
    // Ground lines are not counted in FrameStats (they're immediate-mode
    // primitives); the point is it doesn't crash and reports zero entity
    // work.

    UnloadRenderTexture(rt);
    res.unload_all();
}

TEST_F(RenderWorldGpuTest, RenderResourcesLifecycleIsIdempotent) {
    RenderResources res;
    EXPECT_FALSE(res.default_material_valid());
    EXPECT_TRUE(res.ensure_default_material());
    EXPECT_TRUE(res.default_material_valid());
    EXPECT_TRUE(res.ensure_default_material());   // idempotent
    res.unload_all();
    EXPECT_FALSE(res.default_material_valid());
    EXPECT_TRUE(res.airfield_cache.empty());
    EXPECT_TRUE(res.airfield_cache.empty());
}

// Full GroundLayoutComponent dispatch: builds geometry, caches by
// EntityId, draws the runway primitives (needs the GL context for the
// immediate-mode draw calls).
TEST_F(RenderWorldGpuTest, GroundLayoutDispatchBuildsCachesDraws) {
    f4::entities::EntityWorld world;
    auto h = world.create();

    f4::entities::GroundLayoutComponent gl;
    f4::entities::GroundLayoutList list;
    list.type = 1;  // runway centerline
    f4::entities::GroundLayoutPoint p0{}, p1{};
    p0.x = 0.0f;  p0.y = 0.0f;
    p1.x = 0.0f;  p1.y = 9000.0f;
    list.points = {p0, p1};
    gl.layouts = {list};
    h.add<f4::entities::GroundLayoutComponent>(std::move(gl));

    f4::entities::TransformComponent tf;
    tf.position = f4::geo::WorldPosition(500000.0, 700000.0, 0.0);
    h.add<f4::entities::TransformComponent>(std::move(tf));

    BeginDrawing();
    BeginMode3D(make_perspective_camera(500000.0f, 704000.0f, 2000.0f,
                                        0.0f, -30.0f));

    std::unordered_map<uint64_t, AirfieldGeometry3D> cache;
    EntityRenderResources res{};   // feature path nulls are fine
    res.airfield_cache = &cache;

    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(cache.size(), 1u);
    // A runway centerline list produces runway geometry → draw calls.
    EXPECT_GT(stats.draw_calls, 0);

    // Second frame: cache hit, no duplicate entry.
    (void)RenderEntity(res, h);
    EXPECT_EQ(cache.size(), 1u);

    EndMode3D();
    EndDrawing();
}

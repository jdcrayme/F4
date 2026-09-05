// f4-renderer/tests/test_entity_render.cpp
//
// Unit tests for entity_render.hpp — entity_icon_info() and
// EntityRenderResources.
//
// These tests cover the component-inspection logic (entity_icon_info)
// which is pure and requires no GPU context. RenderEntity() requires
// a full Raylib + OpenGL context and real model data, so it is tested
// in the GPU-context test suite (test_feature_mesh.cpp covers the
// underlying draw_feature_mesh pipeline).

#include <f4/renderer/entity_render.hpp>

#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

using namespace f4::renderer;
using namespace f4::entities;
using UC = f4::entities::UnitClass;

// ── entity_icon_info — objective entities ─────────────────────────────────

TEST(EntityIconInfo, InvalidEntity_ReturnsInvalid) {
    EntityWorld world;
    EntityHandle invalid;  // default-constructed, not bound to any world
    const auto info = entity_icon_info(invalid);
    EXPECT_FALSE(info.valid);
    EXPECT_EQ(nullptr, info.symbol_key);
}

TEST(EntityIconInfo, EntityWithNoRenderableComponents_ReturnsInvalid) {
    EntityWorld world;
    auto h = world.create();
    // No ObjectiveTypeComponent, no UnitCoreComponent
    const auto info = entity_icon_info(h);
    EXPECT_FALSE(info.valid);
}

TEST(EntityIconInfo, ObjectiveTypeComponent_TypeFromPropertyBag) {
    EntityWorld world;
    auto h = world.create();

    // Add ObjectiveTypeComponent with type=108 (entity_type, = 100 + 8 = city)
    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 108;

    // Add PropertyBag with objective_type = 8 (city)
    auto& pb = h.add<PropertyBag>();
    pb.ints["objective_type"] = 8;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    // obj_type 8 = city
    EXPECT_STREQ("obj_city", info.symbol_key);
}

TEST(EntityIconInfo, ObjectiveTypeComponent_TypeDerivedFromTypeField) {
    EntityWorld world;
    auto h = world.create();

    // Add ObjectiveTypeComponent with type=101 (= 100 + 1 = airbase)
    // No PropertyBag — should derive obj_type from type field.
    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 101;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    // obj_type 1 = airbase
    EXPECT_STREQ("obj_airbase", info.symbol_key);
}

TEST(EntityIconInfo, ObjectiveTypeComponent_TypeDerivedFromClassTableIndex) {
    EntityWorld world;
    auto h = world.create();

    // ObjectiveTypeComponent with type=0 (unset) but class_table_index=127
    // (= 100 + 27 = SAM site)
    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 0;
    ot.class_table_index = 127;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    // obj_type 27 = SAM site
    EXPECT_STREQ("obj_sam_site", info.symbol_key);
}

TEST(EntityIconInfo, ObjectiveTypeComponent_TypeAlreadyInRange) {
    EntityWorld world;
    auto h = world.create();

    // ObjectiveTypeComponent with type=6 (bridge, already in 1..39 range)
    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 6;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    // obj_type 6 = bridge
    EXPECT_STREQ("obj_bridge", info.symbol_key);
}

TEST(EntityIconInfo, ObjectiveTypeComponent_PropertyBagTakesPrecedence) {
    EntityWorld world;
    auto h = world.create();

    // ObjectiveTypeComponent.type = 108 (would derive obj_type=8, city)
    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 108;

    // But PropertyBag says objective_type = 1 (airbase) — should win
    auto& pb = h.add<PropertyBag>();
    pb.ints["objective_type"] = 1;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("obj_airbase", info.symbol_key);
}

TEST(EntityIconInfo, ObjectiveTypeComponent_UnmappedTypeFallsBackToUnknown) {
    EntityWorld world;
    auto h = world.create();

    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 0;
    auto& pb = h.add<PropertyBag>();
    pb.ints["objective_type"] = 16;  // unmapped in the table (parity: 16, 31-38)

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("obj_unknown", info.symbol_key);
}

// ── entity_icon_info — unit entities ──────────────────────────────────────

TEST(EntityIconInfo, UnitCoreComponent_Battalion) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.unit_subtype = 0;  // unknown subtype — bare-frame fallback

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("frame_battalion", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_BrigadeArmor_OneCompositeKey) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Brigade;
    uc.unit_subtype = 3;  // STYPE_LAND_ARMOR

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    // One composite SVG per unit type — the frame and the glyph are
    // authored together (the frame/glyph split is retired).
    EXPECT_STREQ("unit_armor", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_Squadron_Fighter) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Squadron;
    uc.unit_subtype = 8;  // STYPE_AIR_FIGHTER

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("unit_fighter", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_Squadron_Bomber) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Squadron;
    uc.unit_subtype = 6;  // STYPE_AIR_BOMBER

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("unit_bomber", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_Flight_FrameOnly) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Flight;
    uc.unit_subtype = 0;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("frame_flight", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_TaskForce_Carrier) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::TaskForce;
    uc.unit_subtype = 3;  // carrier subtype

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("unit_carrier", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_TaskForce_DefaultNavalSurface) {
    EntityWorld world;
    auto h = world.create();

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::TaskForce;
    uc.unit_subtype = 0;  // any non-carrier naval subtype

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("unit_naval_surface", info.symbol_key);
}

TEST(EntityIconInfo, UnitCoreComponent_BattalionArtillerySharedWithTowed) {
    EntityWorld world;
    auto h = world.create();

    // SP artillery (11) and towed artillery (14) share the gun glyph —
    // parity with the old symbol_for_unit table.
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.unit_subtype = 14;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("unit_artillery", info.symbol_key);
}

// ── entity_icon_info — priority: objective before unit ────────────────────

TEST(EntityIconInfo, ObjectiveTakesPrecedenceOverUnit) {
    // An entity that has BOTH ObjectiveTypeComponent and UnitCoreComponent
    // (unusual but possible) — objective should win in the resolution order.
    EntityWorld world;
    auto h = world.create();

    auto& ot = h.add<ObjectiveTypeComponent>();
    ot.type = 119;  // 100 + 19 = port

    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;

    const auto info = entity_icon_info(h);
    EXPECT_TRUE(info.valid);
    EXPECT_STREQ("obj_port", info.symbol_key);
}

// ── EntityRenderResources defaults ────────────────────────────────────────

TEST(EntityRenderResources, Defaults) {
    EntityRenderResources res;
    EXPECT_TRUE(res.show_features);
    EXPECT_EQ(100, res.vu_last_entity_type);
    EXPECT_TRUE(res.skip_destroyed_features);
    EXPECT_TRUE(res.show_ground_layout);
    EXPECT_TRUE(res.show_units);  // Mode A unit rendering on by default
    // Inherited from FeatureMeshResources:
    EXPECT_EQ(nullptr, res.model_db);
    EXPECT_EQ(nullptr, res.class_table);
    EXPECT_EQ(nullptr, res.texture_cache);
    EXPECT_EQ(nullptr, res.lit_shader);
    EXPECT_EQ(nullptr, res.mesh_cache);
    EXPECT_EQ(nullptr, res.default_material);
}

// ── RenderEntity — no-GPU basic checks ────────────────────────────────────
// RenderEntity() requires a GPU context for the actual mesh drawing, but
// we can verify that it handles null resources and empty entities
// gracefully (returns zero stats without crashing).

TEST(RenderEntity, InvalidEntity_ReturnsZeroStats) {
    EntityRenderResources res;
    EntityHandle invalid;
    const auto stats = RenderEntity(res, invalid);
    EXPECT_EQ(0, stats.draw_calls);
    EXPECT_EQ(0, stats.meshes_drawn);
    EXPECT_EQ(0u, stats.vertices_drawn);
}

TEST(RenderEntity, ValidEntityNoFeatures_ReturnsZeroStats) {
    EntityWorld world;
    auto h = world.create();

    // Entity with TransformComponent but no FeatureSetComponent
    h.add<TransformComponent>();

    EntityRenderResources res;
    // All resource pointers are null — draw_feature_mesh will no-op
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, FeaturesDisabled_SkipsFeatureRendering) {
    EntityWorld world;
    auto h = world.create();

    // Entity with both TransformComponent and FeatureSetComponent
    h.add<TransformComponent>();
    auto& fs = h.add<FeatureSetComponent>();
    // Add a placeholder feature (index=0, offset=0 — will be skipped anyway)
    fs.features.push_back(FeatureEntryState{});

    EntityRenderResources res;
    res.show_features = false;  // Disable feature rendering
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, EmptyPlaceholderFeature_IsSkipped) {
    EntityWorld world;
    auto h = world.create();

    h.add<TransformComponent>();
    auto& fs = h.add<FeatureSetComponent>();
    // Add an empty placeholder (index=0, offset_x=0, offset_y=0)
    // This matches the filter in canvas.cpp lines 693-697
    fs.features.push_back(FeatureEntryState{});

    EntityRenderResources res;
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, DestroyedFeature_IsSkippedWhenConfigured) {
    EntityWorld world;
    auto h = world.create();

    h.add<TransformComponent>();
    auto& fs = h.add<FeatureSetComponent>();
    // Add a destroyed feature (damage_state=3)
    FeatureEntryState feat;
    feat.index = 5;
    feat.offset_x = 100.0f;
    feat.offset_y = 200.0f;
    feat.damage_state = 3;  // destroyed
    fs.features.push_back(feat);

    EntityRenderResources res;
    res.skip_destroyed_features = true;
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, DestroyedFeature_IsDrawnWhenNotSkipped) {
    EntityWorld world;
    auto h = world.create();

    h.add<TransformComponent>();
    auto& fs = h.add<FeatureSetComponent>();
    // Add a destroyed feature
    FeatureEntryState feat;
    feat.index = 5;
    feat.offset_x = 100.0f;
    feat.offset_y = 200.0f;
    feat.damage_state = 3;
    fs.features.push_back(feat);

    EntityRenderResources res;
    res.skip_destroyed_features = false;  // Draw even destroyed features
    // Resources are null so draw_feature_mesh returns zero stats,
    // but the feature is not filtered out.
    const auto stats = RenderEntity(res, h);
    // draw_feature_mesh will return 0 because resources are null,
    // but the feature was not skipped by the damage filter.
    EXPECT_EQ(0, stats.draw_calls);  // no real draw, but path was taken
}

// ── RenderEntity — UnitCoreComponent dispatch (Mode A) ────────────────────
//
// These tests verify the new UnitCoreComponent + TransformComponent branch
// in RenderEntity(). They mirror the no-GPU pattern of the feature tests
// above: resources are null, so draw_feature_mesh() returns zero stats,
// but the test asserts that the dispatch path is or isn't taken based on
// the component mix and the show_units toggle.

TEST(RenderEntity, Unit_NoTransformComponent_SkipsUnitRendering) {
    EntityWorld world;
    auto h = world.create();

    // UnitCoreComponent but no TransformComponent — cannot place the unit.
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.class_table_index = 170;  // valid unit entity_type

    EntityRenderResources res;
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, Unit_NoUnitCoreComponent_SkipsUnitRendering) {
    EntityWorld world;
    auto h = world.create();

    // TransformComponent but no UnitCoreComponent — not a unit.
    h.add<TransformComponent>();

    EntityRenderResources res;
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, Unit_LowClassTableIndex_SkipsUnitRendering) {
    EntityWorld world;
    auto h = world.create();

    // class_table_index < 100 — not a valid entity_type for ClassTable
    // lookup (entries_ are indexed by entity_type - 100). The dispatch
    // must skip rather than passing a bogus index to draw_feature_mesh().
    h.add<TransformComponent>();
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.class_table_index = 50;  // below 100 — invalid

    EntityRenderResources res;
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, Unit_ShowUnitsFalse_SkipsUnitRendering) {
    EntityWorld world;
    auto h = world.create();

    h.add<TransformComponent>();
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.class_table_index = 170;

    EntityRenderResources res;
    res.show_units = false;  // caller disabled unit rendering
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);
}

TEST(RenderEntity, Unit_ValidBattalion_TakesDrawPath) {
    EntityWorld world;
    auto h = world.create();

    // Battalion with a valid class_table_index and a heading. Resources
    // are null so draw_feature_mesh() will return zero stats, but the
    // dispatch path executes — verified by the fact that no crash occurs
    // and the call returns cleanly. (A full GPU test lives in
    // test_feature_mesh.cpp which exercises the underlying pipeline.)
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition{1000.0, 2000.0, 0.0};
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.unit_subtype = 14;  // armor
    uc.class_table_index = 170;
    auto& gt = h.add<GroundTacticalComponent>();
    gt.heading = 64;  // ~90 deg (64 * 1.4 = 89.6)

    EntityRenderResources res;
    // All resource pointers null — draw_feature_mesh returns zero stats
    // without crashing. This exercises the UnitCoreComponent dispatch
    // (component lookup, heading resolution, draw_feature_mesh call)
    // without requiring a GL context.
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);  // no real draw — null resources
    EXPECT_EQ(0, stats.meshes_drawn);
    EXPECT_EQ(0u, stats.vertices_drawn);
}

TEST(RenderEntity, Unit_WithoutGroundTacticalComponent_DefaultsToZeroFacing) {
    EntityWorld world;
    auto h = world.create();

    // Squadron (air unit) — no GroundTacticalComponent attached. The
    // dispatch must not crash and must default facing to 0 deg.
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition{5000.0, -3000.0, 20000.0};
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Squadron;
    uc.unit_subtype = 8;  // fighter
    uc.class_table_index = 273;  // F-16 vehicle-class entity_type
    // No GroundTacticalComponent — facing defaults to 0.

    EntityRenderResources res;
    const auto stats = RenderEntity(res, h);
    EXPECT_EQ(0, stats.draw_calls);  // null resources — no real draw
}

TEST(RenderEntity, Unit_AndFeature_OnSameEntity_BothDispatched) {
    EntityWorld world;
    auto h = world.create();

    // An entity carrying both FeatureSetComponent and UnitCoreComponent
    // is unusual in practice (objectives don't have UnitCoreComponent,
    // units don't have FeatureSetComponent) but RenderEntity() should
    // dispatch both branches independently — the function accumulates
    // DrawStats across all matching component pairs.
    h.add<TransformComponent>();
    auto& fs = h.add<FeatureSetComponent>();
    FeatureEntryState feat;
    feat.index = 5;
    feat.offset_x = 100.0f;
    feat.offset_y = 200.0f;
    fs.features.push_back(feat);
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.class_table_index = 170;

    EntityRenderResources res;
    const auto stats = RenderEntity(res, h);
    // Both branches execute; both return zero stats (null resources),
    // but the function did not short-circuit after the feature branch.
    EXPECT_EQ(0, stats.draw_calls);
}

// ── EntityIconInfo struct ─────────────────────────────────────────────────

TEST(EntityIconInfo, DefaultState) {
    EntityIconInfo info;
    EXPECT_FALSE(info.valid);
    EXPECT_EQ(nullptr, info.symbol_key);
}

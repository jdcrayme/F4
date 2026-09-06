// f4-simulation/tests/test_visual_model_component.cpp
//
// Unit tests for VisualModelComponent — the renderable handle.
//
// Tranche 0d: the component no longer carries model_record or ModelState.
// vis_type IS the identity; gear_switch_child replaces the ModelState.switches
// vector. These tests verify the new shape.
//
// Tests verify:
//   1. Defaults to vis_type=0 (renderer should skip drawing)
//   2. Carries LOD + gear_switch_child + texture_set that the host can write
//   3. Lives in the f4::simulation namespace (NOT f4::entities)
//   4. Plays well with sibling components (TransformComponent) on the same entity

#include <gtest/gtest.h>

#include "f4/simulation/visual_model_component.hpp"

#include <f4/entities/entity.hpp>

#include <typeinfo>

using namespace f4::simulation;
using namespace f4::entities;

TEST(VisualModelComponent, DefaultConstructHasZeroVisType) {
    VisualModelComponent vmc;
    EXPECT_EQ(vmc.vis_type, 0);  // 0 = no model; renderer skips
    EXPECT_EQ(vmc.active_lod, 0);
    EXPECT_EQ(vmc.texture_set, 0);
    EXPECT_EQ(vmc.gear_switch_child, 0);  // 0 = gear down (default)
}

TEST(VisualModelComponent, CanAddAndGetFromEntity) {
    EntityWorld world;
    auto h = world.create();

    auto& vmc = h.add<VisualModelComponent>();
    vmc.vis_type = 1052;  // F-16
    vmc.active_lod = 2;
    vmc.texture_set = 1;
    vmc.gear_switch_child = 1;  // gear up

    auto* fetched = h.get<VisualModelComponent>();
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->vis_type, 1052);
    EXPECT_EQ(fetched->active_lod, 2);
    EXPECT_EQ(fetched->texture_set, 1);
    EXPECT_EQ(fetched->gear_switch_child, 1);
}

TEST(VisualModelComponent, ZeroVisTypeIsSafeForRenderer) {
    // The renderer should check vis_type != 0 before drawing.
    // This test verifies the default-constructed state is safe to inspect
    // (no crash, no UB) so the renderer's zero check is sufficient.
    EntityWorld world;
    auto h = world.create();
    auto& vmc = h.add<VisualModelComponent>();

    // Renderer would do: if (vmc.vis_type == 0) return;
    ASSERT_EQ(vmc.vis_type, 0);

    // Even with vis_type=0, the component's other fields are valid
    // and can be safely read/written. The host might write gear state
    // before the model is resolved.
    vmc.gear_switch_child = 1;
    EXPECT_EQ(vmc.gear_switch_child, 1);
}

TEST(VisualModelComponent, LivesInF4SimulationNamespace) {
    // The component lives in f4::simulation (not f4::entities, which stays
    // dependency-free). Verified via typeid.
    VisualModelComponent vmc;
    const std::string name = typeid(vmc).name();
    bool found_simulation = name.find("simulation") != std::string::npos;
    EXPECT_TRUE(found_simulation)
        << "VisualModelComponent should be in f4::simulation namespace. "
        << "typeid name: " << name;
}

TEST(VisualModelComponent, CoexistsWithSiblingComponents) {
    // An "aircraft" entity carries TransformComponent + VisualModelComponent +
    // FlightModelComponent + BrainComponent. This test verifies the first two
    // (the data-only ones) can coexist on one entity without interference.
    EntityWorld world;
    auto h = world.create();

    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(100.0, 200.0, 50.0);
    tf.qw = 1.0;  // identity quaternion

    auto& vmc = h.add<VisualModelComponent>();
    vmc.vis_type = 1052;
    vmc.active_lod = 0;

    EXPECT_NE(h.get<TransformComponent>(), nullptr);
    EXPECT_NE(h.get<VisualModelComponent>(), nullptr);
    EXPECT_EQ(h.get<TransformComponent>()->position.x, 100.0);
    EXPECT_EQ(h.get<TransformComponent>()->position.y, 200.0);
    EXPECT_EQ(h.get<VisualModelComponent>()->vis_type, 1052);
    EXPECT_EQ(h.get<VisualModelComponent>()->active_lod, 0);

    // The entity is now "an aircraft" (well, the visual + spatial parts of one).
    // The entity ID is the binding — there is no AircraftClass wrapper.
}

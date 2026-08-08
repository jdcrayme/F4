// f4-simulation/tests/test_visual_model_component.cpp
//
// Unit tests for VisualModelComponent — the renderable handle.
//
// These tests verify the component:
//   1. Compiles and can be added/queried on an entity
//   2. Defaults to a safe null model_record (renderer should skip drawing)
//   3. Carries LOD + DOF/switch state that the host can write
//   4. Lives in the f4::simulation namespace (NOT f4::entities — this is
//      intentional, to keep f4-entities dependency-free)
//   5. Plays well with sibling components (TransformComponent) on the same entity
//
// The full integration test (VisualModelComponent + FlightModelComponent +
// BrainComponent on one entity, ticking through a taxi scenario) lives in
// test_scenario_loader.cpp + the eventual f4-scenario-player screenshot smoke test.

#include <gtest/gtest.h>

#include "f4/simulation/visual_model_component.hpp"

#include <f4/entities/entity.hpp>
#include <f4/models/model_database.hpp>
#include <f4/models/geometry.hpp>

#include <typeinfo>

using namespace f4::simulation;
using namespace f4::entities;

TEST(VisualModelComponent, DefaultConstructHasNullModelRecord) {
    VisualModelComponent vmc;
    EXPECT_EQ(vmc.model_record, nullptr);
    EXPECT_EQ(vmc.active_lod, 0);
    EXPECT_EQ(vmc.texture_set, 0);
    // ModelState defaults: no switches, no DOFs, LOD 0
    EXPECT_TRUE(vmc.model_state.switches.empty());
    EXPECT_TRUE(vmc.model_state.dofs.empty());
}

TEST(VisualModelComponent, CanAddAndGetFromEntity) {
    EntityWorld world;
    auto h = world.create();

    auto& vmc = h.add<VisualModelComponent>();
    vmc.active_lod = 2;
    vmc.texture_set = 1;

    auto* fetched = h.get<VisualModelComponent>();
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->active_lod, 2);
    EXPECT_EQ(fetched->texture_set, 1);
    EXPECT_EQ(fetched->model_record, nullptr);  // still null — no model resolved
}

TEST(VisualModelComponent, NullModelRecordIsSafeForRenderer) {
    // The renderer should check model_record != nullptr before drawing.
    // This test verifies the default-constructed state is safe to inspect
    // (no crash, no UB) so the renderer's null check is sufficient.
    EntityWorld world;
    auto h = world.create();
    auto& vmc = h.add<VisualModelComponent>();

    // Renderer would do: if (!vmc.model_record) return;
    ASSERT_EQ(vmc.model_record, nullptr);

    // Even with a null model_record, the component's other fields are valid
    // and can be safely read/written. The host might write switch state before
    // the model is resolved (e.g. if the model loads asynchronously).
    f4::models::SwitchState gear;
    gear.switch_number = 10;
    gear.active_child = 0;
    vmc.model_state.switches.push_back(gear);

    EXPECT_EQ(vmc.model_state.switches.size(), 1u);
    EXPECT_EQ(vmc.model_state.switches[0].switch_number, 10);
}

TEST(VisualModelComponent, LivesInF4SimulationNamespace) {
    // Critical: the component must NOT be in f4::entities, because it depends
    // on f4-models (ModelRecord) and f4-entities must stay dependency-free.
    // This test verifies the namespace via typeid.
    VisualModelComponent vmc;
    const std::string name = typeid(vmc).name();
    // The mangled name should contain "simulation" somewhere (compiler-dependent
    // mangling, but the namespace name appears in the demangled form on all
    // major compilers). We check for "simulation" in the raw name as a
    // best-effort guard.
    bool found_simulation = name.find("simulation") != std::string::npos;
    EXPECT_TRUE(found_simulation)
        << "VisualModelComponent should be in f4::simulation namespace. "
        << "typeid name: " << name;
}

TEST(VisualModelComponent, CoexistsWithSiblingComponents) {
    // An "aircraft" entity carries TransformComponent + VisualModelComponent +
    // FlightModelComponent + BrainComponent. This test verifies the first two
    // (the data-only ones) can coexist on one entity without interference.
    // (FlightModelComponent + BrainComponent are tested in their own libs.)
    EntityWorld world;
    auto h = world.create();

    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(100.0, 200.0, 50.0);
    tf.qw = 1.0;  // identity quaternion

    auto& vmc = h.add<VisualModelComponent>();
    vmc.active_lod = 0;

    EXPECT_NE(h.get<TransformComponent>(), nullptr);
    EXPECT_NE(h.get<VisualModelComponent>(), nullptr);
    EXPECT_EQ(h.get<TransformComponent>()->position.x, 100.0);
    EXPECT_EQ(h.get<TransformComponent>()->position.y, 200.0);
    EXPECT_EQ(h.get<VisualModelComponent>()->active_lod, 0);

    // The entity is now "an aircraft" (well, the visual + spatial parts of one).
    // The entity ID is the binding — there is no AircraftClass wrapper.
}

// f4-simulation/include/f4/simulation/visual_model_component.hpp
//
// VisualModelComponent — the renderable handle for an entity.
//
// This is the ECS equivalent of FreeFalcon's DrawableBSP* on SimVehicleClass.
// It carries the entity's visual-model identity (vis_type) plus per-instance
// visual state (active LOD, gear switch, texture set).
//
// CRITICAL DESIGN NOTE (see Docs/AIRCRAFT_BINDING_DESIGN.md):
//   This component is ONLY the renderable handle. It is NOT a god-class
//   wrapper that ties the flight model + brain + 3D model together. In an
//   ECS, the entity ID itself is the binding — an "aircraft" is just an
//   entity that happens to carry TransformComponent + VisualModelComponent
//   + FlightModelComponent + BrainComponent. The brain and FM are siblings
//   resolved via EntityHandle::get<T>() / get_interface<I>(), NOT via raw
//   pointers stored on this component.
//
// Tranche 0d (NO_BINARY_RUNTIME_PLAN.md): the component no longer carries
//   a `const f4::models::ModelRecord*` or `f4::models::ModelState`. The
//   vis_type IS the renderable identity — the renderer resolves the mesh
//   through its own model cache (ModelDatabase for the legacy binary path,
//   RuntimeModelCache for the glTF path). The gear switch (the only DOF/
//   switch the sim animates) is a simple uint8. This cuts f4-models from
//   f4-simulation's link closure: the component header has no f4/models
//   includes, and the simulation never calls ModelDatabase methods.
//
// Why this lives in f4-simulation (not f4-entities):
//   f4-entities is dependency-free (only f4-geo + stdlib) and must stay
//   that way — it's the substrate every other library builds on. Any
//   library can define new components via the Component<T> CRTP base +
//   type_index key without modifying f4-entities.
//
// Why passive (Component<T>, not BehavioralComponent<T>):
//   The visual state is purely a function of the FM's gear flag and the
//   entity's transform. The renderer reads it directly; the host syncs
//   gear_switch_child from the FM each tick. No per-tick update needed.
//
// Dependencies: f4-entities (Component<T>) only. C++20.

#pragma once

#include <f4/entities/entity.hpp>

#include <cstdint>

namespace f4::simulation {

/// Renderable handle for an entity. Carries the visual-model identity
/// (vis_type — the FALCON4.CT visType[0] index) plus per-instance visual
/// state (active LOD, gear switch, texture set).
///
/// Equivalent to FreeFalcon's DrawableBSP* on SimVehicleClass — JUST
/// the renderable, nothing else. The flight model and brain are
/// sibling components on the same entity, resolved via the ECS.
struct VisualModelComponent : entities::Component<VisualModelComponent> {
    /// The VIS TYPE itself (FALCON4.CT's visual-model index), set at
    /// spawn — the renderable IDENTITY of the entity, independent of any
    /// model database. The renderer resolves vis_type → mesh through ITS
    /// OWN model cache (ModelDatabase for the legacy binary path,
    /// RuntimeModelCache for the glTF path). 0 = never resolved (no
    /// class-table entry) — the renderer should skip drawing in that case.
    int16_t vis_type{0};

    /// Active LOD index. Renderer picks based on distance to camera, but
    /// for the taxi demo we lock to LOD 0 (highest detail) since the
    /// camera is close.
    int active_lod{0};

    /// Gear switch child selection (0=down, 1=up). The host syncs this
    /// from the FM's gear flag each tick. The renderer maps it to the
    /// model's switch node child selection (switch #10 on the F-16 per
    /// f4-models-viewer/src/scene.cpp). Replaces the old
    /// f4::models::ModelState (which carried a vector of SwitchState +
    /// DofState — only the gear switch was ever animated by the sim).
    uint8_t gear_switch_child{0};

    /// Optional: which texture set (summer/winter/desert). Default 0.
    int texture_set{0};
};

} // namespace f4::simulation

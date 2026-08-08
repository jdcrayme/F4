// f4-simulation/include/f4/simulation/visual_model_component.hpp
//
// VisualModelComponent — the renderable handle for an entity.
//
// This is the ECS equivalent of FreeFalcon's DrawableBSP* on SimVehicleClass.
// It carries a pointer to the resolved 3D model (from f4-models::ModelDatabase)
// plus per-instance visual state (active LOD, DOF/switch values, texture set).
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
// Why this lives in f4-simulation (not f4-entities):
//   The component holds a const f4::models::ModelRecord*, so it depends on
//   f4-models. f4-entities is currently dependency-free (only f4-geo +
//   stdlib) and must stay that way — it's the substrate every other library
//   builds on. f4-simulation is the orchestration layer proposed in
//   Docs/ARCHITECTURE PROPOSAL.md §13 that depends on both f4-entities and
//   f4-models. Any library can define new components via the Component<T>
//   CRTP base + type_index key without modifying f4-entities.
//
// Why passive (Component<T>, not BehavioralComponent<T>):
//   The visual state is purely a function of the FM's gear flag and the
//   entity's transform. The renderer reads it directly; the host syncs
//   model_state.switches from the FM each tick. No per-tick update needed.
//
// Dependencies: f4-entities (Component<T>), f4-models (ModelRecord, ModelState).
// C++20.

#pragma once

#include <f4/entities/entity.hpp>
#include <f4/models/model_database.hpp>  // for ModelRecord
#include <f4/models/geometry.hpp>        // for ModelState

namespace f4::simulation {

/// Renderable handle for an entity. Carries a pointer to the resolved
/// 3D model (from f4-models::ModelDatabase) plus per-instance visual
/// state (active LOD, DOF/switch values, texture set).
///
/// Equivalent to FreeFalcon's DrawableBSP* on SimVehicleClass — JUST
/// the renderable, nothing else. The flight model and brain are
/// sibling components on the same entity, resolved via the ECS.
struct VisualModelComponent : entities::Component<VisualModelComponent> {
    /// Resolved at scenario load from the entity's visType[0] + ModelDatabase.
    /// Owned by ModelDatabase (long-lived); raw pointer is safe.
    /// May be nullptr if the visType didn't resolve — the renderer should
    /// skip drawing in that case (and ideally log a warning).
    const f4::models::ModelRecord* model_record{nullptr};

    /// Active LOD index into model_record->lods[]. Renderer picks based
    /// on distance to camera, but for the taxi demo we lock to LOD 0
    /// (highest detail) since the camera is close.
    int active_lod{0};

    /// Per-instance DOF and switch state. Mirrors FreeFalcon's
    /// DrawableBSP::DOFValues / SwitchValues / TextureSet.
    /// The host syncs gear-down via the FM's gear flag → switch mask each tick.
    /// For the F-16, switch #10 is the gear (0=down, 1=up) per
    /// f4-models-viewer/src/scene.cpp.
    f4::models::ModelState model_state{};

    /// Optional: which texture set (summer/winter/desert). Default 0.
    /// ModelRecord::n_texture_sets gives the count of available sets.
    int texture_set{0};
};

} // namespace f4::simulation

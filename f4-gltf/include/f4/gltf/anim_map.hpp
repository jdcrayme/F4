// f4-gltf/include/f4/gltf/anim_map.hpp
//
// Animation map — the runtime-side tag resolution pass described in
// ASSET_PIPELINE_SPEC.md §6.3 and Docs/AIRCRAFT_ANIMATION_PLAN.md §5.1.
//
// Built once per model at load (shared across instances). For every
// tagged node (f4 extras kind "dof", or "sw" branch nodes) it records:
//   - the node's chain from the model root (outermost first, inclusive),
//     which the renderer composes into world matrices;
//   - the semantic channel that drives it (from extras "channel"), as
//     the serialized f4::anim name;
//   - the op parameters (rot axis / trans vector / scale target) and
//     the XDOF value-processing flags.
//
// eval_tagged_local() evaluates one node's animated local transform
// from a raw channel value: pure double math, no rendering types —
// the renderer converts to its engine's matrix/quaternion forms.
// Value processing matches FreeFalcon's Process_DOFRot via f4-anim's
// process_dof_value (NEGATE → MINMAX → SUBRANGE → ISDOF → mult).
//
// Switch semantics (FreeFalcon bspnodes.cpp BSwitchNode::Draw): a
// switch channel value is a BITMASK; branch child k is drawn when
// bit k is set. BXSwitchNode's XSWT_REVERSED_EFFECT inverts the mask.

#pragma once

#include <f4/gltf/gltf_loader.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace f4::gltf {

/// One animatable tagged node in the model.
struct AnimNode {
    std::size_t node_index = 0;   ///< index into GltfDocument::nodes
    std::string kind;             ///< "dof" or "sw"
    std::string id;               ///< the §6 tag id (e.g. "gear_leg.0")
    std::string channel;          ///< serialized channel name ("" = unbound)
    int index = -1;               ///< original KoreaObj dof/switch number
    int sw_child = -1;            ///< switch branch bit index ("sw" only)
    bool reversed = false;        ///< switch mask inverted (BXSwitch)

    /// Node path from the model root, outermost first, INCLUSIVE of
    /// this node. Static (untagged) ancestors are excluded — the
    /// renderer composes the full path from the mesh's own node chain.
    std::vector<std::size_t> chain;
};

/// The per-model animation tag map. Shared const across instances;
/// per-instance state lives in f4::anim::AnimValues.
struct AnimMap {
    std::vector<AnimNode> nodes;

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }

    /// Distinct channel names referenced by the map, in first-seen
    /// order (deterministic — matches document node order).
    [[nodiscard]] std::vector<std::string> channels() const;

    /// Find the AnimNode for a document node index, or nullptr.
    [[nodiscard]] const AnimNode* find_by_node(
        std::size_t node_index) const noexcept;
};

/// Resolve the animation tags of a loaded document. Walks the default
/// scene's node graph; nodes outside the scene are ignored.
[[nodiscard]] AnimMap build_anim_map(const GltfDocument& doc);

/// True when the document carries any animation-relevant tag (dof or
/// sw branch) REACHABLE FROM THE DEFAULT SCENE — the same nodes
/// build_anim_map() would resolve. Callers use this to pick the
/// animated vs static draw path. (The legacy flat layout emits
/// dof/sw stub nodes as unreferenced orphans; those don't count.)
[[nodiscard]] bool has_animation_tags(const GltfDocument& doc) noexcept;

/// Compose a node's animated LOCAL transform for a raw channel value.
///
/// local = authored_transform ∘ op(value) where op is:
///   rot   → rotate about extras.axis by process_dof_value(value)
///   trans → translate along extras.trans by process_dof_value(value)
///   scale → scale factor lerp(1, extras.scale_target, processed value),
///           applied per component on top of the authored scale
/// (extras absent → FreeFalcon defaults: rot about +X, identity.)
///
/// Output: translation[3], rotation quaternion [x,y,z,w], scale[3].
/// `raw_value` is in the channel's sim units (radians / feet / 0..1).
/// For "sw" nodes this returns the authored transform (visibility is
/// handled through switch_mask_visible, not transforms).
void eval_tagged_local(const GltfDocument& doc, std::size_t node_index,
                       float raw_value, double out_translation[3],
                       double out_rotation[4], double out_scale[3]);

/// FreeFalcon switch visibility rule: bit k of the mask selects child
/// k; a reversed switch inverts the whole mask first (BXSwitchNode).
[[nodiscard]] inline bool switch_mask_visible(float mask_value, int child,
                                              bool reversed) noexcept {
    const uint32_t mask = reversed
                              ? ~static_cast<uint32_t>(
                                    static_cast<int32_t>(mask_value))
                              : static_cast<uint32_t>(
                                    static_cast<int32_t>(mask_value));
    return ((mask >> child) & 1u) != 0u;
}

} // namespace f4::gltf

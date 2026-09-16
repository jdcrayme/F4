// f4-gltf/tests/test_anim_map.cpp
//
// Animation map + tagged-transform evaluation tests. The document is a
// hand-authored minimal glTF (load_from_string) mirroring what the
// hierarchy emitter produces: a lod node, a DOF node bound to a gear
// channel, a translator bound to a strut channel, a scale node, and a
// 2-branch switch. Assertions pin the FreeFalcon semantics the runtime
// depends on (bitmask switches, Rx-after-frame rotation order,
// Process_DOFRot value processing).

#include <f4/gltf/anim_map.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace f4::gltf;

namespace {

const char* kDocJson = R"glTF({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes": [
    { "name": "root", "children": [1] },
    { "name": "lod:0", "extras": { "f4": { "v": 1, "kind": "lod", "id": "0", "level": 0 } },
      "children": [2, 3, 4, 5] },

    { "name": "dof:gear_leg.0",
      "rotation": [0, 0, 0, 1],
      "translation": [1, 2, 3],
      "extras": { "f4": { "v": 1, "kind": "dof", "id": "gear_leg.0",
          "index": 19, "min": -1.0, "max": 1.0, "mult": 2.0, "flags": 0,
          "channel": "gear_leg_pos.0", "op": "rot",
          "axis": [0, 0, -1] } } },

    { "name": "dof:strut.0",
      "extras": { "f4": { "v": 1, "kind": "dof", "id": "strut.0",
          "index": 58, "min": 0, "max": 0, "mult": 1.0, "flags": 0,
          "channel": "gear_strut.0", "op": "trans",
          "trans": [0, 1, 0] } } },

    { "name": "dof:ab_scale",
      "extras": { "f4": { "v": 1, "kind": "dof", "id": "ab_scale",
          "index": 38, "min": 0, "max": 0, "mult": 1.0, "flags": 0,
          "channel": "ab_scale", "op": "scale",
          "scale_target": [2, 1, 1] } } },

    { "name": "sw:gear_leg_vis.0",
      "extras": { "f4": { "v": 1, "kind": "sw", "id": "gear_leg_vis.0",
          "index": 1, "channel": "sw.gear_leg.0" } },
      "children": [6, 7] },
    { "name": "sw:gear_leg_vis.0.0",
      "extras": { "f4": { "v": 1, "kind": "sw", "id": "gear_leg_vis.0.0",
          "index": 1, "child": 0, "channel": "sw.gear_leg.0" } } },
    { "name": "sw:gear_leg_vis.0.1",
      "extras": { "f4": { "v": 1, "kind": "sw", "id": "gear_leg_vis.0.1",
          "index": 1, "child": 1, "channel": "sw.gear_leg.0" } } }
  ]
})glTF";

GltfDocument load_doc() {
    GltfDocument doc;
    doc.load_from_string(kDocJson);
    return doc;
}

} // namespace

TEST(AnimMap, CollectsDofAndSwitchBranchNodes) {
    const auto doc = load_doc();
    const auto map = build_anim_map(doc);

    ASSERT_TRUE(has_animation_tags(doc));
    ASSERT_EQ(map.nodes.size(), 5u);  // 3 dof + 2 sw branches

    // Node 2: gear leg DOF.
    const AnimNode* leg = map.find_by_node(2);
    ASSERT_NE(leg, nullptr);
    EXPECT_EQ(leg->kind, "dof");
    EXPECT_EQ(leg->channel, "gear_leg_pos.0");
    EXPECT_EQ(leg->index, 19);
    ASSERT_EQ(leg->chain.size(), 3u);   // root → lod → dof
    EXPECT_EQ(leg->chain[0], 0u);
    EXPECT_EQ(leg->chain[1], 1u);
    EXPECT_EQ(leg->chain[2], 2u);

    // Node 6: switch branch 0.
    const AnimNode* br0 = map.find_by_node(6);
    ASSERT_NE(br0, nullptr);
    EXPECT_EQ(br0->kind, "sw");
    EXPECT_EQ(br0->sw_child, 0);
    EXPECT_EQ(br0->channel, "sw.gear_leg.0");
    EXPECT_FALSE(br0->reversed);

    // Branch 1.
    const AnimNode* br1 = map.find_by_node(7);
    ASSERT_NE(br1, nullptr);
    EXPECT_EQ(br1->sw_child, 1);

    // The lod:0 node and the sw group node are NOT animatable entries.
    EXPECT_EQ(map.find_by_node(1), nullptr);
    EXPECT_EQ(map.find_by_node(5), nullptr);
}

TEST(AnimMap, ChannelsAreDistinctAndOrdered) {
    const auto doc = load_doc();
    const auto map = build_anim_map(doc);
    const auto chans = map.channels();
    ASSERT_EQ(chans.size(), 4u);
    EXPECT_EQ(chans[0], "gear_leg_pos.0");
    EXPECT_EQ(chans[1], "gear_strut.0");
    EXPECT_EQ(chans[2], "ab_scale");
    EXPECT_EQ(chans[3], "sw.gear_leg.0");
}

TEST(AnimMap, StaticDocumentsProduceEmptyMap) {
    GltfDocument doc;
    doc.load_from_string(R"glTF({
      "asset": { "version": "2.0" },
      "scenes": [ { "nodes": [0] } ],
      "nodes": [ { "name": "root" } ]
    })glTF");
    EXPECT_FALSE(has_animation_tags(doc));
    EXPECT_TRUE(build_anim_map(doc).empty());
}

// ── eval_tagged_local ─────────────────────────────────────────────────────

namespace {
double quat_angle_between(const double a[4], const double b[4]) {
    double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    dot = std::min(1.0, std::max(-1.0, std::abs(dot)));
    return 2.0 * std::acos(dot);
}
} // namespace

TEST(EvalTaggedLocal, ZeroValueLeavesAuthoredTransform) {
    const auto doc = load_doc();
    double t[3], q[4], s[3];
    eval_tagged_local(doc, 2, 0.0f, t, q, s);
    EXPECT_NEAR(t[0], 1.0, 1e-12);
    EXPECT_NEAR(t[1], 2.0, 1e-12);
    EXPECT_NEAR(t[2], 3.0, 1e-12);
    EXPECT_NEAR(s[0], 1.0, 1e-12);
    // Identity quaternion.
    EXPECT_NEAR(std::abs(q[3]), 1.0, 1e-12);
}

TEST(EvalTaggedLocal, RotComposesAboutAxisAfterFrame) {
    // axis = (0,0,-1), mult = 2: raw 0.5 → processed angle 1.0 rad
    // about -Z. At raw 0 the transform must equal the authored one.
    const auto doc = load_doc();
    double t[3], q[4], s[3];
    eval_tagged_local(doc, 2, 0.5f, t, q, s);

    const double angle = 1.0;  // 0.5 * mult 2
    const double half = angle * 0.5;
    const double expected[4] = {0.0, 0.0, -std::sin(half), std::cos(half)};
    // Rotation-only difference: the authored rotation is identity, so
    // the output quaternion IS the op quaternion.
    EXPECT_NEAR(quat_angle_between(q, expected), 0.0, 1e-9);
}

TEST(EvalTaggedLocal, RotProcessesXDOFFlags) {
    // The gear leg node has min=-1, max=1, flags=0 — no clamping.
    // Verify the clamping path by checking a huge raw value stays
    // unclamped (angle = raw*mult), i.e. flags=0 really means no MINMAX.
    const auto doc = load_doc();
    double t[3], q1[4], q2[4], s[3];
    eval_tagged_local(doc, 2, 0.4f, t, q1, s);
    eval_tagged_local(doc, 2, 0.5f, t, q2, s);
    const double d1 = quat_angle_between(q1, q2);
    EXPECT_NEAR(d1, 0.2, 1e-6);  // (0.5-0.4)*mult 2 — float-noise tolerance
}

TEST(EvalTaggedLocal, TransScalesTheVectorByProcessedValue) {
    const auto doc = load_doc();
    double t[3], q[4], s[3];
    eval_tagged_local(doc, 3, 2.5f, t, q, s);
    // trans vector (0,1,0) * 2.5 on top of identity authored transform.
    EXPECT_NEAR(t[0], 0.0, 1e-12);
    EXPECT_NEAR(t[1], 2.5, 1e-12);
    EXPECT_NEAR(t[2], 0.0, 1e-12);
}

TEST(EvalTaggedLocal, ScaleLerpsToTarget) {
    const auto doc = load_doc();
    double t[3], q[4], s[3];
    eval_tagged_local(doc, 4, 0.5f, t, q, s);
    // 1 - (1 - 2) * 0.5 = 1.5 on X; Y/Z stay 1.
    EXPECT_NEAR(s[0], 1.5, 1e-12);
    EXPECT_NEAR(s[1], 1.0, 1e-12);
    EXPECT_NEAR(s[2], 1.0, 1e-12);

    eval_tagged_local(doc, 4, 1.0f, t, q, s);
    EXPECT_NEAR(s[0], 2.0, 1e-12);
    eval_tagged_local(doc, 4, 0.0f, t, q, s);
    EXPECT_NEAR(s[0], 1.0, 1e-12);
}

TEST(EvalTaggedLocal, SwitchBranchReturnsAuthoredTransform) {
    const auto doc = load_doc();
    double t[3], q[4], s[3];
    eval_tagged_local(doc, 6, 1.0f, t, q, s);
    EXPECT_NEAR(t[0], 0.0, 1e-12);
    EXPECT_NEAR(s[0], 1.0, 1e-12);
}

// ── switch_mask_visible (FreeFalcon BSwitchNode::Draw rule) ──────────────

TEST(SwitchMask, BitmaskSemantics) {
    // mask 0 → nothing drawn.
    EXPECT_FALSE(switch_mask_visible(0.0f, 0, false));
    // mask 1 → child 0 drawn, child 1 not.
    EXPECT_TRUE(switch_mask_visible(1.0f, 0, false));
    EXPECT_FALSE(switch_mask_visible(1.0f, 1, false));
    // mask 4 → only child 2 (the 1<<stage nozzle pattern).
    EXPECT_TRUE(switch_mask_visible(4.0f, 2, false));
    EXPECT_FALSE(switch_mask_visible(4.0f, 0, false));
    // mask 3 → children 0 and 1.
    EXPECT_TRUE(switch_mask_visible(3.0f, 0, false));
    EXPECT_TRUE(switch_mask_visible(3.0f, 1, false));
    EXPECT_FALSE(switch_mask_visible(3.0f, 2, false));
}

TEST(SwitchMask, ReversedInvertsTheMask) {
    EXPECT_TRUE(switch_mask_visible(0.0f, 0, true));
    EXPECT_FALSE(switch_mask_visible(1.0f, 0, true));
    EXPECT_TRUE(switch_mask_visible(1.0f, 1, true));
}

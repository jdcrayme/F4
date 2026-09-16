// f4-models/tests/test_grouped_extraction.cpp
//
// Grouped (hierarchy-preserving) extraction tests — the converter's
// input contract (Docs/AIRCRAFT_ANIMATION_PLAN.md §4.1). Uses the real
// KoreaObj fixture: model 1 is an F-16 with 11 DOFs, 7 switches, 9
// slots (4 DOF nodes + 7 switch nodes in LOD 0 per probe).

#include <f4/models/geometry_grouped.hpp>
#include <f4/models/model_database.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
fs::path fixture_hdr() {
    return MODELS_FIXTURE_DIR + std::string("KoreaObj.HDR");
}
fs::path fixture_lod() {
    return MODELS_FIXTURE_DIR + std::string("KoreaObj.LOD");
}

struct LoadedF16 {
    std::unique_ptr<f4::models::ModelDatabase> db;
};

LoadedF16 load_f16() {
    LoadedF16 out;
    out.db = std::make_unique<f4::models::ModelDatabase>();
    std::string err = out.db->load(fixture_hdr(), fixture_lod());
    if (!err.empty()) throw std::runtime_error(err);
    err = out.db->parse_model(1);
    if (!err.empty()) throw std::runtime_error(err);
    return out;
}
} // namespace

TEST(GroupedExtraction, F16ProducesGroups) {
    if (!fs::exists(fixture_hdr())) GTEST_SKIP() << "fixture missing";
    auto loaded = load_f16();

    const auto geom =
        loaded.db->extract_model_geometry_grouped(1, 0);
    ASSERT_FALSE(geom.meshes.empty());
    EXPECT_GT(geom.total_vertices(), 0u);
    EXPECT_GT(geom.total_triangles(), 0u);
}

TEST(GroupedExtraction, TriangleCountMatchesFlatExtraction) {
    // The grouped walk visits the same primitives as the flat walk at
    // default state (which shows all switch children) — only the
    // grouping and the vertex space differ. Any drift here means the
    // hierarchy path silently drops or duplicates geometry.
    if (!fs::exists(fixture_hdr())) GTEST_SKIP() << "fixture missing";
    auto loaded = load_f16();

    const auto flat = loaded.db->extract_model_geometry(1, 0);
    const auto grouped = loaded.db->extract_model_geometry_grouped(1, 0);

    std::size_t flat_tris = 0;
    for (const auto& m : flat.meshes) flat_tris += m.triangles.size();
    EXPECT_EQ(flat.total_vertices(), grouped.total_vertices());
    EXPECT_EQ(flat_tris, grouped.total_triangles());
}

TEST(GroupedExtraction, ChainsMatchF16TaggedNodes) {
    if (!fs::exists(fixture_hdr())) GTEST_SKIP() << "fixture missing";
    auto loaded = load_f16();

    const auto geom =
        loaded.db->extract_model_geometry_grouped(1, 0);

    // Collect the distinct tagged ancestors across all chains.
    std::set<int32_t> dof_numbers;
    std::set<int32_t> switch_numbers;
    std::set<int32_t> switch_children;
    bool has_static_group = false;

    for (const auto& gm : geom.meshes) {
        if (gm.chain.empty()) {
            has_static_group = true;
            continue;
        }
        for (const auto& a : gm.chain) {
            switch (a.type) {
                case f4::models::BspNodeType::BDofNode:
                case f4::models::BspNodeType::BXDofNode:
                case f4::models::BspNodeType::BTransNode:
                case f4::models::BspNodeType::BScaleNode:
                    dof_numbers.insert(a.number);
                    break;
                case f4::models::BspNodeType::BSwitchNode:
                case f4::models::BspNodeType::BXSwitchNode:
                    switch_numbers.insert(a.number);
                    switch_children.insert(a.switch_child);
                    EXPECT_GE(a.switch_child, 0);
                    break;
                default:
                    ADD_FAILURE() << "unexpected ancestor type in chain";
            }
        }
    }

    EXPECT_TRUE(has_static_group)
        << "the F-16 has untagged fuselage geometry — it must form the "
           "empty-chain static group";

    // From the probe: DOF nodes 2 (lt flap), 3 (rt flap), 9 (lt LEF),
    // 10 (rt LEF); switches 0 (AB), 1/2/3 (gear visibility), 6 (vapor,
    // 2 instances × 4 children each).
    EXPECT_EQ(dof_numbers, (std::set<int32_t>{2, 3, 9, 10}));
    EXPECT_EQ(switch_numbers, (std::set<int32_t>{0, 1, 2, 3, 6}));
    EXPECT_FALSE(switch_children.empty());
}

TEST(GroupedExtraction, SwitchBranchesCarryChildIndices) {
    // Switch 6 (wing vapor) has 4 children per instance — the grouped
    // walk must emit a separate branch group per child, and every
    // switch ancestor's child index must be bit-indexable.
    if (!fs::exists(fixture_hdr())) GTEST_SKIP() << "fixture missing";
    auto loaded = load_f16();

    const auto geom =
        loaded.db->extract_model_geometry_grouped(1, 0);

    int switch6_child0 = 0;
    int switch6_child1 = 0;
    int switch6_child2 = 0;
    int switch6_child3 = 0;

    for (const auto& gm : geom.meshes) {
        for (const auto& a : gm.chain) {
            if ((a.type == f4::models::BspNodeType::BSwitchNode ||
                 a.type == f4::models::BspNodeType::BXSwitchNode) &&
                a.number == 6) {
                switch (a.switch_child) {
                    case 0: ++switch6_child0; break;
                    case 1: ++switch6_child1; break;
                    case 2: ++switch6_child2; break;
                    case 3: ++switch6_child3; break;
                    default: ADD_FAILURE() << "child out of range";
                }
            }
        }
    }
    // Two vapor switch instances (left/right wing) × 4 children.
    EXPECT_EQ(switch6_child0, 2);
    EXPECT_EQ(switch6_child1, 2);
    EXPECT_EQ(switch6_child2, 2);
    EXPECT_EQ(switch6_child3, 2);
}

TEST(GroupedExtraction, DofAncestorsCarryNodeData) {
    // Every DOF ancestor must carry the node's frame + value-processing
    // parameters verbatim — the runtime reconstructs transforms from
    // these, so losing them loses the animation pivot.
    if (!fs::exists(fixture_hdr())) GTEST_SKIP() << "fixture missing";
    auto loaded = load_f16();

    const auto geom =
        loaded.db->extract_model_geometry_grouped(1, 0);

    int dof_ancestors = 0;
    for (const auto& gm : geom.meshes) {
        for (const auto& a : gm.chain) {
            if (a.type == f4::models::BspNodeType::BDofNode ||
                a.type == f4::models::BspNodeType::BXDofNode) {
                ++dof_ancestors;
                EXPECT_GE(a.node_index, 0);
                EXPECT_GE(a.number, 0);
                // The frame must be orthonormal (dof_rotation is a
                // pure orientation into parent space).
                for (int col = 0; col < 3; ++col) {
                    const float len =
                        std::sqrt(a.frame_rotation.m[0][col] *
                                      a.frame_rotation.m[0][col] +
                                  a.frame_rotation.m[1][col] *
                                      a.frame_rotation.m[1][col] +
                                  a.frame_rotation.m[2][col] *
                                      a.frame_rotation.m[2][col]);
                    EXPECT_NEAR(len, 1.0f, 1e-4f)
                        << "dof " << a.number << " frame col " << col;
                }
            }
        }
    }
    // 4 DOF nodes × at least the geometry directly under them (the
    // flap/LEF groups) — one ancestor entry per group mesh.
    EXPECT_GE(dof_ancestors, 4);
}

TEST(GroupedExtraction, IsDeterministic) {
    if (!fs::exists(fixture_hdr())) GTEST_SKIP() << "fixture missing";
    auto loaded = load_f16();

    const auto a = loaded.db->extract_model_geometry_grouped(1, 0);
    const auto b = loaded.db->extract_model_geometry_grouped(1, 0);

    ASSERT_EQ(a.meshes.size(), b.meshes.size());
    for (std::size_t i = 0; i < a.meshes.size(); ++i) {
        ASSERT_EQ(a.meshes[i].chain.size(), b.meshes[i].chain.size());
        ASSERT_EQ(a.meshes[i].mesh.vertices.size(),
                  b.meshes[i].mesh.vertices.size());
        ASSERT_EQ(a.meshes[i].mesh.triangles.size(),
                  b.meshes[i].mesh.triangles.size());
    }
}

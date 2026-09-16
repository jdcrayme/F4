// f4-import/tests/test_hierarchy_emit.cpp
//
// End-to-end hierarchy emission tests (AIRCRAFT_ANIMATION_PLAN.md §4):
// emit the fixture F-16 (model 1) with --hierarchy semantics, load the
// result back with the runtime f4-gltf loader, build the animation
// map, and verify the converter produced an animatable model — tagged
// hierarchy, family-bound channels, real node transforms, and
// triangle parity with the flat path.

#include <f4/gltf/anim_map.hpp>
#include <f4/gltf/gltf_loader.hpp>
#include <f4/import/gltf_emitter.hpp>
#include <f4/import/vocab.hpp>
#include <f4/models/model_database.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>

namespace fs = std::filesystem;

using namespace f4::import;
using f4::models::ModelDatabase;

namespace {

fs::path make_temp_dir(const std::string& suffix) {
    auto p = fs::temp_directory_path() / "f4_hierarchy_emit_test" / suffix;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::unique_ptr<ModelDatabase> load_db() {
    auto db = std::make_unique<ModelDatabase>();
    fs::path fixture = KOREAOBJ_FIXTURE_DIR;
    std::string err = db->load(fixture / "KoreaObj.HDR",
                               fixture / "KoreaObj.LOD");
    if (!err.empty()) throw std::runtime_error(err);
    err = db->parse_model(1);
    if (!err.empty()) throw std::runtime_error(err);
    return db;
}

const FamilyTable& complex_table() {
    static FamilyTable table = [] {
        fs::path p = F4_VOCAB_SOURCE_DIR "/family/complex.json";
        return load_family_table(p);
    }();
    return table;
}

struct HierarchyEmit {
    GltfEmitResult result;
    f4::gltf::GltfDocument doc;
};

HierarchyEmit emit_f16(bool with_family) {
    auto db = load_db();
    auto dir = make_temp_dir(with_family ? "family" : "nofamily");
    GltfEmitOptions opts;
    opts.emit_hierarchy = true;
    if (with_family) opts.family_table = &complex_table();
    HierarchyEmit out;
    out.result = emit_model_as_gltf(*db, 1, dir, "koreaobj:00001", opts);
    out.doc.load(out.result.gltf_path);
    return out;
}

const f4::gltf::Node* find_node(const f4::gltf::GltfDocument& doc,
                                const std::string& name) {
    for (const auto& n : doc.nodes) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

} // namespace

TEST(HierarchyEmit, FlatPathStaysDefault) {
    // Without emit_hierarchy the emitter must keep the legacy shape:
    // one LOD_<n> mesh, placeholder unknown.N nodes — the existing
    // Data/ exports and the static draw path stay untouched.
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    auto db = load_db();
    auto dir = make_temp_dir("flat");
    GltfEmitOptions opts;  // emit_hierarchy defaults to false
    const auto result = emit_model_as_gltf(*db, 1, dir, "koreaobj:00001", opts);
    EXPECT_TRUE(result.channels.empty());
    f4::gltf::GltfDocument doc;
    doc.load(result.gltf_path);
    bool has_lod_mesh = false;
    for (const auto& m : doc.meshes) {
        if (m.name.rfind("LOD_", 0) == 0) has_lod_mesh = true;
    }
    EXPECT_TRUE(has_lod_mesh);
    EXPECT_FALSE(f4::gltf::has_animation_tags(doc));
}

TEST(HierarchyEmit, F16RoundTripsWithBoundChannels) {
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    const auto out = emit_f16(true);

    // The family table bound the F-16's DOFs: 2/3 → flap.l/flap.r,
    // 9/10 → lef.l/lef.r.
    const auto* flap_l = find_node(out.doc, "dof:flap.l");
    ASSERT_NE(flap_l, nullptr);
    ASSERT_TRUE(flap_l->has_f4);
    EXPECT_EQ(flap_l->f4.channel.value_or(""), "flap.l");
    EXPECT_EQ(flap_l->f4.dof_index.value_or(-1), 2);

    const auto* lef_r = find_node(out.doc, "dof:lef.r");
    ASSERT_NE(lef_r, nullptr);
    EXPECT_EQ(lef_r->f4.channel.value_or(""), "lef.r");

    // Switch 6 (vapor) became a group with 4 branch children.
    const auto* vapor = find_node(out.doc, "sw:vapor");
    ASSERT_NE(vapor, nullptr);
    EXPECT_EQ(vapor->f4.channel.value_or(""), "effect.vapor");
    EXPECT_EQ(vapor->children.size(), 4u);
    bool has_branch1 = false;
    for (const auto c : vapor->children) {
        if (out.doc.nodes[c].name == "sw:vapor.1") has_branch1 = true;
    }
    EXPECT_TRUE(has_branch1);

    // The animation map resolves the channels.
    const auto map = f4::gltf::build_anim_map(out.doc);
    EXPECT_FALSE(map.empty());
    const auto chans = map.channels();
    EXPECT_TRUE(std::find(chans.begin(), chans.end(), "flap.l") !=
                chans.end());
    EXPECT_TRUE(std::find(chans.begin(), chans.end(), "sw.gear_leg.0") !=
                chans.end());

    // Result reports the bound channels.
    EXPECT_FALSE(out.result.channels.empty());
    EXPECT_TRUE(std::find(out.result.channels.begin(),
                          out.result.channels.end(),
                          "flap.l") != out.result.channels.end());
}

TEST(HierarchyEmit, DofNodeCarriesFrameAndOp) {
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    const auto out = emit_f16(true);

    const auto* flap_l = find_node(out.doc, "dof:flap.l");
    ASSERT_NE(flap_l, nullptr);

    // The authored frame must survive: the pivot translation (probe
    // showed falcon (-21.22, -11.49, 3.26) for dof 2 — the flap hinge)
    // must land as a glTF translation (basis-converted, meters).
    ASSERT_TRUE(flap_l->translation.has_value());
    const auto& t = *flap_l->translation;
    // falcon y → glTF x: -11.49 ft * 0.3048 ≈ -3.50 m
    EXPECT_NEAR(t[0], -11.49 * 0.3048, 0.01);
    // falcon z → glTF y: 3.26 ft * 0.3048 ≈ 0.99 m
    EXPECT_NEAR(t[1], 3.26 * 0.3048, 0.01);
    // -falcon x → glTF z: 21.22 ft * 0.3048 ≈ 6.47 m
    EXPECT_NEAR(t[2], 21.22 * 0.3048, 0.01);

    // A rotation (the frame orientation) and the rot op with the
    // falcon-X axis mapped to glTF -Z.
    ASSERT_TRUE(flap_l->rotation.has_value());
    EXPECT_EQ(flap_l->f4.op.value_or(""), "rot");
    ASSERT_TRUE(flap_l->f4.axis.has_value());
    EXPECT_NEAR((*flap_l->f4.axis)[0], 0.0, 1e-9);
    EXPECT_NEAR((*flap_l->f4.axis)[1], 0.0, 1e-9);
    EXPECT_NEAR((*flap_l->f4.axis)[2], -1.0, 1e-9);

    // Quaternion is unit length.
    const auto& q = *flap_l->rotation;
    const double len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                                 q[3] * q[3]);
    EXPECT_NEAR(len, 1.0, 1e-9);
}

TEST(HierarchyEmit, TriangleParityWithFlatPath) {
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    auto db = load_db();
    auto flat_dir = make_temp_dir("parity_flat");
    auto hier_dir = make_temp_dir("parity_hier");

    GltfEmitOptions flat_opts;
    const auto flat = emit_model_as_gltf(*db, 1, flat_dir, "koreaobj:00001",
                                         flat_opts);
    GltfEmitOptions hier_opts;
    hier_opts.emit_hierarchy = true;
    hier_opts.family_table = &complex_table();
    const auto hier = emit_model_as_gltf(*db, 1, hier_dir, "koreaobj:00001",
                                         hier_opts);

    // The hierarchy must not gain or lose a single triangle.
    EXPECT_EQ(hier.total_vertices, flat.total_vertices);
    EXPECT_EQ(hier.total_triangles, flat.total_triangles);
    EXPECT_EQ(hier.primitive_count, flat.primitive_count);
    EXPECT_EQ(hier.lod_count, flat.lod_count);
}

TEST(HierarchyEmit, WithoutFamilyEverythingIsUnknownAndUnbound) {
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    const auto out = emit_f16(false);

    // No family table → semantic naming never happens; the F-16's
    // DOF 2 stays unknown.2 with no channel binding.
    const auto* unknown2 = find_node(out.doc, "dof:unknown.2");
    ASSERT_NE(unknown2, nullptr);
    ASSERT_TRUE(unknown2->has_f4);
    EXPECT_FALSE(unknown2->f4.channel.has_value());

    // The anim map still resolves the hierarchy (switch branches are
    // animatable through state), but no channels are bound.
    const auto map = f4::gltf::build_anim_map(out.doc);
    EXPECT_FALSE(map.empty());
    EXPECT_TRUE(map.channels().empty());
}

TEST(HierarchyEmit, VocabTablesLoadAndCoverKeyIndices) {
    const auto tables = load_family_tables(F4_VOCAB_SOURCE_DIR);
    ASSERT_EQ(tables.size(), 4u);  // complex, simple, heli, airdef
    ASSERT_TRUE(tables.count("complex"));

    const auto& complex = tables.at("complex");
    // Gear stacks: FF COMP_GEAR_1..3 = dof 19..21; doors 22..24.
    EXPECT_EQ(complex.dof_at(19)->channel, "gear_leg_pos.0");
    EXPECT_EQ(complex.dof_at(24)->channel, "gear_door_pos.2");
    EXPECT_EQ(complex.dof_at(68)->channel, "gear_leg_pos.3");
    // Wheels 50..57, struts 58..65.
    EXPECT_EQ(complex.dof_at(50)->channel, "wheel_angle.0");
    EXPECT_EQ(complex.dof_at(65)->channel, "gear_strut.7");
    // Switches: AB=0, gear vis 1..3, doors 14..16, holes 17..19,
    // broken 20..22.
    EXPECT_EQ(complex.sw_at(0)->channel, "sw.ab");
    EXPECT_EQ(complex.sw_at(2)->channel, "sw.gear_leg.1");
    EXPECT_EQ(complex.sw_at(17)->channel, "sw.gear_hole.0");
    EXPECT_EQ(complex.sw_at(22)->channel, "sw.gear_broken.2");
    // Unmapped indices degrade to nullptr (stays unknown.N).
    EXPECT_EQ(complex.dof_at(6), nullptr);
    EXPECT_EQ(complex.dof_at(100), nullptr);
}

TEST(HierarchyEmit, FamilyGuessClassifiesFixtureModels) {
    // The F-16 (11 dofs, 7 switches, 9 slots) must classify complex.
    auto db = load_db();
    const auto* rec = db->model(1);
    EXPECT_EQ(guess_family(rec->effective_dofs(), rec->effective_switches(),
                           static_cast<int>(rec->slots.size())),
              "complex");
}

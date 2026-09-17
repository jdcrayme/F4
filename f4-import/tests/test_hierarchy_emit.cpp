// f4-import/tests/test_hierarchy_emit.cpp
//
// End-to-end hierarchy emission tests (AIRCRAFT_ANIMATION_PLAN.md §4):
// emit the fixture F-16 (model 1) with --hierarchy semantics, load the
// result back with the runtime f4-gltf loader, build the animation
// map, and verify the converter produced an animatable model — tagged
// hierarchy, family-bound channels, real node transforms, and
// triangle parity with the flat path.

#include <f4/anim/channels.hpp>
#include <f4/gltf/anim_map.hpp>
#include <f4/gltf/gltf_loader.hpp>
#include <f4/import/gltf_emitter.hpp>
#include <f4/import/vocab.hpp>
#include <f4/models/model_database.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

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

// ── Rest-pose assembly (mirrors the runtime animated draw math) ──────────

void quat_to_mat9(const double q[4], double m[3][3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
}

// Assemble the rest-pose (channel value 0) vertex positions of every
// mesh node under the lod:<level> node, applying each node's chain
// frames exactly the way the runtime animated draw path composes them
// (eval_tagged_local per node, T·R·S; applying the frames as a
// function chain innermost-first is equivalent to the runtime's
// m = m·local matrix fold outermost-first). Returns glTF-space
// positions.
std::vector<std::array<double, 3>> assemble_rest_pose(
    const f4::gltf::GltfDocument& doc, int lod_level) {
    const f4::gltf::Node* lod = nullptr;
    for (const auto& n : doc.nodes) {
        if (n.has_f4 && n.f4.kind == "lod" &&
            n.f4.lod_level.value_or(-1) == lod_level) {
            lod = &n;
            break;
        }
    }
    if (!lod) return {};

    struct Frame { std::size_t node; std::size_t depth; };
    std::vector<Frame> stack;
    for (auto it = lod->children.rbegin(); it != lod->children.rend(); ++it)
        stack.push_back({*it, 0});
    std::vector<std::size_t> path;
    std::vector<std::array<double, 3>> out;

    while (!stack.empty()) {
        const auto fr = stack.back();
        stack.pop_back();
        if (fr.node >= doc.nodes.size()) continue;
        path.resize(fr.depth);
        path.push_back(fr.node);
        const auto& node = doc.nodes[fr.node];
        if (node.mesh.has_value() && *node.mesh < doc.meshes.size()) {
            for (const auto& prim : doc.meshes[*node.mesh].primitives) {
                if (!prim.positions || *prim.positions >= doc.accessors.size())
                    continue;
                const auto& acc = doc.accessors[*prim.positions];
                for (std::size_t i = 0; i < acc.count; ++i) {
                    auto v = doc.read_vec3_float(*prim.positions, i);
                    if (!v) continue;
                    double p[3] = {(*v)[0], (*v)[1], (*v)[2]};
                    for (auto it = path.rbegin(); it != path.rend(); ++it) {
                        double t[3], q[4], s[3];
                        f4::gltf::eval_tagged_local(doc, *it, 0.0f, t, q, s);
                        double r[3][3];
                        quat_to_mat9(q, r);
                        double tmp[3];
                        for (int a = 0; a < 3; ++a) {
                            tmp[a] = r[a][0] * p[0] * s[0] +
                                     r[a][1] * p[1] * s[1] +
                                     r[a][2] * p[2] * s[2];
                        }
                        p[0] = tmp[0] + t[0];
                        p[1] = tmp[1] + t[1];
                        p[2] = tmp[2] + t[2];
                    }
                    out.push_back({p[0], p[1], p[2]});
                }
            }
        }
        for (auto it = node.children.rbegin(); it != node.children.rend();
             ++it) {
            stack.push_back({*it, fr.depth + 1});
        }
    }
    return out;
}

// All POSITION values of one mesh (the flat path's LOD_<n> output).
std::vector<std::array<double, 3>> mesh_positions(
    const f4::gltf::GltfDocument& doc, const std::string& mesh_name) {
    std::vector<std::array<double, 3>> out;
    for (const auto& m : doc.meshes) {
        if (m.name != mesh_name) continue;
        for (const auto& prim : m.primitives) {
            if (!prim.positions || *prim.positions >= doc.accessors.size())
                continue;
            const auto& acc = doc.accessors[*prim.positions];
            for (std::size_t i = 0; i < acc.count; ++i) {
                auto v = doc.read_vec3_float(*prim.positions, i);
                if (v) out.push_back({(*v)[0], (*v)[1], (*v)[2]});
            }
        }
    }
    return out;
}

// Assemble with ANIMATED channel values (mirrors anim_node_local_matrix's
// channel lookup + eval_tagged_local per chain node).
std::vector<std::array<double, 3>> assemble_animated(
    const f4::gltf::GltfDocument& doc, int lod_level,
    const f4::anim::AnimValues& values) {
    const f4::gltf::Node* lod = nullptr;
    for (const auto& n : doc.nodes) {
        if (n.has_f4 && n.f4.kind == "lod" &&
            n.f4.lod_level.value_or(-1) == lod_level) {
            lod = &n;
            break;
        }
    }
    if (!lod) return {};

    struct Frame { std::size_t node; std::size_t depth; };
    std::vector<Frame> stack;
    for (auto it = lod->children.rbegin(); it != lod->children.rend(); ++it)
        stack.push_back({*it, 0});
    std::vector<std::size_t> path;
    std::vector<std::array<double, 3>> out;

    while (!stack.empty()) {
        const auto fr = stack.back();
        stack.pop_back();
        if (fr.node >= doc.nodes.size()) continue;
        path.resize(fr.depth);
        path.push_back(fr.node);
        const auto& node = doc.nodes[fr.node];
        if (node.mesh.has_value() && *node.mesh < doc.meshes.size()) {
            for (const auto& prim : doc.meshes[*node.mesh].primitives) {
                if (!prim.positions || *prim.positions >= doc.accessors.size())
                    continue;
                const auto& acc = doc.accessors[*prim.positions];
                for (std::size_t i = 0; i < acc.count; ++i) {
                    auto v = doc.read_vec3_float(*prim.positions, i);
                    if (!v) continue;
                    double p[3] = {(*v)[0], (*v)[1], (*v)[2]};
                    for (auto rit = path.rbegin(); rit != path.rend(); ++rit) {
                        const auto& cn = doc.nodes[*rit];
                        float value = 0.0f;
                        if (cn.has_f4 && cn.f4.kind == "dof" &&
                            cn.f4.channel.has_value()) {
                            if (auto ch = f4::anim::channel_from_name(
                                    *cn.f4.channel)) {
                                value = values[*ch];
                            }
                        }
                        double t[3], q[4], s[3];
                        f4::gltf::eval_tagged_local(doc, *rit, value, t, q, s);
                        double r[3][3];
                        quat_to_mat9(q, r);
                        double tmp[3];
                        for (int a = 0; a < 3; ++a) {
                            tmp[a] = r[a][0] * p[0] * s[0] +
                                     r[a][1] * p[1] * s[1] +
                                     r[a][2] * p[2] * s[2];
                        }
                        p[0] = tmp[0] + t[0];
                        p[1] = tmp[1] + t[1];
                        p[2] = tmp[2] + t[2];
                    }
                    out.push_back({p[0], p[1], p[2]});
                }
            }
        }
        for (auto it = node.children.rbegin(); it != node.children.rend();
             ++it) {
            stack.push_back({*it, fr.depth + 1});
        }
    }
    return out;
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
    // falcon-X rotation conjugated to glTF +Z: the basis B is improper
    // (det = −1), so B·Rx(θ)·Bᵀ = Rot(+Z, θ) — the axis is +Z, NOT
    // B·x̂ = −Z (which would invert every DOF's deflection).
    ASSERT_TRUE(flap_l->rotation.has_value());
    EXPECT_EQ(flap_l->f4.op.value_or(""), "rot");
    ASSERT_TRUE(flap_l->f4.axis.has_value());
    EXPECT_NEAR((*flap_l->f4.axis)[0], 0.0, 1e-9);
    EXPECT_NEAR((*flap_l->f4.axis)[1], 0.0, 1e-9);
    EXPECT_NEAR((*flap_l->f4.axis)[2], 1.0, 1e-9);

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

TEST(HierarchyEmit, RestPoseAssemblesToFlatGeometry) {
    // THE placement contract: applying the hierarchy's chain frames at
    // channel value 0 (the runtime animated draw path's rest pose) must
    // reproduce the flat path's baked model-space geometry vertex-for-
    // vertex. A miss means dof frames double-apply, are dropped, or are
    // basis-converted wrong — the model renders with displaced control
    // surfaces even before anything animates.
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    auto db = load_db();

    auto flat_dir = make_temp_dir("restpose_flat");
    GltfEmitOptions flat_opts;
    const auto flat = emit_model_as_gltf(*db, 1, flat_dir, "koreaobj:00001",
                                         flat_opts);
    const auto hier = emit_f16(true);

    f4::gltf::GltfDocument fd;
    fd.load(flat.gltf_path);
    f4::gltf::GltfDocument hd;
    hd.load(hier.result.gltf_path);

    const auto assembled = assemble_rest_pose(hd, 0);
    const auto flatv = mesh_positions(fd, "LOD_0");
    ASSERT_FALSE(assembled.empty());
    ASSERT_EQ(assembled.size(), flatv.size());

    // Every assembled vertex must land on some flat vertex (±1 mm).
    double worst = 0.0;
    std::array<double, 3> worst_p{};
    for (const auto& p : assembled) {
        double best = 1e30;
        for (const auto& q : flatv) {
            const double dx = p[0] - q[0], dy = p[1] - q[1],
                          dz = p[2] - q[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) {
                best = d2;
            }
        }
        const double d = std::sqrt(best);
        if (d > worst) {
            worst = d;
            worst_p = p;
        }
    }
    EXPECT_LT(worst, 1e-3)
        << "rest pose diverges from flat geometry; worst nearest-flat "
           "distance (m) = " << worst << " at glTF ("
        << worst_p[0] << ", " << worst_p[1] << ", " << worst_p[2] << ")";
}

// Rest-pose parity for a REAL deep-chain model (the install's model
// 1052 — F-16 with nested gear dof chains). The flat export must be
// produced first:
//   f4import models --install <root> --data %TEMP%\f4_1052_flat \
//                    --model 1052 --vocab f4-import/vocab
// The hierarchy side is the committed Data/ export. Skips when either
// file is absent (CI / fresh clones) — run it locally after any change
// to the emitter's frame handling or the runtime's chain composition.
TEST(HierarchyEmit, RestPoseParity_DeepChains_WhenSamplesAvailable) {
    namespace fs2 = std::filesystem;
    const fs::path flat_gltf = fs2::temp_directory_path() /
        "f4_1052_flat" / "Models" / "koreaobj" / "01052.gltf";
    const fs::path hier_gltf = fs::path("Data") / "Models" / "koreaobj" /
        "01052.gltf";
    if (!fs2::exists(flat_gltf) || !fs2::exists(hier_gltf)) {
        GTEST_SKIP() << "run f4import first";
    }
    f4::gltf::GltfDocument fd;
    fd.load(flat_gltf);
    f4::gltf::GltfDocument hd;
    hd.load(hier_gltf);
    const auto flatv = mesh_positions(fd, "LOD_0");
    ASSERT_FALSE(flatv.empty());
    const f4::gltf::Node* lod = nullptr;
    for (const auto& n : hd.nodes) {
        if (n.has_f4 && n.f4.kind == "lod" &&
            n.f4.lod_level.value_or(-1) == 0) {
            lod = &n;
            break;
        }
    }
    ASSERT_NE(lod, nullptr);

    struct Frame { std::size_t node; std::size_t depth; };
    std::vector<Frame> stack;
    for (auto it = lod->children.rbegin(); it != lod->children.rend(); ++it)
        stack.push_back({*it, 0});
    std::vector<std::size_t> path;
    struct Report {
        double worst;
        std::string chain;
        std::size_t verts;
    };
    std::vector<Report> reports;

    while (!stack.empty()) {
        const auto fr = stack.back();
        stack.pop_back();
        if (fr.node >= hd.nodes.size()) continue;
        path.resize(fr.depth);
        path.push_back(fr.node);
        const auto& node = hd.nodes[fr.node];
        if (node.mesh.has_value() && *node.mesh < hd.meshes.size()) {
            double worst = 0.0;
            std::size_t count = 0;
            for (const auto& prim : hd.meshes[*node.mesh].primitives) {
                if (!prim.positions) continue;
                const auto& acc = hd.accessors[*prim.positions];
                for (std::size_t i = 0; i < acc.count; ++i) {
                    auto v = hd.read_vec3_float(*prim.positions, i);
                    if (!v) continue;
                    double p[3] = {(*v)[0], (*v)[1], (*v)[2]};
                    for (auto rit = path.rbegin(); rit != path.rend();
                         ++rit) {
                        double t[3], q[4], s[3];
                        f4::gltf::eval_tagged_local(hd, *rit, 0.0f, t, q, s);
                        double r[3][3];
                        quat_to_mat9(q, r);
                        double tmp[3];
                        for (int a = 0; a < 3; ++a) {
                            tmp[a] = r[a][0] * p[0] * s[0] +
                                     r[a][1] * p[1] * s[1] +
                                     r[a][2] * p[2] * s[2];
                        }
                        p[0] = tmp[0] + t[0];
                        p[1] = tmp[1] + t[1];
                        p[2] = tmp[2] + t[2];
                    }
                    ++count;
                    double best = 1e30;
                    for (const auto& qv : flatv) {
                        const double dx = p[0] - qv[0];
                        const double dy = p[1] - qv[1];
                        const double dz = p[2] - qv[2];
                        const double d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 < best) best = d2;
                    }
                    worst = std::max(worst, std::sqrt(best));
                }
            }
            std::string chain;
            for (const auto ni : path) chain += hd.nodes[ni].name + ">";
            reports.push_back({worst, chain, count});
        }
        for (auto it = node.children.rbegin(); it != node.children.rend();
             ++it) {
            stack.push_back({*it, fr.depth + 1});
        }
    }

    std::sort(reports.begin(), reports.end(),
              [](const Report& a, const Report& b) { return a.worst > b.worst; });
    std::fprintf(stderr, "[1052-PARITY] parts=%zu\n", reports.size());
    for (std::size_t i = 0; i < reports.size() && i < 10; ++i) {
        std::fprintf(stderr, "[1052-PARITY] worst=%.4f m verts=%zu chain=%s\n",
                     reports[i].worst, reports[i].verts,
                     reports[i].chain.c_str());
    }

    // The nested gear chains were the regression: outermost-first
    // composition displaced them by up to 4.8 m (rest pose), while
    // single-dof chains (flaps/LEFs) assembled exactly.
    double overall = 0.0;
    for (const auto& r : reports) overall = std::max(overall, r.worst);
    EXPECT_LT(overall, 1e-3)
        << "deep-chain rest pose diverges from the flat bake; worst (m) = "
        << overall << " on chain " << reports.front().chain;
}

TEST(HierarchyEmit, AnimatedPoseMatchesFlatBake) {
    // The ANIMATED contract: with a DOF channel driven to a nonzero
    // value, the runtime assembly (frames + rotation op) must reproduce
    // the flat extractor's bake for the same ModelState. This pins the
    // rotation-op axis: the falcon→glTF basis is improper (det = -1),
    // so the falcon local-X rotation conjugates to a glTF +Z rotation —
    // axis (0,0,1). The flipped axis (0,0,-1) deflects every DOF the
    // wrong way (invisible at rest, wrong as soon as anything moves).
    if (!fs::exists(fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.HDR"))
        GTEST_SKIP() << "fixture missing";
    auto db = load_db();

    // Flat bake with the left flap (FF dof 2 → channel flap.l) at 0.3 rad.
    f4::models::ModelState posed;
    f4::models::DofState ds;
    ds.dof_number = 2;
    ds.value = 0.3f;
    posed.dofs.push_back(ds);
    const auto geom = db->extract_model_geometry(1, 0, posed);
    ASSERT_GT(geom.total_vertices(), 0u);

    // Falcon → glTF space: (x,y,z)·ft → (y,z,−x)·0.3048.
    constexpr double kFt = 0.3048;
    std::vector<std::array<double, 3>> flatv;
    for (const auto& m : geom.meshes) {
        for (const auto& v : m.vertices) {
            const auto& p = v.position;
            flatv.push_back({p.y * kFt, p.z * kFt, -p.x * kFt});
        }
    }
    ASSERT_FALSE(flatv.empty());

    const auto hier = emit_f16(true);
    f4::anim::AnimValues values;
    values[f4::anim::Channel::flap_l] = 0.3f;

    f4::gltf::GltfDocument hd;
    hd.load(hier.result.gltf_path);
    const auto assembled = assemble_animated(hd, 0, values);
    ASSERT_EQ(assembled.size(), flatv.size());

    double worst = 0.0;
    for (const auto& p : assembled) {
        double best = 1e30;
        for (const auto& q : flatv) {
            const double dx = p[0] - q[0], dy = p[1] - q[1],
                          dz = p[2] - q[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best) best = d2;
        }
        worst = std::max(worst, std::sqrt(best));
    }
    EXPECT_LT(worst, 1e-3)
        << "animated pose diverges from the flat bake; worst (m) = "
        << worst << " (rotation-op axis or composition is wrong)";
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

    // Air-defense: index 0 is the radar dish sweep — every air-defense
    // model carries it; it drives the continuous radar.dish_spin
    // channel (spinner rig pass).
    const auto& airdef = tables.at("airdef");
    ASSERT_NE(airdef.dof_at(0), nullptr);
    EXPECT_EQ(airdef.dof_at(0)->channel, "radar.dish_spin");
}

TEST(HierarchyEmit, FamilyGuessClassifiesFixtureModels) {
    // The fixture F-16 (11 dofs, 7 switches, 9 slots) must classify
    // complex. The DOF-index set comes from the parsed BSP tree's
    // transform nodes — the same collection the importer CLI does.
    auto db = load_db();
    const auto* rec = db->model(1);
    std::vector<int> dof_indices;
    const auto* tree = db->bsp_tree(1, 0);
    ASSERT_NE(tree, nullptr);
    for (const auto& node : tree->nodes) {
        switch (node.type) {
            case f4::models::BspNodeType::BDofNode:
            case f4::models::BspNodeType::BXDofNode:
            case f4::models::BspNodeType::BTransNode:
            case f4::models::BspNodeType::BScaleNode:
                if (node.dof_number >= 0 &&
                    std::find(dof_indices.begin(), dof_indices.end(),
                              node.dof_number) == dof_indices.end()) {
                    dof_indices.push_back(node.dof_number);
                }
                break;
            default:
                break;
        }
    }
    EXPECT_EQ(guess_family(dof_indices, rec->effective_dofs(),
                           rec->effective_switches(),
                           static_cast<int>(rec->slots.size())),
              "complex");
}

TEST(HierarchyEmit, FamilyGuessRotorPairSeparatesHelisFromRadars) {
    // Counts alone can't tell a 2-DOF helicopter from a 2-DOF ground
    // radar — the rotor pair (dofs 2 AND 4) does. A radar must land in
    // airdef (its sweep binds to radar.dish_spin); a heli keeps the
    // heli table even when it also carries an unrelated index 0.
    EXPECT_EQ(guess_family({2, 4}, 2, 0, 0), "heli");
    EXPECT_EQ(guess_family({0, 2, 4}, 3, 1, 0), "heli");   // heli + extra tag
    EXPECT_EQ(guess_family({0}, 1, 0, 0), "airdef");        // radar sweep only
    EXPECT_EQ(guess_family({0, 1}, 2, 2, 0), "airdef");     // sweep + elevation
    EXPECT_EQ(guess_family({0, 1, 2}, 3, 3, 0), "airdef");  // + turret yaw
    EXPECT_EQ(guess_family({2, 3}, 2, 0, 2), "simple");   // 2 slots → not heli
    EXPECT_EQ(guess_family({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13},
                           14, 5, 6),
              "complex");                                 // full aircraft
}

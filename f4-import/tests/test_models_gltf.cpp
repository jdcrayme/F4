// f4-import/tests/test_models_gltf.cpp
//
// Round-trip tests for the KoreaObj → glTF converter. Loads the bundled
// KoreaObj.HDR/LOD fixtures, converts a few models to glTF, then loads
// the glTF back with f4-gltf to verify the round-trip is lossless for
// the geometry + node-tag metadata.

#include <f4/import/gltf_emitter.hpp>
#include <f4/import/manifest_writer.hpp>
#include <f4/import/doctor.hpp>
#include <f4/assets/manifest.hpp>
#include <f4/gltf/f4_gltf.hpp>
#include <f4/models/model_database.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace f4::import;
namespace fs = std::filesystem;

namespace {

fs::path make_data_dir(const std::string& suffix) {
    auto p = fs::temp_directory_path() / "f4_models_gltf_test" / suffix;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

// Load the KoreaObj fixtures once per test run (the database is cheap
// to build — 1342 models in <1s).
std::unique_ptr<f4::models::ModelDatabase> load_db() {
    auto db = std::make_unique<f4::models::ModelDatabase>();
    fs::path fixture = KOREAOBJ_FIXTURE_DIR;
    auto hdr = fixture / "KoreaObj.HDR";
    auto lod = fixture / "KoreaObj.LOD";
    if (!fs::exists(hdr)) {
        throw std::runtime_error("KoreaObj.HDR fixture not found at " + hdr.string());
    }
    std::string err = db->load(hdr, lod);
    if (!err.empty()) {
        throw std::runtime_error("ModelDatabase load failed: " + err);
    }
    return db;
}

} // namespace

// ── Smoke test: a simple model converts and loads back ────────────────────

TEST(ModelsGltf, SimpleModelRoundTrips) {
    auto db = load_db();
    ASSERT_TRUE(db->valid());

    // Model 2 (airbase section) has 3 LODs, 0 DOFs — a simple test case.
    ASSERT_GT(db->n_models(), 2);
    ASSERT_EQ(db->parse_model(2), "");
    auto data_dir = make_data_dir("simple");

    GltfEmitResult result;
    EXPECT_NO_THROW(result = emit_model_as_gltf(*db, 2, data_dir, "koreaobj:00002"));

    EXPECT_GT(result.total_vertices, 0u);
    EXPECT_GT(result.total_triangles, 0u);
    EXPECT_EQ(result.lod_count, 3u);  // model 2 has 3 LODs
    EXPECT_EQ(result.dof_count, 0u);
    EXPECT_EQ(result.switch_count, 0u);

    // The .gltf + .bin files exist.
    EXPECT_TRUE(fs::exists(data_dir / "00002.gltf"));
    EXPECT_TRUE(fs::exists(data_dir / "00002.bin"));

    // f4-gltf can load it back.
    f4::gltf::GltfDocument doc;
    EXPECT_NO_THROW(doc.load(data_dir / "00002.gltf"));
    EXPECT_EQ(doc.meshes.size(), result.lod_count);
    EXPECT_FALSE(doc.nodes.empty());

    fs::remove_all(data_dir);
}

// ── Complex model: DOFs, switches, slots are tagged ───────────────────────

TEST(ModelsGltf, ComplexModelTagsDofSwitchSlot) {
    auto db = load_db();

    // Model 1 is an F-16: 11 DOFs, 7 switches, 9 slots.
    ASSERT_EQ(db->parse_model(1), "");
    auto data_dir = make_data_dir("complex");

    GltfEmitResult result;
    EXPECT_NO_THROW(result = emit_model_as_gltf(*db, 1, data_dir, "koreaobj:00001"));

    EXPECT_EQ(result.dof_count, 11u);
    EXPECT_EQ(result.switch_count, 7u);
    EXPECT_EQ(result.slot_count, 9u);

    // f4-gltf loads it and the f4 tag counts match.
    f4::gltf::GltfDocument doc;
    EXPECT_NO_THROW(doc.load(data_dir / "00001.gltf"));
    EXPECT_EQ(doc.count_f4_nodes("dof"), result.dof_count);
    EXPECT_EQ(doc.count_f4_nodes("sw"), result.switch_count);
    EXPECT_EQ(doc.count_f4_nodes("slot"), result.slot_count);

    // The DOF nodes are named dof:unknown.N per the §6 grammar.
    EXPECT_NE(doc.find_f4_node("dof", "unknown.0"), nullptr);
    EXPECT_NE(doc.find_f4_node("dof", "unknown.10"), nullptr);
    EXPECT_EQ(doc.find_f4_node("dof", "unknown.11"), nullptr);  // only 11 DOFs (0-10)

    fs::remove_all(data_dir);
}

// ── Coordinate conversion: feet/Z-up → meters/Y-up ────────────────────────

TEST(ModelsGltf, ConvertsToGltfCoords) {
    auto db = load_db();
    ASSERT_EQ(db->parse_model(2), "");
    auto data_dir = make_data_dir("coords");

    // Emit with coordinate conversion (default).
    GltfEmitOptions opts;
    opts.convert_to_gltf_coords = true;
    auto result = emit_model_as_gltf(*db, 2, data_dir, "koreaobj:00002", opts);

    f4::gltf::GltfDocument doc;
    doc.load(data_dir / "00002.gltf");

    // Read the first vertex position. It should be in meters (not feet)
    // and the axes should be swapped (glTF y = falcon z).
    ASSERT_FALSE(doc.meshes.empty());
    ASSERT_FALSE(doc.meshes[0].primitives.empty());
    auto pos_acc = doc.meshes[0].primitives[0].positions;
    ASSERT_TRUE(pos_acc.has_value());
    auto v0 = doc.read_vec3_float(*pos_acc, 0);
    ASSERT_TRUE(v0.has_value());

    // The coordinates should be in the meter range (not hundreds of feet).
    // The F-16 model's bounding box is ~40 feet; in meters that's ~12.
    // A value > 100 would indicate the conversion didn't happen (still feet).
    EXPECT_LT(std::abs((*v0)[0]), 100.0f);
    EXPECT_LT(std::abs((*v0)[1]), 100.0f);
    EXPECT_LT(std::abs((*v0)[2]), 100.0f);

    fs::remove_all(data_dir);
}

// ── Doctor D1/D5 pass on a converted model ───────────────────────────────

TEST(ModelsGltf, DoctorPassesOnConvertedModel) {
    auto db = load_db();
    ASSERT_EQ(db->parse_model(2), "");
    auto data_dir = make_data_dir("doctor");

    auto result = emit_model_as_gltf(*db, 2, data_dir, "koreaobj:00002");

    // Update the manifest so the model is "listed".
    std::vector<f4::assets::Capability> caps;
    caps.push_back({"dofs", f4::assets::CapabilityStatus::present,
                    static_cast<int>(result.dof_count)});
    caps.push_back({"switches", f4::assets::CapabilityStatus::present,
                    static_cast<int>(result.switch_count)});
    caps.push_back({"slots", f4::assets::CapabilityStatus::present,
                    static_cast<int>(result.slot_count)});
    caps.push_back({"anchors", f4::assets::CapabilityStatus::unknown});

    (void)update_manifest_for_asset(
        data_dir,
        f4::assets::AssetId{f4::assets::AssetFamily::koreaobj, "00002"},
        "00002.gltf",
        1, std::move(caps),
        {{"KoreaObj.HDR", "art", ""}, {"KoreaObj.LOD", "art", ""}},
        "test");

    // Run the doctor. D1 (koreaobj .gltf exists + loads) and D5 (node
    // tag grammar) should both pass.
    auto report = run_doctor(data_dir);
    EXPECT_EQ(report.errors, 0u)
        << "doctor should report zero errors:\n" << format_report(report);

    fs::remove_all(data_dir);
}

// ── D5 catches a malformed node tag ───────────────────────────────────────

TEST(ModelsGltf, D5CatchesMismatchedKind) {
    auto data_dir = make_data_dir("d5_mismatch");
    fs::create_directories(data_dir / "Models" / "koreaobj");

    // Write a .gltf with a node named "dof:gear" but f4 extras kind "sw"
    // (a mismatch). D5 should flag this.
    std::string gltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes": [
    { "name": "dof:gear", "extras": { "f4": { "v": 1, "kind": "sw", "id": "gear" } } }
  ],
  "meshes": [ { "name": "m", "primitives": [] } ]
})";
    {
        std::ofstream f(data_dir / "Models" / "koreaobj" / "test.gltf");
        f << gltf;
    }

    // Write a minimal manifest so D9 passes.
    {
        std::ofstream f(data_dir / "manifest.json");
        f << R"({ "f4": { "v": 1 }, "data_dir": "Data/", "assets": [] })";
    }

    auto report = run_doctor(data_dir);
    bool found_d5 = false;
    for (const auto& finding : report.findings) {
        if (finding.check_id == "D5" && finding.message.find("kind") != std::string::npos) {
            found_d5 = true;
            break;
        }
    }
    EXPECT_TRUE(found_d5) << "D5 should flag the kind mismatch";

    fs::remove_all(data_dir);
}

// ── D1 catches a missing .gltf ────────────────────────────────────────────

TEST(ModelsGltf, D1CatchesMissingGltf) {
    auto data_dir = make_data_dir("d1_missing");

    // Manifest references a koreaobj .gltf that doesn't exist on disk.
    std::string manifest_json = R"({
  "f4": { "v": 1 },
  "data_dir": "Data/",
  "assets": [
    { "id": "koreaobj:99999", "path": "Models/koreaobj/99999.gltf",
      "format_version": 1, "capabilities": [],
      "sources": [{"path": "KoreaObj.HDR", "role": "art", "sha256": ""}] }
  ]
})";
    {
        std::ofstream f(data_dir / "manifest.json");
        f << manifest_json;
    }

    auto report = run_doctor(data_dir);
    bool found_d1 = false;
    for (const auto& finding : report.findings) {
        if (finding.check_id == "D1") {
            found_d1 = true;
            break;
        }
    }
    EXPECT_TRUE(found_d1) << "D1 should flag the missing .gltf file";

    fs::remove_all(data_dir);
}

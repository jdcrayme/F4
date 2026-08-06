// f4-models/tests/test_models.cpp
//
// Unit tests for f4-models library.
// Uses the KoreaObj.HDR fixture from the temp directory.

#include <f4/models/f4_models.hpp>

// Access internal poly_parser for header size tests
// (poly_parser.hpp is internal to the library but we need it for testing)
#include "poly_parser.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;

namespace {

/// Get the fixture directory (set by CMake).
std::string fixture_dir() {
    return MODELS_FIXTURE_DIR;
}

/// Check if the HDR fixture exists (may not be available in CI).
bool has_hdr_fixture() {
    auto dir = fixture_dir();
    return fs::exists(dir + "KoreaObj.HDR");
}

/// Get HDR fixture path.
fs::path hdr_path() { return fixture_dir() + "KoreaObj.HDR"; }
/// Get LOD fixture path.
fs::path lod_path() { return fixture_dir() + "KoreaObj.LOD"; }

} // anonymous namespace

// ── Type Tests ─────────────────────────────────────────────────────────────

TEST(ModelsTypes, BspNodeTypeName) {
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::BRoot), "BRoot");
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::BSlotNode), "BSlotNode");
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::BSplitterNode), "BSplitterNode");
    EXPECT_STREQ(f4::models::bsp_node_type_name(f4::models::BspNodeType::Unknown), "Unknown");
}

TEST(ModelsTypes, PolyTypeName) {
    EXPECT_STREQ(f4::models::poly_type_name(f4::models::PolyType::Tex), "Tex");
    EXPECT_STREQ(f4::models::poly_type_name(f4::models::PolyType::ATexGL), "ATexGL");
}

TEST(ModelsTypes, BoundingBox) {
    f4::models::BoundingBox bb{-10, 10, -5, 5, 0, 20};
    EXPECT_FLOAT_EQ(bb.center_x(), 0.0f);
    EXPECT_FLOAT_EQ(bb.center_y(), 0.0f);
    EXPECT_FLOAT_EQ(bb.center_z(), 10.0f);
    EXPECT_FLOAT_EQ(bb.extent_x(), 10.0f);
    EXPECT_FLOAT_EQ(bb.extent_y(), 5.0f);
    EXPECT_FLOAT_EQ(bb.extent_z(), 10.0f);
}

TEST(ModelsTypes, Vec3Equality) {
    f4::models::Vec3 a{1, 2, 3};
    f4::models::Vec3 b{1, 2, 3};
    f4::models::Vec3 c{1, 2, 4};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ── ModelRecord Tests ─────────────────────────────────────────────────────

TEST(ModelRecord, VisualClass) {
    f4::models::ModelRecord air;
    air.n_slots = 9;
    air.n_dof = 11;  // legacy count (n_dofs=0 in vanilla HDR)
    EXPECT_EQ(air.visual_class(), "air");
    EXPECT_EQ(air.effective_dofs(), 11);

    f4::models::ModelRecord ground;
    ground.n_slots = 2;
    ground.n_dof = 3;
    EXPECT_EQ(ground.visual_class(), "ground");

    f4::models::ModelRecord feature;
    feature.n_slots = 0;
    feature.n_dof = 0;
    EXPECT_EQ(feature.visual_class(), "feature");
}

// ── TextureEntry Tests ────────────────────────────────────────────────────

TEST(TextureEntry, Filename) {
    f4::models::TextureEntry te;
    strncpy(te.raw.data(), "runway1.jpg", 12);
    EXPECT_EQ(te.filename(), "runway1.jpg");
}

// ── ModelDatabase Tests (require fixture) ─────────────────────────────────

class ModelsFixtureTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!has_hdr_fixture()) GTEST_SKIP() << "HDR fixture not available";
    }
};

TEST_F(ModelsFixtureTest, LoadHdrOnly) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << "HDR load error: " << err;
    EXPECT_TRUE(db.valid());
    EXPECT_EQ(db.n_models(), 1342);
    EXPECT_GT(db.n_lod_entries(), 0);
    EXPECT_GT(db.n_textures(), 0);
}

TEST_F(ModelsFixtureTest, ColorBankParsed) {
    // The ColorBank must be parsed and exposed so the viewer can resolve
    // Prim.rgba indices into actual RGBA colors. Previously the parser
    // skipped the ColorBank entirely (r.skip(n_colors * 16)) and the
    // viewer treated color indices as packed ABGR — producing garbage.
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    const auto& cb = db.color_bank();
    EXPECT_FALSE(cb.empty()) << "ColorBank should not be empty after load";
    EXPECT_GT(cb.size(), 100u) << "KoreaObj.HDR has ~1596 colors";
    EXPECT_GT(cb.n_darkened, 0) << "KoreaObj.HDR has ~669 darkened colors";

    // Look up common indices found in the fixture's prims (873, 38, 81).
    // Each should produce a non-zero packed RGBA. (Specific values depend
    // on the fixture but every index used by a real prim should resolve.)
    for (int idx : {0, 1, 38, 81, 100, 500, 873}) {
        if (static_cast<std::size_t>(idx) >= cb.size()) continue;
        const uint32_t rgba = cb.rgba_at(idx);
        EXPECT_NE(rgba, 0u)
            << "ColorBank index " << idx << " should resolve to non-zero RGBA";
    }

    // Out-of-range indices should return 0 (transparent), not crash.
    EXPECT_EQ(cb.rgba_at(-1), 0u);
    EXPECT_EQ(cb.rgba_at(static_cast<int>(cb.size())), 0u);
}

TEST_F(ModelsFixtureTest, LoadHdrAndLod) {
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << "Load error: " << err;
    EXPECT_TRUE(db.valid());
    EXPECT_EQ(db.n_models(), 1342);
}

TEST_F(ModelsFixtureTest, ModelAccess) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    // First model
    auto* m0 = db.model(0);
    ASSERT_NE(m0, nullptr);
    EXPECT_EQ(m0->index, 0);
    EXPECT_GE(m0->radius, 0);

    // Out of range
    EXPECT_EQ(db.model(-1), nullptr);
    EXPECT_EQ(db.model(db.n_models()), nullptr);
}

TEST_F(ModelsFixtureTest, ModelDataIntegrity) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    // Model 1 should be an aircraft-like model (F-16)
    auto* m1 = db.model(1);
    ASSERT_NE(m1, nullptr);
    EXPECT_GT(m1->radius, 0);
    EXPECT_GT(m1->n_slots, 0);
    EXPECT_GT(m1->effective_dofs(), 0);  // uses max(legacy, extended)
    EXPECT_EQ(m1->visual_class(), "air");
}

TEST_F(ModelsFixtureTest, QueryBySlots) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto air_models = db.find_by_slots(4, 100);
    EXPECT_GT(air_models.size(), 0);

    auto feature_models = db.find_by_slots(0, 0);
    EXPECT_GT(feature_models.size(), 0);
}

TEST_F(ModelsFixtureTest, QueryByRadius) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto large = db.find_by_radius(50.0f, 1000.0f);
    EXPECT_GT(large.size(), 0);
}

TEST_F(ModelsFixtureTest, QueryByClass) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto air = db.find_by_class("air");
    auto ground = db.find_by_class("ground");
    auto feature = db.find_by_class("feature");

    EXPECT_GT(air.size(), 0);
    EXPECT_GT(ground.size(), 0);
    EXPECT_GT(feature.size(), 0);

    // All models should be accounted for
    EXPECT_EQ(air.size() + ground.size() + feature.size(),
              static_cast<std::size_t>(db.n_models()));
}

TEST_F(ModelsFixtureTest, ParseModelLod) {
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    // Parse model 1 (should have BSP geometry)
    auto parse_err = db.parse_model(1);
    EXPECT_TRUE(parse_err.empty()) << "Parse error: " << parse_err;
}

TEST_F(ModelsFixtureTest, GeometryExtraction) {
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    // Parse model 1 LOD 0 (F-16, should have substantial geometry)
    auto parse_err = db.parse_lod(1, 0);
    ASSERT_TRUE(parse_err.empty()) << "Parse error: " << parse_err;

    // Extract geometry
    auto geom = db.extract_model_geometry(1, 0);

    // Model 1 (F-16) has BSP geometry with real polygons.
    // After fixing poly_parser field order and switch node traversal,
    // we should get actual meshes with triangles.
    EXPECT_GT(geom.meshes.size(), 0u)
        << "Model 1 LOD 0 should produce at least one mesh";
    EXPECT_GT(geom.total_triangles(), 0u)
        << "Model 1 LOD 0 should produce triangles";
    EXPECT_GT(geom.total_vertices(), 0u)
        << "Model 1 LOD 0 should produce vertices";

    // Verify vertex positions are not all zero (real geometry, not default)
    bool has_nonzero_pos = false;
    for (const auto& mesh : geom.meshes) {
        for (const auto& v : mesh.vertices) {
            if (v.position.x != 0 || v.position.y != 0 || v.position.z != 0) {
                has_nonzero_pos = true;
                break;
            }
        }
        if (has_nonzero_pos) break;
    }
    EXPECT_TRUE(has_nonzero_pos) << "Vertices should have non-zero positions";
}

TEST_F(ModelsFixtureTest, BspTreeStructure) {
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    // Parse model 1 and check BSP tree structure
    auto parse_err = db.parse_lod(1, 0);
    ASSERT_TRUE(parse_err.empty()) << parse_err;

    // Extract geometry and verify it produces real output
    auto geom = db.extract_model_geometry(1, 0);
    EXPECT_GT(geom.total_triangles(), 0u)
        << "Model 1 should produce triangles after bug fixes";
}

TEST_F(ModelsFixtureTest, GeometryBatchExtract) {
    // Test geometry extraction across many models to verify robustness.
    // Parse and extract LOD 0 for the first 50 non-DX models.
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    int models_with_geometry = 0;
    int models_parsed = 0;
    int total_triangles = 0;

    int limit = std::min(50, db.n_models());
    for (int idx = 0; idx < limit; ++idx) {
        auto* m = db.model(idx);
        if (!m || m->lods.empty()) continue;

        auto parse_err = db.parse_lod(idx, 0);
        if (!parse_err.empty()) continue;
        models_parsed++;

        auto geom = db.extract_model_geometry(idx, 0);
        if (geom.total_triangles() > 0) {
            models_with_geometry++;
            total_triangles += static_cast<int>(geom.total_triangles());
        }
    }

    // At least some models should produce geometry
    EXPECT_GT(models_parsed, 0) << "Should parse at least some models";
    EXPECT_GT(models_with_geometry, 0)
        << "At least some models should produce geometry";
    EXPECT_GT(total_triangles, 0)
        << "Should have total triangles > 0 across batch";
}

TEST_F(ModelsFixtureTest, GeometryVertexAttributes) {
    // Test that textured models have UV coordinates and texture IDs
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    // Model 1 (F-16) should have textured polygons
    auto parse_err = db.parse_lod(1, 0);
    ASSERT_TRUE(parse_err.empty()) << parse_err;

    auto geom = db.extract_model_geometry(1, 0);
    ASSERT_GT(geom.total_triangles(), 0u);

    // Check that some vertices have UV coordinates
    bool has_uv = false;
    bool has_normals = false;
    bool has_tex_id = false;
    for (const auto& mesh : geom.meshes) {
        for (const auto& v : mesh.vertices) {
            if (v.uv.u != 0 || v.uv.v != 0) has_uv = true;
            if (v.normal.x != 0 || v.normal.y != 0 || v.normal.z != 0) has_normals = true;
            if (v.tex_id >= 0) has_tex_id = true;
        }
    }
    EXPECT_TRUE(has_uv) << "Textured model should have UV coordinates";
    EXPECT_TRUE(has_normals) << "Model should have face normals";
    EXPECT_TRUE(has_tex_id) << "Textured model should have texture IDs";
}

TEST_F(ModelsFixtureTest, GeometryRobustnessWide) {
    // Parse a wide range of models to verify no crashes or corruption.
    // Test models with different characteristics.
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    // Test specific model indices: air, ground, feature types
    for (int idx : {1, 2, 3, 5, 10, 42, 100, 200, 500, 1000}) {
        if (idx >= db.n_models()) continue;
        auto* m = db.model(idx);
        if (!m) continue;

        // Parse all LODs
        for (int lod = 0; lod < static_cast<int>(m->lods.size()); ++lod) {
            auto pe = db.parse_lod(idx, lod);
            EXPECT_TRUE(pe.empty())
                << "Parse error for model " << idx << " LOD " << lod << ": " << pe;

            if (pe.empty()) {
                auto geom = db.extract_model_geometry(idx, lod);
                // Just verify no crash — geometry may be empty for some LODs
                (void)geom;
            }
        }
    }
}

TEST_F(ModelsFixtureTest, LineAndPointPrimitivesEmitted) {
    // Far LODs often use LineF / PointF primitives for distance markers.
    // The old prim_to_mesh silently dropped anything with n_verts < 3,
    // which made ~145 LODs in this fixture render as empty. After the
    // fix, those should produce Lines / Points instead.
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    int models_with_lines = 0;
    int models_with_points = 0;
    int models_with_tris = 0;

    // Sample every 6th parent (matches the diagnostic script).
    int stride = std::max(1, db.n_models() / 200);
    for (int idx = 0; idx < db.n_models(); idx += stride) {
        auto* m = db.model(idx);
        if (!m || m->lods.empty()) continue;

        for (int lod = 0; lod < static_cast<int>(m->lods.size()); ++lod) {
            auto pe = db.parse_lod(idx, lod);
            if (!pe.empty()) continue;

            auto geom = db.extract_model_geometry(idx, lod);
            for (const auto& mesh : geom.meshes) {
                if (mesh.kind == f4::models::PrimitiveKind::Lines &&
                    !mesh.lines.empty()) {
                    ++models_with_lines;
                }
                if (mesh.kind == f4::models::PrimitiveKind::Points &&
                    !mesh.points.empty()) {
                    ++models_with_points;
                }
                if (mesh.kind == f4::models::PrimitiveKind::Triangles &&
                    !mesh.triangles.empty()) {
                    ++models_with_tris;
                }
            }
        }
    }

    // Without the LineF/PointF fix, models_with_lines == 0 and a big chunk
    // of LODs would be empty. After the fix, we expect at least some lines.
    EXPECT_GT(models_with_lines, 0)
        << "LineF primitives should now be emitted as PrimitiveKind::Lines";
    EXPECT_GT(models_with_tris, 0)
        << "Triangle primitives should still be emitted";
    // Points are rarer; just verify the path doesn't crash.
}

TEST_F(ModelsFixtureTest, EmptyLodCountReduced) {
    // Regression test for the "switch children not walked" + "per-subtree
    // pool binding" fixes. Before the fixes, ~26% of sampled LODs returned
    // 0 vertices. After the fixes, it should be < 5%.
    f4::models::ModelDatabase db;
    auto err = db.load(hdr_path(), lod_path());
    ASSERT_TRUE(err.empty()) << err;

    int total_lods = 0;
    int empty_lods = 0;
    int stride = std::max(1, db.n_models() / 200);
    for (int idx = 0; idx < db.n_models(); idx += stride) {
        auto* m = db.model(idx);
        if (!m || m->lods.empty()) continue;
        for (int lod = 0; lod < static_cast<int>(m->lods.size()); ++lod) {
            auto pe = db.parse_lod(idx, lod);
            if (!pe.empty()) continue;
            ++total_lods;
            auto geom = db.extract_model_geometry(idx, lod);
            if (geom.total_vertices() == 0) ++empty_lods;
        }
    }

    ASSERT_GT(total_lods, 100) << "sample should cover >100 LODs";
    const double empty_pct = 100.0 * empty_lods / total_lods;
    EXPECT_LT(empty_pct, 5.0)
        << "Empty LOD rate is " << empty_pct << "% (was 26% before fixes; "
        << "should now be < 5%). " << empty_lods << " empty out of "
        << total_lods;
}

TEST_F(ModelsFixtureTest, ModelGeometryTypes) {
    // Test the geometry types themselves
    f4::models::Mesh m1;
    m1.vertices = {
        {{0,0,0}, {0,0,1}, {0,0}, 0, -1},
        {{1,0,0}, {0,0,1}, {1,0}, 0, -1},
        {{1,1,0}, {0,0,1}, {1,1}, 0, -1},
    };
    m1.triangles = {{0, 1, 2, -1}};
    m1.tex_id = -1;
    EXPECT_EQ(m1.triangle_count(), 1u);

    f4::models::Mesh m2;
    m2.vertices = {
        {{0,1,0}, {0,0,1}, {0,1}, 0, -1},
        {{1,1,0}, {0,0,1}, {1,1}, 0, -1},
        {{1,2,0}, {0,0,1}, {1,2}, 0, -1},
    };
    m2.triangles = {{0, 1, 2, -1}};
    m2.tex_id = -1;

    f4::models::ModelGeometry geom;
    geom.meshes = {m1, m2};
    EXPECT_EQ(geom.total_triangles(), 2u);
    EXPECT_EQ(geom.total_vertices(), 6u);

    auto merged = geom.merged();
    EXPECT_EQ(merged.triangle_count(), 2u);
    EXPECT_EQ(merged.vertices.size(), 6u);
    // Check that triangle indices were remapped
    EXPECT_EQ(merged.triangles[1].v0, 3u);  // was 0 + offset 3
}

TEST_F(ModelsFixtureTest, Version) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;
    EXPECT_EQ(db.version(), 0x03087000u);
}

TEST_F(ModelsFixtureTest, LodNames) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;
    // Vanilla F4 KoreaObj.HDR should not have LOD names
    EXPECT_FALSE(db.has_lod_names());
}

// ── JSON Export Tests ──────────────────────────────────────────────────────

TEST_F(ModelsFixtureTest, ModelListJson) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto json = f4::models::model_list_json(db);
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("\"command\""), std::string::npos);
    EXPECT_NE(json.find("\"n_models\""), std::string::npos);
    EXPECT_NE(json.find("1342"), std::string::npos);
}

TEST_F(ModelsFixtureTest, ModelRecordJson) {
    f4::models::ModelDatabase db;
    auto err = db.load_hdr(hdr_path());
    ASSERT_TRUE(err.empty()) << err;

    auto* m = db.model(1);
    ASSERT_NE(m, nullptr);
    auto json = f4::models::model_record_json(*m);
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("\"radius\""), std::string::npos);
}

// ── File Finder Tests ──────────────────────────────────────────────────────

TEST(FileFinder, FindInTempDir) {
    // Try to find the fixture files
    fs::path temp_dir = fixture_dir();
    auto [hdr, lod] = f4::models::ModelDatabase::find_koreaobj_files(temp_dir);

    if (fs::exists(temp_dir / "KoreaObj.HDR")) {
        EXPECT_FALSE(hdr.empty());
        EXPECT_FALSE(lod.empty());
    }
}

// ── DX Format Detection ───────────────────────────────────────────────────

TEST(DxDetection, Checksum) {
    // Valid DX header: lower 16 bits == complement of upper 16 bits
    // Example: 0x1234EDCB -> (0x1234 & 0xFFFF) should equal (~0xEDCB >> 0 & 0xFFFF)
    uint32_t valid_dx = 0x0000FFFF; // (0x0000) == (~0xFFFF >> 16) & 0xFFFF = 0x0000... no
    // Let's compute a valid one: version=0x1234, checksum=~0x1234=0xEDCB
    // Packed: 0xEDCB1234
    uint32_t dx = 0xEDCB1234u;
    EXPECT_TRUE((dx & 0xFFFF) == ((~dx >> 16) & 0xFFFF));

    // A BSP tag count (e.g. 308) should NOT match DX format
    uint32_t bsp = 308;
    EXPECT_FALSE((bsp & 0xFFFF) == ((~bsp >> 16) & 0xFFFF));
}

// ── PolyType Header Size Tests ────────────────────────────────────────────

TEST(PolyParser, HeaderSizes) {
    // Verify on-disk header sizes match FreeFalcon's class layout
    namespace fm = f4::models;
    using f4::models::detail::prim_header_size;

    // PointF/LineF → PrimPointFC/PrimLineFC = 16
    EXPECT_EQ(prim_header_size(fm::PolyType::PointF), 16);
    EXPECT_EQ(prim_header_size(fm::PolyType::LineF), 16);

    // F/AF → PolyFC = 32
    EXPECT_EQ(prim_header_size(fm::PolyType::F), 32);
    EXPECT_EQ(prim_header_size(fm::PolyType::AF), 32);

    // FL/AFL → PolyFCN = 36
    EXPECT_EQ(prim_header_size(fm::PolyType::FL), 36);
    EXPECT_EQ(prim_header_size(fm::PolyType::AFL), 36);

    // G/AG → PolyVC = 32
    EXPECT_EQ(prim_header_size(fm::PolyType::G), 32);
    EXPECT_EQ(prim_header_size(fm::PolyType::AG), 32);

    // GL/AGL → PolyVCN = 36
    EXPECT_EQ(prim_header_size(fm::PolyType::GL), 36);
    EXPECT_EQ(prim_header_size(fm::PolyType::AGL), 36);

    // Tex/ATex/CTex/CATex/BAptTex → PolyTexFC = 40
    EXPECT_EQ(prim_header_size(fm::PolyType::Tex), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::ATex), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::CTex), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::CATex), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::BAptTex), 40);

    // TexL/ATexL/CTexL/CATexL → PolyTexFCN = 44
    EXPECT_EQ(prim_header_size(fm::PolyType::TexL), 44);
    EXPECT_EQ(prim_header_size(fm::PolyType::ATexL), 44);
    EXPECT_EQ(prim_header_size(fm::PolyType::CTexL), 44);
    EXPECT_EQ(prim_header_size(fm::PolyType::CATexL), 44);

    // TexG/ATexG/CTexG/CATexG → PolyTexVC = 40
    EXPECT_EQ(prim_header_size(fm::PolyType::TexG), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::ATexG), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::CTexG), 40);
    EXPECT_EQ(prim_header_size(fm::PolyType::CATexG), 40);

    // TexGL/ATexGL/CTexGL/CATexGL → PolyTexVCN = 44
    EXPECT_EQ(prim_header_size(fm::PolyType::TexGL), 44);
    EXPECT_EQ(prim_header_size(fm::PolyType::ATexGL), 44);
    EXPECT_EQ(prim_header_size(fm::PolyType::CTexGL), 44);
    EXPECT_EQ(prim_header_size(fm::PolyType::CATexGL), 44);
}

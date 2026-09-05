// f4-import/tests/test_textures_gltf.cpp
//
// Tranche 0c tests: glTF material emission + KoreaObj.TEX → PNG export.
//
// Materials: models with textured meshes must emit spec-compliant
// primitives (nested "attributes"), TEXCOORD_0 accessors, and a
// materials/textures/images chain pointing at textures/NNNNN.png.
//
// PNG export: needs KoreaObj.TEX. Resolution order:
//   1. F4_KOREAOBJ_TEX environment variable
//   2. KOREAOBJ_TEX_FALLBACK (the repo's temp/KoreaObj.TEX)
//   3. KoreaObj.TEX next to the fixture HDR/LOD
// Tests that need it GTEST_SKIP with a clear message when absent.

#include <f4/import/gltf_emitter.hpp>
#include <f4/import/texture_png.hpp>
#include <f4/gltf/f4_gltf.hpp>
#include <f4/models/model_database.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace f4::import;
namespace fs = std::filesystem;

namespace {

fs::path make_out_dir(const std::string& suffix) {
    auto p = fs::temp_directory_path() / "f4_textures_gltf_test" / suffix;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::unique_ptr<f4::models::ModelDatabase> load_db() {
    auto db = std::make_unique<f4::models::ModelDatabase>();
    fs::path fixture = KOREAOBJ_FIXTURE_DIR;
    std::string err = db->load(fixture / "KoreaObj.HDR", fixture / "KoreaObj.LOD");
    if (!err.empty()) {
        throw std::runtime_error("ModelDatabase load failed: " + err);
    }
    return db;
}

fs::path find_tex_file() {
    if (const char* env = std::getenv("F4_KOREAOBJ_TEX")) {
        if (fs::exists(env)) return env;
    }
    fs::path fallback = KOREAOBJ_TEX_FALLBACK;
    if (fs::exists(fallback)) return fallback;
    fs::path next_to_fixture = fs::path(KOREAOBJ_FIXTURE_DIR) / "KoreaObj.TEX";
    if (fs::exists(next_to_fixture)) return next_to_fixture;
    return {};
}

// Find a model whose LOD0 geometry has a triangle mesh with the wanted
// texture affinity, scanning a bounded range for speed.
struct Found {
    int model_index = -1;
    int32_t tex_id = -1;
};

Found find_textured_model(f4::models::ModelDatabase& db, int scan_limit) {
    Found found;
    const int limit = std::min(scan_limit, db.n_models());
    for (int i = 0; i < limit; ++i) {
        if (db.parse_model(i) != "") continue;
        f4::models::ModelState state;
        auto geom = db.extract_model_geometry(i, 0, state);
        for (const auto& m : geom.meshes) {
            if (m.triangles.empty() || m.tex_id < 0) continue;
            found.model_index = i;
            found.tex_id = m.tex_id;
            return found;
        }
    }
    return found;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.good()) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

} // namespace

// ── Textured model emits materials + images + spec-compliant attributes ──

TEST(TexturesGltf, TexturedModelEmitsMaterials) {
    auto db = load_db();
    ASSERT_TRUE(db->valid());

    Found found = find_textured_model(*db, 300);
    ASSERT_GE(found.model_index, 0)
        << "no textured model found in the first 300 (fixtures are the "
           "real KoreaObj database — something is broken)";

    auto out_dir = make_out_dir("textured");
    char id[32];
    std::snprintf(id, sizeof(id), "koreaobj:%05d", found.model_index);
    GltfEmitResult result;
    EXPECT_NO_THROW(result = emit_model_as_gltf(*db, found.model_index, out_dir, id));

    EXPECT_GT(result.texture_count, 0u);
    EXPECT_GT(result.material_count, 0u);
    EXPECT_GT(result.primitive_count, 0u);

    const std::string json = read_file(result.gltf_path);
    ASSERT_FALSE(json.empty());

    EXPECT_NE(json.find("\"materials\""), std::string::npos);
    EXPECT_NE(json.find("\"images\""), std::string::npos);
    EXPECT_NE(json.find("\"textures\""), std::string::npos);
    EXPECT_NE(json.find("\"baseColorTexture\""), std::string::npos);
    EXPECT_NE(json.find("\"attributes\""), std::string::npos);
    EXPECT_NE(json.find("\"TEXCOORD_0\""), std::string::npos);

    // Image URIs point at the textures/ subdirectory.
    EXPECT_NE(json.find("textures/"), std::string::npos);

    // Round-trip through f4-gltf: nested attributes must parse.
    f4::gltf::GltfDocument doc;
    EXPECT_NO_THROW(doc.load(result.gltf_path));
    ASSERT_FALSE(doc.meshes.empty());
    bool saw_uv = false;
    for (const auto& mesh : doc.meshes) {
        for (const auto& prim : mesh.primitives) {
            EXPECT_TRUE(prim.positions.has_value());
            EXPECT_TRUE(prim.normals.has_value());
            EXPECT_TRUE(prim.indices.has_value());
            if (prim.texcoords0.has_value()) saw_uv = true;
        }
    }
    EXPECT_TRUE(saw_uv) << "a textured model should carry TEXCOORD_0";

    fs::remove_all(out_dir);
}

// ── Emission is deterministic for the same input ──────────────────────────

TEST(TexturesGltf, EmissionIsDeterministic) {
    auto db = load_db();
    Found found = find_textured_model(*db, 300);
    ASSERT_GE(found.model_index, 0);

    auto dir1 = make_out_dir("det1");
    auto dir2 = make_out_dir("det2");
    emit_model_as_gltf(*db, found.model_index, dir1, "koreaobj:det");
    emit_model_as_gltf(*db, found.model_index, dir2, "koreaobj:det");

    EXPECT_FALSE(read_file(dir1 / "det.gltf").empty());
    EXPECT_EQ(read_file(dir1 / "det.gltf"), read_file(dir2 / "det.gltf"))
        << "same input should produce byte-identical glTF";

    fs::remove_all(dir1);
    fs::remove_all(dir2);
}

// ── PNG export writes a decodable image with the right dimensions ─────────

TEST(TexturesGltf, PngExportMatchesDecodedDimensions) {
    auto tex_path = find_tex_file();
    if (tex_path.empty()) {
        GTEST_SKIP() << "no KoreaObj.TEX available (set F4_KOREAOBJ_TEX)";
    }

    auto db = load_db();
    std::string err = db->load_tex(tex_path);
    ASSERT_EQ(err, "") << err;

    auto out_dir = make_out_dir("png");
    TexturePngResult result;
    EXPECT_NO_THROW(result = write_texture_png(*db, 0, out_dir));

    EXPECT_TRUE(fs::exists(result.png_path));
    EXPECT_GT(result.width, 0);
    EXPECT_EQ(result.width, result.height);  // KoreaObj textures are square

    // Minimal PNG structure check: 8-byte signature + IHDR with the
    // declared dimensions (big-endian width/height at bytes 16..23).
    const std::string png = read_file(result.png_path);
    ASSERT_GT(png.size(), 24u);

    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    EXPECT_EQ(std::memcmp(png.data(), kSig, 8), 0) << "not a PNG file";

    const auto be32 = [&](std::size_t off) {
        return (static_cast<uint32_t>(static_cast<uint8_t>(png[off])) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(png[off + 1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(png[off + 2])) << 8) |
               static_cast<uint32_t>(static_cast<uint8_t>(png[off + 3]));
    };
    EXPECT_EQ(be32(16), static_cast<uint32_t>(result.width));
    EXPECT_EQ(be32(20), static_cast<uint32_t>(result.height));

    fs::remove_all(out_dir);
}

// ── PNG export fails cleanly for an undecodable index ────────────────────

TEST(TexturesGltf, PngExportRejectsBadIndex) {
    auto tex_path = find_tex_file();
    if (tex_path.empty()) {
        GTEST_SKIP() << "no KoreaObj.TEX available (set F4_KOREAOBJ_TEX)";
    }

    auto db = load_db();
    ASSERT_EQ(db->load_tex(tex_path), "");

    auto out_dir = make_out_dir("badidx");
    EXPECT_ANY_THROW(write_texture_png(*db, db->n_textures() + 100, out_dir));

    fs::remove_all(out_dir);
}

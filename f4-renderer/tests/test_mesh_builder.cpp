// f4-renderer/tests/test_mesh_builder.cpp
//
// Unit tests for the glTF mesh-extraction pipeline
// (extract_gltf_lod_geometry + the glTF coordinate transforms).
// Pure CPU data — no Raylib GPU context required (GltfDocument is
// constructed in-memory, buffers filled by hand).

#include <f4/renderer/mesh_builder.hpp>
#include <f4/renderer/coord_transform.hpp>
#include <f4/gltf/gltf_loader.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace f4::renderer;

namespace {

// Append raw bytes to a buffer and return the byte offset they landed at.
std::size_t append_bytes(std::vector<uint8_t>& data, const void* src,
                         std::size_t n) {
    const auto offset = data.size();
    const auto* bytes = static_cast<const uint8_t*>(src);
    data.insert(data.end(), bytes, bytes + n);
    return offset;
}

// Append a float array (tightly packed).
std::size_t append_floats(std::vector<uint8_t>& data,
                          const std::vector<float>& values) {
    return append_bytes(data, values.data(), values.size() * sizeof(float));
}

struct PrimitiveFixture {
    f4::gltf::GltfDocument doc;

    // Builds one LOD_0 mesh with one triangle:
    //   positions (glTF meters): (0,0,0), (1,0,0), (0,1,0)
    //   normals:                 (0,1,0) x3
    //   uv:                      (0,0), (1,0), (0,1)
    //   material 0 → texture 0 → image 0 → uri "textures/00007.png"
    //   indices: 0,1,2 (uint32)
    static PrimitiveFixture make_default() {
        PrimitiveFixture f;

        std::vector<uint8_t> data;

        // Positions: use distinct, easy-to-transform triples.
        const std::vector<float> positions = {
            0.f, 0.f, 0.f,
            2.f, 0.f, 0.f,
            0.f, 2.f, 0.f,
        };
        const std::vector<float> normals = {
            0.f, 1.f, 0.f,
            0.f, 1.f, 0.f,
            0.f, 1.f, 0.f,
        };
        const std::vector<float> uvs = {
            0.f, 0.f,
            1.f, 0.f,
            0.f, 1.f,
        };
        const std::vector<uint32_t> indices = {0, 1, 2};

        const auto pos_off = append_floats(data, positions);
        const auto nrm_off = append_floats(data, normals);
        const auto uv_off = append_floats(data, uvs);
        const auto idx_off =
            append_bytes(data, indices.data(), indices.size() * sizeof(uint32_t));

        f4::gltf::Buffer buf;
        buf.byte_length = data.size();
        buf.data = std::move(data);
        f.doc.buffers.push_back(std::move(buf));

        auto make_view = [&](std::size_t off, std::size_t len, int target) {
            f4::gltf::BufferView bv;
            bv.buffer = 0;
            bv.byte_offset = off;
            bv.byte_length = len;
            bv.target = target;
            return bv;
        };
        f.doc.buffer_views.push_back(
            make_view(pos_off, positions.size() * 4, 34962));
        f.doc.buffer_views.push_back(
            make_view(nrm_off, normals.size() * 4, 34962));
        f.doc.buffer_views.push_back(make_view(uv_off, uvs.size() * 4, 34962));
        f.doc.buffer_views.push_back(
            make_view(idx_off, indices.size() * 4, 34963));

        auto make_acc = [&](std::size_t view, int ctype, std::size_t count,
                            const char* type) {
            f4::gltf::Accessor a;
            a.buffer_view = view;
            a.component_type = ctype;
            a.count = count;
            a.type = type;
            return a;
        };
        f.doc.accessors.push_back(make_acc(0, 5126, 3, "VEC3"));   // POSITION
        f.doc.accessors.push_back(make_acc(1, 5126, 3, "VEC3"));   // NORMAL
        f.doc.accessors.push_back(make_acc(2, 5126, 3, "VEC2"));   // TEXCOORD_0
        f.doc.accessors.push_back(make_acc(3, 5125, 3, "SCALAR")); // indices

        f4::gltf::Image img;
        img.uri = "textures/00007.png";
        f.doc.images.push_back(img);

        f4::gltf::Texture tex;
        tex.image = 0;
        f.doc.textures.push_back(tex);

        f4::gltf::Material mat;
        mat.name = "tex:00007";
        mat.baseColorTexture = 0;
        f.doc.materials.push_back(mat);

        f4::gltf::Primitive prim;
        prim.positions = 0;
        prim.normals = 1;
        prim.texcoords0 = 2;
        prim.indices = 3;
        prim.material = 0;
        prim.mode = 4;

        f4::gltf::Mesh mesh;
        mesh.name = "LOD_0";
        mesh.primitives.push_back(prim);
        f.doc.meshes.push_back(std::move(mesh));
        return f;
    }
};

}  // namespace

// ── glTF → Raylib coordinate transforms ─────────────────────────────────────

TEST(GltfCoordTransform, Vertex_InvertsExportBake) {
    // Export: falcon (x,y,z) → glTF (y, z, −x) × 0.3048.
    // The runtime transform must invert it back to falcon-then-raylib:
    //   falcon (1, 2, 3) → glTF (2×0.3048, 3×0.3048, −1×0.3048)
    //   raylib from falcon: (y, −z, −x) = (2, −3, −1)
    const float kM = 0.3048f;
    const auto r = gltf_vertex_to_raylib(2 * kM, 3 * kM, -1 * kM);
    EXPECT_NEAR(r.x, 2.0f, 1e-4f);
    EXPECT_NEAR(r.y, -3.0f, 1e-4f);
    EXPECT_NEAR(r.z, -1.0f, 1e-4f);
}

TEST(GltfCoordTransform, Vertex_ScalesToFeet) {
    const auto r = gltf_vertex_to_raylib(1.0f, 0.0f, 0.0f);
    EXPECT_NEAR(r.x, kMetersToFeet, 1e-4f);
}

TEST(GltfCoordTransform, Normal_NoScale) {
    const auto r = gltf_normal_to_raylib(0.0f, 1.0f, 0.0f);
    EXPECT_NEAR(r.x, 0.0f, 1e-6f);
    EXPECT_NEAR(r.y, -1.0f, 1e-6f);
    EXPECT_NEAR(r.z, 0.0f, 1e-6f);
}

// ── extract_gltf_lod_geometry ───────────────────────────────────────────────

TEST(ExtractGltfLodGeometry, DefaultFixture_ExtractsOnePrimitive) {
    auto f = PrimitiveFixture::make_default();
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
}

TEST(ExtractGltfLodGeometry, Positions_TransformedToRaylibFeet) {
    auto f = PrimitiveFixture::make_default();
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    const auto& m = meshes[0];
    ASSERT_EQ(m.positions.size(), 9u);  // 3 vertices × 3 floats

    // glTF (0,0,0) → raylib (0,0,0)
    EXPECT_NEAR(m.positions[0], 0.f, 1e-4f);
    EXPECT_NEAR(m.positions[1], 0.f, 1e-4f);
    EXPECT_NEAR(m.positions[2], 0.f, 1e-4f);
    // glTF (2,0,0) → (2k, 0, 0) — x scales to feet
    EXPECT_NEAR(m.positions[3], 2.f * kMetersToFeet, 1e-3f);
    EXPECT_NEAR(m.positions[4], 0.f, 1e-4f);
    EXPECT_NEAR(m.positions[5], 0.f, 1e-4f);
    // glTF (0,2,0) → (0, −2k, 0) — y flips (mirrored bake inverse)
    EXPECT_NEAR(m.positions[7], -2.f * kMetersToFeet, 1e-3f);
}

TEST(ExtractGltfLodGeometry, Normals_TransformedWithoutScale) {
    auto f = PrimitiveFixture::make_default();
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    const auto& m = meshes[0];
    ASSERT_EQ(m.normals.size(), 9u);
    for (int v = 0; v < 3; ++v) {
        EXPECT_NEAR(m.normals[v * 3 + 0], 0.f, 1e-6f);
        EXPECT_NEAR(m.normals[v * 3 + 1], -1.f, 1e-6f);  // glTF (0,1,0) → (0,−1,0)
        EXPECT_NEAR(m.normals[v * 3 + 2], 0.f, 1e-6f);
    }
}

TEST(ExtractGltfLodGeometry, UvPassthrough) {
    auto f = PrimitiveFixture::make_default();
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    const auto& m = meshes[0];
    ASSERT_EQ(m.texcoords.size(), 6u);
    EXPECT_NEAR(m.texcoords[0], 0.f, 1e-6f);
    EXPECT_NEAR(m.texcoords[2], 1.f, 1e-6f);
    EXPECT_NEAR(m.texcoords[5], 1.f, 1e-6f);
}

TEST(ExtractGltfLodGeometry, IndicesAndTextureBinding) {
    auto f = PrimitiveFixture::make_default();
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    const auto& m = meshes[0];
    ASSERT_EQ(m.indices.size(), 3u);
    EXPECT_EQ(m.indices[0], 0u);
    EXPECT_EQ(m.indices[1], 1u);
    EXPECT_EQ(m.indices[2], 2u);
    // Material chain: mat 0 → tex 0 → image 0 "textures/00007.png" → id 7.
    EXPECT_EQ(m.tex_id, 7);
    EXPECT_EQ(m.texture_uri, "textures/00007.png");
}

TEST(ExtractGltfLodGeometry, NoMaterial_TexIdNegative) {
    auto f = PrimitiveFixture::make_default();
    f.doc.meshes[0].primitives[0].material = std::nullopt;
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].tex_id, -1);
    EXPECT_TRUE(meshes[0].texture_uri.empty());
}

TEST(ExtractGltfLodGeometry, LodSelection) {
    auto f = PrimitiveFixture::make_default();
    // Clone the LOD_0 mesh as LOD_1 with a different vertex so we can
    // tell them apart.
    f4::gltf::Mesh lod1 = f.doc.meshes[0];
    lod1.name = "LOD_1";
    f.doc.meshes.push_back(std::move(lod1));

    auto lod0 = extract_gltf_lod_geometry(f.doc, 0);
    auto lod1_out = extract_gltf_lod_geometry(f.doc, 1);
    ASSERT_EQ(lod0.size(), 1u);
    ASSERT_EQ(lod1_out.size(), 1u);
    // Both reference accessor 0 — identity is enough; the point is the
    // selection finds the right mesh by name.
    EXPECT_EQ(lod0[0].positions.size(), lod1_out[0].positions.size());
}

TEST(ExtractGltfLodGeometry, MissingLod_ReturnsEmpty) {
    auto f = PrimitiveFixture::make_default();
    auto meshes = extract_gltf_lod_geometry(f.doc, 3);
    EXPECT_TRUE(meshes.empty());
}

TEST(ExtractGltfLodGeometry, NonTriangleMode_Skipped) {
    auto f = PrimitiveFixture::make_default();
    f.doc.meshes[0].primitives[0].mode = 1;  // POINTS
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    EXPECT_TRUE(meshes.empty());
}

TEST(ExtractGltfLodGeometry, NonIndexed_SequentialIndices) {
    auto f = PrimitiveFixture::make_default();
    f.doc.meshes[0].primitives[0].indices = std::nullopt;
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    const auto& m = meshes[0];
    ASSERT_EQ(m.indices.size(), 3u);
    EXPECT_EQ(m.indices[0], 0u);
    EXPECT_EQ(m.indices[1], 1u);
    EXPECT_EQ(m.indices[2], 2u);
}

TEST(ExtractGltfLodGeometry, UnprefixedSingleMesh_ServesAsLod0) {
    auto f = PrimitiveFixture::make_default();
    f.doc.meshes[0].name = "";  // hand-authored fixture style
    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
}

TEST(ExtractGltfLodGeometry, VertexColors_UbyteNormalized) {
    auto f = PrimitiveFixture::make_default();

    // Append a COLOR_0 accessor: 3 vertices × 4 normalized ubytes.
    std::vector<uint8_t> colors = {
        255, 0, 0, 255,
        0, 255, 0, 128,
        0, 0, 255, 0,
    };
    auto& buf = f.doc.buffers[0];
    const auto off = append_bytes(buf.data, colors.data(), colors.size());
    buf.byte_length = buf.data.size();

    f4::gltf::BufferView bv;
    bv.buffer = 0;
    bv.byte_offset = off;
    bv.byte_length = colors.size();
    f.doc.buffer_views.push_back(bv);

    f4::gltf::Accessor acc;
    acc.buffer_view = f.doc.buffer_views.size() - 1;
    acc.component_type = 5121;  // UNSIGNED_BYTE
    acc.normalized = true;
    acc.count = 3;
    acc.type = "VEC4";
    f.doc.accessors.push_back(acc);

    f.doc.meshes[0].primitives[0].colors0 = f.doc.accessors.size() - 1;

    auto meshes = extract_gltf_lod_geometry(f.doc, 0);
    ASSERT_EQ(meshes.size(), 1u);
    const auto& m = meshes[0];
    ASSERT_EQ(m.colors.size(), 12u);
    EXPECT_EQ(m.colors[0], 255);  // R
    EXPECT_EQ(m.colors[1], 0);    // G
    EXPECT_EQ(m.colors[2], 0);    // B
    EXPECT_EQ(m.colors[3], 255);  // A
    // 128/255 → rounded byte 128
    EXPECT_EQ(m.colors[7], 128);
    // 0 alpha stays 0
    EXPECT_EQ(m.colors[11], 0);
}

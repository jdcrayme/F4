// f4-gltf/tests/test_gltf_loader.cpp
//
// Tests for the minimal glTF 2.0 loader. Uses synthetic glTF JSON +
// binary buffers built in the system temp area so the suite is hermetic.

#include <f4/gltf/f4_gltf.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::gltf;
namespace fs = std::filesystem;

namespace {

fs::path make_test_dir(const std::string& suffix) {
    auto p = fs::temp_directory_path() / "f4_gltf_test" / suffix;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << contents;
}

void write_binary_file(const fs::path& p, const std::vector<uint8_t>& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

// A minimal glTF with one triangle (3 vertices, 3 indices).
// Positions: (0,0,0), (1,0,0), (0,1,0). Indices: 0,1,2.
const char* kTriangleGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0] } ],
  "nodes": [ { "name": "root", "mesh": 0 } ],
  "meshes": [ {
    "name": "triangle",
    "primitives": [ {
      "POSITION": 0,
      "indices": 1,
      "mode": 4
    } ]
  } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
      "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR" }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 12, "target": 34963 }
  ],
  "buffers": [ { "byteLength": 48, "uri": "triangle.bin" } ]
})";

std::vector<uint8_t> triangle_buffer() {
    // 3 x float[3] (positions) + 3 x uint32 (indices) = 36 + 12 = 48 bytes
    std::vector<uint8_t> buf;
    auto push_float = [&](float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
    };
    auto push_u32 = [&](uint32_t u) {
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(u >> (i * 8)));
    };
    // Position 0: (0, 0, 0)
    push_float(0); push_float(0); push_float(0);
    // Position 1: (1, 0, 0)
    push_float(1); push_float(0); push_float(0);
    // Position 2: (0, 1, 0)
    push_float(0); push_float(1); push_float(0);
    // Indices: 0, 1, 2
    push_u32(0); push_u32(1); push_u32(2);
    return buf;
}

} // namespace

TEST(GltfLoader, ParsesMinimalTriangle) {
    auto dir = make_test_dir("triangle");
    write_file(dir / "triangle.gltf", kTriangleGltf);
    write_binary_file(dir / "triangle.bin", triangle_buffer());

    GltfDocument doc;
    EXPECT_NO_THROW(doc.load(dir / "triangle.gltf"));

    EXPECT_EQ(doc.buffers.size(), 1u);
    EXPECT_EQ(doc.buffer_views.size(), 2u);
    EXPECT_EQ(doc.accessors.size(), 2u);
    EXPECT_EQ(doc.meshes.size(), 1u);
    EXPECT_EQ(doc.nodes.size(), 1u);
    EXPECT_EQ(doc.scenes.size(), 1u);
    EXPECT_EQ(doc.scene, 0);
    fs::remove_all(dir);
}

TEST(GltfLoader, BufferDataIsLoaded) {
    auto dir = make_test_dir("buffer_loaded");
    write_file(dir / "triangle.gltf", kTriangleGltf);
    write_binary_file(dir / "triangle.bin", triangle_buffer());

    GltfDocument doc;
    doc.load(dir / "triangle.gltf");

    EXPECT_EQ(doc.buffers[0].data.size(), 48u);
    fs::remove_all(dir);
}

TEST(GltfLoader, ReadsPositions) {
    auto dir = make_test_dir("positions");
    write_file(dir / "triangle.gltf", kTriangleGltf);
    write_binary_file(dir / "triangle.bin", triangle_buffer());

    GltfDocument doc;
    doc.load(dir / "triangle.gltf");

    // Accessor 0 = POSITION, 3 VEC3 floats.
    auto v0 = doc.read_vec3_float(0, 0);
    ASSERT_TRUE(v0.has_value());
    EXPECT_FLOAT_EQ((*v0)[0], 0.0f);
    EXPECT_FLOAT_EQ((*v0)[1], 0.0f);
    EXPECT_FLOAT_EQ((*v0)[2], 0.0f);

    auto v1 = doc.read_vec3_float(0, 1);
    ASSERT_TRUE(v1.has_value());
    EXPECT_FLOAT_EQ((*v1)[0], 1.0f);

    auto v2 = doc.read_vec3_float(0, 2);
    ASSERT_TRUE(v2.has_value());
    EXPECT_FLOAT_EQ((*v2)[1], 1.0f);
    fs::remove_all(dir);
}

TEST(GltfLoader, ReadsIndices) {
    auto dir = make_test_dir("indices");
    write_file(dir / "triangle.gltf", kTriangleGltf);
    write_binary_file(dir / "triangle.bin", triangle_buffer());

    GltfDocument doc;
    doc.load(dir / "triangle.gltf");

    // Accessor 1 = indices, 3 uint32.
    EXPECT_EQ(doc.read_index_u32(1, 0), 0u);
    EXPECT_EQ(doc.read_index_u32(1, 1), 1u);
    EXPECT_EQ(doc.read_index_u32(1, 2), 2u);
    fs::remove_all(dir);
}

TEST(GltfLoader, ParsesNodeHierarchy) {
    auto dir = make_test_dir("hierarchy");
    const char* json = R"({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [0] } ],
      "nodes": [
        { "name": "root", "children": [1, 2] },
        { "name": "child1", "mesh": 0 },
        { "name": "child2" }
      ],
      "meshes": [ { "name": "m", "primitives": [] } ]
    })";
    write_file(dir / "test.gltf", json);

    GltfDocument doc;
    EXPECT_NO_THROW(doc.load(dir / "test.gltf"));

    ASSERT_EQ(doc.nodes.size(), 3u);
    EXPECT_EQ(doc.nodes[0].name, "root");
    EXPECT_EQ(doc.nodes[0].children.size(), 2u);
    EXPECT_EQ(doc.nodes[0].children[0], 1u);
    EXPECT_EQ(doc.nodes[0].children[1], 2u);
    EXPECT_TRUE(doc.nodes[1].mesh.has_value());
    EXPECT_EQ(*doc.nodes[1].mesh, 0u);
    EXPECT_FALSE(doc.nodes[2].mesh.has_value());
    fs::remove_all(dir);
}

TEST(GltfLoader, ParsesF4Extras) {
    auto dir = make_test_dir("f4_extras");
    const char* json = R"({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [0] } ],
      "nodes": [
        { "name": "dof:gear", "extras": { "f4": {
            "v": 1, "kind": "dof", "id": "gear", "index": 7,
            "min": 0.0, "max": 1.57, "mult": 1.0, "flags": 0
        } } },
        { "name": "lod:0", "extras": { "f4": {
            "v": 1, "kind": "lod", "id": "0", "level": 0
        } } }
      ]
    })";
    write_file(dir / "test.gltf", json);

    GltfDocument doc;
    EXPECT_NO_THROW(doc.load(dir / "test.gltf"));

    ASSERT_EQ(doc.nodes.size(), 2u);
    EXPECT_TRUE(doc.nodes[0].has_f4);
    EXPECT_EQ(doc.nodes[0].f4.kind, "dof");
    EXPECT_EQ(doc.nodes[0].f4.id, "gear");
    EXPECT_EQ(doc.nodes[0].f4.dof_index.value_or(-1), 7);
    EXPECT_FLOAT_EQ(doc.nodes[0].f4.dof_min.value_or(-1), 0.0f);
    EXPECT_FLOAT_EQ(doc.nodes[0].f4.dof_max.value_or(-1), 1.57f);

    EXPECT_TRUE(doc.nodes[1].has_f4);
    EXPECT_EQ(doc.nodes[1].f4.kind, "lod");
    EXPECT_EQ(doc.nodes[1].f4.lod_level.value_or(-1), 0);
    fs::remove_all(dir);
}

TEST(GltfLoader, CountF4NodesByKind) {
    auto dir = make_test_dir("count");
    const char* json = R"({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [0] } ],
      "nodes": [
        { "name": "dof:gear", "extras": { "f4": { "kind": "dof", "id": "gear" } } },
        { "name": "dof:flap.l", "extras": { "f4": { "kind": "dof", "id": "flap.l" } } },
        { "name": "sw:hook", "extras": { "f4": { "kind": "sw", "id": "hook" } } },
        { "name": "lod:0", "extras": { "f4": { "kind": "lod", "id": "0" } } }
      ]
    })";
    write_file(dir / "test.gltf", json);

    GltfDocument doc;
    doc.load(dir / "test.gltf");

    EXPECT_EQ(doc.count_f4_nodes("dof"), 2u);
    EXPECT_EQ(doc.count_f4_nodes("sw"), 1u);
    EXPECT_EQ(doc.count_f4_nodes("lod"), 1u);
    EXPECT_EQ(doc.count_f4_nodes("anchor"), 0u);
    fs::remove_all(dir);
}

TEST(GltfLoader, FindF4NodeById) {
    auto dir = make_test_dir("find");
    const char* json = R"({
      "asset": { "version": "2.0" },
      "scene": 0,
      "scenes": [ { "nodes": [0] } ],
      "nodes": [
        { "name": "dof:gear", "extras": { "f4": { "kind": "dof", "id": "gear" } } }
      ]
    })";
    write_file(dir / "test.gltf", json);

    GltfDocument doc;
    doc.load(dir / "test.gltf");

    const Node* n = doc.find_f4_node("dof", "gear");
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->name, "dof:gear");
    EXPECT_EQ(doc.find_f4_node("dof", "flap"), nullptr);
    fs::remove_all(dir);
}

// ── f4 tag grammar helpers ─────────────────────────────────────────────────

TEST(F4TagGrammar, ParsesDofNodeName) {
    std::string kind, id;
    EXPECT_TRUE(parse_f4_node_name("dof:gear", kind, id));
    EXPECT_EQ(kind, "dof");
    EXPECT_EQ(id, "gear");
}

TEST(F4TagGrammar, ParsesNodeWithInstance) {
    std::string kind, id;
    EXPECT_TRUE(parse_f4_node_name("dof:flap.l", kind, id));
    EXPECT_EQ(kind, "dof");
    EXPECT_EQ(id, "flap.l");
}

TEST(F4TagGrammar, RejectsUnknownKind) {
    std::string kind, id;
    EXPECT_FALSE(parse_f4_node_name("foobar:gear", kind, id));
}

TEST(F4TagGrammar, RejectsMissingColon) {
    std::string kind, id;
    EXPECT_FALSE(parse_f4_node_name("dofgear", kind, id));
}

TEST(F4TagGrammar, RejectsEmptyId) {
    std::string kind, id;
    EXPECT_FALSE(parse_f4_node_name("dof:", kind, id));
}

TEST(F4TagGrammar, IsReservedKind) {
    EXPECT_TRUE(is_reserved_kind("dof"));
    EXPECT_TRUE(is_reserved_kind("sw"));
    EXPECT_TRUE(is_reserved_kind("slot"));
    EXPECT_TRUE(is_reserved_kind("anchor"));
    EXPECT_TRUE(is_reserved_kind("lod"));
    EXPECT_TRUE(is_reserved_kind("col"));
    EXPECT_FALSE(is_reserved_kind("foobar"));
    EXPECT_FALSE(is_reserved_kind(""));
}

TEST(GltfLoader, ThrowsOnMissingFile) {
    GltfDocument doc;
    EXPECT_THROW(doc.load("/no/such/file.gltf"), std::runtime_error);
}

// ── Materials / textures / images (Tranche 0d — the texture-binding
//    chain the RuntimeModelCache walks) ─────────────────────────────────

TEST(GltfLoader, ParsesMaterialTextureChain) {
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "root"}],
        "images": [
            {"name": "tex42", "uri": "textures/00042.png"},
            {"uri": "textures/00043.png"}
        ],
        "textures": [
            {"name": "t0", "source": 0},
            {"source": 1}
        ],
        "materials": [
            {"name": "tex:00042",
             "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
             "alphaMode": "MASK", "alphaCutoff": 0.4},
            {"name": "vertexcolor"}
        ],
        "meshes": []
    })";
    f4::gltf::GltfDocument doc;
    doc.load_from_string(json);

    ASSERT_EQ(doc.images.size(), 2u);
    EXPECT_EQ(doc.images[0].uri, "textures/00042.png");
    ASSERT_EQ(doc.textures.size(), 2u);
    ASSERT_TRUE(doc.textures[0].image.has_value());
    EXPECT_EQ(*doc.textures[0].image, 0u);
    ASSERT_EQ(doc.materials.size(), 2u);

    // Full chain: material → texture → image URI.
    auto uri = doc.material_basecolor_uri(0);
    ASSERT_TRUE(uri.has_value());
    EXPECT_EQ(*uri, "textures/00042.png");

    // The vertexcolor material has no base-color texture.
    EXPECT_FALSE(doc.material_basecolor_uri(1).has_value());
    // Out-of-range index → nullopt.
    EXPECT_FALSE(doc.material_basecolor_uri(99).has_value());
    // No material at all → nullopt.
    EXPECT_FALSE(doc.material_basecolor_uri(std::nullopt).has_value());
}

TEST(GltfLoader, ParsesPrimitiveMaterialIndex) {
    const std::string json = R"({
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "root"}],
        "materials": [{"name": "tex:00007"}],
        "meshes": [{
            "name": "LOD_0",
            "primitives": [
                {"attributes": {"POSITION": 0}, "mode": 4, "material": 0},
                {"attributes": {"POSITION": 0}, "mode": 4}
            ]
        }],
        "accessors": [{"componentType": 5126, "count": 3,
                        "type": "VEC3",
                        "min": [0,0,0], "max": [0,0,0]}],
        "bufferViews": [],
        "buffers": []
    })";
    f4::gltf::GltfDocument doc;
    doc.load_from_string(json);

    ASSERT_EQ(doc.meshes.size(), 1u);
    ASSERT_EQ(doc.meshes[0].primitives.size(), 2u);
    ASSERT_TRUE(doc.meshes[0].primitives[0].material.has_value());
    EXPECT_EQ(*doc.meshes[0].primitives[0].material, 0u);
    EXPECT_FALSE(doc.meshes[0].primitives[1].material.has_value());
}

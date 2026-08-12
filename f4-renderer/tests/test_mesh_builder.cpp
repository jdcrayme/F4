// f4-renderer/tests/test_mesh_builder.cpp
//
// Unit tests for mesh_builder's resolve_vertex_color function.
// This can be tested without a Raylib GPU context — it only needs
// f4::models::ColorBank.

#include <f4/renderer/mesh_builder.hpp>
#include <f4/models/geometry.hpp>
#include <f4/models/model_database.hpp>

#include <gtest/gtest.h>

using namespace f4::renderer;

// ── resolve_vertex_color ──────────────────────────────────────────────────────

TEST(ResolveVertexColor, IndexZero_Textured_ReturnsWhite) {
    f4::models::ColorBank cb; // empty color bank
    auto c = resolve_vertex_color(0, cb, true);
    EXPECT_EQ(c.r, 255);
    EXPECT_EQ(c.g, 255);
    EXPECT_EQ(c.b, 255);
    EXPECT_EQ(c.a, 255);
}

TEST(ResolveVertexColor, IndexZero_NonTextured_ReturnsGrey) {
    f4::models::ColorBank cb; // empty color bank
    auto c = resolve_vertex_color(0, cb, false);
    EXPECT_EQ(c.r, 180);
    EXPECT_EQ(c.g, 180);
    EXPECT_EQ(c.b, 180);
    EXPECT_EQ(c.a, 255);
}

TEST(ResolveVertexColor, ColorBankLookup) {
    f4::models::ColorBank cb;
    // Set up one color at index 1: ABGR packed as (R<<24 | G<<16 | B<<8 | A)
    f4::models::ColorEntry entry;
    entry.r = 255;
    entry.g = 0;
    entry.b = 128;
    entry.a = 200;
    cb.colors.push_back(entry); // index 0
    cb.colors.push_back(entry); // index 1

    auto c = resolve_vertex_color(1, cb, false);
    EXPECT_EQ(c.r, 255);
    EXPECT_EQ(c.g, 0);
    EXPECT_EQ(c.b, 128);
    EXPECT_EQ(c.a, 200);
}

TEST(ResolveVertexColor, ColorBankLookup_MissingIndex) {
    f4::models::ColorBank cb; // empty — no colors
    // Index 5 in empty bank: rgba_at returns 0
    // Since index < 4096 and rgba_at returns 0, we fall through
    // to the packed RGBA path
    auto c = resolve_vertex_color(5, cb, false);
    // With rgba_at returning 0, we get the fallback packed RGBA decode
    // of index 5: R=5, G=0, B=0, A=0
    EXPECT_EQ(c.r, 5);
    EXPECT_EQ(c.g, 0);
    EXPECT_EQ(c.b, 0);
    EXPECT_EQ(c.a, 0);
}

TEST(ResolveVertexColor, LargeValue_PackedRGBA) {
    f4::models::ColorBank cb; // empty
    // Large value (>= 4096) should be treated as packed RGBA
    // 0xFF0000FF = R=255, G=0, B=0, A=255 (little-endian packed)
    const uint32_t packed = 0xFF0000FF;
    auto c = resolve_vertex_color(packed, cb, false);
    EXPECT_EQ(c.r, static_cast<unsigned char>(packed & 0xFF));         // FF = 255
    EXPECT_EQ(c.g, static_cast<unsigned char>((packed >> 8) & 0xFF));  // 00
    EXPECT_EQ(c.b, static_cast<unsigned char>((packed >> 16) & 0xFF)); // 00
    EXPECT_EQ(c.a, static_cast<unsigned char>((packed >> 24) & 0xFF)); // FF = 255
}

// ── build_mesh_entries ────────────────────────────────────────────────────────

TEST(BuildMeshEntries, EmptyGeometry_ProducesNoEntries) {
    f4::models::ModelGeometry geom;
    std::vector<::Mesh> raylib_meshes;
    auto entries = build_mesh_entries(geom, raylib_meshes);
    EXPECT_TRUE(entries.empty());
}

TEST(BuildMeshEntries, SingleMesh_ProducesOneEntry) {
    f4::models::ModelGeometry geom;
    f4::models::Mesh m;
    m.tex_id = 3;
    m.kind = f4::models::PrimitiveKind::Triangles;
    geom.meshes.push_back(m);

    std::vector<::Mesh> raylib_meshes(1);
    auto entries = build_mesh_entries(geom, raylib_meshes);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].tex_id, 3);
}

TEST(BuildMeshEntries, MultipleMeshes_CorrectTexIds) {
    f4::models::ModelGeometry geom;
    for (int i = 0; i < 5; ++i) {
        f4::models::Mesh m;
        m.tex_id = i * 10;
        m.kind = f4::models::PrimitiveKind::Triangles;
        geom.meshes.push_back(m);
    }
    std::vector<::Mesh> raylib_meshes(5);
    auto entries = build_mesh_entries(geom, raylib_meshes);
    ASSERT_EQ(entries.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(entries[i].tex_id, i * 10);
    }
}

TEST(BuildMeshEntries, MoreGeometryThanMeshes_SkipsMissing) {
    f4::models::ModelGeometry geom;
    for (int i = 0; i < 3; ++i) {
        f4::models::Mesh m;
        m.tex_id = i;
        m.kind = f4::models::PrimitiveKind::Triangles;
        geom.meshes.push_back(m);
    }
    // Only 1 raylib mesh but 3 geometry meshes
    std::vector<::Mesh> raylib_meshes(1);
    auto entries = build_mesh_entries(geom, raylib_meshes);
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].tex_id, 0);
    EXPECT_EQ(entries[1].tex_id, 1);
    EXPECT_EQ(entries[2].tex_id, 2);
}

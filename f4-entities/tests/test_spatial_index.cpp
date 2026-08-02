// test_spatial_index.cpp — the SpatialIndex acceleration structure.

#include <gtest/gtest.h>
#include <f4/entities/f4_entities.hpp>

using namespace f4::entities;

TEST(SpatialIndex, InsertAndQueryFindsNearby) {
    SpatialIndex idx(1000.0);   // 1000 ft cells
    idx.insert(EntityId::make(1, 1), 0.0, 0.0, 0.0);
    idx.insert(EntityId::make(2, 1), 500.0, 0.0, 0.0);   // same cell
    idx.insert(EntityId::make(3, 1), 1500.0, 0.0, 0.0);  // next cell
    auto ids = idx.query_radius(0, 0, 0, 600.0);
    EXPECT_EQ(ids.size(), 2u);   // entries 1 and 2 within 600 ft
}

TEST(SpatialIndex, QueryAcrossCellBoundary) {
    // Query radius straddles cell boundaries — must scan neighbor cells.
    SpatialIndex idx(1000.0);
    idx.insert(EntityId::make(1, 1), 0.0, 0.0, 0.0);
    idx.insert(EntityId::make(2, 1), 999.0, 999.0, 999.0);
    auto ids = idx.query_radius(0, 0, 0, 1800.0);
    EXPECT_EQ(ids.size(), 2u);
}

TEST(SpatialIndex, RemoveDeletesEntry) {
    SpatialIndex idx(1000.0);
    EntityId a = EntityId::make(1, 1);
    idx.insert(a, 0.0, 0.0, 0.0);
    EXPECT_EQ(idx.query_radius(0, 0, 0, 100.0).size(), 1u);
    idx.remove(a);
    EXPECT_EQ(idx.query_radius(0, 0, 0, 100.0).size(), 0u);
}

TEST(SpatialIndex, UpdateMovesEntry) {
    SpatialIndex idx(1000.0);
    EntityId a = EntityId::make(1, 1);
    idx.insert(a, 0.0, 0.0, 0.0);
    idx.update(a, 50000.0, 0.0, 0.0);   // move far away
    EXPECT_EQ(idx.query_radius(0, 0, 0, 100.0).size(), 0u);
    EXPECT_EQ(idx.query_radius(50000, 0, 0, 100.0).size(), 1u);
}

TEST(SpatialIndex, EmptyQueryReturnsEmpty) {
    SpatialIndex idx(1000.0);
    EXPECT_TRUE(idx.query_radius(0, 0, 0, 100.0).empty());
}

TEST(SpatialIndex, NegativeRadiusReturnsEmpty) {
    SpatialIndex idx(1000.0);
    idx.insert(EntityId::make(1, 1), 0.0, 0.0, 0.0);
    EXPECT_TRUE(idx.query_radius(0, 0, 0, -10.0).empty());
}

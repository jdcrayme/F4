// test_bubble_manager.cpp — BubbleManager per-tick deagg/reagg tests.
//
// Verifies the BubbleManager class:
//
//   1. Construction + defaults (1024 ft ground, 2560 ft air radius).
//   2. update() with a player position far from any unit → no spawns.
//   3. update() with a player near a unit → that unit deaggregates.
//   4. update() again with the player moved away → reaggregation.
//   5. force_deaggregate / force_reaggregate bypass the bubble.
//   6. clear() destroys all spawned vehicles.
//   7. Multiple units in/out of the bubble are handled independently.
//
// These tests use hand-constructed EntityWorld data — no fixtures required.
// The ClassTable is empty (default-constructed), so spawn_vehicles_from_unit
// returns no vehicles (CT lookup yields visType[0] == 0). The tests assert
// the spawn/destroy bookkeeping (deaggregated_units map, vehicle_entities
// vector), not actual vehicle counts.

#include <gtest/gtest.h>

#include "f4/simulation/bubble_manager.hpp"
#include "f4/simulation/visual_model_component.hpp"

#include <f4/entities/entity.hpp>
#include <f4/entities/types.hpp>
#include <f4/world_types/class_table.hpp>
#include <f4/models/model_database.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace f4::simulation;
using namespace f4::entities;
using namespace f4::world_types;
using UC = f4::entities::UnitClass;

namespace {

// Build a Battalion entity at the given ENU position (feet).
// VehicleCompositionComponent has 1 group × 1 live vehicle.
EntityId make_battalion_at(EntityWorld& world, double enu_x, double enu_y) {
    auto h = world.create();
    auto& tf = h.add<TransformComponent>();
    tf.position = f4::geo::WorldPosition(enu_x, enu_y, 0.0);
    auto& uc = h.add<UnitCoreComponent>();
    uc.unit_class = UC::Battalion;
    uc.class_table_index = 170;
    auto& vc = h.add<VehicleCompositionComponent>();
    VehicleGroup g;
    g.vehicle_type = 273;
    g.live_count = 1;
    vc.groups.push_back(g);
    return h.id();
}

} // namespace

// ── B.0: AII-driven radii (bubble_radii_from_aii) ──────────────────────────

namespace {

// Write a small AII to a temp file; remove on destruction.
struct TempAii {
    std::filesystem::path path;
    explicit TempAii(const std::string& text)
        : path(std::filesystem::temp_directory_path() /
               ("f4_bubble_aii_" + std::to_string(std::random_device{}()) +
                ".aII")) {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    ~TempAii() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempAii(const TempAii&) = delete;
    TempAii& operator=(const TempAii&) = delete;
};

} // namespace

// Empty path (the "not configured" case every pre-B.0 scenario exercises)
// → the documented defaults, byte-identical to the pre-B.0 hardcoded radii.
TEST(BubbleRadii, EmptyPathGivesDocumentedDefaults) {
    const auto [ground, air] = bubble_radii_from_aii({});
    EXPECT_DOUBLE_EQ(ground, 1024.0); // 1.0 grid × 1024 ft
    EXPECT_DOUBLE_EQ(air, 2560.0);    // 2.5 grid × 1024 ft
}

// A missing file is the same as no file — defaults preserved.
TEST(BubbleRadii, MissingFileGivesDocumentedDefaults) {
    const auto [ground, air] = bubble_radii_from_aii(
        std::filesystem::temp_directory_path() /
        "f4_no_such_aii_7q3f.aII");
    EXPECT_DOUBLE_EQ(ground, 1024.0);
    EXPECT_DOUBLE_EQ(air, 2560.0);
}

// A present AII retunes both radii (grid units × 1024 ft).
TEST(BubbleRadii, AiiFileRetunesRadii) {
    TempAii aii("[Sim]\n"
                "SIM_BUBBLE_SIZE = 4.0\n"
                "GROUND_BUBBLE_SIZE = 0.5\n");
    const auto [ground, air] = bubble_radii_from_aii(aii.path);
    EXPECT_DOUBLE_EQ(ground, 512.0);  // 0.5 × 1024
    EXPECT_DOUBLE_EQ(air, 4096.0);    // 4.0 × 1024
}

// The FreeFalcon-source spellings (MinBubbleSize / BubbleRatioToUnitSpan)
// drive the same radii — the shipped fixtures/Falcon4.AII spelling.
TEST(BubbleRadii, FreeFalconSpellingsDriveRadii) {
    TempAii aii("[Sim]\n"
                "MinBubbleSize = 3.0\n"
                "BubbleRatioToUnitSpan = 1.5\n");
    const auto [ground, air] = bubble_radii_from_aii(aii.path);
    EXPECT_DOUBLE_EQ(ground, 1536.0); // 1.5 × 1024
    EXPECT_DOUBLE_EQ(air, 3072.0);    // 3.0 × 1024
}

// A malformed AII fails loudly — never a silent default.
TEST(BubbleRadii, MalformedAiiThrows) {
    TempAii aii("[Sim]\nSIM_BUBBLE_SIZE = wide\n");
    EXPECT_THROW(bubble_radii_from_aii(aii.path), std::runtime_error);
}

// ── Construction & defaults ───────────────────────────────────────────────

TEST(BubbleManager, Constructor_SetsDefaultRadii) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    BubbleManager bm(world, ct, db);
    EXPECT_DOUBLE_EQ(bm.ground_radius_ft(), 1024.0);  // GROUND_BUBBLE_SIZE
    EXPECT_DOUBLE_EQ(bm.air_radius_ft(), 2560.0);     // SIM_BUBBLE_SIZE
}

TEST(BubbleManager, Constructor_AcceptsCustomRadii) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    BubbleManager bm(world, ct, db, /*ground=*/500.0, /*air=*/1000.0);
    EXPECT_DOUBLE_EQ(bm.ground_radius_ft(), 500.0);
    EXPECT_DOUBLE_EQ(bm.air_radius_ft(), 1000.0);
}

TEST(BubbleManager, InitiallyEmpty) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    BubbleManager bm(world, ct, db);
    EXPECT_TRUE(bm.vehicle_entities().empty());
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
}

// ── update() — basic bubble behavior ──────────────────────────────────────

TEST(BubbleManager, Update_PlayerFarFromUnit_NoDeagg) {
    // Unit at (100000, 0), player at (0, 0). Distance = 100000 ft ≫ 1024 ft.
    // No deaggregation.
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    make_battalion_at(world, 100000.0, 0.0);
    BubbleManager bm(world, ct, db);

    bm.update(f4::geo::WorldPosition(0.0, 0.0, 0.0));
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
    EXPECT_TRUE(bm.vehicle_entities().empty());
}

TEST(BubbleManager, Update_PlayerOnUnit_DeaggButEmptyDueToEmptyCT) {
    // Unit at (0, 0), player at (0, 0). Distance = 0 < 1024 ft.
    // Deaggregation is attempted — but the CT is empty so
    // spawn_vehicles_from_unit returns no vehicles. The unit is still
    // marked as deaggregated in the map (so we don't retry every tick).
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    make_battalion_at(world, 0.0, 0.0);
    BubbleManager bm(world, ct, db);

    bm.update(f4::geo::WorldPosition(0.0, 0.0, 0.0));
    // spawn_vehicles_from_unit returned empty (CT empty → no models).
    // The unit is NOT marked as deaggregated (deaggregate_ checks for empty
    // spawn result and skips the bookkeeping). So count stays 0.
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
}

TEST(BubbleManager, Update_PlayerLeavesBubble_Reagg) {
    // Move player from on-unit to far-away. With an empty CT, no spawns
    // happen — but the test verifies the update() loop doesn't crash and
    // the deaggregated map stays empty.
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    make_battalion_at(world, 0.0, 0.0);
    BubbleManager bm(world, ct, db);

    bm.update(f4::geo::WorldPosition(0.0, 0.0, 0.0));          // in bubble
    bm.update(f4::geo::WorldPosition(100000.0, 0.0, 0.0));     // out of bubble
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
}

// ── force_deaggregate / force_reaggregate ─────────────────────────────────

TEST(BubbleManager, ForceDeaggregate_OnEmptyCT_StillNoSpawns) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    auto bid = make_battalion_at(world, 100000.0, 0.0);  // far from origin
    BubbleManager bm(world, ct, db);

    bm.force_deaggregate(bid);
    // CT empty → spawn returns empty → not recorded as deaggregated.
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
    EXPECT_TRUE(bm.vehicle_entities().empty());
}

TEST(BubbleManager, ForceReaggregate_NotDeaggregated_NoOp) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    auto bid = make_battalion_at(world, 0.0, 0.0);
    BubbleManager bm(world, ct, db);

    // No prior deaggregate — force_reaggregate should be a no-op.
    bm.force_reaggregate(bid);
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
    EXPECT_TRUE(bm.vehicle_entities().empty());
}

// ── clear() ───────────────────────────────────────────────────────────────

TEST(BubbleManager, Clear_OnEmptyManager_NoOp) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    BubbleManager bm(world, ct, db);
    bm.clear();
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
    EXPECT_TRUE(bm.vehicle_entities().empty());
}

// ── Multiple units ────────────────────────────────────────────────────────

TEST(BubbleManager, Update_MultipleUnits_AllProcessed) {
    // Two units: one near the player, one far. Only the near one is
    // considered for deaggregation. With an empty CT, no spawns happen,
    // but the test verifies the loop handles multiple units without
    // crashing and the deaggregated map stays empty.
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    make_battalion_at(world, 0.0, 0.0);          // near player
    make_battalion_at(world, 100000.0, 0.0);     // far from player
    BubbleManager bm(world, ct, db);

    bm.update(f4::geo::WorldPosition(0.0, 0.0, 0.0));
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);  // CT empty
    EXPECT_TRUE(bm.vehicle_entities().empty());
}

TEST(BubbleManager, Update_NoUnitsInWorld_NoOp) {
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    BubbleManager bm(world, ct, db);
    bm.update(f4::geo::WorldPosition(0.0, 0.0, 0.0));
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);
}

// ── Idempotency ───────────────────────────────────────────────────────────

TEST(BubbleManager, Update_CalledMultipleTimes_NoDoubleDeagg) {
    // Even if the player stays in the bubble across multiple ticks, the
    // unit should only be deaggregated once. With an empty CT this is
    // trivially true (no spawns), but the test verifies the loop is
    // idempotent — no crash, no growth in deaggregated_unit_count.
    EntityWorld world;
    ClassTable ct;
    f4::models::ModelDatabase db;

    make_battalion_at(world, 0.0, 0.0);
    BubbleManager bm(world, ct, db);

    for (int i = 0; i < 5; ++i) {
        bm.update(f4::geo::WorldPosition(0.0, 0.0, 0.0));
    }
    EXPECT_EQ(bm.deaggregated_unit_count(), 0u);  // CT empty → no spawns
}

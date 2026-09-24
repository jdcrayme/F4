// test_strike_aimpoint.cpp — EMPL-2d, the AIM-POINT RULE.
//
// The save's per-mission aim-point element (the wire waypoint's
// target_building byte, carried on the route as aimpoint_feature)
// names the feature on the target objective the planner meant the
// stick to destroy. resolve_feature_aim is the whole contract:
// indexed feature when in range and alive, else the EMPL-1a nominal
// first-alive walk. Pure function — no brain, no world.

#include "f4/ai/brain_component.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using f4::ai::resolve_feature_aim;
using f4::entities::FeatureEntryState;
using f4::entities::FeatureSetComponent;

FeatureEntryState feature(double ox, double oy, double oz,
                          std::uint8_t damage = 0) {
    FeatureEntryState f{};
    f.offset_x = ox;
    f.offset_y = oy;
    f.offset_z = oz;
    f.damage_state = damage;
    return f;
}

TEST(StrikeAimpoint, IndexedAliveFeatureIsTheAim) {
    // The planner indexed feature 1; the stick aims THERE, not at the
    // first-alive walk's feature 0.
    FeatureSetComponent fs;
    fs.features = {feature(500, 0, 0), feature(200, 40, 10),
                   feature(-300, 0, 0)};
    const auto aim =
        resolve_feature_aim(fs, 1, f4::geo::WorldPosition{1000, 2000, 50});
    EXPECT_DOUBLE_EQ(aim.x, 1200.0);
    EXPECT_DOUBLE_EQ(aim.y, 2040.0);
    EXPECT_DOUBLE_EQ(aim.z, 60.0);
}

TEST(StrikeAimpoint, NoneSentinelFallsBackToFirstAlive) {
    // 255 = the wire's "none": the EMPL-1a nominal rule.
    FeatureSetComponent fs;
    fs.features = {feature(500, 0, 0), feature(200, 40, 10)};
    const auto aim =
        resolve_feature_aim(fs, 255, f4::geo::WorldPosition{0, 0, 0});
    EXPECT_DOUBLE_EQ(aim.x, 500.0);
    EXPECT_DOUBLE_EQ(aim.y, 0.0);
}

TEST(StrikeAimpoint, DeadAimPointFallsBackToFirstAlive) {
    // The indexed element is spent (VIS 3): the employment continues
    // against the objective — first alive wins.
    FeatureSetComponent fs;
    fs.features = {feature(500, 0, 0, 3), feature(200, 40, 10)};
    const auto aim =
        resolve_feature_aim(fs, 0, f4::geo::WorldPosition{0, 0, 0});
    EXPECT_DOUBLE_EQ(aim.x, 200.0);
    EXPECT_DOUBLE_EQ(aim.y, 40.0);
}

TEST(StrikeAimpoint, DamagedButAliveAimPointIsStillTheAim) {
    // VIS 2 (damaged) is not rubble — the indexed element is the aim.
    FeatureSetComponent fs;
    fs.features = {feature(500, 0, 0), feature(200, 40, 10, 2)};
    const auto aim =
        resolve_feature_aim(fs, 1, f4::geo::WorldPosition{0, 0, 0});
    EXPECT_DOUBLE_EQ(aim.x, 200.0);
    EXPECT_DOUBLE_EQ(aim.y, 40.0);
}

TEST(StrikeAimpoint, OutOfRangeIndexFallsBackToFirstAlive) {
    FeatureSetComponent fs;
    fs.features = {feature(500, 0, 0)};
    const auto aim =
        resolve_feature_aim(fs, 9, f4::geo::WorldPosition{0, 0, 0});
    EXPECT_DOUBLE_EQ(aim.x, 500.0);
}

TEST(StrikeAimpoint, NoAliveFeatureReturnsTheBase) {
    // All rubble: the aim stays the objective center (the stick aborts
    // per the delivery gate — the center hit takes no feature).
    FeatureSetComponent fs;
    fs.features = {feature(500, 0, 0, 3), feature(200, 40, 10, 3)};
    const auto aim = resolve_feature_aim(
        fs, 1, f4::geo::WorldPosition{1000, 2000, 50});
    EXPECT_DOUBLE_EQ(aim.x, 1000.0);
    EXPECT_DOUBLE_EQ(aim.y, 2000.0);
    EXPECT_DOUBLE_EQ(aim.z, 50.0);
}

}  // namespace

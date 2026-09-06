// f4-world-types/tests/test_layout_types.cpp
//
// Verifies the enum constants extracted into f4-world-types match the
// values f4-simulation::campaign_bridge depends on (the same numeric
// values as f4-world-convert's ObjectiveType / PointType / PointListType).

#include <f4/world_types/layout_types.hpp>

#include <gtest/gtest.h>

namespace wt = f4::world_types;

TEST(LayoutTypes, ObjectiveTypeValues) {
    // The values campaign_bridge checks against (TYPE_AIRBASE / TYPE_AIRSTRIP).
    EXPECT_EQ(static_cast<int>(wt::TYPE_AIRBASE), 1);
    EXPECT_EQ(static_cast<int>(wt::TYPE_AIRSTRIP), 2);
    EXPECT_EQ(static_cast<int>(wt::TYPE_BRIDGE), 6);
    EXPECT_EQ(static_cast<int>(wt::TYPE_TOWN), 39);
}

TEST(LayoutTypes, PointTypeValues) {
    // The values campaign_bridge uses to classify layout points.
    EXPECT_EQ(static_cast<int>(wt::PT_RUNWAY), 1);
    EXPECT_EQ(static_cast<int>(wt::PT_TAKEOFF), 2);
    EXPECT_EQ(static_cast<int>(wt::PT_TAXI), 3);
    EXPECT_EQ(static_cast<int>(wt::PT_TAKE_RUNWAY), 15);
}

TEST(LayoutTypes, PointListTypeValues) {
    // The values campaign_bridge uses to find runway/parking lists.
    EXPECT_EQ(static_cast<int>(wt::PLT_RUNWAY), 1);
    EXPECT_EQ(static_cast<int>(wt::PLT_PARK), 11);
    EXPECT_EQ(static_cast<int>(wt::PLT_FOLLOW_ME), 15);
    EXPECT_EQ(static_cast<int>(wt::PLT_RUNWAY_DIM), 8);
}

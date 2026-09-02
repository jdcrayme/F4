// f4-world-viewer/tests/test_enum_text.cpp
//
// B.3 QC tranche: the WP_ACTION table's authoritative anchors. The
// pre-B.3 guess swapped TAKEOFF/LAND; the TestCamp routes (action 1 at
// the departure airbase, action 7 back home) only decode correctly with
// the campwp.h mapping. These pins keep it that way.

#include <f4/viewer/enum_text.hpp>

#include <gtest/gtest.h>

using namespace f4::viewer;

TEST(WpActionName, AuthoritativeAnchors) {
    EXPECT_STREQ(wp_action_name(0), "Nothing");
    EXPECT_STREQ(wp_action_name(1), "Takeoff");   // WP_TAKEOFF (campwp.h)
    EXPECT_STREQ(wp_action_name(7), "Land");      // WP_LAND
    EXPECT_STREQ(wp_action_name(4), "Refuel");
    EXPECT_STREQ(wp_action_name(16), "SAD");
    EXPECT_STREQ(wp_action_name(17), "Strike");
    EXPECT_STREQ(wp_action_name(19), "SEAD");
    EXPECT_STREQ(wp_action_name(24), "Tanker");
    EXPECT_STREQ(wp_action_name(255), "Unknown");
}

TEST(FormatCampaignTime, DayHourMinuteSecond) {
    char buf[24];
    format_campaign_time(0, buf, sizeof(buf));
    EXPECT_STREQ(buf, "D0 00:00:00");

    // 38,574,360 s = 446 days 10:42:24? No — 38574360 / 86400 = 446 days
    // 6h... let's just pin the arithmetic: 86400*446 = 38,534,400 →
    // remainder 39,960 = 11h 06m 00s.
    format_campaign_time(38574360, buf, sizeof(buf));
    EXPECT_STREQ(buf, "D446 11:06:00");

    format_campaign_time(3661, buf, sizeof(buf));
    EXPECT_STREQ(buf, "D0 01:01:01");

    // Negative relative times render as-is.
    format_campaign_time(-5, buf, sizeof(buf));
    EXPECT_STREQ(buf, "-5");
}

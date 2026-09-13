// f4-world-types/tests/test_day_night.cpp
//
// Task 73: the solar model — pinned elevations at the fixed points (solar
// noon, midnight, solstices), the band boundaries, and the v1 documented
// approximations (solar time, wrapping).

#include <f4/world_types/day_night.hpp>

#include <gtest/gtest.h>

#include <cmath>

namespace wt = f4::world_types;

TEST(DayNight, SolarNoonIsHighAtKoreaLatitude) {
    // Solar noon at the March equinox (day 81 — the model's zero-
    // declination crossing) at 37.5 N: elevation = 90 - 37.5 = 52.5 deg.
    const double elev = wt::solar_elevation_deg(43200.0, 81, 37.5);
    EXPECT_NEAR(elev, 52.5, 0.5);
    EXPECT_EQ(wt::daylight_band(elev), wt::DaylightBand::Day);
}

TEST(DayNight, MidnightIsDeepNight) {
    // Midnight equinox at 37.5 N: -52.5 deg by symmetry.
    const double elev = wt::solar_elevation_deg(0.0, 81, 37.5);
    EXPECT_NEAR(elev, -52.5, 0.5);
    EXPECT_EQ(wt::daylight_band(elev), wt::DaylightBand::Night);
}

TEST(DayNight, SolsticeDeclinations) {
    // Day 172 (~Jun 21) is the June solstice: +23.44; day 355 (~Dec 21):
    // -23.44. The +10 phase shift puts the extrema within a day of both.
    EXPECT_NEAR(wt::solar_declination_deg(172), 23.44, 0.35);
    EXPECT_NEAR(wt::solar_declination_deg(355), -23.44, 0.35);
    // Equinox (day 81, the model's zero crossing — the Spencer-phase
    // cosine crosses zero at d+10 = 91.31, i.e. day 81).
    EXPECT_NEAR(wt::solar_declination_deg(81), 0.0, 0.3);
}

TEST(DayNight, WinterNoonIsLowerThanSummerNoon) {
    // Same clock, same latitude: the season moves the noon elevation by
    // twice the declination — the ordering is the pinned contract.
    const double jun = wt::solar_elevation_deg(43200.0, 172, 37.5);
    const double dec = wt::solar_elevation_deg(43200.0, 355, 37.5);
    EXPECT_GT(jun, dec);
    EXPECT_NEAR(jun - dec, 2.0 * 23.44, 1.0);
}

TEST(DayNight, BandBoundaries) {
    // Contract: > 0 Day, [-6, 0] CivilTwilight, < -6 Night.
    EXPECT_EQ(wt::daylight_band(0.001), wt::DaylightBand::Day);
    EXPECT_EQ(wt::daylight_band(0.0), wt::DaylightBand::CivilTwilight);
    EXPECT_EQ(wt::daylight_band(-6.0), wt::DaylightBand::CivilTwilight);
    EXPECT_EQ(wt::daylight_band(-6.001), wt::DaylightBand::Night);
}

TEST(DayNight, SunriseSunsetBandWalk) {
    // Equinox (day 81) at 37.5 N: sunrise ~06:00, sunset ~18:00 solar,
    // civil twilight +-30 min around each. Walk the day: night ->
    // twilight -> day -> day -> twilight -> night (the -6 deg civil
    // crossing is ~18.4 h, so 18.5 h is already night — pinned exactly).
    EXPECT_EQ(wt::daylight_band_at(4.0 * 3600.0, 81, 37.5),
              wt::DaylightBand::Night);
    EXPECT_EQ(wt::daylight_band_at(5.75 * 3600.0, 81, 37.5),
              wt::DaylightBand::CivilTwilight);
    EXPECT_EQ(wt::daylight_band_at(6.25 * 3600.0, 81, 37.5),
              wt::DaylightBand::Day);
    EXPECT_EQ(wt::daylight_band_at(17.75 * 3600.0, 81, 37.5),
              wt::DaylightBand::Day);
    EXPECT_EQ(wt::daylight_band_at(18.5 * 3600.0, 81, 37.5),
              wt::DaylightBand::Night);
    EXPECT_EQ(wt::daylight_band_at(20.0 * 3600.0, 81, 37.5),
              wt::DaylightBand::Night);
}

TEST(DayNight, SecondsWrap) {
    // Raw campaign seconds beyond a day wrap (a host may pass unbounded
    // campaign time); negative values wrap too.
    const double base = wt::solar_elevation_deg(43200.0, 80, 37.5);
    EXPECT_NEAR(wt::solar_elevation_deg(43200.0 + 86400.0, 80, 37.5), base,
                1e-9);
    EXPECT_NEAR(wt::solar_elevation_deg(-43200.0, 80, 37.5), base, 1e-9);
}

TEST(DayNight, DayOfYearClamps) {
    // Out-of-range day-of-year clamps into [1, 366] rather than feeding
    // the declination cosine garbage.
    EXPECT_NEAR(wt::solar_declination_deg(0),
                wt::solar_declination_deg(1), 1e-12);
    EXPECT_NEAR(wt::solar_declination_deg(400),
                wt::solar_declination_deg(366), 1e-12);
}

TEST(DayNight, SouthernHemisphereSeasonsReversed) {
    // The southern mirror: at -37.5 the June solstice is WINTER (low but
    // above the horizon at noon) and December is summer — the season
    // ordering flips, the sign convention does not.
    const double jun = wt::solar_elevation_deg(43200.0, 172, -37.5);
    const double dec = wt::solar_elevation_deg(43200.0, 355, -37.5);
    EXPECT_GT(jun, 0.0);  // winter noon sun still clears the horizon
    EXPECT_GT(dec, jun);  // summer noon is the high one
    EXPECT_NEAR(dec - jun, 2.0 * 23.44, 1.0);
}

TEST(DayNight, BandNames) {
    EXPECT_STREQ(wt::band_name(wt::DaylightBand::Day), "day");
    EXPECT_STREQ(wt::band_name(wt::DaylightBand::CivilTwilight),
                 "civil_twilight");
    EXPECT_STREQ(wt::band_name(wt::DaylightBand::Night), "night");
}

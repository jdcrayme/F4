// f4-world-types/tests/test_weather.cpp
//
// Task 73: the weather state surface — condition names round-trip, the
// per-condition profiles, the visual scaling contract (clear-day = 1.0,
// the floors), and the band wind interpolation (including the shortest-
// arc direction blend).

#include <f4/world_types/weather.hpp>

#include <gtest/gtest.h>

namespace wt = f4::world_types;

TEST(WeatherCondition, NameRoundTrip) {
    for (const auto c : {wt::WeatherCondition::Clear,
                         wt::WeatherCondition::Hazy,
                         wt::WeatherCondition::Inclement}) {
        const auto parsed =
            wt::condition_from_name(wt::condition_name(c));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, c);
    }
}

TEST(WeatherCondition, UnknownNameIsNullopt) {
    // The loader throws on these — the parse contract is nullopt, never a
    // silent Clear.
    EXPECT_FALSE(wt::condition_from_name("storm").has_value());
    EXPECT_FALSE(wt::condition_from_name("").has_value());
    EXPECT_FALSE(wt::condition_from_name("Clear").has_value()); // case
}

TEST(WeatherDefaults, AreTheZeroChangeState) {
    // A default-constructed state is exactly the pre-Task-73 environment:
    // clear, no deck, 40 NM vis, calm bands, standard temperature, no
    // turbulence — every scale function returns 1.0 against it.
    const wt::WeatherState s{};
    EXPECT_EQ(s.condition, wt::WeatherCondition::Clear);
    EXPECT_DOUBLE_EQ(s.cloud_cover_tenths, 0.0);
    EXPECT_DOUBLE_EQ(s.visibility_nm, 40.0);
    EXPECT_DOUBLE_EQ(s.wind_low.speed_kts, 0.0);
    EXPECT_DOUBLE_EQ(s.turb_factor, 0.0);
    EXPECT_DOUBLE_EQ(wt::weather_visual_scale(s), 1.0);
    EXPECT_DOUBLE_EQ(
        wt::combined_visual_scale(s, wt::DaylightBand::Day), 1.0);
}

TEST(WeatherProfiles, OrderedSeverity) {
    // Monotonic severity: Clear is the best of everything, Inclement the
    // worst — the profiles are the evolution driver's targets and any
    // inversion would make a "worsening" front improve visibility.
    const auto& c = wt::profile_for(wt::WeatherCondition::Clear);
    const auto& h = wt::profile_for(wt::WeatherCondition::Hazy);
    const auto& i = wt::profile_for(wt::WeatherCondition::Inclement);
    EXPECT_GT(c.visibility_nm, h.visibility_nm);
    EXPECT_GT(h.visibility_nm, i.visibility_nm);
    EXPECT_LT(c.cloud_cover_tenths, h.cloud_cover_tenths);
    EXPECT_LT(h.cloud_cover_tenths, i.cloud_cover_tenths);
    EXPECT_LT(c.turb_factor, i.turb_factor);
    EXPECT_LT(c.wind_speed_kts, i.wind_speed_kts);
}

TEST(WeatherVisualScale, WeatherFloor) {
    // Below the floor, worse visibility does not keep degrading the
    // scale — 0.1 is the whiteout floor (close-in detection survives).
    wt::WeatherState s{};
    s.visibility_nm = 0.0;
    EXPECT_DOUBLE_EQ(wt::weather_visual_scale(s),
                     wt::kMinWeatherVisualScale);
    s.visibility_nm = 4.0;
    EXPECT_DOUBLE_EQ(wt::weather_visual_scale(s),
                     wt::kMinWeatherVisualScale);
    s.visibility_nm = 20.0;
    EXPECT_DOUBLE_EQ(wt::weather_visual_scale(s), 0.5);
}

TEST(WeatherVisualScale, DaylightFactors) {
    EXPECT_DOUBLE_EQ(wt::daylight_visual_scale(wt::DaylightBand::Day), 1.0);
    EXPECT_DOUBLE_EQ(
        wt::daylight_visual_scale(wt::DaylightBand::CivilTwilight), 0.5);
    EXPECT_DOUBLE_EQ(wt::daylight_visual_scale(wt::DaylightBand::Night),
                     0.1);
}

TEST(WeatherVisualScale, CompoundFloor) {
    // Night whiteout: the naive product (0.1 * 0.1 = 0.01) is floored at
    // 0.05 — close-in lights-only detection stays possible.
    wt::WeatherState s{};
    s.visibility_nm = 0.5;
    EXPECT_DOUBLE_EQ(
        wt::combined_visual_scale(s, wt::DaylightBand::Night), 0.05);
    // Clear night: the plain product applies (0.1).
    wt::WeatherState clear{};
    EXPECT_DOUBLE_EQ(
        wt::combined_visual_scale(clear, wt::DaylightBand::Night), 0.1);
}

TEST(WeatherWind, BandInterpolation) {
    // The documented band shape: low holds to 5k, blends to medium across
    // 5k..10k, medium to high across 10k..30k, high above.
    wt::WeatherState s{};
    s.wind_low = {270.0, 10.0};
    s.wind_medium = {270.0, 30.0};
    s.wind_high = {90.0, 50.0};
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 0.0), 10.0);
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 5000.0), 10.0);
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 7500.0), 20.0);
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 10000.0), 30.0);
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 20000.0), 40.0);
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 30000.0), 50.0);
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, 40000.0), 50.0);
}

TEST(WeatherWind, DirectionShortestArc) {
    // A 350 -> 10 deg blend goes through north (the 20-deg arc), not
    // backwards through 180.
    wt::WeatherState s{};
    s.wind_low = {350.0, 0.0};
    s.wind_medium = {10.0, 0.0};
    s.wind_high = {10.0, 0.0};
    EXPECT_DOUBLE_EQ(wt::wind_dir_at_ft(s, 7500.0), 0.0);
}

TEST(WeatherWind, NegativeAltitudeIsSurface) {
    wt::WeatherState s{};
    s.wind_low = {180.0, 12.0};
    s.wind_medium = {90.0, 20.0};
    s.wind_high = {45.0, 60.0};
    EXPECT_DOUBLE_EQ(wt::wind_speed_at_ft(s, -500.0), 12.0);
    EXPECT_DOUBLE_EQ(wt::wind_dir_at_ft(s, -500.0), 180.0);
}

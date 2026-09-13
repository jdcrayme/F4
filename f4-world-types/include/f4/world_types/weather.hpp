// f4-world-types/include/f4/world_types/weather.hpp
//
// Weather — the theater environment state (Task 73, Weather v1).
//
// The v1 weather surface, in FreeFalcon's own shape but theater-uniform:
// ONE WeatherState for the theater (FreeFalcon's WeatherClass keeps
// per-grid-cell cloud cover/level — GetCloudCover(x, y) — a v2 expansion
// this layout anticipates but does not pay for yet). What FreeFalcon's
// campaign weather carries (campaign/include/weather.h) and what v1 keeps:
//
//   condition          — the 3-state condition model: Clear / Hazy /
//                        Inclement (FreeFalcon's UpdateCondition states).
//   cloud_cover/base   — tenths-of-cover + the stratus base, in tenths and
//                        feet (FreeFalcon stratusBase/cumulusBase; v1 keeps
//                        ONE deck — cover + base — and drops the second
//                        stratus layer until a consumer needs it).
//   visibility         — horizontal visibility, NM (the detection surface).
//   wind               — three altitude bands (FreeFalcon windMin/windMed/
//                        windMax are exactly a low/medium/high band set),
//                        each a direction + speed pair; interpolated by
//                        altitude through wind_at_ft().
//   temperature        — surface temperature, Celsius (TemperatureAt).
//   turb_factor        — the turbulence multiplier FreeFalcon hands the
//                        flight model (0..1).
//
// The v1 CONSUMER is the detection model: visual range scales with the
// daylight band (day_night.hpp) and the weather state; the radar, IR
// flyout, and flight-model surfaces read later tranches (wind/turbulence
// land with their FM consumer; documented in CHANGES.md Task 73). The
// scaling functions below are the SHARED arithmetic — f4-ai never links
// f4-world-types, so hosts compute the scale here-defined and push the
// plain double through the brain-facing setters; f4-simulation's
// WeatherSystem is the reference host.
//
// Everything is deterministic pure data + pure functions; evolution lives
// with the host (f4-simulation's WeatherSystem), not here.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "f4/world_types/day_night.hpp"

namespace f4::world_types {

/// The FreeFalcon condition model (UpdateCondition's three states).
enum class WeatherCondition : std::uint8_t {
    Clear,
    Hazy,
    Inclement,
};

/// Condition name: "clear" | "hazy" | "inclement" (scenario/QC spelling).
[[nodiscard]] const char* condition_name(WeatherCondition c) noexcept;

/// Parse a condition name (case-sensitive, the scenario/QC spelling).
/// std::nullopt on anything else — the scenario loader throws on that,
/// it never silently defaults.
[[nodiscard]] std::optional<WeatherCondition>
condition_from_name(std::string_view name) noexcept;

/// One altitude band of the wind field: meteorological FROM direction
/// (degrees clockwise from north the wind blows FROM) and speed in knots.
struct WindBand {
    double dir_deg{0.0};
    double speed_kts{0.0};
};

/// The theater-uniform weather state. Defaults = a clear, still, standard
/// day — the "no weather configured" state every pre-Task-73 scenario ran
/// under (and the exact state the defaults-first discipline pins: a
/// scenario without a weather block behaves bit-identically to before).
struct WeatherState {
    WeatherCondition condition{WeatherCondition::Clear};
    double cloud_cover_tenths{0.0};   ///< 0..10 (FreeFalcon tenths)
    double cloud_base_ft{0.0};        ///< stratus deck base MSL (0 = none)
    double visibility_nm{40.0};       ///< horizontal visibility
    WindBand wind_low{};              ///< surface .. 10,000 ft
    WindBand wind_medium{};           ///< 10,000 .. 30,000 ft
    WindBand wind_high{};             ///< above 30,000 ft
    double temperature_c{15.0};       ///< surface temperature
    double turb_factor{0.0};          ///< 0..1 (FreeFalcon turbFactor)
};

/// The per-condition target profile the evolution driver steers toward
/// (FreeFalcon's UpdateCondition re-derives cloud/visibility/wind targets
/// per state; these are the v1 tuning values, documented in CHANGES.md).
struct WeatherProfile {
    double cloud_cover_tenths{0.0};
    double cloud_base_ft{0.0};
    double visibility_nm{40.0};
    double temperature_offset_c{0.0}; ///< surface-temp offset from standard
    double turb_factor{0.0};
    double wind_speed_kts{0.0};       ///< surface-band speed target
    double wind_high_kts{0.0};        ///< high-band speed target
};

/// The profile for a condition (the evolution driver's target set).
[[nodiscard]] const WeatherProfile& profile_for(WeatherCondition c) noexcept;

/// Clear-day reference visibility the weather visual scale normalizes
/// against (NM) — the profile's Clear visibility; documented constant.
inline constexpr double kReferenceVisibilityNm = 40.0;

/// Floor of the weather-only visual scale (a whiteout never zeroes the
/// eyeball — 0.1 keeps close-in detection possible, matching FreeFalcon's
/// own inclement-visual behavior).
inline constexpr double kMinWeatherVisualScale = 0.1;

/// Weather-only factor on the visual detection range: the clear-day
/// ceiling (the SensorFusionConfig max_visual_range_nm) times this is the
/// weather-limited visual range. Clear = exactly 1.0 (the zero-change
/// guarantee); worse visibility clamps at kMinWeatherVisualScale.
[[nodiscard]] double weather_visual_scale(const WeatherState& s) noexcept;

/// Daylight-band factor on the visual detection range: Day 1.0,
/// CivilTwilight 0.5, Night 0.1 (shapes at night are cockpit-light/lights
/// only — FreeFalcon's own night-visual behavior).
[[nodiscard]] double daylight_visual_scale(DaylightBand band) noexcept;

/// Combined visual factor: weather x daylight, floored at 0.05 (the
/// night-whiteout floor — both effects never compound below it).
[[nodiscard]] double combined_visual_scale(const WeatherState& s,
                                           DaylightBand band) noexcept;

/// Wind speed at an altitude MSL (feet), band-interpolated: the low band
/// holds to 10,000 ft, the high band from 30,000 ft, linear blend across
/// the medium band between them. FreeFalcon's WindSpeedInFeetPerSecond is
/// the per-position variant; v1 is position-independent (theater-uniform).
[[nodiscard]] double wind_speed_at_ft(const WeatherState& s,
                                      double altitude_ft) noexcept;

/// Wind direction at an altitude MSL (degrees FROM), band-interpolated
/// with the same band edges as wind_speed_at_ft. The interpolation is the
/// shortest angular arc (a 350 -> 10 deg shift blends through north).
[[nodiscard]] double wind_dir_at_ft(const WeatherState& s,
                                    double altitude_ft) noexcept;

} // namespace f4::world_types

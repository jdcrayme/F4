// f4-world-types/include/f4/world_types/day_night.hpp
//
// Day/night — the solar-geometry surface (Task 73, Weather v1).
//
// Pure, dependency-free astronomy for the campaign clock: given a time of
// day, a day of year, and the theater latitude, compute the sun's
// elevation and classify it into the daylight bands the detection model
// consumes. Engine-agnostic by construction — no sim, no campaign, no
// renderer types; just math over doubles.
//
// FreeFalcon reference:
//   - The legacy engine derives day/night from the campaign's time-of-day
//     (CampaignClass current_time) with the theater's fixed location; the
//     visual models read a light level from it.
//   - FreeFalcon's WeatherClass (campaign/include/weather.h) carries the
//     `weatherDay` counter — the day-of-year input this module takes.
//
// Model (v1) — the standard simplified solar position:
//   declination = -23.44 deg * cos(2*pi * (day_of_year + 10) / 365.24)
//   hour_angle  = 360 deg * (seconds_of_day / 86400) - 180 deg
//                 (solar noon at seconds_of_day = 43200)
//   elevation   = asin(sin(lat)sin(decl) + cos(lat)cos(decl)cos(hour_angle))
//
// Documented v1 approximations (deliberate, pinned by tests):
//   * Solar time: seconds_of_day is interpreted as LOCAL SOLAR time —
//     solar noon at 43200 s. No equation-of-time correction, no
//     longitude/timezone offset (Falcon 4's campaign clock is already a
//     zulu-like abstraction; the correction is sub-degree per theater).
//   * Day of year drives the declination only — no leap/365.25 drift.
//   * Sunrise/sunset are derived, not tabulated: the band boundaries
//     (below) are the classification, there is no separate sun-rise time
//     function to disagree with.
//
// Bands (detection-model contract — see weather.hpp's visual scaling):
//   Day            elevation >  0 deg
//   CivilTwilight  -6 deg <= elevation <= 0 deg (sun on/under the horizon,
//                  usable light: shapes readable, colors not)
//   Night          elevation < -6 deg (civil dusk end)
//
// The Korea theater default latitude (37.5 N) matches the campaign's
// geography; a host flying another theater passes its own.

#pragma once

#include <cstdint>

namespace f4::world_types {

/// The three daylight bands the detection model distinguishes (v1).
/// Detection-relevant, not astronomical hair-splitting: nautical and
/// astronomical twilight are folded into Night (no readable ground
/// detail for a pilot's eyeball in either).
enum class DaylightBand : std::uint8_t {
    Night,
    CivilTwilight,
    Day,
};

/// Band name: "night" | "civil_twilight" | "day" (scenario/QC spelling).
[[nodiscard]] const char* band_name(DaylightBand band) noexcept;

/// Korea-theater default latitude (degrees north; the campaign's Korea
/// map spans roughly 34..43 N — 37.5 is the mid-point the v1 model
/// defaults to).
inline constexpr double kDefaultTheaterLatitudeDeg = 37.5;

/// Solar elevation above the horizon, degrees, for the simplified model
/// above. seconds_of_day is LOCAL SOLAR time ([0, 86400)); values outside
/// wrap modulo 86400 (a host may pass raw campaign seconds). day_of_year
/// is 1-based (Jan 1 = 1); values outside [1, 366] clamp. Negative
/// latitudes are southern-hemisphere-correct (cos/sin handle the sign).
[[nodiscard]] double solar_elevation_deg(double seconds_of_day,
                                         int day_of_year,
                                         double latitude_deg);

/// Solar declination for day_of_year (degrees; -23.44 .. +23.44), exposed
/// for tests and diagnostics — the elevation function uses it internally.
[[nodiscard]] double solar_declination_deg(int day_of_year);

/// Classify an elevation into the detection bands (contract above).
[[nodiscard]] DaylightBand daylight_band(double elevation_deg) noexcept;

/// Convenience: elevation + classification in one call.
[[nodiscard]] DaylightBand daylight_band_at(double seconds_of_day,
                                            int day_of_year,
                                            double latitude_deg);

} // namespace f4::world_types

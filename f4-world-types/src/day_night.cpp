// f4-world-types/src/day_night.cpp
//
// Day/night implementation — see day_night.hpp for the model contract.

#include "f4/world_types/day_night.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace f4::world_types {

namespace {

constexpr double kSecondsPerDay = 86400.0;
// Earth's axial tilt (obliquity of the ecliptic), degrees — the standard
// simplified-position constant (23.4397 rounded to the model's precision).
constexpr double kAxialTiltDeg = 23.44;
// Mean tropical year length the day-of-year phase uses (the "+10" shifts
// the cosine so day 1 sits just past the solstice phase, the textbook
// Spencer-formula approximation).
constexpr double kDaysPerYear = 365.24;
constexpr double kCivilDuskDeg = -6.0;

double wrap_seconds(double seconds_of_day) noexcept {
    const double wrapped = std::fmod(seconds_of_day, kSecondsPerDay);
    return wrapped < 0.0 ? wrapped + kSecondsPerDay : wrapped;
}

double deg2rad(double deg) noexcept { return deg * std::numbers::pi / 180.0; }

} // namespace

const char* band_name(DaylightBand band) noexcept {
    switch (band) {
    case DaylightBand::Night:
        return "night";
    case DaylightBand::CivilTwilight:
        return "civil_twilight";
    case DaylightBand::Day:
        return "day";
    }
    return "night"; // unreachable; silences -Wreturn-type on exotic ABIs
}

double solar_declination_deg(int day_of_year) {
    const int clamped = std::clamp(day_of_year, 1, 366);
    const double phase =
        2.0 * std::numbers::pi * (static_cast<double>(clamped) + 10.0) /
        kDaysPerYear;
    return -kAxialTiltDeg * std::cos(phase);
}

double solar_elevation_deg(double seconds_of_day, int day_of_year,
                           double latitude_deg) {
    const double t = wrap_seconds(seconds_of_day);
    const double decl = deg2rad(solar_declination_deg(day_of_year));
    // Hour angle: solar noon (43200 s) at 0, advancing 15 deg/hour westward.
    const double hour_angle =
        deg2rad(360.0 * (t / kSecondsPerDay) - 180.0);
    const double lat = deg2rad(latitude_deg);
    const double sin_alt = std::sin(lat) * std::sin(decl) +
                           std::cos(lat) * std::cos(decl) *
                               std::cos(hour_angle);
    // Clamp guards the asin domain against rounding at the poles.
    return std::asin(std::clamp(sin_alt, -1.0, 1.0)) * 180.0 /
           std::numbers::pi;
}

DaylightBand daylight_band(double elevation_deg) noexcept {
    if (elevation_deg > 0.0) return DaylightBand::Day;
    if (elevation_deg >= kCivilDuskDeg) return DaylightBand::CivilTwilight;
    return DaylightBand::Night;
}

DaylightBand daylight_band_at(double seconds_of_day, int day_of_year,
                              double latitude_deg) {
    return daylight_band(
        solar_elevation_deg(seconds_of_day, day_of_year, latitude_deg));
}

} // namespace f4::world_types

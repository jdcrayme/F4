// f4-world-types/src/weather.cpp
//
// Weather implementation — see weather.hpp for the model contract.

#include "f4/world_types/weather.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

namespace f4::world_types {

const char* condition_name(WeatherCondition c) noexcept {
    switch (c) {
    case WeatherCondition::Clear:
        return "clear";
    case WeatherCondition::Hazy:
        return "hazy";
    case WeatherCondition::Inclement:
        return "inclement";
    }
    return "clear"; // unreachable; silences -Wreturn-type on exotic ABIs
}

std::optional<WeatherCondition>
condition_from_name(std::string_view name) noexcept {
    if (name == "clear") return WeatherCondition::Clear;
    if (name == "hazy") return WeatherCondition::Hazy;
    if (name == "inclement") return WeatherCondition::Inclement;
    return std::nullopt;
}

const WeatherProfile& profile_for(WeatherCondition c) noexcept {
    // v1 tuning values (CHANGES.md Task 73). Shapes:
    //   Clear      — 40 NM vis, no deck, calm; the reference day.
    //   Hazy       — 12 NM vis, thin high deck, light wind; the "you can
    //                fight but you see it late" day.
    //   Inclement  — 3 NM vis, 3,000 ft deck at 8/10, gusty bands, the
    //                turbulence and the cold offset.
    // The medium/high wind bands scale off the surface target (the
    // boundary-layer shape FreeFalcon's windMin/Med/Max set encodes).
    static const WeatherProfile clear_p{};
    static const WeatherProfile hazy_p{
        .cloud_cover_tenths = 3.0,
        .cloud_base_ft = 18000.0,
        .visibility_nm = 12.0,
        .temperature_offset_c = -2.0,
        .turb_factor = 0.1,
        .wind_speed_kts = 8.0,
        .wind_high_kts = 40.0,
    };
    static const WeatherProfile inclement_p{
        .cloud_cover_tenths = 8.0,
        .cloud_base_ft = 3000.0,
        .visibility_nm = 3.0,
        .temperature_offset_c = -6.0,
        .turb_factor = 0.7,
        .wind_speed_kts = 20.0,
        .wind_high_kts = 80.0,
    };
    switch (c) {
    case WeatherCondition::Clear:
        return clear_p;
    case WeatherCondition::Hazy:
        return hazy_p;
    case WeatherCondition::Inclement:
        return inclement_p;
    }
    return clear_p; // unreachable; silences -Wreturn-type on exotic ABIs
}

double weather_visual_scale(const WeatherState& s) noexcept {
    if (s.visibility_nm >= kReferenceVisibilityNm) return 1.0;
    const double raw = s.visibility_nm / kReferenceVisibilityNm;
    return std::max(raw, kMinWeatherVisualScale);
}

double daylight_visual_scale(DaylightBand band) noexcept {
    switch (band) {
    case DaylightBand::Day:
        return 1.0;
    case DaylightBand::CivilTwilight:
        return 0.5;
    case DaylightBand::Night:
        return 0.1;
    }
    return 1.0; // unreachable; silences -Wreturn-type on exotic ABIs
}

double combined_visual_scale(const WeatherState& s,
                             DaylightBand band) noexcept {
    // The compound floor: night-whiteout is 0.05, not 0.01 — the
    // close-in lights-only detection stays possible, matching the two
    // individual floors' spirit rather than their naive product.
    return std::max(weather_visual_scale(s) * daylight_visual_scale(band),
                    0.05);
}

namespace {

/// Band edges (feet MSL) — weather.hpp's documented contract.
constexpr double kLowBandTopFt = 10000.0;
constexpr double kHighBandBaseFt = 30000.0;

double shortest_arc_blend(double from_deg, double to_deg, double t) noexcept {
    const double delta =
        std::fmod(to_deg - from_deg + 540.0, 360.0) - 180.0;
    // Wrap to [0, 360): a 350 -> 10 blend passes through 360/0, and the
    // interpolated direction must land ON 0, not on 360 (which is the
    // same bearing but breaks exact-equality contracts).
    const double raw = from_deg + delta * t;
    return std::fmod(raw + 360.0, 360.0);
}

} // namespace

double wind_speed_at_ft(const WeatherState& s, double altitude_ft) noexcept {
    const double lo = s.wind_low.speed_kts;
    const double mid = s.wind_medium.speed_kts;
    const double hi = s.wind_high.speed_kts;
    if (altitude_ft <= 0.0) return lo;
    if (altitude_ft >= kHighBandBaseFt) return hi;
    if (altitude_ft <= kLowBandTopFt) {
        // Surface band holds to 10k (the boundary layer), then blends
        // toward the medium band across the low band's top half.
        constexpr double kBlendStartFt = 5000.0;
        if (altitude_ft <= kBlendStartFt) return lo;
        const double t =
            (altitude_ft - kBlendStartFt) / (kLowBandTopFt - kBlendStartFt);
        return lo + (mid - lo) * t;
    }
    const double t = (altitude_ft - kLowBandTopFt) /
                     (kHighBandBaseFt - kLowBandTopFt);
    return mid + (hi - mid) * t;
}

double wind_dir_at_ft(const WeatherState& s, double altitude_ft) noexcept {
    const double lo = s.wind_low.dir_deg;
    const double mid = s.wind_medium.dir_deg;
    const double hi = s.wind_high.dir_deg;
    if (altitude_ft <= 0.0) return lo;
    if (altitude_ft >= kHighBandBaseFt) return hi;
    if (altitude_ft <= kLowBandTopFt) {
        constexpr double kBlendStartFt = 5000.0;
        if (altitude_ft <= kBlendStartFt) return lo;
        const double t =
            (altitude_ft - kBlendStartFt) / (kLowBandTopFt - kBlendStartFt);
        return shortest_arc_blend(lo, mid, t);
    }
    const double t = (altitude_ft - kLowBandTopFt) /
                     (kHighBandBaseFt - kLowBandTopFt);
    return shortest_arc_blend(mid, hi, t);
}

} // namespace f4::world_types

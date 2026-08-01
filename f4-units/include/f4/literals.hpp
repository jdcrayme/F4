#pragma once

#include "quantity.hpp"
#include "units.hpp"
#include "aviation.hpp"

/// User-defined literal operators for f4-units.
///
/// Usage:
///   using namespace f4::literals;
///   auto dist  = 1000.0_ft;
///   auto speed = 250.0_kn;
///   auto temp  = 15.0_C;
///   auto mach  = 0.85_mach;
///   auto cas   = 450.0_kcas;
namespace f4::literals {

// --- Length ---
constexpr auto operator""_mm(long double v) { return Quantity<Millimeters>(static_cast<double>(v)); }
constexpr auto operator""_cm(long double v) { return Quantity<Centimeters>(static_cast<double>(v)); }
constexpr auto operator""_m(long double v)  { return Quantity<Meters>(static_cast<double>(v)); }
constexpr auto operator""_km(long double v) { return Quantity<Kilometers>(static_cast<double>(v)); }
constexpr auto operator""_in(long double v) { return Quantity<Inches>(static_cast<double>(v)); }
constexpr auto operator""_ft(long double v) { return Quantity<Feet>(static_cast<double>(v)); }
constexpr auto operator""_mi(long double v) { return Quantity<Miles>(static_cast<double>(v)); }
constexpr auto operator""_nm(long double v) { return Quantity<NauticalMiles>(static_cast<double>(v)); }

// --- Mass ---
constexpr auto operator""_g(long double v)   { return Quantity<Grams>(static_cast<double>(v)); }
constexpr auto operator""_kg(long double v)  { return Quantity<Kilograms>(static_cast<double>(v)); }
constexpr auto operator""_lb(long double v)  { return Quantity<Pounds>(static_cast<double>(v)); }
constexpr auto operator""_slug(long double v) { return Quantity<Slugs>(static_cast<double>(v)); }

// --- Time ---
constexpr auto operator""_ms(long double v)  { return Quantity<Milliseconds>(static_cast<double>(v)); }
constexpr auto operator""_s(long double v)   { return Quantity<Seconds>(static_cast<double>(v)); }
constexpr auto operator""_min(long double v) { return Quantity<Minutes>(static_cast<double>(v)); }
constexpr auto operator""_hr(long double v)  { return Quantity<Hours>(static_cast<double>(v)); }

// --- Temperature ---
constexpr auto operator""_K(long double v)   { return Quantity<Kelvin>(static_cast<double>(v)); }
constexpr auto operator""_C(long double v)   { return Quantity<Celsius>(static_cast<double>(v)); }
constexpr auto operator""_F(long double v)   { return Quantity<Fahrenheit>(static_cast<double>(v)); }
constexpr auto operator""_R(long double v)   { return Quantity<Rankine>(static_cast<double>(v)); }

// --- Angle ---
constexpr auto operator""_rad(long double v) { return Quantity<Radians>(static_cast<double>(v)); }
constexpr auto operator""_deg(long double v) { return Quantity<Degrees>(static_cast<double>(v)); }
constexpr auto operator""_mil(long double v) { return Quantity<Mils>(static_cast<double>(v)); }

// --- Speed ---
constexpr auto operator""_mps(long double v) { return Quantity<MetersPerSecond>(static_cast<double>(v)); }
constexpr auto operator""_fps(long double v) { return Quantity<FeetPerSecond>(static_cast<double>(v)); }
constexpr auto operator""_kph(long double v) { return Quantity<KilometersPerHour>(static_cast<double>(v)); }
constexpr auto operator""_kn(long double v)  { return Quantity<Knots>(static_cast<double>(v)); }
constexpr auto operator""_mph(long double v) { return Quantity<MilesPerHour>(static_cast<double>(v)); }

// --- Pressure ---
constexpr auto operator""_Pa(long double v)  { return Quantity<Pascals>(static_cast<double>(v)); }
constexpr auto operator""_hPa(long double v) { return Quantity<Hectopascals>(static_cast<double>(v)); }
constexpr auto operator""_psi(long double v) { return Quantity<PSI>(static_cast<double>(v)); }
constexpr auto operator""_psf(long double v) { return Quantity<PSF>(static_cast<double>(v)); }
constexpr auto operator""_inHg(long double v) { return Quantity<InHg>(static_cast<double>(v)); }
constexpr auto operator""_atm(long double v) { return Quantity<Atmospheres>(static_cast<double>(v)); }

// --- Force ---
constexpr auto operator""_N(long double v)   { return Quantity<Newtons>(static_cast<double>(v)); }
constexpr auto operator""_kN(long double v)  { return Quantity<KiloNewtons>(static_cast<double>(v)); }
constexpr auto operator""_lbf(long double v) { return Quantity<PoundForce>(static_cast<double>(v)); }

// --- Area ---
constexpr auto operator""_m2(long double v)  { return Quantity<SquareMeters>(static_cast<double>(v)); }
constexpr auto operator""_ft2(long double v) { return Quantity<SquareFeet>(static_cast<double>(v)); }

// --- Density ---
constexpr auto operator""_kgm3(long double v) { return Quantity<KilogramsPerCubicMeter>(static_cast<double>(v)); }

// --- Aviation-specific (phantom dimensions) ---
constexpr auto operator""_mach(long double v) { return Quantity<MachUnit>(static_cast<double>(v)); }
constexpr auto operator""_kcas(long double v) { return Quantity<CASKnots>(static_cast<double>(v)); }

} // namespace f4::literals
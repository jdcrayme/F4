/**
 * @file f4_units.hpp
 * @brief Master include for the f4-units library.
 *
 * f4-units: Compile-time physical quantity types for flight simulation.
 * A header-only C++20 library providing type-safe dimensional analysis
 * with zero runtime overhead.
 *
 * Quick start:
 * @code
 *   #include <f4/f4_units.hpp>
 *   using namespace f4::literals;
 *
 *   auto dist   = 1000.0_ft;
 *   auto meters = dist.to<f4::Meters>();   // 304.8 m
 *   auto speed  = 250.0_kn + 50.0_mps;    // heterogeneous addition
 *   auto mach   = 0.85_mach;                // typed Mach number
 *   auto cas    = 450.0_kcas;               // typed CAS (not a Speed!)
 * @endcode
 *
 * Design principles:
 *   - All conversions are compile-time constant expressions (zero runtime cost)
 *   - Cross-dimension multiplication/division produces correct result dimensions
 *   - Phantom dimensions (CAS, Mach) prevent accidental mixing
 *   - Temperature uses affine conversions (not simple ratios)
 *   - Context-dependent conversions (CAS<->TAS, Mach<->speed) are
 *     intentionally EXCLUDED and must be provided by IAtmosphereProvider
 */

#pragma once

#include "dimension.hpp"
#include "unit.hpp"
#include "dimensions.hpp"
#include "units.hpp"
#include "aviation.hpp"
#include "quantity.hpp"
#include "derived.hpp"
#include "literals.hpp"
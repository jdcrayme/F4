#pragma once

#include "dimension.hpp"

namespace f4 {

/// Unit tag: associates a dimension with a conversion factor to the SI base unit.
///
/// For most units, Offset = 0 and conversion is a simple ratio:
///   value_in_base = value * ToBase
///
/// For temperature units, Offset encodes the affine transformation:
///   value_in_base = value * ToBase + Offset
///
/// Conversion from unit A to unit B is:
///   B_value = A_value * (A.ToBase / B.ToBase) + (A.Offset - B.Offset) / B.ToBase
///
/// Examples:
///   Meters  = Unit<LengthDim, 1.0>              // 1 m = 1 m (base)
///   Feet    = Unit<LengthDim, 0.3048>           // 1 ft = 0.3048 m
///   Kelvin  = Unit<TemperatureDim, 1.0, 0.0>   // K = 1.0*K + 0
///   Celsius = Unit<TemperatureDim, 1.0, 273.15> // K = 1.0*C + 273.15
template<typename Dim, double ToBase, double Offset = 0.0>
struct Unit {
    using dimension = Dim;
    static constexpr double to_base = ToBase;
    static constexpr double offset = Offset;
};

/// Convenience alias for the SI base unit of any dimension (factor = 1.0, no offset).
template<typename Dim>
using BaseUnit = Unit<Dim, 1.0, 0.0>;

} // namespace f4

# f4-units

Compile-time physical quantity types for flight simulation.
A header-only C++20 library providing type-safe dimensional analysis
with zero runtime overhead.

## Quick Start

```cpp
#include <f4/f4_units.hpp>
using namespace f4::literals;

auto dist   = 1000.0_ft;
auto meters = dist.to<f4::Meters>();   // 304.8 m
auto speed  = 250.0_kn + 50.0_mps;    // heterogeneous addition
auto mach   = 0.85_mach;                // typed Mach number
auto cas    = 450.0_kcas;               // typed CAS (not a Speed!)
```

## Building

```bash
cmake -B build -S .
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake 3.20+ and a C++20 compiler (MSVC 19.28+, GCC 10+, Clang 12+).
Tests use [Google Test](https://github.com/google/googletest) v1.14.0,
fetched automatically via CMake's FetchContent.

## Design Principles

- **Zero runtime overhead** — all conversions are compile-time constant expressions.
- **Cross-dimension arithmetic** produces correct result dimensions:
  `Speed * Time = Length`, `Force / Area = Pressure`.
- **Phantom dimensions** (CAS, Mach) prevent accidental mixing with physical
  quantities. CAS is not a Speed; Mach is not Dimensionless.
- **Temperature uses affine conversions** (offset-aware), not simple ratios.
- **Context-dependent conversions** (CAS to TAS, Mach to speed) are intentionally
  excluded and belong in a future `IAtmosphereProvider`.

## Supported Dimensions

| Dimension | Units |
-----------|-------|
| Length | mm, cm, m, km, in, ft, mi, nmi |
| Mass | g, kg, oz, lb, slug |
| Time | ms, s, min, hr |
| Temperature | K, C, F, R |
| Angle | rad, deg, arcmin, arcsec, mil |
| Speed | m/s, ft/s, km/h, kn, mph |
| Acceleration | m/s^2, ft/s^2, g |
| Force | N, kN, lbf, kgf |
| Pressure | Pa, hPa, mbar, kPa, psi, psf, atm, inHg, mmHg |
| Area | m^2, ft^2, km^2, nmi^2, mi^2 |
| Volume | m^3, ft^3, L, US gal, imp gal |
| Density | kg/m^3, slug/ft^3 |
| Energy | J, kJ, BTU, ft-lbf |
| Power | W, kW, hp, lb-ft/s |
| Frequency | Hz, kHz, MHz |
| Mass Flow Rate | kg/s, kg/hr, lb/hr |
| Torque | N-m, lb-ft, in-lb |
| **CAS** (phantom) | knots, m/s |
| **Mach** (phantom) | (unitless) |

## Library Structure

```
f4-units/
  include/f4/
    f4_units.hpp   Master include
    dimension.hpp   Dimension template and comparison traits
    unit.hpp        Unit tag with conversion factor and offset
    dimensions.hpp  Concrete dimension aliases (LengthDim, SpeedDim, ...)
    units.hpp       Concrete unit aliases (Meters, Knots, Pascals, ...)
    aviation.hpp    Phantom units: CAS (CASKnots, CASMPS), Mach (MachUnit)
    quantity.hpp    Quantity<DimTag, UnitTag, Rep> class and free operators
    derived.hpp     speed_of_sound(), dynamic_pressure(), mach_number(), ...
    literals.hpp    User-defined literals (_m, _ft, _kn, _mach, _kcas, ...)
  tests/
    test_dimension.cpp    Compile-time dimension trait tests
    test_conversions.cpp  Unit conversion roundtrip tests
    test_arithmetic.cpp   Arithmetic, qpow, qsqrt, comparisons, literals
    test_temperature.cpp  Affine temperature conversion tests
    test_aviation.cpp     CAS and Mach type-safety tests
    test_derived.cpp      Derived quantities and ISA reference values
```

## License

See LICENSE file in the repository root.

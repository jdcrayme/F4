#include <f4/f4_units.hpp>
#include <iostream>
#include <cmath>
#include <cstdlib>

using namespace f4;
using namespace f4::literals;

static int g_pass = 0;
static int g_fail = 0;

void check(bool ok, const char* msg) {
    if (ok) { g_pass++; }
    else { g_fail++; std::cerr << "  FAIL: " << msg << "\n"; }
}

void approx(double actual, double expected, double margin, const char* msg) {
    if (std::abs(actual - expected) <= margin) { g_pass++; }
    else { g_fail++; std::cerr << "  FAIL: " << msg << " got " << actual << " expected ~" << expected << "\n"; }
}

// ========== DIMENSION ==========
void test_dimension() {
    std::cout << "\n=== dimension ===\n";

    // Compile-time checks (if these fail, it won't compile)
    static_assert(same_dimension_v<LengthDim, LengthDim>);
    static_assert(same_dimension_v<SpeedDim, SpeedDim>);
    static_assert(!same_dimension_v<LengthDim, SpeedDim>);
    static_assert(!same_dimension_v<CASDim, SpeedDim>);
    static_assert(!same_dimension_v<CASDim, PressureDim>);
    static_assert(!same_dimension_v<CASDim, Dimensionless>);
    static_assert(!same_dimension_v<MachDim, Dimensionless>);
    static_assert(!same_dimension_v<CASDim, MachDim>);
    static_assert(same_dimension_v<CASDim, CASDim>);
    static_assert(same_dimension_v<MachDim, MachDim>);
    check(true, "static asserts");

    // dim_multiply
    using R1 = dim_multiply<SpeedDim, TimeDim>;
    static_assert(R1::length == 1 && R1::time == 0);
    static_assert(same_dimension_v<R1, LengthDim>);
    check(same_dimension_v<R1, LengthDim>, "Speed*Time=Length");

    // dim_divide
    using R2 = dim_divide<ForceDim, AreaDim>;
    static_assert(R2::length == -1 && R2::mass == 1 && R2::time == -2);
    static_assert(same_dimension_v<R2, PressureDim>);
    check(same_dimension_v<R2, PressureDim>, "Force/Area=Pressure");

    // dim_invert
    using R3 = dim_invert<SpeedDim>;
    check(R3::length == -1 && R3::time == 1, "invert Speed");

    // physical_dimension
    check(physical_dimension<LengthDim>, "Length is physical");
    check(!physical_dimension<CASDim>, "CAS is not physical");
    check(!physical_dimension<MachDim>, "Mach is not physical");

    // density * speed^2 = pressure
    using SpdSq = dim_multiply<SpeedDim, SpeedDim>;
    using DynP = dim_multiply<DensityDim, SpdSq>;
    check(same_dimension_v<DynP, PressureDim>, "density*speed^2=pressure");
}

// ========== CONVERSIONS ==========
void test_conversions() {
    std::cout << "\n=== conversions ===\n";

    approx(Quantity<Feet>(1000.0).to<Meters>().value(), 304.8, 1e-9, "ft->m");
    approx(Quantity<NauticalMiles>(1.0).to<Meters>().value(), 1852.0, 1e-6, "nm->m");
    approx(Quantity<Meters>(100.0).to<Feet>().to<Meters>().value(), 100.0, 1e-9, "m->ft->m roundtrip");
    approx(Quantity<Kilometers>(1.0).to<Miles>().value(), 0.621371, 1e-5, "km->mi");

    approx(Quantity<Knots>(100.0).to<MetersPerSecond>().value(), 51.4444, 1e-3, "kn->mps");
    approx(Quantity<Knots>(100.0).to<MilesPerHour>().value(), 115.078, 1e-3, "kn->mph");
    approx(Quantity<Knots>(250.0).to<MetersPerSecond>().to<Knots>().value(), 250.0, 1e-9, "kn->mps->kn roundtrip");

    approx(Quantity<PSI>(1.0).to<Pascals>().value(), 6894.757, 1e-3, "psi->Pa");
    approx(Quantity<Atmospheres>(1.0).to<Pascals>().value(), 101325.0, 1e-6, "atm->Pa");
    approx(Quantity<InHg>(29.92).to<Pascals>().value(), 101325.0, 10.0, "29.92inHg=1atm");
    approx(Quantity<PSF>(144.0).to<PSI>().value(), 1.0, 1e-9, "144psf=1psi");

    approx(Quantity<Pounds>(1.0).to<Kilograms>().value(), 0.45359237, 1e-9, "lb->kg");
    approx(Quantity<Slugs>(1.0).to<Kilograms>().value(), 14.593903, 1e-6, "slug->kg");

    approx(Quantity<Hours>(1.0).to<Seconds>().value(), 3600.0, 1e-9, "hr->s");

    approx(Quantity<Degrees>(180.0).to<Radians>().value(), 3.14159265358979, 1e-9, "180deg=pi");
    approx(Quantity<Degrees>(360.0).to<Radians>().value(), 6.28318530717959, 1e-9, "360deg=2pi");
    approx(Quantity<Mils>(6400.0).to<Degrees>().value(), 360.0, 1e-6, "6400mil=360deg");

    approx(Quantity<SquareFeet>(1.0).to<SquareMeters>().value(), 0.09290304, 1e-9, "ft2->m2");

    approx(Quantity<PoundForce>(1.0).to<Newtons>().value(), 4.44822, 1e-5, "lbf->N");
}

// ========== TEMPERATURE ==========
void test_temperature() {
    std::cout << "\n=== temperature ===\n";

    approx(Quantity<Celsius>(0.0).to<Kelvin>().value(), 273.15, 1e-9, "0C=273.15K");
    approx(Quantity<Kelvin>(273.15).to<Celsius>().value(), 0.0, 1e-9, "273.15K=0C");
    approx(Quantity<Fahrenheit>(32.0).to<Kelvin>().value(), 273.15, 1e-6, "32F=273.15K");
    approx(Quantity<Fahrenheit>(212.0).to<Kelvin>().value(), 373.15, 1e-6, "212F=373.15K");
    approx(Quantity<Kelvin>(273.15).to<Fahrenheit>().value(), 32.0, 1e-6, "273.15K=32F");
    approx(Quantity<Fahrenheit>(100.0).to<Celsius>().value(), 37.7778, 1e-4, "100F=37.7778C");
    approx(Quantity<Celsius>(100.0).to<Fahrenheit>().value(), 212.0, 1e-6, "100C=212F");
    approx(Quantity<Fahrenheit>(-40.0).to<Celsius>().value(), -40.0, 1e-6, "-40F=-40C");
    approx(Quantity<Rankine>(491.67).to<Kelvin>().value(), 273.15, 1e-6, "491.67R=273.15K");
    approx(Quantity<Fahrenheit>(32.0).to<Rankine>().value(), 491.67, 1e-3, "32F=491.67R");
    approx(Quantity<Rankine>(0.0).to<Fahrenheit>().value(), -459.67, 1e-3, "0R=-459.67F");

    // Roundtrips
    approx(Quantity<Celsius>(-40.0).to<Kelvin>().to<Celsius>().value(), -40.0, 1e-9, "C->K->C");
    approx(Quantity<Fahrenheit>(98.6).to<Kelvin>().to<Fahrenheit>().value(), 98.6, 1e-6, "F->K->F");
    approx(Quantity<Celsius>(20.0).to<Rankine>().to<Celsius>().value(), 20.0, 1e-6, "C->R->C");

    // Heterogeneous comparison
    check(Quantity<Kelvin>(273.15) == Quantity<Celsius>(0.0), "273.15K == 0C");
    check(Quantity<Kelvin>(273.15) > Quantity<Celsius>(-1.0), "273.15K > -1C");
    check(Quantity<Kelvin>(273.15) < Quantity<Celsius>(1.0), "273.15K < 1C");
}

// ========== ARITHMETIC ==========
void test_arithmetic() {
    std::cout << "\n=== arithmetic ===\n";

    // Same-type
    approx((Quantity<Meters>(100.0) + Quantity<Meters>(50.0)).value(), 150.0, 1e-9, "m+m");
    approx((Quantity<Meters>(100.0) - Quantity<Meters>(30.0)).value(), 70.0, 1e-9, "m-m");
    approx((-Quantity<Meters>(50.0)).value(), -50.0, 1e-9, "-m");

    // Scalar
    approx((100.0_m * 2.0).value(), 200.0, 1e-9, "m*2");
    approx((3.0 * 100.0_m).value(), 300.0, 1e-9, "3*m");
    approx((100.0_m / 4.0).value(), 25.0, 1e-9, "m/4");

    // Compound
    auto d = Quantity<Meters>(100.0);
    d += Quantity<Meters>(50.0);
    d *= 2.0;
    d /= 5.0;
    approx(d.value(), 60.0, 1e-9, "compound assignment");

    // Heterogeneous +
    auto r1 = Quantity<Meters>(100.0) + Quantity<Feet>(100.0);
    approx(r1.value(), 130.48, 1e-6, "m+ft");
    auto r2 = 200.0_kn + 50.0_mps;
    approx(r2.value(), 297.192, 0.01, "kn+mps");

    // Cross-dimension: Speed*Time=Length
    auto dist = 100.0_kn * 2.0_hr;
    approx(dist.to<Kilometers>().value(), 370.4, 0.01, "kn*hr=km");

    // Mass*Accel=Force
    auto f = Quantity<Kilograms>(1000.0) * Quantity<MetersPerSecondSquared>(9.81);
    approx(f.to<Newtons>().value(), 9810.0, 1e-6, "kg*m/s2=N");

    // Force/Area=Pressure
    auto p = 10000.0_N / Quantity<SquareMeters>(1.0);
    approx(p.to<Pascals>().value(), 10000.0, 1e-6, "N/m2=Pa");
    approx(p.to<PSI>().value(), 1.45038, 1e-3, "Pa=psi");

    // Length*Length=Area
    auto a = 10.0_m * 10.0_m;
    approx(a.to<SquareMeters>().value(), 100.0, 1e-9, "m*m=m2");

    // Length/Time=Speed
    auto s = 100.0_m / Quantity<Seconds>(10.0);
    approx(s.to<MetersPerSecond>().value(), 10.0, 1e-9, "m/s=m/s");

    // qpow & qsqrt
    approx(qpow<2>(10.0_m).to<SquareMeters>().value(), 100.0, 1e-9, "m^2=100m2");
    approx(qpow<3>(5.0_m).to<CubicMeters>().value(), 125.0, 1e-9, "m^3=125m3");
    approx(qpow<-1>(10.0_m).value(), 0.1, 1e-9, "m^-1=0.1");
    approx(qsqrt(Quantity<SquareMeters>(100.0)).to<Meters>().value(), 10.0, 1e-9, "sqrt(m2)=m");

    // Comparisons
    auto m1 = 100.0_m;
    check(m1 == Quantity<Meters>(100.0), "m==m");
    check(m1 != Quantity<Meters>(200.0), "m!=m");
    check(m1 < Quantity<Meters>(200.0), "m<m");
    check(m1 > Quantity<Meters>(50.0), "m>m");
    check(100.0_m > 300.0_ft, "100m>300ft");
    check(100.0_m < 400.0_ft, "100m<400ft");

    // Literals
    approx((5.0_m + 3.0_ft).value(), 5.9144, 1e-9, "5m+3ft");
    auto kn_val = 250.0_kn;
    approx(kn_val.to<MetersPerSecond>().value(), 128.611, 1e-3, "250kn");
    auto psi_val = 14.7_psi;
    approx(psi_val.to<Pascals>().value(), 101353.0, 1.0, "14.7psi");
    auto a2 = 2.0_m * 3.0_m;
    approx(a2.to<SquareMeters>().value(), 6.0, 1e-9, "2m*3m");

    // in() / in_base()
    auto ft_val = 1000.0_ft;
    approx(ft_val.in<Meters>(), 304.8, 1e-9, "in<meters>()");
    approx(ft_val.in_base(), 304.8, 1e-9, "in_base()");
}

// ========== AVIATION ==========
void test_aviation() {
    std::cout << "\n=== aviation ===\n";

    // CAS
    approx(Quantity<CASKnots>(450.0).value(), 450.0, 1e-9, "CAS value");
    auto cas_val = 450.0_kcas;
    approx(cas_val.value(), 450.0, 1e-9, "CAS literal");
    approx(Quantity<CASKnots>(100.0).to<CASMetersPerSecond>().value(), 51.4444, 1e-6, "CAS kn->mps");
    approx(Quantity<CASKnots>(350.0).to<CASMetersPerSecond>().to<CASKnots>().value(), 350.0, 1e-9, "CAS roundtrip");
    approx((400.0_kcas * 2.0).value(), 800.0, 1e-9, "CAS*2");

    // Mach
    approx(Quantity<MachUnit>(0.85).value(), 0.85, 1e-9, "Mach value");
    auto mach_val = 0.85_mach;
    approx(mach_val.value(), 0.85, 1e-9, "Mach literal");
    approx((0.8_mach + Quantity<MachUnit>(0.05)).value(), 0.85, 1e-9, "Mach+");

    // Type safety (static_asserts exercised)
    check(!same_dimension_v<CASDim, MachDim>, "CAS!=Mach");
    check(400.0_kcas < 450.0_kcas, "CAS comparison");
    check(0.8_mach < 1.2_mach, "Mach comparison");
}

// ========== DERIVED ==========
void test_derived() {
    std::cout << "\n=== derived ===\n";

    // Speed of sound
    auto sos_sl = speed_of_sound(isa::sea_level_temp);
    approx(sos_sl.value(), 340.29, 0.01, "SOS SL");
    auto sos_tp = speed_of_sound(Quantity<Kelvin>(216.65));
    approx(sos_tp.value(), 295.07, 0.01, "SOS tropopause");
    check(speed_of_sound(Quantity<Kelvin>(300.0)) > speed_of_sound(Quantity<Kelvin>(250.0)), "SOS increases with T");

    // Dynamic pressure
    auto q1 = dynamic_pressure(isa::sea_level_density, Quantity<MetersPerSecond>(340.0));
    approx(q1.to<Pascals>().value(), 70805.0, 1.0, "qbar SL 340m/s");
    auto q0 = dynamic_pressure(isa::sea_level_density, Quantity<MetersPerSecond>(0.0));
    approx(q0.to<Pascals>().value(), 0.0, 1e-9, "qbar zero speed");

    // Mach number
    auto m1 = mach_number(Quantity<MetersPerSecond>(340.0), Quantity<MetersPerSecond>(340.3));
    approx(m1.value(), 0.999, 0.001, "Mach 340/340.3");
    auto m2 = mach_number(Quantity<MetersPerSecond>(250.0), isa::sea_level_sos);
    approx(m2.value(), 0.735, 0.001, "Mach 250 SL");
    auto m3 = mach_number(Quantity<MetersPerSecond>(500.0), isa::sea_level_sos);
    approx(m3.value(), 1.469, 0.001, "Mach 500 SL");

    // Wing loading
    auto w_f16 = weight_from_mass(Quantity<Kilograms>(12000.0));
    auto wl_f16 = wing_loading(w_f16, Quantity<SquareMeters>(27.87));
    approx(wl_f16.to<Pascals>().value(), 4222.4, 1.0, "F16 WL");
    auto w_c172 = weight_from_mass(Quantity<Kilograms>(1000.0));
    auto wl_c172 = wing_loading(w_c172, Quantity<SquareMeters>(16.2));
    approx(wl_c172.to<Pascals>().value(), 605.3, 1.0, "C172 WL");

    // Weight from mass
    approx(weight_from_mass(Quantity<Kilograms>(1.0)).to<Newtons>().value(), 9.80665, 1e-9, "1kg=9.81N");
    approx(weight_from_mass(Quantity<Kilograms>(100.0)).to<PoundForce>().value(), 220.46, 0.01, "100kg=220.46lbf");

    // Full scenario: F-16 at 250kn ISA SL
    auto tas = Quantity<MetersPerSecond>(250.0 * 1852.0 / 3600.0);
    auto sos = speed_of_sound(isa::sea_level_temp);
    auto mach = mach_number(tas, sos);
    auto q = dynamic_pressure(isa::sea_level_density, tas);
    approx(mach.value(), 0.378, 0.001, "scenario mach");
    approx(q.to<Pascals>().value(), 10130.0, 10.0, "scenario qbar");
    approx(q.to<PSF>().value(), 211.5, 1.0, "scenario qbar psf");

    // ISA values
    approx(isa::sea_level_temp.to<Celsius>().value(), 15.0, 1e-9, "ISA 15C");
    approx(isa::sea_level_pressure.to<Pascals>().value(), 101325.0, 1e-6, "ISA 101325Pa");
    approx(isa::sea_level_density.value(), 1.225, 1e-9, "ISA 1.225");
    approx(isa::sea_level_sos.to<MetersPerSecond>().value(), 340.29, 0.01, "ISA 340.29");
}

int main() {
    test_dimension();
    test_conversions();
    test_temperature();
    test_arithmetic();
    test_aviation();
    test_derived();
    std::cout << "\n==============================\n";
    if (g_fail == 0) std::cout << "All " << g_pass << " checks passed.\n";
    else std::cout << g_fail << " of " << (g_pass+g_fail) << " checks FAILED.\n";
    std::cout << "==============================\n";
    return g_fail;
}

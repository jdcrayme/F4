// f4-sensors/src/signature.cpp — SignatureComponent's grid lookup.
//
// The grid is referenced, not owned (the f4::data::SignatureDataLibrary
// lives with the host); the full definition comes from f4-data, which is
// a PRIVATE dependency of f4-sensors (see CMakeLists.txt).

#include <f4/sensors/signature.hpp>

#include <f4/data/signature_data.hpp>

#define _USE_MATH_DEFINES

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif // !M_PI

namespace f4::sensors {

double SignatureComponent::effective_rcs_m2(double aspect_rad,
                                            double elevation_deg) const {
    if (rcs_grid == nullptr) return rcs_m2;
    const double aspect_deg =
        std::abs(aspect_rad) * (180.0 / M_PI);
    return rcs_grid->value_at(aspect_deg, elevation_deg);
}

double SignatureComponent::ir_signature_value(double aspect_rad) const {
    if (sig_data == nullptr) return 1.0;
    const double aspect_deg =
        std::abs(aspect_rad) * (180.0 / M_PI);
    const f4::data::SignatureGrid* grid = &sig_data->ir1;
    switch (ir_power) {
        case IrPowerMode::Baseline:    grid = &sig_data->ir0; break;
        case IrPowerMode::Afterburner: grid = &sig_data->ir1; break;
        case IrPowerMode::Max:         grid = &sig_data->ir2; break;
    }
    return grid->value_at(aspect_deg, 0.0);
}

double SignatureComponent::visual_signature_value(double aspect_rad) const {
    if (sig_data == nullptr) return 1.0;
    const double aspect_deg =
        std::abs(aspect_rad) * (180.0 / M_PI);
    return sig_data->visual.value_at(aspect_deg, 0.0);
}

} // namespace f4::sensors

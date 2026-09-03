// f4-sensors/src/detection.cpp — pure detection model. See detection.hpp.

#include <f4/sensors/detection.hpp>

// Full definition for the SimData grid path (TargetSignature carries the
// grid as a forward-declared pointer; f4-data is a PRIVATE dependency of
// f4-sensors — owners that set rcs_grid include f4/data/signature_data.hpp
// themselves).
#include <f4/data/signature_data.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace f4::sensors {

namespace {

struct LobeKnot {
    double deg;
    double factor;
};

// Placeholder fighter lobe shape (deg off nose -> RCS fraction):
// strong head-on return, deep sidelobe notch at beam, tail between.
// Symmetric around the nose-tail axis by construction (aspect in [0, pi]).
constexpr LobeKnot kLobeKnots[] = {
    {0.0,   1.00},
    {30.0,  0.90},
    {60.0,  0.60},
    {90.0,  0.30},
    {120.0, 0.40},
    {150.0, 0.55},
    {180.0, 0.65},
};

constexpr double kClosureReferenceFps = 2000.0;  // closure_factor knee
constexpr double kClosureMaxEffect    = 0.25;    // +-25% range
constexpr double kSureThingFraction   = 0.75;    // Pd == 1 inside this fraction of R_det

[[nodiscard]] constexpr double clamp01(double v) noexcept {
    return std::min(1.0, std::max(0.0, v));
}

} // namespace

double aspect_lobe_factor(double aspect_rad) noexcept {
    // Clamp to [0, pi] (negative aspects wrap to the mirror angle).
    double a = std::clamp(std::abs(aspect_rad), 0.0, M_PI);
    const double deg = a * (180.0 / M_PI);

    // Piecewise-linear interpolation over the knot table.
    for (std::size_t i = 1; i < std::size(kLobeKnots); ++i) {
        if (deg <= kLobeKnots[i].deg) {
            const auto& lo = kLobeKnots[i - 1];
            const auto& hi = kLobeKnots[i];
            const double t = (deg - lo.deg) / (hi.deg - lo.deg);
            return lo.factor + t * (hi.factor - lo.factor);
        }
    }
    return kLobeKnots[std::size(kLobeKnots) - 1].factor;
}

double detection_range_nm(const RadarParameters& params,
                          double rcs_m2,
                          double aspect_rad,
                          double closure_fps) noexcept {
    const double rcs_eff = std::max(rcs_m2 * aspect_lobe_factor(aspect_rad), 1e-6);
    const double rcs_ratio = rcs_eff / std::max(params.reference_rcs_m2, 1e-6);
    const double fourth_root = std::pow(rcs_ratio, 0.25);
    const double closure_factor =
        1.0 + std::clamp(closure_fps / kClosureReferenceFps,
                         -kClosureMaxEffect, kClosureMaxEffect);
    return std::max(params.reference_range_nm, 0.0) * fourth_root * closure_factor;
}

double detection_range_nm(const RadarParameters& params,
                          const TargetSignature& sig) noexcept {
    // The SimData grid path (SIGDATA/RCSDAT): when the target carries a
    // SignatureGrid, its value_at(aspect, elevation) IS the effective
    // RCS in m^2 — the grid encodes the lobes, so the placeholder
    // aspect_lobe_factor does not stack on top. Callers without data
    // (grid == nullptr) take the exact pre-SimData path.
    if (sig.rcs_grid != nullptr) {
        const double aspect_deg =
            std::clamp(std::abs(sig.aspect_rad) * (180.0 / M_PI), 0.0, 180.0);
        const double grid_rcs =
            sig.rcs_grid->value_at(aspect_deg, sig.elevation_deg);
        const double rcs_ratio =
            std::max(grid_rcs, 1e-6) / std::max(params.reference_rcs_m2, 1e-6);
        const double fourth_root = std::pow(rcs_ratio, 0.25);
        const double closure_factor =
            1.0 + std::clamp(sig.closure_fps / kClosureReferenceFps,
                             -kClosureMaxEffect, kClosureMaxEffect);
        return std::max(params.reference_range_nm, 0.0) * fourth_root *
               closure_factor;
    }
    return detection_range_nm(params, sig.rcs_m2, sig.aspect_rad,
                              sig.closure_fps);
}

double detection_probability(const RadarParameters& params,
                             const TargetSignature& sig,
                             double range_nm) noexcept {
    const double r_det = detection_range_nm(params, sig);
    if (r_det <= 0.0 || range_nm >= r_det) return 0.0;
    const double knee = kSureThingFraction * r_det;
    if (range_nm <= knee) return 1.0;
    return clamp01((r_det - range_nm) / (r_det - knee));
}

} // namespace f4::sensors

// f4-sensors/include/f4/sensors/detection.hpp
//
// The pure radar detection model: no ECS, no RNG, no state. Given a radar
// parameter card and a target signature, answer two questions:
//
//   1. detection_range_nm()   — how far out could this target be detected?
//   2. detection_probability()— P(detect) at a given range (0..1).
//
// The model is a physically-motivated placeholder shaped like the real thing
// (range ramp, fourth-root RCS scaling, aspect lobes, closure effect). When
// FreeFalcon's Falcon4.RCD detection tables are imported (M2+), the lobe and
// ramp parameters move onto RadarParameters and these functions keep their
// signatures — call sites do not change.
//
// Determinism: these functions are pure. SAMPLING a detection is the caller's
// job (RadarSimComponent rolls a seeded uniform), exactly like the damage
// model draws nothing and the gun applies its own seeded dispersion.

#pragma once

#include <f4/sensors/radar_types.hpp>

namespace f4::sensors {

/// Aspect lobe factor in [~0.3, 1.0]: how much of the target's nominal RCS is
/// presented at a given angle off its nose. Piecewise-linear interpolation of
/// a plausible fighter lobe shape (nose-on strongest, beam-on weakest, tail
/// between), documented as a placeholder until RCD data lands.
/// aspect_rad is clamped to [0, pi].
[[nodiscard]] double aspect_lobe_factor(double aspect_rad) noexcept;

/// Detection range (NM) for a target with `rcs_m2` at `aspect_rad` and
/// `closure_fps` closure:
///   R = reference_range * (rcs_eff / reference_rcs)^(1/4) * closure_factor
/// where rcs_eff = rcs * aspect_lobe_factor(aspect) and closure_factor
/// = 1 + clamp(closure_fps / 2000, -0.25, +0.25)  (closing extends,
/// opening shrinks, both capped at +-25%).
[[nodiscard]] double detection_range_nm(const RadarParameters& params,
                                        double rcs_m2,
                                        double aspect_rad,
                                        double closure_fps) noexcept;

/// Convenience overload taking the signature struct.
[[nodiscard]] double detection_range_nm(const RadarParameters& params,
                                        const TargetSignature& sig) noexcept;

/// Probability of detecting a target at `range_nm` given its signature.
///   1.0                              inside 0.75 * R_det (sure-thing zone)
///   linear ramp 1 -> 0               between 0.75 * R_det and R_det
///   0.0                              beyond R_det
/// The 0.75 knee matches the burn-through intuition: inside three quarters
/// of the detection range the radar essentially always sees the target.
[[nodiscard]] double detection_probability(const RadarParameters& params,
                                           const TargetSignature& sig,
                                           double range_nm) noexcept;

} // namespace f4::sensors

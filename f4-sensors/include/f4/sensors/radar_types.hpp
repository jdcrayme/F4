// f4-sensors/include/f4/sensors/radar_types.hpp
//
// Data types shared by the airborne radar model: radar parameters, the scan
// volume, the radar mode, and the per-target signature input.
//
// FreeFalcon reference: Falcon4.RCD (radar class data) holds per-radar range
// and detection arcs; the objective-side RadarComponent (f4-entities) already
// carries that data for ground emitters. THIS file models the AIRBORNE radar:
// a parameter set shaped so a future RCD loader can replace the defaults
// without touching call sites (same strategy as WeaponClassTable in
// f4-weapons: built-in placeholder data now, real import later).
//
// All angles are radians. Bearings are CW from north (ENU frame, matching
// f4::geo::to_bra and SensorFusion's heading convention).

#pragma once

#include <cmath>
#include <compare>

#include <f4/geo/constants.hpp>

namespace f4::sensors {

// ============================================================================
// RadarParameters — the radar's detection envelope.
//
// reference_range_nm is the detection range against reference_rcs_m2 (head-on
// aspect, nominal closure). Detection range for any other RCS scales with the
// fourth root of the RCS ratio (the radar-equation range scaling):
//
//     R_det(rcs) = reference_range_nm * (rcs / reference_rcs_m2)^(1/4)
//
// (doubling the RCS buys ~19% more range; halving it costs ~16%). The aspect
// and closure corrections live in detection.hpp — the parameters here stay a
// plain data card.
// ============================================================================
struct RadarParameters {
    double reference_range_nm = 40.0;   // vs reference_rcs_m2, head-on
    double reference_rcs_m2   = 5.0;    // reference target RCS (a fighter)
};

// ============================================================================
// RadarMode — search vs single-target track (STT lock).
//
// In Search mode the antenna sweeps the ScanVolume and every detected contact
// becomes/updates a track file. In Track mode the antenna parks on one
// target: only that target is scanned (but the detection roll still applies),
// and victims equipped with an RWR see a LOCK strobe instead of a search
// strobe — that is what triggers the victim's RWR lock warning.
// ============================================================================
enum class RadarMode {
    Search,   // RWS — volume sweep
    Track,    // STT — parked on locked_target
};

// ============================================================================
// ScanVolume — where the antenna is pointing while in Search mode.
//
// azimuth_center_rad is the bar center (CW from north) and
// azimuth_half_width_rad the half-width, so a 120-degree bar centered north
// is center=0, half_width=pi/3. Gimbal limits fall out naturally: a radar
// with +-60 degrees of azimuth gimbal can never point the bar outside that,
// which the owner (the AI or the host) enforces when steering the center.
//
// Elevation limits are relative to the radar's own horizon (rad is up).
// range_scale_nm is the range knob — contacts beyond it are not searched
// even if the detection model would see them (the pilot flies the range
// scale, not the radar's max).
// ============================================================================
struct ScanVolume {
    double azimuth_center_rad      = 0.0;    // CW from north
    double azimuth_half_width_rad  = M_PI / 3.0;   // 60 deg half-width => 120 deg bar
    double elevation_min_rad       = -M_PI / 6.0;  // -30 deg
    double elevation_max_rad       = +M_PI / 6.0;  // +30 deg
    double range_scale_nm          = 160.0;

    /// Is (bearing, elevation, range) inside the searched volume?
    /// Bearing containment uses the SHORTEST angular difference, so a bar
    /// centered on north correctly contains bearings near 350 and 10 degrees.
    [[nodiscard]] bool contains(double bearing_rad,
                                double elevation_rad,
                                double range_nm) const noexcept {
        // Shortest signed angular difference (bearing - center) in [-pi, pi].
        double d = bearing_rad - azimuth_center_rad;
        while (d >  M_PI) d -= 2.0 * M_PI;
        while (d < -M_PI) d += 2.0 * M_PI;
        if (std::abs(d) > azimuth_half_width_rad) return false;
        if (elevation_rad < elevation_min_rad || elevation_rad > elevation_max_rad) return false;
        return range_nm <= range_scale_nm;
    }

    auto operator<=>(const ScanVolume&) const = default;
};

// ============================================================================
// TargetSignature — everything the detection model knows about the target.
//
//   rcs_m2       — radar cross section (square meters). Entities without a
//                  SignatureComponent read as the radar's reference RCS.
//   aspect_rad   — angle off the TARGET's nose (0 = head-on, pi = tail-on).
//                  Head-on presents the biggest lobes; beam-on the smallest.
//   closure_fps  — rate at which the RANGE is decreasing (positive = closing,
//                  negative = opening), feet per second. Closing targets
//                  are detected slightly farther out (doppler + burn-through
//                  intuition, see detection.hpp).
// ============================================================================
struct TargetSignature {
    double rcs_m2      = 5.0;
    double aspect_rad  = 0.0;
    double closure_fps = 0.0;
};

} // namespace f4::sensors

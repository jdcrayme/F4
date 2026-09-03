// f4-sensors/include/f4/sensors/signature.hpp
//
// SignatureComponent — a target's radar cross section (passive).
//
// Entities without one read as the scanning radar's reference RCS (5 m^2,
// a fighter — see RadarParameters). Add this component to shape a target's
// signature: a stealth fighter (~0.01-0.1 m^2), a bomber (~10-100 m^2),
// a airliner-sized transport (~100+ m^2). The detection model scales the
// detection range with the fourth root of the RCS (radar equation).
//
// This lives in f4-sensors (not f4-entities) because signature is a SENSOR
// concept — the entity model itself is agnostic about how observable a
// thing is. When the campaign data pipeline lands real RCS per unit type,
// the loader populates this component from that data.
//
// SimData upgrade (the "RCD data lands" moment detection.hpp documented):
// the component can now carry a per-unit-type ASPECT-DEPENDENT RCS grid
// (f4-data's SignatureGrid, converted from SimData.zip's
// SIGDATA/RCSDAT/*.RCS breakpoint tables). The grid is referenced, NOT
// owned — the library (f4::data::SignatureDataLibrary) lives with the
// host/simulation, exactly like the brain archetype pointer on
// BrainComponent. With no grid set, the component behaves exactly as
// before (scalar RCS + the placeholder lobe model).

#pragma once

#include <f4/entities/entity.hpp>

namespace f4::data {
struct SignatureGrid;   // f4/data/signature_data.hpp (fwd — keeps this
                        // header free of the f4-data dependency)
}

namespace f4::sensors {

struct SignatureComponent : public entities::Component<SignatureComponent> {
    double rcs_m2 = 5.0;   // square meters; 5.0 = generic fighter

    /// Optional aspect-dependent RCS grid (m^2 by azimuth/elevation off
    /// the target's axes). Non-owning: the SignatureDataLibrary that owns
    /// it outlives the entity. When set, the radar detection model uses
    /// grid.value_at(azimuth, elevation) INSTEAD of rcs_m2 × the
    /// placeholder aspect_lobe_factor — the shipped generic grid is flat
    /// 10 m^2, which reads like a bomber-sized target until real
    /// per-type grids land.
    const f4::data::SignatureGrid* rcs_grid = nullptr;

    /// Effective RCS (m^2) at the given aspect/elevation: the grid lookup
    /// when rcs_grid is set, else the scalar rcs_m2 (the caller applies
    /// the lobe factor; kept separate so data-free callers are unchanged).
    [[nodiscard]] double effective_rcs_m2(double aspect_rad,
                                          double elevation_deg = 0.0) const;
};

} // namespace f4::sensors

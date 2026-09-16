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

#include <cstdint>

#include <f4/entities/entity.hpp>

namespace f4::data {
struct SignatureGrid;             // f4/data/signature_data.hpp (fwd — keeps this
struct AircraftSignatureData;     // header free of the f4-data dependency)
}

namespace f4::sensors {

/// Which IR band the IR signature accessors read from the aircraft's
/// signature data (SimData SIGDATA's three IR breakpoint tables — see
/// f4-data's AircraftSignatureData: ir0 baseline, ir1 afterburner, ir2
/// maximum). The band is a PROPERTY OF THE OBSERVER'S interest, not the
/// target's, so it rides the target's signature component as the power
/// state the IR sensors should assume.
enum class IrPowerMode : std::uint8_t {
    Baseline    = 0,   // ir0 — dry power, the cruise signature
    Afterburner = 1,   // ir1 — mil/AB plume (the default: a fighting
                       //      aircraft is not cruising)
    Max         = 2,   // ir2 — the maximum (ground-effect hot) signature
};

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

    /// The full five-grid signature record (RCS + ir0/ir1/ir2 + visual),
    /// from the same SignatureDataLibrary. Non-owning. When set, the IRST
    /// and visual sensors read the target's IR / visual grids through
    /// ir_signature_value()/visual_signature_value() below; the radar
    /// keeps reading rcs_grid (set alongside by the same host code) so
    /// the radar path is byte-identical to the pre-IR grid wiring.
    const f4::data::AircraftSignatureData* sig_data = nullptr;

    /// The power state IR sensors assume for this target (which IR band
    /// ir_signature_value() interpolates). Default Afterburner — a
    /// maneuvering/fighting aircraft; hosts set it from the FM's throttle
    /// when that fidelity lands.
    IrPowerMode ir_power = IrPowerMode::Afterburner;

    /// Effective RCS (m^2) at the given aspect/elevation: the grid lookup
    /// when rcs_grid is set, else the scalar rcs_m2 (the caller applies
    /// the lobe factor; kept separate so data-free callers are unchanged).
    [[nodiscard]] double effective_rcs_m2(double aspect_rad,
                                          double elevation_deg = 0.0) const;

    /// The target's IR signature ratio at the given aspect (azimuth off
    /// the target's nose; elevation is not modeled — the IR grids' az
    /// breakpoints carry the hot-rear lobe). Reads the ir0/ir1/ir2 grid
    /// selected by ir_power; 1.0 when no sig_data (the data-free default:
    /// every target reads as the reference airframe the IRST card's
    /// nominal range was authored against).
    [[nodiscard]] double ir_signature_value(double aspect_rad) const;

    /// The target's visual size factor at the given aspect (the VIS
    /// grid's interpolated ratio; 1.0 when no sig_data — the generic
    /// shipped grid is flat 1.0, so data-free behavior is identical).
    [[nodiscard]] double visual_signature_value(double aspect_rad) const;
};

} // namespace f4::sensors

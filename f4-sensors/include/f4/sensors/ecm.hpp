// f4-sensors/include/f4/sensors/ecm.hpp
//
// EcmComponent — an onboard radar jammer (passive state, active effect).
//
// The SENSORS_COUNTERMEASURES_PLAN §8 ECM leg: the radar burn-through
// model. Two consumers read this component; neither needs it to tick:
//
//   1. RadarSimComponent::perform_scan — every LIVE ENEMY jammer inside
//      the beam toward a candidate raises the noise floor that candidate
//      is read against; the radar's detection range degrades (the range
//      the detection ramp reads stretches by 1/(1-W)). Close the range
//      and the echo wins through — burn-through. No EcmComponent in the
//      world (the fidelity gate's off state) → the scan is arithmetic-
//      free and byte-identical.
//   2. update_rwr — a jammer is an EMITTER: the victim's RWR hears the
//      noise and shows the Jamming warning (a strobe, not a lock).
//
// The model is a documented placeholder SHAPED like the physics (one-way
// noise power ~ 1/r², the jammer must lie in the antenna's receiving
// corridor toward the candidate), exactly like the detection model it
// feeds — when real ECM data lands (Falcon4.DAT ECM tables), the
// strength/radius fields move onto data cards and nothing else changes.
//
// Attachment discipline (the countermeasure gate's rule): the component
// only exists when the scenario turned the fidelity on AND the aircraft
// opted in (the scenario's per-aircraft "ecm" field — no unit-data
// source exists yet to decide who jams).

#pragma once

#include <string>

#include <f4/entities/entity.hpp>

namespace f4::sensors {

struct EcmComponent : public entities::Component<EcmComponent> {
    /// Jammer power, 1.0 = the reference jammer (its noise at the
    /// burn-through range exactly matches the radar's reference
    /// performance — W = 1 at d = burn_through_range_nm scaled by
    /// strength). 0 = off.
    double jamming_strength = 1.0;

    /// The burn-through range (NM): the distance at which the victim
    /// radar's echo beats this jammer's noise. Inside it the jammer's
    /// weight saturates; outside it falls with the one-way square
    /// (1/r²).
    double burn_through_range_nm = 20.0;

    /// IFF reference (the radar's own_team convention): a jammer never
    /// degrades a radar carrying the same team string.
    std::string own_team = "red";

    /// Master switch (a pod can be off while carried).
    bool enabled = true;
};

} // namespace f4::sensors

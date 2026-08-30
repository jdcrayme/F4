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

#pragma once

#include <f4/entities/entity.hpp>

namespace f4::sensors {

struct SignatureComponent : public entities::Component<SignatureComponent> {
    double rcs_m2 = 5.0;   // square meters; 5.0 = generic fighter
};

} // namespace f4::sensors

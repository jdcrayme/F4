// f4-sensors/include/f4/sensors/f4_sensors.hpp
//
// Umbrella header — include this to pull in the whole sensor model.
//
// Library contents:
//   radar_types.hpp    — RadarParameters, ScanVolume, RadarMode, TargetSignature
//   detection.hpp      — pure detection model (range + probability)
//   signature.hpp      — SignatureComponent (target RCS)
//   track_store.hpp    — TrackFile / TrackStore (quality, decay, IFF, NCTR)
//   radar_component.hpp— RadarSimComponent (ECS behavioral, priority 45)
//   rwr.hpp            — RWR model + component + world sweep + message
//   messages.hpp       — radar track acquired/dropped messages
//
// Dependency policy (mirrors f4-weapons): f4-geo, f4-math, f4-entities,
// f4-messaging. Deliberately NOT f4-ai (tactics consume sensors, never the
// reverse) and NOT f4-weapons (the missile's seeker-source indirection is a
// std::function the host wires — no library link needed).

#pragma once

#include <f4/sensors/detection.hpp>
#include <f4/sensors/messages.hpp>
#include <f4/sensors/radar_component.hpp>
#include <f4/sensors/radar_types.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/sensors/signature.hpp>
#include <f4/sensors/track_store.hpp>

namespace f4::sensors {

inline constexpr const char* kVersion = "0.1.0";

} // namespace f4::sensors

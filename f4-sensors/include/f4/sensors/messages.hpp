// f4-sensors/include/f4/sensors/messages.hpp
//
// Sensor-domain messages — plain structs, one per event, per the
// flight-model bus convention. f4-sensors publishes on state TRANSITIONS
// only (track acquired / dropped / RWR lock or launch onset); per-scan
// detections and search strobes are queryable state, not bus traffic.

#pragma once

#include <cstdint>

namespace f4::sensors {

/// A radar (radar_entity_id) gained a track on target_entity_id this scan.
struct RadarTrackAcquiredMessage {
    std::uint64_t radar_entity_id = 0;
    std::uint64_t target_entity_id = 0;
    double time_s = 0.0;
};

/// A radar LOST a track (quality decayed out or went stale). Exactly once
/// per drop transition.
struct RadarTrackDroppedMessage {
    std::uint64_t radar_entity_id = 0;
    std::uint64_t target_entity_id = 0;
    double time_s = 0.0;
};

} // namespace f4::sensors

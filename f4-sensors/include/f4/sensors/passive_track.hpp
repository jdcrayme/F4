// f4-sensors/include/f4/sensors/passive_track.hpp
//
// PassiveTrackStore — the contact book for the PASSIVE sensors (IRST,
// visual). The radar's TrackStore carries track quality, NCTR strings,
// and state machines because radar tracking is a persistent act (gate,
// correlate, decay). The passive sensors are simpler: a scan either sees
// the thing or it doesn't, and what they keep is "what I saw last and
// when" — a contact book, not a filter.
//
// Shape (mirrors TrackStore's role so the sensors feel the same):
//   on_detection(id, pos, vel, now) — insert-or-refresh the contact.
//   decay(now, hold_s)              — returns the ids whose last_seen
//                                     is older than hold_s (the caller
//                                     publishes/expires them); they are
//                                     erased.
// Deterministic: pure bookkeeping, no RNG, no time source of its own —
// the sensor stamps `now` from its host-stamped clock.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>

namespace f4::sensors {

struct PassiveContact {
    f4::geo::WorldPosition position{};
    f4::math::Vec3<double> velocity{};
    double last_seen_s = 0.0;
    std::uint32_t detections = 0;
};

class PassiveTrackStore {
public:
    /// Insert or refresh. Returns true when this is a NEW contact (the
    /// caller can treat first-seen as an acquisition event).
    bool on_detection(std::uint64_t id,
                      const f4::geo::WorldPosition& pos,
                      const f4::math::Vec3<double>& vel,
                      double now_s);

    /// Erase every contact whose last_seen is older than `hold_s`;
    /// returns their ids (order unspecified — the map's iteration order).
    [[nodiscard]] std::vector<std::uint64_t> decay(double now_s,
                                                   double hold_s);

    [[nodiscard]] const PassiveContact* find(std::uint64_t id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return contacts_.size(); }
    [[nodiscard]] bool empty() const noexcept { return contacts_.empty(); }
    void clear() noexcept { contacts_.clear(); }

    [[nodiscard]] auto begin() const noexcept { return contacts_.begin(); }
    [[nodiscard]] auto end() const noexcept { return contacts_.end(); }

private:
    std::unordered_map<std::uint64_t, PassiveContact> contacts_;
};

} // namespace f4::sensors

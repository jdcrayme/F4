// f4-sensors/src/passive_track.cpp — PassiveTrackStore's bookkeeping.

#include <f4/sensors/passive_track.hpp>

namespace f4::sensors {

bool PassiveTrackStore::on_detection(std::uint64_t id,
                                     const f4::geo::WorldPosition& pos,
                                     const f4::math::Vec3<double>& vel,
                                     double now_s) {
    auto [it, inserted] = contacts_.try_emplace(id);
    inserted ? it->second.detections = 1 : ++it->second.detections;
    it->second.position = pos;
    it->second.velocity = vel;
    it->second.last_seen_s = now_s;
    return inserted;
}

std::vector<std::uint64_t> PassiveTrackStore::decay(double now_s,
                                                    double hold_s) {
    std::vector<std::uint64_t> dropped;
    for (auto it = contacts_.begin(); it != contacts_.end();) {
        if (now_s - it->second.last_seen_s > hold_s) {
            dropped.push_back(it->first);
            it = contacts_.erase(it);
        } else {
            ++it;
        }
    }
    return dropped;
}

const PassiveContact* PassiveTrackStore::find(
    std::uint64_t id) const noexcept {
    const auto it = contacts_.find(id);
    return it == contacts_.end() ? nullptr : &it->second;
}

} // namespace f4::sensors

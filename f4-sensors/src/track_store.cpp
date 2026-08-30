// f4-sensors/src/track_store.cpp — track files with quality decay. See track_store.hpp.

#include <f4/sensors/track_store.hpp>

#include <algorithm>
#include <cmath>

namespace f4::sensors {

void TrackStore::on_detection(std::uint64_t entity_id,
                              const f4::geo::WorldPosition& position,
                              const f4::math::Vec3<double>& velocity,
                              double time_s,
                              std::string team,
                              std::string nctr) {
    auto [it, inserted] = tracks_.try_emplace(entity_id);
    TrackFile& t = it->second;
    if (inserted) {
        t.entity_id = entity_id;
        t.first_detected_s = time_s;
        t.quality = 0.0;
    }
    t.position = position;
    t.velocity = velocity;
    t.last_detected_s = time_s;
    t.team = std::move(team);
    t.nctr = std::move(nctr);
    t.hostile_by_iff = (!t.team.empty()) && t.team != own_team_;
    t.quality = std::min(1.0, t.quality + cfg_.quality_gain);
    recompute_state(t, /*detected_this_pass=*/true);
}

std::vector<std::uint64_t> TrackStore::decay_untracked(double time_s) {
    std::vector<std::uint64_t> dropped_now;
    for (auto& [id, t] : tracks_) {
        if (t.state == TrackState::Dropped) continue;
        if (t.last_detected_s >= time_s) continue;  // detected this pass

        const double age = time_s - t.last_detected_s;
        t.quality *= std::exp(-age / std::max(cfg_.decay_tau_s, 1e-6));
        const bool stale = age > cfg_.stale_timeout_s;
        if (stale) t.quality = 0.0;  // gone too long to keep pretending
        recompute_state(t, /*detected_this_pass=*/false);
        if (t.state == TrackState::Dropped) {
            dropped_now.push_back(id);
        }
    }
    return dropped_now;
}

const TrackFile* TrackStore::find(std::uint64_t entity_id) const {
    const auto it = tracks_.find(entity_id);
    return it == tracks_.end() ? nullptr : &it->second;
}

TrackFile* TrackStore::find(std::uint64_t entity_id) {
    const auto it = tracks_.find(entity_id);
    return it == tracks_.end() ? nullptr : &it->second;
}

std::vector<const TrackFile*> TrackStore::live() const {
    std::vector<const TrackFile*> out;
    for (const auto& [id, t] : tracks_) {
        if (t.state != TrackState::Dropped) out.push_back(&t);
    }
    return out;
}

std::vector<const TrackFile*> TrackStore::established() const {
    std::vector<const TrackFile*> out;
    for (const auto& [id, t] : tracks_) {
        if (t.state == TrackState::Established || t.state == TrackState::Coasting) {
            out.push_back(&t);
        }
    }
    return out;
}

std::size_t TrackStore::live_count() const {
    std::size_t n = 0;
    for (const auto& [id, t] : tracks_) {
        if (t.state != TrackState::Dropped) ++n;
    }
    return n;
}

std::size_t TrackStore::purge_dropped() {
    std::size_t removed = 0;
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (it->second.state == TrackState::Dropped) {
            it = tracks_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void TrackStore::recompute_state(TrackFile& t, bool detected_this_pass) const {
    if (t.quality < cfg_.drop_quality ||
        t.quality <= 0.0) {
        t.state = TrackState::Dropped;
        return;
    }
    if (t.quality >= cfg_.established_quality) {
        t.state = detected_this_pass ? TrackState::Established
                                     : TrackState::Coasting;
    } else {
        t.state = TrackState::Tentative;
    }
}

} // namespace f4::sensors

// f4-sensors/include/f4/sensors/track_store.hpp
//
// Track files — the radar's memory of what it has seen.
//
// One TrackFile per detected entity. Quality builds with repeated detections
// (each scan hit adds quality_gain, clamped to 1.0) and decays exponentially
// when scans are missed (tau seconds time constant). The state ladder:
//
//   Tentative   — recently detected but quality below the established
//                 threshold (a "splotch" the pilot hasn't confirmed)
//   Established — quality >= established_quality AND detected in the latest
//                 scan (a solid track; the AI may engage)
//   Coasting    — was established but the latest scan missed it; quality
//                 still above the established threshold. The radar keeps
//                 predicting; the AI should treat it as stale.
//   Dropped     — quality collapsed or the track went stale
//                 (stale_timeout_s with no detection). Kept in the store
//                 (queryable) until purge_dropped(); command_track()
//                 refuses to lock a dropped track.
//
// IFF: the store is constructed with the owner's team string; every track
// carries the target's team tag and hostile_by_iff = (team != own_team).
// NCTR (Non-Cooperative Target Recognition): the CALLER decides when the
// identity string is known and passes it with the detection; the store just
// remembers it. This keeps the recognition policy (how many scans, which
// quality threshold) out of the data structure.
//
// Determinism: the store is an std::map keyed by entity_id — iteration is
// always in ascending id order, no RNG anywhere. Decay math is pure.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <f4/geo/position.hpp>
#include <f4/math/vec3.hpp>

namespace f4::sensors {

enum class TrackState {
    Tentative,
    Established,
    Coasting,
    Dropped,
};

struct TrackFile {
    std::uint64_t entity_id = 0;
    f4::geo::WorldPosition position{};     // last detected position (ENU ft)
    f4::math::Vec3<double> velocity{};     // last detected velocity (ENU ft/s)
    double quality = 0.0;                  // [0, 1]
    TrackState state = TrackState::Tentative;
    double first_detected_s = 0.0;
    double last_detected_s = 0.0;
    std::string nctr;                      // resolved identity ("" = unknown)
    std::string team;                      // target's team tag ("" = unknown)
    bool hostile_by_iff = false;           // team != own_team
};

struct TrackStoreConfig {
    double quality_gain        = 0.34;  // per detection (2 scans -> established)
    double decay_tau_s         = 8.0;   // exponential time constant when coasting
    double established_quality = 0.6;   // >= this => Established (when scanned this pass)
    double drop_quality        = 0.05;  // < this => Dropped
    double stale_timeout_s     = 20.0;  // no detection this long => Dropped
};

class TrackStore {
public:
    TrackStore() = default;
    explicit TrackStore(std::string own_team, TrackStoreConfig cfg = {})
        : own_team_(std::move(own_team)), cfg_(cfg) {}

    /// Record a detection this scan. Creates or refreshes the track:
    /// quality = min(1, quality + gain), position/velocity/team/nctr
    /// overwritten, last_detected_s = time_s, state recomputed
    /// (Established if quality >= threshold, else Tentative).
    void on_detection(std::uint64_t entity_id,
                      const f4::geo::WorldPosition& position,
                      const f4::math::Vec3<double>& velocity,
                      double time_s,
                      std::string team,
                      std::string nctr);

    /// End-of-scan bookkeeping: every track whose last_detected_s is older
    /// than `time_s` MISSED this scan — decay its quality by
    /// exp(-(time_s - last_detected_s) / tau), recompute state, drop if
    /// collapsed or stale. Returns the ids that transitioned to Dropped in
    /// THIS call (so the caller can publish drop messages exactly once).
    [[nodiscard]] std::vector<std::uint64_t> decay_untracked(double time_s);

    /// Transitioned to Dropped earlier, not yet purged.
    [[nodiscard]] const TrackFile* find(std::uint64_t entity_id) const;
    [[nodiscard]] TrackFile* find(std::uint64_t entity_id);

    /// Non-dropped tracks (ascending entity_id — deterministic).
    [[nodiscard]] std::vector<const TrackFile*> live() const;
    /// Established-or-Coasting tracks (the AI's engagement picture).
    [[nodiscard]] std::vector<const TrackFile*> established() const;
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] std::size_t total_count() const { return tracks_.size(); }

    /// Remove Dropped tracks. Returns how many were removed.
    std::size_t purge_dropped();

    [[nodiscard]] const std::string& own_team() const noexcept { return own_team_; }
    [[nodiscard]] const TrackStoreConfig& config() const noexcept { return cfg_; }

private:
    void recompute_state(TrackFile& t, bool detected_this_pass) const;

    std::string own_team_;
    TrackStoreConfig cfg_{};
    std::map<std::uint64_t, TrackFile> tracks_;
};

} // namespace f4::sensors

// f4-sensors/include/f4/sensors/radar_component.hpp
//
// RadarSimComponent — the airborne radar as an ECS behavioral component.
//
// One component per radar-equipped entity. Each scan_interval_s it sweeps
// its ScanVolume (Search mode) or parks on the locked target (Track mode),
// rolls the pure detection model against every candidate, and maintains its
// TrackStore. Publishing is transition-only: RadarTrackAcquiredMessage on
// first detection of an entity, RadarTrackDroppedMessage when its track
// decays out.
//
// ECS framing:
//   priority 45 — physics pass, AFTER flight models (50) so scans see this
//                 tick's aircraft positions, BEFORE missile sims (40) so
//                 guidance and the scan read the same picture.
//
// What it scans: every OTHER non-clutter entity with a TransformComponent,
// friendly or hostile (matching SensorFusion's Phase D note — radar detects
// ALL contacts; hostility is IFF classification afterwards). Dead entities
// are not filtered yet (SensorFusion doesn't filter them either); the M3 host
// decides whether corpses still paint.
//
// AGG-2b — the candidate walk rides the world's air-picture roster
// (f4-entities' AirPictureRoster — the wired SpatialIndex term): the
// non-clutter membership is refreshed against the structural epoch + a
// 1 s behavioral-flip cadence and SHARED across every radar and the
// picture walk, so the per-scan cost is O(air picture) instead of
// O(theater) (FID_OPT_PLAN §5's 20.8 s residual). The scan re-applies
// the clutter and range gates FRESH per member (idempotent predicates
// over fresh transforms), the candidate order stays the bucket's
// entity-index order, and the RNG stream the rolls consume is
// unchanged for the shared population — the only observable delta is
// the roster's bounded behavioral-flip latency (a parked aircraft that
// starts moving is a candidate within one revalidation window instead
// of instantly; the plan's §5 re-pinning covers it).
//
// Detection sampling: a seeded std::mt19937 per component. Same seed + same
// scenario => same detection sequence (the reproducibility discipline that
// made the landing work debuggable).
//
// Time: static set_sim_time()/sim_time() mirrors MissileSimComponent — the
// host stamps the clock each tick; a world-level clock can replace it
// without API change later.
//
// NCTR: after nctr_after_scans detections of the same entity, the track's
// NCTR string resolves to that entity's CampaignIdentityComponent callsign
// ("" until then). The policy is simple; the track store just carries it.

#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>

#include <f4/entities/entity.hpp>
#include <f4/messaging/bus.hpp>
#include <f4/sensors/detection.hpp>
#include <f4/sensors/messages.hpp>
#include <f4/sensors/signature.hpp>
#include <f4/sensors/track_store.hpp>

namespace f4::sensors {

class RadarSimComponent : public entities::BehavioralComponent<RadarSimComponent> {
public:
    int priority() const noexcept override { return 45; }

    void on_attached(entities::EntityHandle& self) override { owner_ = self; }
    void update(double dt, messaging::MessageBus& bus) override;

    // --- Simulation time (host-stamped; mirrors MissileSimComponent) --------
    static void set_sim_time(double t) { sim_time_s() = t; }
    static double sim_time() { return sim_time_s(); }

    // --- Configuration (public fields: data cards, live-tunable) -----------
    // Baked into the track store / RNG lazily on the first update(), so
    // spawn code can create the entity, add the component, THEN set these
    // fields before the sim loop starts.
    RadarParameters params{};
    ScanVolume scan{};                 // Search-mode volume
    double scan_interval_s = 1.0;      // one sweep per second
    // AGG-2b: the sweep's PHASE inside its interval (seconds). 0.0 — the
    // default — keeps the pre-AGG-2b behavior byte-identical: every radar
    // primes scan_timer_ at zero and all sweeps land on the interval's
    // edges. A host (the campaign spawn path, AGG-2b's per-unit detection
    // cadences) sets a deterministic per-unit phase — stagger_phase-
    // shaped — so co-mounted radars don't all sweep on the same tick (the
    // reference's HOTSPOT_FIX jitter: "spread the herd"). The fmod carry
    // below keeps the cadence exact for any phase.
    double scan_phase_s = 0.0;
    // AGG-2b: the air-picture roster's behavioral-flip revalidation
    // cadence this radar drives the shared roster with (seconds; the
    // picture walk drives the same world roster with its own default).
    // Data, not a constant — the same rule every engine cadence keeps.
    double roster_revalidate_s = 1.0;
    std::uint32_t rng_seed = 0x46344ull;
    std::string own_team = "blue";     // IFF reference
    std::uint32_t nctr_after_scans = 2;
    TrackStoreConfig track_config{};

    // --- Commands ----------------------------------------------------------
    /// Return to Search mode (RWS).
    void command_search();
    /// STT-lock a target. Requires a live (non-dropped) track on it; returns
    /// false (and changes nothing) otherwise.
    bool command_track(std::uint64_t target_id);

    // --- State accessors -----------------------------------------------------
    [[nodiscard]] RadarMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::uint64_t locked_target() const noexcept { return locked_target_id_; }
    [[nodiscard]] const TrackStore& tracks() const noexcept { return tracks_; }
    [[nodiscard]] TrackStore& tracks() noexcept { return tracks_; }
    [[nodiscard]] std::uint64_t scans_performed() const noexcept { return scans_; }

private:
    void perform_scan(messaging::MessageBus& bus);

    entities::EntityHandle owner_{};
    static double& sim_time_s() {
        static double t = 0.0;
        return t;
    }

    RadarMode mode_ = RadarMode::Search;
    std::uint64_t locked_target_id_ = 0;
    TrackStore tracks_{"blue", TrackStoreConfig{}};  // re-baked on first update
    std::mt19937 rng_{};
    bool initialized_ = false;
    double scan_timer_ = 0.0;
    bool phase_primed_ = false;        // AGG-2b: scan_phase_s applied once
    std::uint64_t scans_ = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> detection_counts_;
};

} // namespace f4::sensors

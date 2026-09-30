// f4-sensors/src/radar_component.cpp — the airborne radar scan loop.
//
// Each scan: build the candidate set (search volume contents, or the locked
// target in Track mode), roll detection_probability() per candidate against
// a seeded mt19937, feed hits into the TrackStore, then decay_untracked()
// the misses and publish acquired/dropped transitions.

#include <f4/sensors/radar_component.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <f4/geo/relative.hpp>
#include <f4/sensors/ecm.hpp>

namespace f4::sensors {

namespace {

constexpr double kStationarySpeedFps = 1.0;  // below this, treat as nose-on

inline double wrap_2pi(double a) noexcept {
    while (a < 0.0)  a += 2.0 * M_PI;
    while (a >= 2.0 * M_PI) a -= 2.0 * M_PI;
    return a;
}

inline double angle_diff(double a, double b) noexcept {
    double d = a - b;
    while (d >  M_PI) d -= 2.0 * M_PI;
    while (d < -M_PI) d += 2.0 * M_PI;
    return d;
}

} // namespace

void RadarSimComponent::command_search() {
    mode_ = RadarMode::Search;
    locked_target_id_ = 0;
}

bool RadarSimComponent::command_track(std::uint64_t target_id) {
    const TrackFile* t = tracks_.find(target_id);
    if (t == nullptr || t->state == TrackState::Dropped) {
        return false;  // cannot lock what the radar is not tracking
    }
    mode_ = RadarMode::Track;
    locked_target_id_ = target_id;
    return true;
}

void RadarSimComponent::update(double dt, messaging::MessageBus& bus) {
    if (!owner_.valid() || owner_.world() == nullptr) return;
    if (!initialized_) {
        // First-tick bake: config fields set after add<>() are honored.
        tracks_ = TrackStore{own_team, track_config};
        rng_.seed(rng_seed);
        initialized_ = true;
    }
    if (!phase_primed_) {
        // AGG-2b: the per-unit sweep phase (scan_phase_s), applied once on
        // the first update — the same first-tick bake the track store
        // rides, so spawn code that sets the phase after add<>() is
        // honored. 0.0 (the default) primes the timer at exactly the
        // pre-AGG-2b starting value: every pre-existing scan schedule is
        // byte-identical. A positive phase shifts THIS radar's sweeps
        // inside its interval (the reference's per-unit jitter — "spread
        // the herd"); the fmod carry below keeps the cadence exact.
        phase_primed_ = true;
        if (scan_interval_s > 0.0) {
            scan_timer_ = std::fmod(scan_phase_s, scan_interval_s);
        }
    }
    scan_timer_ += dt;
    if (scan_timer_ < scan_interval_s) return;
    // Carry the remainder so the scan rate stays exact regardless of tick
    // length (scan_interval 1.0 s on 0.2 s ticks fires on ticks 5, 10, ...).
    scan_timer_ = std::fmod(scan_timer_, scan_interval_s);
    perform_scan(bus);
}

void RadarSimComponent::perform_scan(messaging::MessageBus& bus) {
    const auto* world = owner_.world();
    const auto* own_tf = owner_.get<entities::TransformComponent>();
    if (world == nullptr || own_tf == nullptr) return;

    const double now = sim_time();
    ++scans_;

    // --- Candidate set -----------------------------------------------------
    // Search: every other transform-bearing entity. Track: only the locked
    // target (the antenna is parked); if it vanished, go back to search.
    const f4::geo::WorldPosition own_pos = own_tf->position;
    const f4::math::Vec3<double> own_vel{own_tf->vx, own_tf->vy, own_tf->vz};
    const double cutoff_ft =
        8.0 * params.reference_range_nm * 6076.11548;  // the range pre-gate

    std::vector<entities::EntityId> candidates;
    if (mode_ == RadarMode::Track) {
        entities::EntityHandle locked(entities::EntityId{locked_target_id_},
                                      const_cast<entities::EntityWorld*>(world));
        if (locked_target_id_ != 0 && locked.get<entities::TransformComponent>() != nullptr) {
            candidates.push_back(entities::EntityId{locked_target_id_});
        } else {
            command_search();  // target gone — antenna returns to sweep
        }
    }
    if (mode_ == RadarMode::Search) {
        // AGG-2b: the candidate walk rides the world's air-picture roster
        // (f4-entities' AirPictureRoster — the wired SpatialIndex term).
        // The roster holds the NON-CLUTTER membership, refreshed against
        // the structural epoch plus a behavioral-flip cadence, and shared
        // across every radar and the picture walk — the pre-AGG-2b walk
        // resolved the full transform bucket PER RADAR PER SCAN only to
        // reject 99.8% of it with the clutter gate (FID_OPT_PLAN §5's
        // 20.8 s residual). The per-member work below is the pre-OPT-3
        // scan's own loop, re-applied FRESH: the handle resolution the
        // FID-OPT-3 ref-walk avoided is now paid on the small member set
        // (~100-200 of ~7,400), and the clutter + range gates re-run
        // idempotently over fresh transforms — a member that landed since
        // the roster's rebuild is skipped exactly as the uncached walk
        // would skip it. The roster's members() is entity-index order (the
        // bucket order), so the candidate SET, its ORDER, and the RNG
        // stream the detection rolls consume are the pre-AGG-2b scan's for
        // the shared population; the only observable delta is the roster's
        // bounded flip latency (a parked aircraft that starts moving
        // becomes a candidate within one revalidation window instead of
        // instantly — AGGREGATE_CLOCK_PLAN §5's re-pinning covers it).
        const_cast<entities::EntityWorld*>(world)->refresh_air_roster(
            now, roster_revalidate_s);
        for (const auto eid : world->air_roster()) {
            if (eid.value == owner_.id().value) continue;
            entities::EntityHandle h(eid,
                                     const_cast<entities::EntityWorld*>(world));
            const auto* tf = h.get<entities::TransformComponent>();
            if (tf == nullptr) continue;  // died since the rebuild
            if (tf->is_ground_clutter()) continue;
            const double dxr = tf->position.x - own_pos.x;
            const double dyr = tf->position.y - own_pos.y;
            const double dzr = tf->position.z - own_pos.z;
            if (dxr * dxr + dyr * dyr + dzr * dzr >
                cutoff_ft * cutoff_ft) {
                continue;
            }
            candidates.push_back(eid);
        }
    }

    // --- Roll each candidate against the detection model --------------------
    std::uniform_real_distribution<double> uniform01{0.0, 1.0};

    // --- ECM burn-through (the jamming tranche) ------------------------------
    // The live ENEMY jammers, once per scan: bearing from this radar and
    // their weight AT THIS RADAR — one-way noise power falls with 1/r²,
    // saturating inside the jammer's burn-through range. A corpse stops
    // jamming; a friendly pod never degrades an own-team radar. No
    // EcmComponent in the world (the fidelity gate's off state) → the
    // bucket walk is one index probe and the per-candidate block below is
    // arithmetic-free: every pre-ECM scan is byte-identical.
    struct JammerRead {
        double bearing_rad;   // CW from north, this radar -> jammer
        double weight_scale;  // strength * min(1, (R_bt / d)²)
    };
    std::vector<JammerRead> jammers;
    for (const auto& [jid, ecm] :
         world->with_component_ref<EcmComponent>()) {
        if (!ecm->enabled) continue;
        if (ecm->own_team == own_team) continue;  // friendly pod
        entities::EntityHandle jh(jid,
                                  const_cast<entities::EntityWorld*>(world));
        const auto* jtf = jh.get<entities::TransformComponent>();
        if (jtf == nullptr) continue;
        if (const auto* dmg = jh.get<entities::DamageStateComponent>();
            dmg != nullptr && dmg->killed) {
            continue;  // corpses don't jam
        }
        const double dxj = jtf->position.x - own_pos.x;
        const double dyj = jtf->position.y - own_pos.y;
        const double dzj = jtf->position.z - own_pos.z;
        const double d_nm =
            std::sqrt(dxj * dxj + dyj * dyj + dzj * dzj) / 6076.11548;
        const double square =
            (ecm->burn_through_range_nm * ecm->burn_through_range_nm) /
            std::max(d_nm * d_nm, 1e-6);
        jammers.push_back(JammerRead{
            std::atan2(dxj, dyj),  // CW from north (ENU convention)
            ecm->jamming_strength * std::min(1.0, square)});
    }

    for (const auto eid : candidates) {
        entities::EntityHandle h(eid, const_cast<entities::EntityWorld*>(world));
        const auto* tf = h.get<entities::TransformComponent>();
        if (tf == nullptr) continue;

        const f4::geo::WorldPosition tgt_pos = tf->position;
        const f4::math::Vec3<double> tgt_vel{tf->vx, tf->vy, tf->vz};

        // Ground-clutter rejection (the C6 campaign-scale finding):
        // the air-to-air radar tracks air picture, not parking ramps.
        // The M2 placeholder walked every transform-bearing entity,
        // which at 2-aircraft scenario scale was free — at campaign
        // scale (48 armed radars x ~4,400 entities per sweep) it
        // detected HALF of all candidates every second (measured:
        // 125k track-creating detections/s in a 36-s war), flooding
        // the track stores with ground clutter at both a perf and a
        // fidelity cost. The shared TransformComponent::is_ground_
        // clutter predicate (stationary AND below every Korea terrain
        // post) is also the reference's shape: FreeFalcon's radar air
        // picture never paints parked vehicles. A stationary entity at
        // altitude still tracks.
        // (FID-OPT-3: the Search walk above pre-applies this gate so the
        // handle resolution is only paid by survivors — this repeated
        // check is idempotent and keeps Track-mode candidates gated.)
        if (tf->is_ground_clutter()) {
            continue;
        }

        // Range pre-rejection (the campaign-scale scan walk): the
        // detection model's maximum range is reference_range x
        // fourth-root(rcs ratio) x closure. For every real signature
        // (fighter-scale RCS grids, closure bounded by the model) that
        // product sits far below 8x the reference range, so a candidate
        // beyond the cutoff can never roll a detection — skipping it
        // BEFORE the geometry chain (to_bra, atan2, LOS, volume trig)
        // is output-identical to the pd==0 path while cutting the
        // per-candidate cost at campaign scale (a populated save:
        // ~4,400 transform-bearing entities per radar sweep; most sit
        // beyond any radar's horizon).
        // (FID-OPT-3: pre-applied by the Search walk above, same shape.)
        {
            constexpr double kScanCutoffMultiplier = 8.0;
            constexpr double kNmToFt = 6076.11548;
            const double loop_cutoff_ft =
                kScanCutoffMultiplier * params.reference_range_nm * kNmToFt;
            const double dxr = tgt_pos.x - own_pos.x;
            const double dyr = tgt_pos.y - own_pos.y;
            const double dzr = tgt_pos.z - own_pos.z;
            if (dxr * dxr + dyr * dyr + dzr * dzr >
                loop_cutoff_ft * loop_cutoff_ft) {
                continue;
            }
        }

        // Geometry (ENU: x=east, y=north, z=up).
        const f4::geo::BRA bra = f4::geo::to_bra(own_pos, tgt_pos);
        const double dx = tgt_pos.x - own_pos.x;
        const double dy = tgt_pos.y - own_pos.y;
        const double dz = tgt_pos.z - own_pos.z;
        const double horizontal = std::sqrt(dx * dx + dy * dy);
        const double elevation = std::atan2(dz, std::max(horizontal, 1.0));

        // Signature: RCS from SignatureComponent (default = radar reference),
        // aspect off the target's nose, closure along the line of sight.
        // SimData upgrade: a SignatureComponent carrying an RCS grid
        // (SIGDATA/RCSDAT) routes the whole lobe shape through the data.
        TargetSignature sig;
        if (const auto* signature = h.get<SignatureComponent>()) {
            sig.rcs_m2 = signature->rcs_m2;
            sig.rcs_grid = signature->rcs_grid;
        } else {
            sig.rcs_m2 = params.reference_rcs_m2;
        }
        // Grid elevation: the radar's LOS elevation is the target-
        // referenced elevation approximation available here (a target
        // flying level under the radar reads low in the grid — the
        // generic grids are elevation-flat anyway).
        sig.elevation_deg = elevation * (180.0 / M_PI);
        const double tgt_speed = tgt_vel.length();
        if (tgt_speed > kStationarySpeedFps) {
            const double tgt_heading = std::atan2(tgt_vel.x, tgt_vel.y);  // CW from north
            const double bearing_to_radar = f4::geo::to_bra(tgt_pos, own_pos).bearing_rad;
            sig.aspect_rad = std::abs(angle_diff(bearing_to_radar, tgt_heading));
        } else {
            sig.aspect_rad = 0.0;  // stationary: nose-on assumption
        }
        const f4::math::Vec3<double> los{
            (tgt_pos.x - own_pos.x) / std::max(bra.range_ft, 1.0),
            (tgt_pos.y - own_pos.y) / std::max(bra.range_ft, 1.0),
            (tgt_pos.z - own_pos.z) / std::max(bra.range_ft, 1.0)};
        const f4::math::Vec3<double> rel_vel = tgt_vel - own_vel;
        sig.closure_fps = -(rel_vel.dot(los));  // positive = range shrinking

        // Volume containment (Search mode only — in Track the antenna is
        // parked on the target, no bar geometry applies).
        if (mode_ == RadarMode::Search &&
            !scan.contains(bra.bearing_rad, elevation, bra.range_nm())) {
            continue;
        }

        // The detection roll — degraded by the enemy jammers in the
        // antenna's receiving corridor toward this candidate (the ECM
        // burn-through model): each jammer inside the beam toward the
        // candidate raises the noise floor, the effective detection
        // range shrinks by (1 - W), and the ramp reads the STRETCHED
        // range. W caps at 0.95 (a blanket never fully blinds — closing
        // the range is how the echo wins through). No jammers → W = 0 →
        // the roll is exactly the pre-ECM arithmetic and the RNG stream
        // is untouched.
        double pd = 0.0;
        {
            double jamming = 0.0;
            if (!jammers.empty()) {
                const double bearing_to_tgt =
                    std::atan2(tgt_pos.x - own_pos.x, tgt_pos.y - own_pos.y);
                for (const auto& j : jammers) {
                    if (std::abs(angle_diff(bearing_to_tgt, j.bearing_rad)) <=
                        scan.azimuth_half_width_rad) {
                        jamming += j.weight_scale;
                    }
                }
                jamming = std::min(jamming, 0.95);
            }
            const double range_for_ramp =
                jamming > 0.0 ? bra.range_nm() / (1.0 - jamming)
                              : bra.range_nm();
            pd = detection_probability(params, sig, range_for_ramp);
        }
        if (pd <= 0.0) continue;
        if (uniform01(rng_) >= pd) continue;

        // --- Detection: create or refresh the track -------------------------
        const TrackFile* existing = tracks_.find(eid.value);
        const bool was_live = existing != nullptr &&
                              existing->state != TrackState::Dropped;

        std::string team;
        if (auto team_tag = h.get_tag(entities::tags::TEAM)) {
            if (const auto* s = team_tag->as_string()) team = *s;
        }
        std::string nctr;
        const std::uint32_t count = ++detection_counts_[eid.value];
        if (count >= nctr_after_scans) {
            if (const auto* ident = h.get<entities::CampaignIdentityComponent>()) {
                nctr = ident->callsign;
            }
        }

        tracks_.on_detection(eid.value, tgt_pos, tgt_vel, now, team, nctr);
        if (!was_live) {
            bus.publish(RadarTrackAcquiredMessage{
                owner_.id().value, eid.value, now});
        }
    }

    // --- Decay the misses, publish drops --------------------------------------
    for (const auto dropped_id : tracks_.decay_untracked(now)) {
        bus.publish(RadarTrackDroppedMessage{
            owner_.id().value, dropped_id, now});
        if (mode_ == RadarMode::Track && dropped_id == locked_target_id_) {
            command_search();  // lock cannot outlive its track
        }
    }
}

} // namespace f4::sensors

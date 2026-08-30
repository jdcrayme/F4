// f4-sensors/src/radar_component.cpp — the airborne radar scan loop.
//
// Each scan: build the candidate set (search volume contents, or the locked
// target in Track mode), roll detection_probability() per candidate against
// a seeded mt19937, feed hits into the TrackStore, then decay_untracked()
// the misses and publish acquired/dropped transitions.

#include <f4/sensors/radar_component.hpp>

#include <algorithm>
#include <cmath>

#include <f4/geo/relative.hpp>

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
        for (const auto eid : world->with_component<entities::TransformComponent>()) {
            if (eid.value == owner_.id().value) continue;
            candidates.push_back(eid);
        }
    }

    // --- Roll each candidate against the detection model --------------------
    std::uniform_real_distribution<double> uniform01{0.0, 1.0};

    const f4::geo::WorldPosition own_pos = own_tf->position;
    const f4::math::Vec3<double> own_vel{own_tf->vx, own_tf->vy, own_tf->vz};

    for (const auto eid : candidates) {
        entities::EntityHandle h(eid, const_cast<entities::EntityWorld*>(world));
        const auto* tf = h.get<entities::TransformComponent>();
        if (tf == nullptr) continue;

        const f4::geo::WorldPosition tgt_pos = tf->position;
        const f4::math::Vec3<double> tgt_vel{tf->vx, tf->vy, tf->vz};

        // Geometry (ENU: x=east, y=north, z=up).
        const f4::geo::BRA bra = f4::geo::to_bra(own_pos, tgt_pos);
        const double dx = tgt_pos.x - own_pos.x;
        const double dy = tgt_pos.y - own_pos.y;
        const double dz = tgt_pos.z - own_pos.z;
        const double horizontal = std::sqrt(dx * dx + dy * dy);
        const double elevation = std::atan2(dz, std::max(horizontal, 1.0));

        // Signature: RCS from SignatureComponent (default = radar reference),
        // aspect off the target's nose, closure along the line of sight.
        TargetSignature sig;
        if (const auto* signature = h.get<SignatureComponent>()) {
            sig.rcs_m2 = signature->rcs_m2;
        } else {
            sig.rcs_m2 = params.reference_rcs_m2;
        }
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

        // The detection roll.
        const double pd = detection_probability(params, sig, bra.range_nm());
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

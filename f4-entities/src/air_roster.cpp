// f4-entities/air_roster.cpp — the air-picture roster's maintenance rule.
// See air_roster.hpp for the contract; entity.hpp's TransformComponent
// carries is_ground_clutter() — the predicate whose flips this cache
// tracks (structural: instantly via the epoch; behavioral: within the
// caller's revalidation cadence).

#include <f4/entities/air_roster.hpp>

namespace f4::entities {

void AirPictureRoster::refresh(const EntityWorld& world, double now_s,
                               double revalidate_interval_s) {
    const std::uint64_t epoch = world.structural_epoch();
    // Structural flips: the epoch moved (or nothing primed yet) —
    // rebuild now, whatever the cadence says. The epoch compare is the
    // cheap path: a quiet theater (the 4,000-parked norm) never bumps
    // it, so the steady state rides the cadence alone.
    if (!primed_ || epoch != epoch_) {
        rebuild_(world);
        last_refresh_s_ = now_s;
        return;
    }
    // Behavioral flips: the cadence. `now_s` is the host-stamped sim
    // clock — a wall clock here would make the refresh schedule (and
    // with it every detection timeline that rides the membership)
    // replay-irreproducible. interval <= 0 = revalidate every call
    // (the exact-control knob).
    if (revalidate_interval_s <= 0.0 ||
        (now_s - last_refresh_s_) >= revalidate_interval_s) {
        rebuild_(world);
        last_refresh_s_ = now_s;
    }
}

void AirPictureRoster::rebuild_(const EntityWorld& world) {
    // One pass over the transform ref bucket — the SAME walk the
    // uncached consumers ran, now amortized: once per rebuild instead
    // of once per radar scan per scan. Bucket order is entity-index
    // order by the ref index's invariant, so members() reproduces the
    // uncached walks' candidate/contact order exactly (the RNG streams
    // that ride that order are preserved).
    members_.clear();
    index_.clear();
    for (const auto& [eid, tf] :
         world.with_component_ref<TransformComponent>()) {
        if (tf->is_ground_clutter()) continue;
        members_.push_back(eid);
        // The spatial side captures positions at rebuild time — the
        // within_radius() convenience surface. Fresh values live in the
        // transforms; this index never answers for them.
        index_.insert(eid, tf->position.x, tf->position.y, tf->position.z);
    }
    epoch_ = world.structural_epoch();
    primed_ = true;
    ++rebuilds_;
}

std::vector<EntityId> AirPictureRoster::within_radius(
    double cx, double cy, double cz, double radius) const {
    return index_.query_radius(cx, cy, cz, radius);
}

} // namespace f4::entities

// f4-sensors/src/rwr.cpp — RWR pure model + world-level sweep. See rwr.hpp.

#include <f4/sensors/rwr.hpp>

#include <algorithm>
#include <cmath>

#include <f4/entities/entity.hpp>
#include <f4/geo/relative.hpp>

namespace f4::sensors {

namespace {

constexpr double kFeetPerNm = 6076.11548;

inline int warning_rank(RwrWarningType t) noexcept {
    switch (t) {
        case RwrWarningType::Launch: return 0;
        case RwrWarningType::Lock:   return 1;
        case RwrWarningType::Search: return 2;
    }
    return 3;
}

} // namespace

std::vector<RwrWarning> RwrModel::evaluate(
    const std::vector<EmitterReading>& readings,
    const f4::geo::WorldPosition& own_pos,
    double time_s) const {
    std::vector<RwrWarning> out;
    const double max_range_ft = cfg_.max_range_nm * kFeetPerNm;

    for (const auto& r : readings) {
        const f4::geo::BRA bra = f4::geo::to_bra(own_pos, r.position);
        if (bra.range_ft > max_range_ft) continue;

        // One emitter reads as its most severe class: missile beats lock
        // beats search (the same radar can be strobing and locked — lock
        // wins because it is the actionable threat).
        RwrWarningType type;
        if (r.is_missile) {
            type = RwrWarningType::Launch;
        } else if (r.is_locked_on_self) {
            type = RwrWarningType::Lock;
        } else if (r.is_illuminating_self) {
            type = RwrWarningType::Search;
        } else {
            continue;  // emitter active but not touching us
        }
        out.push_back(RwrWarning{type, r.emitter_id,
                                 bra.bearing_rad, bra.range_ft, time_s});
    }

    std::sort(out.begin(), out.end(), [](const RwrWarning& a, const RwrWarning& b) {
        const int ra = warning_rank(a.type);
        const int rb = warning_rank(b.type);
        if (ra != rb) return ra < rb;
        return a.emitter_id < b.emitter_id;
    });
    return out;
}

std::size_t update_rwr(entities::EntityWorld& world,
                       messaging::MessageBus& bus,
                       double time_s,
                       const RwrConfig& cfg) {
    const RwrModel model{cfg};

    // --- Gather emitter readings once per emitter kind -----------------------
    // Radars: every RadarSimComponent in the world. Missiles: entities with
    // the ROLE="missile" tag (geometry-based launch detection — the missile's
    // plume/seeker is the emitter; no f4-weapons dependency needed).
    struct EmitterRecord {
        std::uint64_t id;
        f4::geo::WorldPosition position;
        bool is_missile;
        bool is_locked_on_any;        // Track mode
        std::uint64_t locked_target;  // valid when is_locked_on_any
        bool is_searching;            // Search mode (sweeping)
        const RadarSimComponent* radar;  // for scan-volume tests, nullptr for missiles
    };

    std::vector<EmitterRecord> emitters;
    for (const auto eid : world.with_component<RadarSimComponent>()) {
        entities::EntityHandle h(eid, &world);
        const auto* radar = h.get<RadarSimComponent>();
        const auto* tf = h.get<entities::TransformComponent>();
        if (radar == nullptr || tf == nullptr) continue;
        emitters.push_back(EmitterRecord{
            eid.value, tf->position,
            /*is_missile=*/false,
            radar->mode() == RadarMode::Track,
            radar->locked_target(),
            radar->mode() == RadarMode::Search,
            radar});
    }
    for (const auto eid : world.with_component<entities::TransformComponent>()) {
        entities::EntityHandle h(eid, &world);
        auto role = h.get_tag(entities::tags::ROLE);
        if (!role.has_value()) continue;
        const auto* s = role->as_string();
        if (s == nullptr || *s != "missile") continue;
        const auto* tf = h.get<entities::TransformComponent>();
        if (tf == nullptr) continue;
        emitters.push_back(EmitterRecord{
            eid.value, tf->position,
            /*is_missile=*/true,
            false, 0, false, nullptr});
    }

    // --- Update every victim's RWR -------------------------------------------
    std::size_t updated = 0;
    for (const auto vid : world.with_component<RwrComponent>()) {
        entities::EntityHandle victim(vid, &world);
        auto* rwr = victim.get<RwrComponent>();
        const auto* vt = victim.get<entities::TransformComponent>();
        if (rwr == nullptr || vt == nullptr) continue;

        std::vector<EmitterReading> readings;
        for (const auto& e : emitters) {
            if (e.id == vid.value) continue;  // own radar never warns itself
            EmitterReading r;
            r.emitter_id = e.id;
            r.position = e.position;
            r.is_missile = e.is_missile;

            if (e.is_locked_on_any && e.locked_target == vid.value) {
                r.is_locked_on_self = true;   // parked on us: LOCK
            } else if (e.is_searching && e.radar != nullptr) {
                // Search strobe: the beam currently covers us if we are in
                // the swept volume AND within the radar's detection reach.
                const f4::geo::BRA bra = f4::geo::to_bra(e.position, vt->position);
                const double dx = vt->position.x - e.position.x;
                const double dy = vt->position.y - e.position.y;
                const double dz = vt->position.z - e.position.z;
                const double horizontal = std::sqrt(dx * dx + dy * dy);
                const double elevation = std::atan2(dz, std::max(horizontal, 1.0));
                if (e.radar->scan.contains(bra.bearing_rad, elevation, bra.range_nm())) {
                    r.is_illuminating_self = true;
                }
            }
            if (r.is_missile || r.is_locked_on_self || r.is_illuminating_self) {
                readings.push_back(r);
            }
        }

        const std::vector<RwrWarning> previous = rwr->warnings;
        rwr->warnings = model.evaluate(readings, vt->position, time_s);

        const bool lock_was = rwr->lock_active;
        const bool launch_was = rwr->launch_active;
        rwr->lock_active = std::any_of(rwr->warnings.begin(), rwr->warnings.end(),
            [](const RwrWarning& w) { return w.type == RwrWarningType::Lock; });
        rwr->launch_active = std::any_of(rwr->warnings.begin(), rwr->warnings.end(),
            [](const RwrWarning& w) { return w.type == RwrWarningType::Launch; });
        rwr->new_lock = rwr->lock_active && !lock_was;
        rwr->new_launch = rwr->launch_active && !launch_was;

        // Publish transitions: every Lock/Launch emitter not in the previous
        // picture of the same type. Search strobes stay component-state.
        for (const auto& w : rwr->warnings) {
            if (w.type == RwrWarningType::Search) continue;
            const bool known = std::any_of(previous.begin(), previous.end(),
                [&](const RwrWarning& p) {
                    return p.type == w.type && p.emitter_id == w.emitter_id;
                });
            if (!known) {
                bus.publish(RwrWarningMessage{
                    vid.value, w.type, w.emitter_id,
                    w.bearing_rad, w.range_ft, time_s});
            }
        }
        ++updated;
    }
    return updated;
}

} // namespace f4::sensors

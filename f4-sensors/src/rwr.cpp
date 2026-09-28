// f4-sensors/src/rwr.cpp — RWR pure model + world-level sweep. See rwr.hpp.

#include <f4/sensors/rwr.hpp>
#include <f4/sensors/ecm.hpp>
#include <f4/geo/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <f4/entities/entity.hpp>
#include <f4/geo/relative.hpp>

namespace f4::sensors {

namespace {

constexpr double kFeetPerNm = f4::geo::FEET_PER_NM;  // single-sourced (was a literal)

inline int warning_rank(RwrWarningType t) noexcept {
    switch (t) {
        case RwrWarningType::Launch:  return 0;
        case RwrWarningType::Lock:    return 1;
        case RwrWarningType::Jamming: return 2;
        case RwrWarningType::Search:  return 3;
    }
    return 4;
}

} // namespace

std::vector<RwrWarning> RwrModel::evaluate(
    const std::vector<EmitterReading>& readings,
    const f4::geo::WorldPosition& own_pos,
    double time_s,
    double receiver_heading_rad) const {
    std::vector<RwrWarning> out;
    // SimData (.RWR) parameters: sensitivity scales the receiver range
    // (generic 1.0 = unchanged); FOV limits gate emitters (generic
    // 180/90 = nothing gated).
    const double max_range_ft =
        cfg_.max_range_nm * cfg_.sensitivity * kFeetPerNm;
    const double el_limit_rad = cfg_.el_limit_deg / (180.0 / M_PI);
    const double az_limit_rad = cfg_.az_limit_deg / (180.0 / M_PI);
    const bool gate_azimuth = std::isfinite(receiver_heading_rad);

    for (const auto& r : readings) {
        const f4::geo::BRA bra = f4::geo::to_bra(own_pos, r.position);
        if (bra.range_ft > max_range_ft) continue;

        // Receiver FOV (elevation is world-frame and always tested;
        // azimuth needs the receiver's heading — NaN passes through,
        // which is the generic.rwr omni contract).
        const double dx = r.position.x - own_pos.x;
        const double dy = r.position.y - own_pos.y;
        const double dz = r.position.z - own_pos.z;
        const double horizontal = std::sqrt(dx * dx + dy * dy);
        const double emitter_elevation =
            std::atan2(dz, std::max(horizontal, 1.0));
        if (emitter_elevation > el_limit_rad ||
            emitter_elevation < -el_limit_rad) {
            continue;
        }
        if (gate_azimuth) {
            double rel = bra.bearing_rad - receiver_heading_rad;
            while (rel > M_PI) rel -= 2.0 * M_PI;
            while (rel < -M_PI) rel += 2.0 * M_PI;
            if (std::abs(rel) > az_limit_rad) continue;
        }

        // One emitter reads as its most severe class: missile beats lock
        // beats jamming beats search (the same radar can be strobing and
        // locked — lock wins because it is the actionable threat; an
        // entity that both radars and jams reads as whichever of its
        // readings is most severe).
        RwrWarningType type;
        if (r.is_missile) {
            type = RwrWarningType::Launch;
        } else if (r.is_locked_on_self) {
            type = RwrWarningType::Lock;
        } else if (r.is_jamming_self) {
            type = RwrWarningType::Jamming;
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
        bool is_jamming;              // live EcmComponent (nullptr radar path)
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
            radar,
            /*is_jamming=*/false});
    }
    // Missile emitters: the ROLE="missile" tag bucket — O(1) through the
    // Phase-D tag index (with_tag_ref), NOT a walk over every
    // TransformComponent entity. At campaign scale (a populated save:
    // ~4,400 entities) the old walk cost a 35 KB id copy + ~4,400 tag
    // lookups per tick just to find the handful of live missiles; the
    // bucket holds exactly them. (geometry-based launch detection — the
    // missile's plume/seeker is the emitter; no f4-weapons dependency.)
    for (const auto& eid : world.with_tag_ref(
             entities::tags::ROLE,
             entities::TagValue::from(std::string("missile")))) {
        entities::EntityHandle h(eid, &world);
        const auto* tf = h.get<entities::TransformComponent>();
        if (tf == nullptr) continue;
        emitters.push_back(EmitterRecord{
            eid.value, tf->position,
            /*is_missile=*/true,
            false, 0, false, nullptr,
            /*is_jamming=*/false});
    }
    // Jammers (the ECM tranche): every live EcmComponent is an emitter —
    // the noise its pod puts out reaches receivers inside RWR range. A
    // corpse stops jamming (the corpse rule every emitter obeys). An
    // entity that BOTH radars and jams keeps its radar record and gains
    // the flag (one record per emitter; the model reads the most severe
    // class). No EcmComponent in the world (the fidelity gate's off
    // state) → the bucket is empty and this loop costs one index probe.
    for (const auto& [eid, ecm] :
         world.with_component_ref<EcmComponent>()) {
        if (!ecm->enabled) continue;
        bool seen = false;
        for (auto& e : emitters) {
            if (e.id != eid.value) continue;
            e.is_jamming = true;   // the radar/jammer hybrid
            seen = true;
            break;
        }
        if (seen) continue;
        entities::EntityHandle h(eid, &world);
        const auto* tf = h.get<entities::TransformComponent>();
        if (tf == nullptr) continue;
        if (const auto* dmg = h.get<entities::DamageStateComponent>();
            dmg != nullptr && dmg->killed) {
            continue;  // corpses don't emit
        }
        emitters.push_back(EmitterRecord{
            eid.value, tf->position,
            /*is_missile=*/false,
            false, 0, false, nullptr,
            /*is_jamming=*/true});
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
            r.is_jamming_self = e.is_jamming;

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
            if (r.is_missile || r.is_locked_on_self || r.is_illuminating_self ||
                r.is_jamming_self) {
                readings.push_back(r);
            }
        }

        // Receiver heading for the azimuth FOV gate: the victim's
        // velocity when it is actually moving; NaN (omni) when parked —
        // a stationary receiver has no nose to gate against, and the
        // generic.rwr default is omni anyway.
        double receiver_heading = std::numeric_limits<double>::quiet_NaN();
        const double speed =
            std::sqrt(vt->vx * vt->vx + vt->vy * vt->vy + vt->vz * vt->vz);
        if (speed > 1.0) {
            receiver_heading = std::atan2(vt->vx, vt->vy);  // CW from north
        }

        const std::vector<RwrWarning> previous = rwr->warnings;
        rwr->warnings =
            model.evaluate(readings, vt->position, time_s, receiver_heading);

        const bool lock_was = rwr->lock_active;
        const bool launch_was = rwr->launch_active;
        rwr->lock_active = std::any_of(rwr->warnings.begin(), rwr->warnings.end(),
            [](const RwrWarning& w) { return w.type == RwrWarningType::Lock; });
        rwr->launch_active = std::any_of(rwr->warnings.begin(), rwr->warnings.end(),
            [](const RwrWarning& w) { return w.type == RwrWarningType::Launch; });
        rwr->new_lock = rwr->lock_active && !lock_was;
        rwr->new_launch = rwr->launch_active && !launch_was;

        // Publish transitions: every Lock/Launch/Jamming emitter not in the
        // previous picture of the same type. Search strobes stay
        // component-state.
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

// f4-ai/src/sensor_fusion.cpp
//
// SensorFusion implementation. See sensor_fusion.hpp for design notes.
//
// FreeFalcon reference (sfusion.cpp):
//   - 4 detection sources (GCI/RWR/Radar/Visual); any one enables canSee
//   - Threat scoring: hostile +50 if combatClass 2-4
//   - ATA > 90deg -> score /= 2 (target pointed away = less threatening)
//   - Combat class guess: speed>300kts OR alt<10000ft -> fighter(4)
//   - Skill-dependent update interval (Recruit=10s, Rookie=7s, Veteran=5s, Ace=1s)
//   - EWMA smoothing on ataDot/rangeDot (0.85/0.15 blend)
//
// All geometry goes through f4::geo primitives (to_bra, WorldPosition) so
// the AI never touches raw coordinate math — the strong-typed position
// system is the source of truth.

#include "f4/ai/sensor_fusion.hpp"

#include <f4/math/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <f4/geo/constants.hpp>
#include <f4/geo/relative.hpp>

namespace f4::ai {

namespace {

using f4::entities::TransformComponent;
using f4::entities::EntityHandle;
using f4::entities::EntityId;
using f4::entities::tags::ROLE;
using f4::entities::tags::TEAM;
using f4::geo::WorldPosition;
using f4::geo::BRA;
using f4::geo::FEET_PER_NM;
using f4::geo::PI;
using f4::geo::TWO_PI;

// Skill-dependent parameters (per AI_IMPLEMENTATION_PLAN §9).
// Indexed by static_cast<std::size_t>(SkillLevel).
constexpr double UPDATE_INTERVALS_SEC[] = {
    10.0,  // Recruit
    7.0,   // Rookie
    5.0,   // Veteran
    1.0,   // Ace
};

constexpr double REACTION_DELAYS_SEC[] = {
    3.0,   // Recruit
    2.0,   // Rookie
    1.0,   // Veteran
    0.3,   // Ace
};

// 1 knot = 1.68781 ft/s (exact: 1 knot = 1852 m/hr, 1 m = 3.28084 ft, 1 hr = 3600 s)
constexpr double FPS_PER_KNOT = 1.687809857;

// Normalize angle to [0, 2*pi).
inline double wrap_2pi(double a) noexcept {
    while (a < 0.0)  a += TWO_PI;
    while (a >= TWO_PI) a -= TWO_PI;
    return a;
}

// Shortest signed angular difference (a - b), result in [-pi, pi].
inline double angle_diff(double a, double b) noexcept {
    double d = a - b;
    while (d >  PI) d -= TWO_PI;
    while (d < -PI) d += TWO_PI;
    return d;
}

// Velocity-vector heading (radians, CW from north, [0, 2*pi)).
// In the sim-local ENU frame: heading = atan2(east, north) = atan2(v.x, v.y).
inline double velocity_heading_rad(const WorldPosition& v) noexcept {
    if (v.x == 0.0 && v.y == 0.0) return 0.0;
    double h = std::atan2(v.x, v.y);  // atan2(east, north)
    if (h < 0.0) h += TWO_PI;
    return h;
}

inline double speed_fps(const WorldPosition& v) noexcept {
    return f4::math::Vec3d{v.x, v.y, v.z}.length();
}

// FreeFalcon combat_class heuristic (sfusion.cpp:504):
//   speed > 300 kts OR alt < 10000 ft => fighter (4)
// We extend the ladder with intermediate classes for richer threat scoring:
//   0 = stationary, 1 = possible, 2 = armed, 3 = maneuvering, 4 = fighter.
// The fighter threshold (4) matches FreeFalcon exactly so the +50 hostile
// bonus fires at the same inputs.
int guess_combat_class(double speed_fps_val, double alt_ft) noexcept {
    const double speed_kts = speed_fps_val / FPS_PER_KNOT;
    if (speed_kts > 300.0 || alt_ft < 10000.0) return 4;  // fighter
    if (speed_kts > 150.0) return 3;                       // maneuvering
    if (speed_kts >  50.0) return 2;                       // armed
    if (speed_kts >   0.0) return 1;                       // possible
    return 0;                                               // stationary
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void SensorFusion::initialize(std::uint64_t ownship_id,
                              entities::EntityWorld& world,
                              messaging::MessageBus& bus,
                              SkillLevel skill,
                              Config cfg) {
    ownship_id_ = ownship_id;
    world_      = &world;
    bus_        = &bus;
    skill_      = skill;
    cfg_        = cfg;
    update_timer_ = 0.0;  // refresh on first update()
    targets_.clear();
    prev_targets_.clear();
}

void SensorFusion::update(double dt) {
    if (!world_) return;
    update_timer_ -= dt;
    if (update_timer_ <= 0.0) {
        rebuild_target_list();
        update_timer_ = update_interval_sec(skill_);
    }
}

void SensorFusion::force_refresh() {
    if (!world_) return;
    rebuild_target_list();
    update_timer_ = update_interval_sec(skill_);
}

const TargetInfo* SensorFusion::primary_target() const noexcept {
    const TargetInfo* best = nullptr;
    for (const auto& t : targets_) {
        if (!can_see(t)) continue;
        if (!best || t.threat_score > best->threat_score) {
            best = &t;
        }
    }
    return best;
}

const TargetInfo* SensorFusion::threat_target() const noexcept {
    const TargetInfo* best = nullptr;
    for (const auto& t : targets_) {
        if (!can_see(t)) continue;
        if (t.combat_class < 2) continue;  // only fighters / maneuvering
        if (!t.is_hostile) continue;       // 2-ship: never fight a friendly
        if (!best || t.threat_score > best->threat_score) {
            best = &t;
        }
    }
    return best;
}

const TargetInfo* SensorFusion::sorted_threat_target(
    std::uint64_t lead_engaged_id) const noexcept {
    // Two passes over the same filtered set as threat_target(): first the
    // hostiles the LEAD has not taken (the free bandits), then — only if
    // that pass found nothing — the lead's own target (support the kill).
    // A free bandit with a LOWER score outranks the lead's engaged target
    // with the highest score: that is the point of the sort.
    const TargetInfo* best_free = nullptr;
    const TargetInfo* lead_target = nullptr;
    for (const auto& t : targets_) {
        if (!can_see(t)) continue;
        if (t.combat_class < 2) continue;
        if (!t.is_hostile) continue;
        if (t.entity_id == lead_engaged_id) {
            lead_target = &t;
            continue;
        }
        if (best_free == nullptr || t.threat_score > best_free->threat_score) {
            best_free = &t;
        }
    }
    if (best_free != nullptr) return best_free;
    return lead_target;  // nullptr when the lead isn't fighting either
}

const TargetInfo* SensorFusion::missile_threat() const noexcept {
    const TargetInfo* best = nullptr;
    for (const auto& t : targets_) {
        if (!can_see(t)) continue;
        if (!t.is_missile) continue;
        // HOSTILE missiles only (M3 tactics): your own missile (team copied
        // from you at launch) or a wingman's missile passing by is not a
        // threat to defend against — without this filter the shooter's own
        // AMRAAM became its nearest "incoming" missile the tick after
        // release and the brain immediately broke off its own engagement.
        if (!t.is_hostile) continue;
        if (!best || t.range_ft < best->range_ft) {
            best = &t;
        }
    }
    return best;
}

double SensorFusion::update_interval_sec(SkillLevel s) noexcept {
    const auto i = static_cast<std::size_t>(s);
    if (i < std::size(UPDATE_INTERVALS_SEC)) return UPDATE_INTERVALS_SEC[i];
    return 5.0;  // default Veteran
}

double SensorFusion::reaction_delay_sec(SkillLevel s) noexcept {
    const auto i = static_cast<std::size_t>(s);
    if (i < std::size(REACTION_DELAYS_SEC)) return REACTION_DELAYS_SEC[i];
    return 2.0;  // default Rookie
}

// ============================================================================
// Internals
// ============================================================================

void SensorFusion::rebuild_target_list() {
    // Snapshot the previous target list for EWMA rate computation.
    prev_targets_ = std::move(targets_);
    targets_.clear();

    if (!world_ || ownship_id_ == 0) return;

    // Find ownship. with_component<TransformComponent>() returns every entity
    // that has a transform — we filter by id. (No direct EntityId lookup API
    // exists; the linear scan is acceptable because this runs at most once
    // per skill-dependent update interval, not per minor frame.)
    //
    // Phase D note: we intentionally do NOT use with_tag(TEAM, "red") here
    // even though it's now O(1). Radar detects ALL contacts within range
    // (friendly and hostile) — geometry is computed for every transform
    // entity, and the team tag is only consulted later to classify
    // hostility. Switching to a team-filtered query would silently drop
    // friendly contacts from the target list, breaking RWR/situational
    // awareness.
    auto candidates = world_->with_component<TransformComponent>();

    const TransformComponent* ownship_tf = nullptr;
    WorldPosition ownship_pos{};
    WorldPosition ownship_vel{};

    for (auto eid : candidates) {
        if (eid.value == ownship_id_) {
            EntityHandle h(eid, world_);
            ownship_tf = h.get<TransformComponent>();
            if (ownship_tf) {
                ownship_pos = ownship_tf->position;
                ownship_vel = ownship_tf->velocity();
            }
            // Cache the ownship's team for the own-relative hostility rule.
            // Empty = no tag => legacy blue-perspective rule downstream.
            if (auto team_tag = h.get_tag(TEAM)) {
                if (const auto* s = team_tag->as_string()) {
                    own_team_ = *s;
                }
            }
            break;
        }
    }
    if (!ownship_tf) return;  // ownship not in world (shouldn't happen)

    // Build target list from every OTHER entity with a TransformComponent.
    for (auto eid : candidates) {
        if (eid.value == ownship_id_) continue;

        EntityHandle h(eid, world_);
        auto* tf = h.get<TransformComponent>();
        if (!tf) continue;

        TargetInfo t;
        t.entity_id = eid.value;
        t.position  = tf->position;
        t.velocity  = tf->velocity();

        // Hostility check via team tag — OWN-RELATIVE (M3 tactics): a
        // target is hostile when its team differs from the OWNSHIP's team.
        // The old blue-perspective rule (team "red" => hostile) made a
        // red-team brain blind to blue fighters, so two-sided AI combat
        // could never work: the red bandit scored the blue shooter at 0,
        // never saw a threat, never defended. When the ownship itself
        // carries NO team tag (single-ship legacy tests), the legacy rule
        // ("red" => hostile) is kept so existing scenarios behave
        // identically. (Future: campaign team-stance data — TeamComponent
        // stance[other_slot] — replaces the 2-team assumption.)
        if (auto team_tag = h.get_tag(TEAM)) {
            if (const auto* s = team_tag->as_string()) {
                t.is_hostile = own_team_.empty() ? (*s == "red")
                                                 : (*s != own_team_);
            }
        }
        // Missile classification via role tag.
        if (auto role_tag = h.get_tag(ROLE)) {
            if (const auto* s = role_tag->as_string()) {
                t.is_missile = (*s == "missile");
            }
        }

        compute_geometry(t, ownship_pos, ownship_vel, *tf);
        compute_threat_score(t);

        // Detection sources (after geometry, so we can range-gate).
        // M2: a DetectionPolicy override replaces the built-in rules (the
        // f4-sensors-backed adapter arrives at M3). Without one, the legacy
        // rules apply: GCI sees everything within the theater by
        // definition; radar/RWR are range-gated and hostile-filtered;
        // visual is range-only.
        if (policy_ != nullptr) {
            const auto v = policy_->classify(t);
            t.detected_by_radar  = v.radar;
            t.detected_by_rwr    = v.rwr;
            t.detected_by_visual = v.visual;
            t.detected_by_gci    = v.gci;
        } else {
            t.detected_by_gci    = true;
            t.detected_by_radar  = (t.range_nm <= cfg_.max_radar_range_nm)  && t.is_hostile;
            t.detected_by_rwr    = (t.range_nm <= cfg_.max_rwr_range_nm)    && t.is_hostile;
            t.detected_by_visual = (t.range_nm <= cfg_.max_visual_range_nm);
        }

        // EWMA smoothing — find the previous snapshot for this entity.
        const TargetInfo* prev = nullptr;
        for (const auto& p : prev_targets_) {
            if (p.entity_id == t.entity_id) { prev = &p; break; }
        }
        const double dt = update_interval_sec(skill_);
        update_ewma(t, prev, dt);

        targets_.push_back(std::move(t));
    }
}

void SensorFusion::compute_geometry(TargetInfo& t,
                                    const WorldPosition& ownship_pos,
                                    const WorldPosition& ownship_vel,
                                    const TransformComponent& /*target_tf*/) {
    // BRA from ownship to target. f4::geo::to_bra returns bearing (CW from
    // north), slant range, and the target's MSL altitude (= position.z in
    // the ENU frame).
    const BRA bra = f4::geo::to_bra(ownship_pos, t.position);
    t.range_ft = bra.range_ft;
    t.range_nm = bra.range_ft / FEET_PER_NM;

    // Azimuth: bearing from ownship's NOSE (not from north).
    // own_heading is the velocity-vector heading; if stationary, it's 0
    // (north), which is the convention FreeFalcon uses for ground ops.
    const double own_heading = velocity_heading_rad(ownship_vel);
    t.azimuth_rad = wrap_2pi(angle_diff(bra.bearing_rad, own_heading));

    // Elevation angle to target (above horizon = positive).
    const double dx = t.position.x - ownship_pos.x;  // east, ownship->target
    const double dy = t.position.y - ownship_pos.y;  // north, ownship->target
    const double dz = t.position.z - ownship_pos.z;  // up, ownship->target
    const double horiz = std::sqrt(dx * dx + dy * dy);
    t.elevation_rad = (horiz > 0.0) ? std::atan2(dz, horiz) : 0.0;

    // ATA (Aspect Tail Angle): 0 = target's nose pointing at us.
    // angle between target's velocity heading and the bearing from target
    // back to ownship. The bearing from target to ownship is the REVERSE of
    // the ownship->target displacement, so we negate dx/dy.
    const double target_heading = velocity_heading_rad(t.velocity);
    const double to_own_east  = -dx;  // east from target to ownship
    const double to_own_north = -dy;  // north from target to ownship
    const double reverse_bearing = std::atan2(to_own_east, to_own_north);
    t.ata_rad = std::fabs(angle_diff(target_heading, reverse_bearing));

    // ATA from ownship perspective: angle between ownship's velocity heading
    // and the bearing from ownship to target. 0 = we're nose-on the target.
    t.ata_from_rad = std::fabs(angle_diff(own_heading, bra.bearing_rad));
}

void SensorFusion::compute_threat_score(TargetInfo& t) {
    // Combat class from speed + altitude (FreeFalcon sfusion.cpp:504).
    // Altitude is position.z (z-up in the ENU frame).
    const double alt_ft = t.position.z;
    const double spd    = speed_fps(t.velocity);
    t.combat_class = guess_combat_class(spd, alt_ft);

    // FreeFalcon threat scoring (sfusion.cpp):
    //   base 100 if hostile
    //   +50 if combat_class 2-4 (armed through fighter)
    //   if ATA > pi/2 (90deg): score /= 2 (target pointed away)
    //
    // The ATA halving is only meaningful when the target is actually moving
    // — a stationary target's "heading" defaults to north (velocity_heading_rad
    // returns 0 for zero velocity), which would spuriously trigger the
    // halving whenever the ownship happens to be to the target's south.
    // Skip the halving when the target has no meaningful velocity; this
    // matches the intent ("target is running away = less threatening").
    double score = 0.0;
    if (t.is_hostile) {
        score = 100.0;
        if (t.combat_class >= 2 && t.combat_class <= 4) {
            score += 50.0;
        }
        if (spd > 1.0 && t.ata_rad > PI / 2.0) {
            score *= 0.5;
        }
    }
    // Missiles are max-threat ONLY when hostile (M3 tactics): a missile
    // in flight from our own wingman — or the one WE just fired — must
    // never outrank the enemy fighter we are engaging, or the shooter
    // would start defending against its own weapon. launch_missile()
    // copies the shooter's team tag onto the missile, so team comparison
    // cleanly separates "ours" from "theirs".
    if (t.is_missile && t.is_hostile) {
        score = 200.0;
    }
    t.threat_score = score;
}

void SensorFusion::update_ewma(TargetInfo& t,
                               const TargetInfo* prev,
                               double dt) {
    // FreeFalcon EWMA: new = alpha * sample + (1 - alpha) * old.
    // alpha = 0.15 (15% new, 85% old) — per AI_IMPLEMENTATION_PLAN §6.
    //
    // We compute the rate per second (delta / dt) so the smoothing is
    // frame-rate independent. FreeFalcon uses per-frame deltas implicitly
    // assuming a fixed frame rate; we make the assumption explicit.
    if (!prev || dt <= 0.0) {
        t.atadot   = 0.0;
        t.rangedot = 0.0;
        return;
    }

    // ata_dot: rate of change of ATA, signed shortest-path.
    const double ata_sample   = angle_diff(t.ata_rad, prev->ata_rad) / dt;
    // range_dot: positive = closing (range decreasing).
    const double range_sample = (prev->range_ft - t.range_ft) / dt;

    const double a = cfg_.ewma_alpha;
    t.atadot   = a * ata_sample   + (1.0 - a) * prev->atadot;
    t.rangedot = a * range_sample + (1.0 - a) * prev->rangedot;
}

} // namespace f4::ai

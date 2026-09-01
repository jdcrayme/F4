// f4-ai/src/collision_avoid_module.cpp
//
// CollisionAvoidModule implementation — digi_cavoid.cpp's CollisionCheck +
// CollisionAvoid, ported 1:1 (see collision_avoid_module.hpp for the
// constant table and the escape-point rule).

#include "f4/ai/modules/collision_avoid_module.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace f4::ai::modules {

namespace {

constexpr double PI = 3.14159265358979323846;

[[nodiscard]] inline double heading_error(double desired,
                                          double current) noexcept {
    double e = desired - current;
    while (e > PI) e -= 2.0 * PI;
    while (e < -PI) e += 2.0 * PI;
    return e;
}

} // anonymous namespace

double CollisionAvoidModule::react_time_sec() const noexcept {
    // reactTime = (GS_LIMIT / maxGs) * reactFact, maxGs clamped to the
    // reference's 2.5 G floor (digimain.cpp: "maxGs clamped to min 2.5G").
    const double max_g = std::max(2.5, cfg_.own_max_g);
    return (cfg_.gs_limit / max_g) * cfg_.react_fact;
}

void CollisionAvoidModule::reset() {
    avoiding_ = false;
    intruder_id_ = 0;
    hold_timer_ = 0.0;
    escape_az_ = 0.0;
    escape_target_alt_ft_ = 0.0;
    air_.reset_integrators();
}

AirSteering::Input CollisionAvoidModule::steering_input(
    const flight::IAircraftState& s) const noexcept {
    AirSteering::Input in;
    in.position = geo::WorldPosition(s.position_east_ft(),
                                     s.position_north_ft(),
                                     s.altitude_msl_ft());
    in.heading_rad = s.heading_rad();
    in.pitch_rad = s.pitch_angle_rad();
    in.roll_rad = s.roll_angle_rad();
    in.roll_rate_radps = s.roll_rate_radps();
    in.pitch_rate_radps = s.pitch_rate_radps();
    in.vs_fpm = s.vertical_speed_fpm();
    in.vcas_kts = s.vcas_kts();
    in.alt_msl_ft = s.altitude_msl_ft();
    return in;
}

AIControlOutput CollisionAvoidModule::update(
    double dt, const flight::IAircraftState* state) {
    AIControlOutput out{};

    if (state == nullptr) {
        if (avoiding_) reset();
        return out;
    }

    if (!cfg_.enabled) {
        if (avoiding_) reset();
        return out;
    }

    // --- Detection (CollisionCheck, digi_cavoid.cpp:12) -------------------
    const geo::WorldPosition own_pos{
        state->position_east_ft(), state->position_north_ft(),
        state->altitude_msl_ft()};
    // Own velocity: the host's transform snapshot when it pushed the
    // traffic (same-frame relative geometry); otherwise derived from
    // heading + CAS + VS (the standalone/unit-test case — CAS-vs-TAS
    // error is a few percent of speed over a <1 s extrapolation).
    const geo::WorldPosition own_vel = own_velocity_.value_or(
        geo::WorldPosition{
            state->vcas_kts() * (6076.12 / 3600.0) *
                std::sin(state->heading_rad()),
            state->vcas_kts() * (6076.12 / 3600.0) *
                std::cos(state->heading_rad()),
            state->vertical_speed_fpm() / 60.0});

    const double react_time = react_time_sec();
    const double h = cfg_.h_range_ft;

    const Intruder* worst = nullptr;
    double worst_time = 1.0e9;

    for (const auto& intr : traffic_) {
        // The committed firing pass: the brain-exempted intruder (the
        // WVR gun target inside the employment band) does not exist for
        // this rung — the weapons own that geometry.
        if (intr.entity_id == exempt_id_) continue;

        const geo::WorldPosition rel{
            intr.position.x - own_pos.x,
            intr.position.y - own_pos.y,
            intr.position.z - own_pos.z};
        const double range = std::sqrt(rel.x * rel.x + rel.y * rel.y +
                                       rel.z * rel.z);
        if (range < 1.0) continue;  // co-located: degenerate, skip

        const geo::WorldPosition rel_vel{
            intr.velocity.x - own_vel.x,
            intr.velocity.y - own_vel.y,
            intr.velocity.z - own_vel.z};
        // Range rate: positive = opening. A diverging geometry has no
        // collision to find (the reference breaks out of the
        // extrapolation the moment range starts diverging).
        const double rangedot = (rel.x * rel_vel.x + rel.y * rel_vel.y +
                                 rel.z * rel_vel.z) / range;
        if (rangedot >= 0.0 && range > h) continue;

        // timeToImpact = (range - hRange) / -rangedot
        if (range > h && rangedot < 0.0) {
            const double time_to_impact = (range - h) / (-rangedot);
            if (time_to_impact > react_time) continue;  // no collision
        }
        // (range <= h: already inside the bubble — a collision NOW.)

        // Linear extrapolation, dt = 0.05 .. reactTime step 0.1 (the
        // reference's loop). Early-out when separation starts diverging.
        double t = 0.05;
        double prev_sep = range;
        bool diverging = false;
        for (int step = 0; t <= react_time + 1e-9; t += 0.1, ++step) {
            const double sx = rel.x + rel_vel.x * t;
            const double sy = rel.y + rel_vel.y * t;
            const double sz = rel.z + rel_vel.z * t;
            const double sep = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (sep < h) {
                // Predicted collision at ~t. Worst-case ordering: the
                // EARLIEST impact breaks first.
                if (t < worst_time) {
                    worst_time = t;
                    worst = &intr;
                }
                break;
            }
            if (step > 0 && sep > prev_sep) {  // diverging: resolved
                diverging = true;
                break;
            }
            prev_sep = sep;
        }
        (void)diverging;
    }

    // --- Escape (CollisionAvoid: TrackPoint to the escape point) ---------
    if (worst != nullptr) {
        // (Re)arm the break + the linger window.
        avoiding_ = true;
        intruder_id_ = worst->entity_id;
        hold_timer_ = cfg_.avoid_hold_sec;

        // Bearing to the intruder (rad CW from north).
        const double dx = worst->position.x - own_pos.x;
        const double dy = worst->position.y - own_pos.y;
        const double bearing = std::atan2(dx, dy);

        // Escape side: OPPOSITE to the target's roll (its roll RATE —
        // the reference's droll). A target rolling right escapes us to
        // the LEFT of the bearing; not rolling -> break RIGHT (the
        // aviation head-on convention; see header for why it is
        // load-bearing).
        const double az_off = cfg_.escape_az_deg * PI / 180.0;
        double side;
        if (worst->roll_rate_radps > 1.0e-3) {
            side = -1.0;  // target rolling right -> escape left
        } else if (worst->roll_rate_radps < -1.0e-3) {
            side = +1.0;  // target rolling left -> escape right
        } else {
            side = +1.0;  // level: both aircraft break right
        }
        escape_az_ = bearing + side * az_off;

        // Escape point: escape_range at escape_el UP along escape_az.
        // The TrackPoint's altitude component: own alt + range*sin(el).
        const double el = cfg_.escape_el_deg * PI / 180.0;
        escape_target_alt_ft_ =
            own_pos.z + cfg_.escape_range_ft * std::sin(el);
    } else if (avoiding_) {
        // No predicted collision: fly the LAST break out for the linger
        // window, then release (a break is a maneuver, not a twitch).
        hold_timer_ -= dt;
        if (hold_timer_ <= 0.0) {
            reset();
            return out;
        }
    } else {
        return out;  // clean sky
    }

    // --- Fly the break ----------------------------------------------------
    // Max-performance toward the escape point: lifted bank + VS caps. The
    // AirSteering heading/VS cascade flies it.
    //
    // The comfort limiters come OFF for the break (they are the
    // nav-comfort tune, built to keep a 30-50 s phugoid tamed over
    // minutes of enroute flight): STAB-E29's 400 fpm/s VS-command slew
    // lets the escape command reach only ~280 fpm inside the 0.7-s react
    // window, and STAB-E46's energy damper would chop the throttle and
    // dump the board against the escape climb. A break lives seconds —
    // none of the phugoid machinery it fights applies — so the break
    // commands the VS cap NOW (slew disabled, guard disabled). The
    // roll-in + pull then develops the geometry the miss is made of
    // (measured without this: a 55-ft miss on a pure head-on; the
    // reference's TrackPoint(maxGs, cornerSpeed) is exactly this
    // intent).
    air_.max_vs_fpm = cfg_.max_vs_fpm;
    air_.max_bank_rad = cfg_.max_bank_rad;
    air_.vs_slew_fpm_per_s = -1.0;    // STAB-E29: OFF during the break
    air_.balloon_guard_fpm = 1.0e9;   // STAB-E46: OFF during the break

    out = air_.steer(escape_az_, escape_target_alt_ft_,
                     cfg_.escape_speed_kts, steering_input(*state));

    // Full afterburner, boards in — the break is a max-performance
    // maneuver (set explicitly; the speed-loop PI would sit at a cruise
    // setting when the escape speed happens to match the current speed,
    // and the proportional brake would creep out against the AB
    // acceleration).
    out.throttle_cmd = 1.5;
    out.speed_brake_cmd = -1.0;

    // The override: preempt every other rung except ground avoid.
    out.has_override = true;
    return out;
}

} // namespace f4::ai::modules

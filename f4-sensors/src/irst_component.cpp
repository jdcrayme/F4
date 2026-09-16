// f4-sensors/src/irst_component.cpp — the IRST scan loop.
//
// Same skeleton as RadarSimComponent::perform_scan (candidate walk,
// pre-gates, geometry, seeded roll), with the IR model swapped in:
// gimbal-limit gates instead of a scan volume, sqrt(intensity) range
// scaling instead of the radar's fourth-root RCS, and the card's ground
// factor for ground targets.

#include <f4/sensors/irst_component.hpp>

#include <f4/sensors/signature.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include <f4/geo/relative.hpp>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif // !M_PI

namespace f4::sensors {

namespace {

constexpr double kStationarySpeedFps = 1.0;   // radar's convention
constexpr double kFeetPerNm = 6076.11548;

inline double angle_diff(double a, double b) noexcept {
    double d = a - b;
    while (d >  M_PI) d -= 2.0 * M_PI;
    while (d < -M_PI) d += 2.0 * M_PI;
    return d;
}

} // namespace

double irst_detection_range_nm(const IrstParameters& params, double ir_sig,
                               bool ground_target) noexcept {
    const double sig = std::max(ir_sig, 0.0);
    const double ground =
        ground_target ? params.ground_factor : 1.0;
    return params.nominal_range_nm *
           std::sqrt(sig / params.reference_ir_value) * ground;
}

double irst_detection_probability(double range_nm, double r_det_nm) noexcept {
    if (r_det_nm <= 0.0) return 0.0;
    if (range_nm <= 0.75 * r_det_nm) return 1.0;
    if (range_nm >= r_det_nm) return 0.0;
    return 1.0 - (range_nm - 0.75 * r_det_nm) / (0.25 * r_det_nm);
}

void IrstComponent::update(double dt, messaging::MessageBus& bus) {
    if (!owner_.valid() || owner_.world() == nullptr) return;
    if (!initialized_) {
        rng_.seed(rng_seed);
        initialized_ = true;
    }
    scan_timer_ += dt;
    if (scan_timer_ < scan_interval_s) return;
    scan_timer_ = std::fmod(scan_timer_, scan_interval_s);
    perform_scan(bus);
}

void IrstComponent::perform_scan(messaging::MessageBus& bus) {
    const auto* world = owner_.world();
    const auto* own_tf = owner_.get<entities::TransformComponent>();
    if (world == nullptr || own_tf == nullptr) return;

    const double now = sim_time();
    ++scans_;

    const f4::geo::WorldPosition own_pos = own_tf->position;
    const f4::math::Vec3<double> own_vel{own_tf->vx, own_tf->vy, own_tf->vz};
    const double own_speed = own_vel.length();

    // Own heading (CW from north) for the azimuth gimbal gate. Stationary
    // -> NaN -> the azimuth gate is off (the RWR omni contract).
    double own_heading_rad = std::numeric_limits<double>::quiet_NaN();
    if (own_speed > kStationarySpeedFps) {
        own_heading_rad = std::atan2(own_vel.x, own_vel.y);
    }

    const double cutoff_ft =
        params.scan_cutoff_multiplier * params.nominal_range_nm * kFeetPerNm;

    std::uniform_real_distribution<double> uniform01{0.0, 1.0};

    for (const auto& [eid, tf] :
         world->with_component_ref<entities::TransformComponent>()) {
        if (eid.value == owner_.id().value) continue;

        // Ground clutter: the airframe IRST tracks the air picture.
        if (!params.track_ground_clutter && tf->is_ground_clutter()) {
            continue;
        }

        const f4::geo::WorldPosition tgt_pos = tf->position;

        // Range pre-gate (the radar's campaign-scale shape).
        const double dxr = tgt_pos.x - own_pos.x;
        const double dyr = tgt_pos.y - own_pos.y;
        const double dzr = tgt_pos.z - own_pos.z;
        const double dist2 = dxr * dxr + dyr * dyr + dzr * dzr;
        if (dist2 > cutoff_ft * cutoff_ft) continue;

        // Geometry (ENU): bearing off the nose + elevation.
        const f4::geo::BRA bra = f4::geo::to_bra(own_pos, tgt_pos);
        const double horizontal = std::sqrt(dxr * dxr + dyr * dyr);
        const double elevation =
            std::atan2(dzr, std::max(horizontal, 1.0));

        // Gimbal gates. Azimuth: |bearing off own heading| (off when
        // stationary). Elevation: |elevation| off the horizon.
        if (!std::isnan(own_heading_rad)) {
            const double off_nose =
                std::abs(angle_diff(bra.bearing_rad, own_heading_rad));
            if (off_nose > params.az_limit_deg * (M_PI / 180.0)) continue;
        }
        if (std::abs(elevation) > params.el_limit_deg * (M_PI / 180.0)) {
            continue;
        }

        // IR signature: the target's SIGDATA IR band (hot-rear aspect
        // lobe), 1.0 when the target carries no signature data.
        double ir_sig = 1.0;
        bool ground_target = false;
        entities::EntityHandle h(eid, const_cast<entities::EntityWorld*>(world));
        if (const auto* signature = h.get<SignatureComponent>()) {
            const f4::math::Vec3<double> tgt_vel{tf->vx, tf->vy, tf->vz};
            const double tgt_speed = tgt_vel.length();
            double aspect_rad = 0.0;
            if (tgt_speed > kStationarySpeedFps) {
                const double tgt_heading = std::atan2(tf->vx, tf->vy);
                const double bearing_to_ir =
                    f4::geo::to_bra(tgt_pos, own_pos).bearing_rad;
                aspect_rad =
                    std::abs(angle_diff(bearing_to_ir, tgt_heading));
            }
            ir_sig = signature->ir_signature_value(aspect_rad);
        }
        ground_target = tf->is_ground_clutter();

        // The detection roll.
        const double r_det = irst_detection_range_nm(params, ir_sig,
                                                     ground_target);
        const double range_nm = bra.range_nm();
        const double pd = irst_detection_probability(range_nm, r_det);
        if (pd <= 0.0) continue;
        if (uniform01(rng_) >= pd) continue;

        contacts_.on_detection(
            eid.value, tgt_pos,
            f4::math::Vec3<double>{tf->vx, tf->vy, tf->vz}, now);
    }

    // Age the contact book; unseen contacts drop after the hold.
    (void)bus;   // no transition publishing this tranche (see header)
    for (const auto dropped : contacts_.decay(now, contact_hold_s)) {
        (void)dropped;
    }
}

} // namespace f4::sensors

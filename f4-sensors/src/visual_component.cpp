// f4-sensors/src/visual_component.cpp — the visual scan loop.
//
// Deterministic threshold detection (the original signal law), gimbal
// gates like the IRST, no RNG, no clutter rejection — you can see
// parked aircraft.

#include <f4/sensors/visual_component.hpp>

#include <f4/sensors/signature.hpp>

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

double visual_signal(double gain, double visual_sig, double range_ft) noexcept {
    const double r = std::max(range_ft, 1.0);
    const double s = std::max(visual_sig, 0.0);
    return gain * s / (r * r);
}

double visual_detection_range_ft(double gain, double visual_sig) noexcept {
    return std::sqrt(std::max(gain * std::max(visual_sig, 0.0), 0.0));
}

void VisualComponent::update(double dt, messaging::MessageBus& bus) {
    if (!owner_.valid() || owner_.world() == nullptr) return;
    scan_timer_ += dt;
    if (scan_timer_ < scan_interval_s) return;
    scan_timer_ = std::fmod(scan_timer_, scan_interval_s);
    perform_scan(bus);
}

void VisualComponent::perform_scan(messaging::MessageBus& bus) {
    const auto* world = owner_.world();
    const auto* own_tf = owner_.get<entities::TransformComponent>();
    if (world == nullptr || own_tf == nullptr) return;

    const double now = sim_time();
    ++scans_;

    const f4::geo::WorldPosition own_pos = own_tf->position;
    const f4::math::Vec3<double> own_vel{own_tf->vx, own_tf->vy, own_tf->vz};
    const double own_speed = own_vel.length();

    double own_heading_rad = std::numeric_limits<double>::quiet_NaN();
    if (own_speed > kStationarySpeedFps) {
        own_heading_rad = std::atan2(own_vel.x, own_vel.y);
    }

    // Pre-gate ceiling: the strongest signature the scan can see is
    // bounded by the multiplier; candidates beyond it never cross the
    // threshold. (The multiplier applies to the REFERENCE range —
    // sqrt(gain) — since the signature scales the range, not the gate.)
    const double cutoff_ft =
        params.scan_cutoff_multiplier *
        visual_detection_range_ft(params.gain, 1.0);

    for (const auto& [eid, tf] :
         world->with_component_ref<entities::TransformComponent>()) {
        if (eid.value == owner_.id().value) continue;

        const f4::geo::WorldPosition tgt_pos = tf->position;
        const double dxr = tgt_pos.x - own_pos.x;
        const double dyr = tgt_pos.y - own_pos.y;
        const double dzr = tgt_pos.z - own_pos.z;
        const double dist2 = dxr * dxr + dyr * dyr + dzr * dzr;
        if (dist2 > cutoff_ft * cutoff_ft) continue;

        // Geometry + gates (the IRST's shape).
        const f4::geo::BRA bra = f4::geo::to_bra(own_pos, tgt_pos);
        const double horizontal = std::sqrt(dxr * dxr + dyr * dyr);
        const double elevation =
            std::atan2(dzr, std::max(horizontal, 1.0));
        if (!std::isnan(own_heading_rad)) {
            const double off_nose =
                std::abs(angle_diff(bra.bearing_rad, own_heading_rad));
            if (off_nose > params.az_limit_deg * (M_PI / 180.0)) continue;
        }
        if (std::abs(elevation) > params.el_limit_deg * (M_PI / 180.0)) {
            continue;
        }

        // Visual signature: the target's VIS grid ratio (1.0 data-free).
        double vis_sig = 1.0;
        entities::EntityHandle h(eid, const_cast<entities::EntityWorld*>(world));
        if (const auto* signature = h.get<SignatureComponent>()) {
            const double tgt_speed =
                f4::math::Vec3<double>{tf->vx, tf->vy, tf->vz}.length();
            double aspect_rad = 0.0;
            if (tgt_speed > kStationarySpeedFps) {
                const double tgt_heading = std::atan2(tf->vx, tf->vy);
                const double bearing_to_eye =
                    f4::geo::to_bra(tgt_pos, own_pos).bearing_rad;
                aspect_rad =
                    std::abs(angle_diff(bearing_to_eye, tgt_heading));
            }
            vis_sig = signature->visual_signature_value(aspect_rad);
        }

        // The threshold — deterministic.
        const double signal =
            visual_signal(params.gain, vis_sig, bra.range_ft);
        if (signal < 1.0) continue;

        contacts_.on_detection(
            eid.value, tgt_pos,
            f4::math::Vec3<double>{tf->vx, tf->vy, tf->vz}, now);
    }

    (void)bus;   // no transition publishing this tranche (see header)
    for (const auto dropped : contacts_.decay(now, contact_hold_s)) {
        (void)dropped;
    }
}

} // namespace f4::sensors

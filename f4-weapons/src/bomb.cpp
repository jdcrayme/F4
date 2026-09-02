// f4-weapons/src/bomb.cpp — the pure ballistic flyout + release math.

#include <f4/weapons/bomb.hpp>
#include <f4/weapons/missile.hpp>   // kGravityFps2, atmosphere_density

#include <cmath>

namespace f4::weapons {

BombConfig BombConfig::from_record(const WeaponClassRecord& rec) {
    BombConfig c;
    c.mass_lb = (rec.launch_mass_lb > 0.0) ? rec.launch_mass_lb
                                           : rec.burnout_mass_lb;
    c.ref_area_ft2 = rec.ref_area_ft2;
    c.cd = rec.cd;
    c.lethal_radius_ft = rec.lethal_radius_ft;
    c.tof_limit_s = rec.tof_limit_s;
    return c;
}

void Bomb::release(const BombConfig& config,
                   const f4::geo::WorldPosition& pos,
                   const f4::math::Vec3<double>& vel,
                   double impact_plane_z) {
    cfg_ = config;
    release_ = pos;
    pos_ = pos;
    vel_ = vel;
    impact_plane_z_ = impact_plane_z;
    t_ = 0.0;
    // Released below (or at) the plane: terminal immediately — a release
    // under the impact altitude is a ground collision, not a flight.
    status_ = (pos_.z <= impact_plane_z_) ? BombStatus::Impact
                                          : BombStatus::Released;
}

void Bomb::tick(double dt) {
    if (terminal()) return;

    const double v = vel_.length();
    if (v > 1.0e-6) {
        // Drag along the velocity vector (missile.hpp's model, no thrust).
        const double rho = atmosphere_density(pos_.z);
        const double drag_acc =
            0.5 * rho * v * v * cfg_.cd * cfg_.ref_area_ft2 / cfg_.mass_lb;
        const double dv = std::min(drag_acc * dt, v);  // never reverse
        vel_.x -= vel_.x / v * dv;
        vel_.y -= vel_.y / v * dv;
        vel_.z -= vel_.z / v * dv;
    }

    vel_.z -= kGravityFps2 * dt;

    pos_.x += vel_.x * dt;
    pos_.y += vel_.y * dt;
    pos_.z += vel_.z * dt;
    t_ += dt;

    if (pos_.z <= impact_plane_z_) {
        // Report the position AT the plane (linear interpolation over the
        // last step — keeps the impact point inside the lethal-radius math
        // honest at coarse QC time steps).
        const double dz_above = impact_plane_z_ - pos_.z + vel_.z * dt;
        const double vz = vel_.z;
        const double frac = (vz < -1.0e-9 && dz_above > 0.0)
            ? std::min(1.0, dz_above / (-vz * dt)) : 1.0;
        pos_.z = impact_plane_z_;
        pos_.x -= vel_.x * dt * (1.0 - frac);
        pos_.y -= vel_.y * dt * (1.0 - frac);
        status_ = BombStatus::Impact;
        return;
    }

    if (cfg_.tof_limit_s > 0.0 && t_ >= cfg_.tof_limit_s) {
        status_ = BombStatus::Expired;
    }
}

double bomb_fall_time_s(double dz_ft) noexcept {
    if (dz_ft <= 0.0) return 0.0;
    return std::sqrt(2.0 * dz_ft / kGravityFps2);
}

double bomb_vacuum_range_ft(double dz_ft, double speed_ftps) noexcept {
    return speed_ftps * bomb_fall_time_s(dz_ft);
}

} // namespace f4::weapons

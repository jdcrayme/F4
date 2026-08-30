// f4-weapons/src/missile.cpp — 3-DOF point-mass flyout. See missile.hpp.

#include <f4/weapons/missile.hpp>

#include <algorithm>
#include <cmath>

namespace f4::weapons {

double atmosphere_density(double alt_ft) {
    // Sea-level standard density 0.0023769 slugs/ft^3, ~24,000 ft scale
    // height. Clamped at zero altitude (no negative densities underground).
    const double alt = std::max(0.0, alt_ft);
    return 0.0023769 * std::exp(-alt / 24000.0);
}

MissileConfig MissileConfig::from_record(const WeaponClassRecord& rec) {
    MissileConfig c;
    c.launch_mass_lb  = rec.launch_mass_lb;
    c.burnout_mass_lb = rec.burnout_mass_lb;
    c.thrust_lbf      = rec.thrust_lbf;
    c.burn_time_s     = rec.burn_time_s;
    c.ref_area_ft2    = rec.ref_area_ft2;
    c.cd              = rec.cd;
    c.max_speed_fts   = rec.max_speed_fts;
    c.max_g           = rec.max_g;
    c.guidance_gain   = rec.guidance_gain;
    c.seeker_half_angle_rad = rec.seeker_half_angle_deg * 3.14159265358979323846 / 180.0;
    c.seeker_max_range_ft   = rec.seeker_max_range_ft;
    c.fuze_radius_ft        = rec.fuze_radius_ft;
    c.lethal_radius_ft      = rec.lethal_radius_ft;
    c.tof_limit_s           = rec.tof_limit_s;
    return c;
}

void Missile::launch(const MissileConfig& config,
                     const f4::geo::WorldPosition& pos,
                     const f4::math::Vec3<double>& vel) {
    cfg_ = config;
    pos_ = pos;
    vel_ = vel;
    mass_ = cfg_.launch_mass_lb;
    t_ = 0.0;
    status_ = MissileStatus::Guided;
    last_range_ = -1.0;
    min_range_ = -1.0;
    grow_ticks_ = 0;
}

bool Missile::seeker_sees(const TargetSnapshot& target) const {
    if (!target.valid) {
        return false;
    }
    const double dx = target.position.x - pos_.x;
    const double dy = target.position.y - pos_.y;
    const double dz = target.position.z - pos_.z;
    const double range = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (range <= 0.0 || range > cfg_.seeker_max_range_ft) {
        return false;
    }
    const double speed = vel_.length();
    if (speed <= 0.0) {
        // No velocity axis to measure the boresight against yet (a missile
        // launched at rest) — treat the LOS as on-boresight.
        return true;
    }
    const double dot = (vel_.x * dx + vel_.y * dy + vel_.z * dz) / (speed * range);
    // Angle between velocity axis and LOS; inside the cone iff cos >= cos(half).
    const double cos_half = std::cos(cfg_.seeker_half_angle_rad);
    return dot >= cos_half;
}

void Missile::tick(double dt, const TargetSnapshot& target) {
    if (dt <= 0.0 || terminal()) {
        return;  // terminal states (and degenerate dt) are no-ops
    }

    // ---- Seeker track management -----------------------------------------
    if (status_ == MissileStatus::Guided) {
        if (!seeker_sees(target)) {
            // Cone/range/track loss -> coast ballistic.
            status_ = MissileStatus::Ballistic;
        }
    } else if (status_ == MissileStatus::Ballistic && seeker_sees(target)) {
        // M2: seeker re-acquisition. An active seeker that regains the
        // target (back inside cone + range, with a valid track) resumes
        // guidance — FreeFalcon's re-scan-after-loss behavior. M1 skipped
        // this deliberately; the seeker-source hook made it testable.
        status_ = MissileStatus::Guided;
    }

    // ---- Relative geometry (for guidance + fuze) --------------------------
    const bool has_target = target.valid;
    double range = -1.0;
    f4::math::Vec3<double> los{};       // missile -> target
    f4::math::Vec3<double> rel_vel{};   // target - missile
    if (has_target) {
        los = f4::math::Vec3<double>{target.position.x - pos_.x,
                                     target.position.y - pos_.y,
                                     target.position.z - pos_.z};
        rel_vel = target.velocity - vel_;
        range = los.length();
        last_range_ = range;
        if (min_range_ < 0.0 || range < min_range_) {
            min_range_ = range;
            grow_ticks_ = 0;
        } else if (range > min_range_) {
            ++grow_ticks_;
        }
    }

    // ---- Guidance (PN) -----------------------------------------------------
    if (status_ == MissileStatus::Guided && has_target && range > 0.0) {
        const f4::math::Vec3<double> omega =
            los.cross(rel_vel) / (range * range);              // LOS rate (rad/s)
        const double closing = -(los.dot(rel_vel) / range);    // + when closing
        const double speed = vel_.length();
        if (speed > 0.0) {
            f4::math::Vec3<double> a_cmd =
                omega.cross(vel_ / speed) * (cfg_.guidance_gain * closing);
            // Clamp to the lateral-G envelope.
            const double a_max = cfg_.max_g * kGravityFps2;
            const double a_mag = a_cmd.length();
            if (a_mag > a_max) {
                a_cmd = a_cmd / a_mag * a_max;
            }
            vel_ += a_cmd * dt;
        }
    }

    // ---- Thrust + mass depletion -------------------------------------------
    if (motor_burning()) {
        const double speed = vel_.length();
        if (speed > 0.0) {
            vel_ += (vel_ / speed) * (cfg_.thrust_lbf / mass_ * kGravityFps2 * dt);
            // lbf / slug * ft/s^2: mass in slugs == lb / 32.174, hence the
            // kGravityFps2 factor above (thrust_lbf / (lb/g) = lbf*g/lb).
        }
        const double burn_rate =
            (cfg_.launch_mass_lb - cfg_.burnout_mass_lb) / std::max(cfg_.burn_time_s, 1e-9);
        mass_ = std::max(cfg_.burnout_mass_lb, mass_ - burn_rate * dt);
    }

    // ---- Drag ----------------------------------------------------------------
    const double speed = vel_.length();
    if (speed > 0.0 && cfg_.ref_area_ft2 > 0.0 && mass_ > 0.0) {
        const double rho = atmosphere_density(pos_.z);
        const double q = 0.5 * rho * speed * speed;           // dynamic pressure
        const double drag_accel = q * cfg_.cd * cfg_.ref_area_ft2 / mass_
                                  * kGravityFps2;             // lbf -> ft/s^2 on slugs
        const double decel = std::min(drag_accel * dt, speed); // never reverse
        vel_ -= (vel_ / speed) * decel;
    }

    // ---- Gravity ----------------------------------------------------------------
    vel_.z -= kGravityFps2 * dt;

    // ---- Speed cap (AFTER gravity: the envelope applies to the final state) ----
    const double vcap = cfg_.max_speed_fts;
    if (vcap > 0.0) {
        const double v = vel_.length();
        if (v > vcap) {
            vel_ = vel_ / v * vcap;
        }
    }

    // ---- Integrate position -------------------------------------------------------
    pos_.x += vel_.x * dt;
    pos_.y += vel_.y * dt;
    pos_.z += vel_.z * dt;

    t_ += dt;

    // ---- Fuze / self-destruct --------------------------------------------------------
    if (has_target) {
        const bool fuze_hit = range <= cfg_.fuze_radius_ft;
        // Proximity fuze on closest approach: inside 8x lethal radius and the
        // range has been growing for ~0.5 s worth of ticks — the missile has
        // passed the target; detonate at the closest range achieved.
        const bool closest_approach =
            grow_ticks_ >= 2 && min_range_ > cfg_.fuze_radius_ft &&
            min_range_ <= cfg_.lethal_radius_ft * 8.0 && closing_velocity_nonpositive(target);
        if (fuze_hit || closest_approach) {
            status_ = MissileStatus::Detonated;
            return;
        }
    }
    if (t_ >= cfg_.tof_limit_s) {
        status_ = MissileStatus::Expired;
    }
}

bool Missile::closing_velocity_nonpositive(const TargetSnapshot& target) const {
    if (!target.valid) {
        return true;
    }
    const double dx = target.position.x - pos_.x;
    const double dy = target.position.y - pos_.y;
    const double dz = target.position.z - pos_.z;
    const double range = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (range <= 0.0) {
        return true;
    }
    const f4::math::Vec3<double> los{dx, dy, dz};
    const f4::math::Vec3<double> rel_vel = target.velocity - vel_;
    return (los.dot(rel_vel) / range) >= 0.0;  // not closing
}

} // namespace f4::weapons

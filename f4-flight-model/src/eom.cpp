// f4-flight-model/eom.cpp
//
// Equations of motion implementation.
//
// Ported from F4Flight's eom.cpp, which is a port of FreeFalcon's eom.cpp.
//
// The EOM integrates the rigid-body 6-DOF state:
//   1. Body rates (p, q, r) from commanded G and roll rate
//   2. Quaternion orientation (Forward Euler)
//   3. Ground attitude clamp + nose-wheel steering (if on ground)
//   4. Kinematic trig cache
//   5. True airspeed (with ground friction if on ground)
//   6. World position (with ground clamp)
//
// Key formulas and bug fixes documented inline.

#include "f4/flight/eom.hpp"

#include <algorithm>
#include <cmath>

namespace f4::flight {

using f4::math::Quatd;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
EquationsOfMotion::EquationsOfMotion(const data::AircraftGeometry* geom,
                                     const data::AuxAero* aux)
    : geom_(geom), aux_(aux) {}

// ---------------------------------------------------------------------------
// update: run the full EOM for one time step.
// ---------------------------------------------------------------------------
void EquationsOfMotion::update(double dt, const PilotInput& input,
                                AircraftState& state) const {
    KinematicState& k = state.kin;
    AeroState& a = state.aero;
    FcsState& f = state.fcs;
    GearState& g = state.gear;

    // Step 1: compute body rates from commanded G and roll rate
    calcBodyRates(dt, state.qsom, a.cnalpha,
                  k.cosmu, k.cosgam, k.singam,
                  k.cosbet, k.cosalp, k.sinalp,
                  state.loads.nzcgs, state.loads.nycgw, f.pstab,
                  aux_ ? aux_->pitchMomentum : 1.0,
                  aux_ ? aux_->pitchElasticity : 1.0,
                  state);

    // Step 2: integrate quaternion orientation
    calcBodyOrientation(dt, state);

    // Step 3: ground attitude clamp + nose-wheel steering
    // When on ground and close to the terrain, clamp roll to 0 and limit
    // pitch. Apply nose-wheel steering from the rudder pedals.
    if (!g.inAir && (g.groundZ_ft - k.z) < (g.minHeight_ft + 5.0)) {
        // Zero roll and yaw rates (but NOT pitch — needed for rotation damping)
        k.p = 0.0;
        k.r = 0.0;
        // Clamp roll to 0 (wings level on ground)
        k.phi = zero_angle();
        // Clamp pitch to a reasonable range for ground operation
        k.theta = angle_from_radians(std::clamp(to_radians(k.theta),
                                                  -2.0 * DTR, 15.0 * DTR));

        // Nose-wheel steering: turn the aircraft via rudder pedal input.
        // Steer rate is higher at low speed (for taxi) and lower at high speed.
        double steerRate;
        if (k.vt < 50.0) {
            steerRate = 30.0;  // deg/s at taxi speed
        } else if (k.vt < 150.0) {
            // Linear fade from 30 to 5 deg/s
            steerRate = 30.0 * (150.0 - k.vt) / 100.0 + 5.0;
        } else {
            steerRate = 5.0;   // deg/s at high speed (rudder authority)
        }
        // Positive ypedal = right turn = psi decreases (NED CCW frame)
        const double psi_delta_rad = -input.ypedal * steerRate * DTR * dt;
        k.psi = angle_from_radians(to_radians(k.psi) + psi_delta_rad);
        // Wrap psi to [-pi, pi]
        double psi_rad = to_radians(k.psi);
        if (psi_rad > PI)  psi_rad -= TWO_PI;
        if (psi_rad < -PI) psi_rad += TWO_PI;
        k.psi = angle_from_radians(psi_rad);

        // Rebuild quaternion from clamped euler angles
        // ZYX convention: q = qz(psi) * qy(theta) * qx(phi)
        const double phi_rad   = to_radians(k.phi);
        const double theta_rad = to_radians(k.theta);
        const double psi_rad2  = to_radians(k.psi);
        const double cr = std::cos(phi_rad * 0.5);
        const double sr = std::sin(phi_rad * 0.5);
        const double cp = std::cos(theta_rad * 0.5);
        const double sp = std::sin(theta_rad * 0.5);
        const double cy = std::cos(psi_rad2 * 0.5);
        const double sy = std::sin(psi_rad2 * 0.5);
        k.quat = Quatd(cr * cp * cy + sr * sp * sy,
                       sr * cp * cy - cr * sp * sy,
                       cr * sp * cy + sr * cp * sy,
                       cr * cp * sy - sr * sp * cy).normalized();
    }

    // Step 4: recompute kinematic trig cache
    trigonometry(state);

    // Step 5: integrate true airspeed
    // xwprop is 0 because thrust is already folded into a.xwaero by the
    // flight model orchestrator (Bug #2 fix from F4Flight).
    calculateVt(dt, g.muFric, k.singam, a.xwaero, 0.0, state);

    // Step 6: integrate position and apply ground clamp
    integratePosition(dt, k.cosgam, k.singam, k.cossig, k.sinsig,
                      state.windX, state.windY, state);
}

// ---------------------------------------------------------------------------
// calcBodyRates: compute body rates (p, q, r) from commanded G and roll rate.
//
// Uses the velocity-axis formulation from FreeFalcon eom.cpp:595-717.
// The pitch rate is derived from the commanded normal load factor (nzcgs)
// using the turn-rate equation, then filtered through a first-order lag.
// ---------------------------------------------------------------------------
void EquationsOfMotion::calcBodyRates(double dt, double qsom, double cnalpha,
                                       double cosmu, double cosgam, double singam,
                                       double cosbet, double cosalp, double sinalp,
                                       double nzcgs, double nycgw, double pstab,
                                       double pitchMomentum, double pitchElasticity,
                                       AircraftState& state) const {
    (void)qsom; (void)cnalpha;
    (void)singam; (void)cosbet; (void)cosalp; (void)sinalp;
    (void)pitchMomentum;

    KinematicState& k = state.kin;
    FcsState& f = state.fcs;
    AeroState& a = state.aero;

    // Avoid division by zero at very low airspeed
    const double tempVt = std::max(4.0, std::fabs(k.vt));

    // --- Pitch rate command (qptchc) ---
    // The turn-rate equation: q = atan(nz * g / V) - gravity_compensation
    //
    // qptchc = atan(nzcgs * g / V) - atan(gear_term) - atan(gravity_term)
    //
    // The gear_term accounts for the pitch moment from gear struts
    // (when gear is partially extended, it creates a nose-down moment).
    // The gravity_term compensates for the gravity-induced turn rate
    // in banked flight.
    const double gearPos = a.gearPos;
    double qptchc;
    if (gearPos < 1.0) {
        // Gear transitioning: use 0.2 factor
        qptchc = std::atan(nzcgs * GRAVITY / tempVt)
               - std::atan(0.2 * gearPos * state.qsom / tempVt);
    } else {
        // Gear down: use 0.1 factor
        qptchc = std::atan(nzcgs * GRAVITY / tempVt)
               - std::atan(0.1 * gearPos * state.qsom / tempVt);
    }
    // Gravity turn-rate compensation: in banked flight, gravity creates a
    // turn rate that the FCS must cancel to maintain the commanded G.
    qptchc -= std::atan(cosmu * cosgam * GRAVITY / tempVt);

    // Filter the pitch rate command through a first-order lag.
    // Time constant is scaled by pitchElasticity (aircraft structural flexibility).
    const double tau_q = f.tp01 * pitchElasticity;
    k.q = f.pitchRateLag.step(qptchc, tau_q, dt);

    // --- Yaw rate ---
    // r = (nycgw + cosgam * sinmu) * g / V
    // The cosgam*sinmu term is the gravity-induced yaw rate in banked flight.
    k.r = (nycgw + cosgam * k.sinmu) * GRAVITY / tempVt;

    // --- Roll rate ---
    // Roll rate comes directly from the FCS roll channel (already filtered).
    k.p = pstab;

    // --- Body-rate clamps ---
    // These prevent quaternion tumbling during transients. Without them,
    // a large transient (e.g. from a stall) can produce body rates that
    // cause the quaternion to diverge, which produces garbage euler angles
    // on the next frame, which produces more garbage, etc.
    k.p = std::clamp(k.p, -4.5, 4.5);  // roll:  ±4.5 rad/s (257 deg/s)
    k.q = std::clamp(k.q, -3.0, 3.0);  // pitch: ±3.0 rad/s (172 deg/s)
    k.r = std::clamp(k.r, -4.0, 4.0);  // yaw:   ±4.0 rad/s (229 deg/s)

    // --- Integrated roll (for FCS roll damping) ---
    // startRoll accumulates the total roll, which the FCS uses to scale
    // the roll rate command as the aircraft approaches the target bank.
    f.startRoll += k.p * dt;
}

// ---------------------------------------------------------------------------
// calcBodyOrientation: integrate the quaternion from body rates.
//
// Uses the exponential map on SO(3) for quaternion integration:
//   q_{n+1} = q_n * exp(0.5 * dt * omega_quat)
// where omega_quat = (0, p, q, r) is the angular velocity in body frame.
//
// The exponential map preserves the unit-quaternion constraint to machine
// precision (no re-normalization needed for small dt*omega), unlike Forward
// Euler (q += dt * q_dot) which drifts off the unit sphere and must be
// re-normalized every step. For large angular rates or coarse time steps
// the exponential map is also more accurate because it integrates along
// the geodesic of SO(3), not along the tangent space.
//
// Implementation:
//   Let omega = (p, q, r) and theta = |omega| * dt.
//   If theta is very small (< 1e-10), use the first-order approximation
//   dq = 0.5 * dt * omega to avoid division by zero.
//   Otherwise:
//     axis = omega / |omega|
//     half_angle = 0.5 * theta
//     dq = (cos(half_angle), axis * sin(half_angle))
//     q_{n+1} = q_n * dq
//
// This is the Rodrigues rotation formula in quaternion form.
// ---------------------------------------------------------------------------
void EquationsOfMotion::calcBodyOrientation(double dt, AircraftState& state) const {
    KinematicState& k = state.kin;

    const double p = k.p, q = k.q, r = k.r;

    // Angular velocity magnitude * dt
    const double omegaMag = std::sqrt(p * p + q * q + r * r);
    const double theta = omegaMag * dt;

    Quatd dq;
    if (theta < 1e-10) {
        // First-order approximation for very small rotations
        // dq ≈ (1, 0.5*dt*p, 0.5*dt*q, 0.5*dt*r)
        dq = Quatd(1.0, 0.5 * dt * p, 0.5 * dt * q, 0.5 * dt * r).normalized();
    } else {
        // Exponential map: dq = (cos(theta/2), (omega/|omega|) * sin(theta/2))
        const double halfTheta = 0.5 * theta;
        const double invOmegaMag = 1.0 / omegaMag;
        const double s = std::sin(halfTheta);
        dq = Quatd(std::cos(halfTheta),
                    s * p * invOmegaMag,
                    s * q * invOmegaMag,
                    s * r * invOmegaMag);
    }

    // Right-multiply: q_{n+1} = q_n * dq (body-frame angular velocity)
    k.quat = (k.quat * dq).normalized();

    // Recover euler angles from the quaternion (ZYX convention)
    // theta = asin(2*(w*y - x*z))
    // psi   = atan2(2*(w*z + x*y), 1 - 2*(y^2 + z^2))
    // phi   = atan2(2*(w*x + y*z), 1 - 2*(x^2 + y^2))
    const double w2 = k.quat.w, x2 = k.quat.x, y2 = k.quat.y, z2 = k.quat.z;
    k.theta = angle_from_radians(std::asin(std::clamp(2.0 * (w2 * y2 - x2 * z2), -1.0, 1.0)));
    k.psi   = angle_from_radians(std::atan2(2.0 * (w2 * z2 + x2 * y2), 1.0 - 2.0 * (y2 * y2 + z2 * z2)));
    k.phi   = angle_from_radians(std::atan2(2.0 * (w2 * x2 + y2 * z2), 1.0 - 2.0 * (x2 * x2 + y2 * y2)));
}

// ---------------------------------------------------------------------------
// trigonometry: recompute the kinematic trig cache.
//
// Computes sin/cos of all angles and derives the velocity-vector euler
// angles (sigma, gmma, mu) from the body angles and alpha/beta.
// ---------------------------------------------------------------------------
void EquationsOfMotion::trigonometry(AircraftState& state) const {
    KinematicState& k = state.kin;
    AeroState& a = state.aero;

    // Extract radians once for the trig calls. The body angles (theta, phi,
    // psi) and the aero angles (alpha, beta) are all stored as the strong
    // Angle type; sin/cos are dimensionless.
    const double theta_rad = to_radians(k.theta);
    const double phi_rad   = to_radians(k.phi);
    const double psi_rad   = to_radians(k.psi);
    const double alpha_rad = to_radians(a.alpha);
    const double beta_rad  = to_radians(a.beta);

    // Body angle trig
    k.sinthe = std::sin(theta_rad);  k.costhe = std::cos(theta_rad);
    k.sinphi = std::sin(phi_rad);    k.cosphi = std::cos(phi_rad);
    k.sinpsi = std::sin(psi_rad);    k.cospsi = std::cos(psi_rad);

    // Alpha/beta trig
    k.sinalp = std::sin(alpha_rad);  k.cosalp = std::cos(alpha_rad);
    k.sinbet = std::sin(beta_rad);   k.cosbet = std::cos(beta_rad);

    // Velocity-vector euler angles (approximation):
    //   gamma (flight path angle) = theta - alpha * cos(phi)
    //   sigma (velocity heading) = psi + beta * cos(theta)
    //   mu (velocity roll)       = phi
    k.gmma  = angle_from_radians(theta_rad - alpha_rad * k.cosphi);
    k.sigma = angle_from_radians(psi_rad + beta_rad * k.costhe);
    k.mu    = k.phi;

    const double gmma_rad  = to_radians(k.gmma);
    const double sigma_rad = to_radians(k.sigma);
    const double mu_rad    = to_radians(k.mu);
    k.singam = std::sin(gmma_rad);  k.cosgam = std::cos(gmma_rad);
    k.sinsig = std::sin(sigma_rad); k.cossig = std::cos(sigma_rad);
    k.sinmu  = std::sin(mu_rad);    k.cosmu  = std::cos(mu_rad);

    // World-frame velocities from true airspeed + wind
    k.xdot = k.vt * k.cosgam * k.cossig + state.windX;
    k.ydot = k.vt * k.cosgam * k.sinsig + state.windY;
    k.zdot = -k.vt * k.singam;
}

// ---------------------------------------------------------------------------
// calculateVt: integrate true airspeed.
//
// In air: vtDot = xwaero + xwprop - g * sin(gamma)
// On ground: subtract rolling friction (muFric * weight_on_wheels * g * dt)
// ---------------------------------------------------------------------------
void EquationsOfMotion::calculateVt(double dt, double muFric, double singam,
                                     double xwaero, double xwprop,
                                     AircraftState& state) const {
    KinematicState& k = state.kin;
    GearState& g = state.gear;

    if (g.inAir) {
        // Airborne: integrate airspeed from axial force + gravity
        const double vtDot = xwaero + xwprop - GRAVITY * singam;
        k.vt += vtDot * dt;
        k.vt = std::max(0.01, k.vt);  // floor to avoid division by zero
        state.vtDot = vtDot;
        state.netAccel = vtDot * dt;
    } else {
        // On ground: add rolling friction
        // weightOnWheels is floored at 0.5 so braking works at high speed
        // where lift reduces the normal force.
        const double weightOnWheels = std::clamp(state.loads.nzcgs, 0.5, 1.0);
        const double fric = (muFric + std::fabs(0.3 * state.kin.sinbet))
                          * weightOnWheels * GRAVITY * dt;
        const double vtDot = xwaero + xwprop - GRAVITY * singam;
        state.netAccel = vtDot * dt - fric;
        k.vt = std::max(0.0, k.vt + state.netAccel);
        state.vtDot = vtDot;
    }
}

// ---------------------------------------------------------------------------
// integratePosition: integrate world-frame position and apply ground clamp.
// ---------------------------------------------------------------------------
void EquationsOfMotion::integratePosition(double dt, double cosgam, double singam,
                                           double cossig, double sinsig,
                                           double windX, double windY,
                                           AircraftState& state) const {
    KinematicState& k = state.kin;
    GearState& g = state.gear;

    // World-frame velocities (already computed in trigonometry, but
    // recompute here in case they were modified)
    k.xdot = k.vt * cosgam * cossig + windX;
    k.ydot = k.vt * cosgam * sinsig + windY;
    k.zdot = -k.vt * singam;

    // Integrate position (Forward Euler)
    k.x += k.xdot * dt;
    k.y += k.ydot * dt;
    k.z += k.zdot * dt;

    // Ground clamp: prevent the aircraft from sinking through the terrain.
    // The clamp target is groundZ - minHeight (the body should sit minHeight
    // ABOVE the terrain, where minHeight is the gear strut extended length).
    // Only clamp when descending (zdot > 0 in NED = moving down), so the
    // clamp doesn't trigger on aircraft spawned on the ground.
    const double clampZ = g.groundZ_ft - g.minHeight_ft;
    if (k.z > clampZ && k.zdot > 0.0) {
        k.z = clampZ;
        k.zdot = 0.0;
        k.singam = 0.0;
        k.cosgam = 1.0;
        k.gmma = zero_angle();
    }
}

}  // namespace f4::flight

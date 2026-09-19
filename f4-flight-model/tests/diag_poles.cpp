// f4-flight-model/tests/diag_poles.cpp
//
// Pole-based flight-control diagnosis tool (not a GoogleTest).
//
// Implements Phases A/B/C of Docs/archive/FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md:
//
//   A. Trim -> finite-difference linearization of the FlightModel major-frame
//      map x[k+1] = F(x[k], u[k]) -> eigenvalues (f4-math eigen_real.hpp).
//      The linearization includes EVERY carried dynamic state: FCS filter
//      histories, integrator states, the actuator positions, the load-factor
//      and trig-cache values the FCS reads one minor frame stale, and (with
//      --ai) the AirSteering closed-loop integrators.
//
//   B. Speed-stability map: S_u = d(T-D)/dV at trim from the aero + engine
//      models directly, with the predicted speed-mode damping
//      zeta = -S_u * V / (2*sqrt(2)*g) (plan appendix 9.1/9.2) next to the
//      measured eigenvalues.
//
//   C. Loop-at-a-time analysis:
//        --qdamp-scale x   scale the FCS pitch-rate damper (L4)
//        --freeze-bias     freeze the 1-G alpha bias' sensed feedback (L3)
//        --ai-gain-scale x scale the AirSteering vs_gain/throttle_gain (L1/L2)
//      and --ai closes the AI cruise law (AirSteering::steer) around the
//      plant for the full closed-loop spectrum.
//
// State vector (wings-level, centered-pedals, in-air trim; documented
// exclusions at the bottom of this comment):
//   vt, theta, phi, z
//   aero: cl, clalph0, clift0, cnalpha, gearPos, tefPos, lefPos
//   fcs : pitchIntegral(y,u), pitchRateLag(y,u), pitchAlphaLag(y1,y2,u1,u2),
//         rollRateLag(y,u)
//   loads: nzcgs, nycgw                       (FCS reads them one frame stale)
//   trig : sinalp, cosalp, sinbet, cosbet, singam, cosgam, sinmu, cosmu,
//          costhe, cosphi                      (FCS reads them one frame stale)
//   engine: rpmLag(y,u)
//   ai (only with --ai): speed_integral, alt_integral, vs_target, prev_alpha_est
//
// Deliberate exclusions (each is either a pure same-frame function of the
// states above, or a frozen nonlinear element — documented in the plan):
//   x, y, psi, sinthe, sinphi, sinsig, cossig   (no feedback path)
//   p, q, r, pstab, vcas, qbar, qsom, mach, rpm (pure lag/algebraic outputs)
//   beta + yaw channel states (runYaw clamps beta=0 with centered pedals —
//       the yaw channel contributes no dynamics in these trims; itself a
//       finding: the model has NO Dutch-roll dynamics with pedals centered)
//   fuel mass (burn time constant ~hours), stall-state relay (frozen None),
//   startRoll (feeds back only when the bank limiter engages, maxRoll < 80).
//
// Build: see f4-flight-model/tests/CMakeLists.txt (target `diag_poles`).
// Offline build (no CMake in some sandboxes): compile f4-flight-model/src/*.cpp
// f4-data/src/*.cpp f4-messaging/src/bus.cpp f4-entities/src/entity.cpp
// f4-flight-api/src/pilot_input.cpp f4-ai/src/air_steering.cpp + this file.

#include "f4/flight/flight_model.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/atmosphere.hpp"
#include "f4/flight/constants.hpp"
#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/engine.hpp"
#include "f4/data/config_loader.hpp"
#include "f4/math/eigen_real.hpp"
#include "f4/ai/air_steering.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

using namespace f4::flight;
using f4::ai::AirSteering;
using f4::math::Vec3d;
using cdouble = std::complex<double>;

namespace {

constexpr double kDefaultMajorDt = 1.0 / 60.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kD2R = kPi / 180.0;
const Vec3d kFlatNormal{0.0, 0.0, -1.0};

// ---------------------------------------------------------------------------
// Small dense REAL linear solver (Gaussian elimination, partial pivoting).
// Used by the Newton fixed-point iteration on (J - I) dx = -r.
// ---------------------------------------------------------------------------
bool solve_real(std::vector<double> A, std::vector<double>& b, std::size_t n) {
    for (std::size_t k = 0; k < n; ++k) {
        std::size_t p = k;
        double best = std::fabs(A[k * n + k]);
        for (std::size_t i = k + 1; i < n; ++i) {
            const double m = std::fabs(A[i * n + k]);
            if (m > best) { best = m; p = i; }
        }
        if (best < 1e-300) return false;
        if (p != k) {
            for (std::size_t j = 0; j < n; ++j) std::swap(A[k * n + j], A[p * n + j]);
            std::swap(b[k], b[p]);
        }
        for (std::size_t i = k + 1; i < n; ++i) {
            const double f = A[i * n + k] / A[k * n + k];
            if (f == 0.0) continue;
            for (std::size_t j = k; j < n; ++j) A[i * n + j] -= f * A[k * n + j];
            b[i] -= f * b[k];
        }
    }
    for (std::size_t i = n; i-- > 0;) {
        double s = b[i];
        for (std::size_t j = i + 1; j < n; ++j) s -= A[i * n + j] * b[j];
        b[i] = s / A[i * n + i];
    }
    return true;
}

// ---------------------------------------------------------------------------
// State packer: ordered (name, fd-scale, get, set) entries over the
// AircraftState plus (optionally) the AirSteering integrators.
//
// Entry flags:
//   synthetic — identity-map Newton variable (e.g. the trim throttle: the
//               map leaves it unchanged, but the vt row depends on it, so
//               Newton uses it to null the axial residual). Excluded from
//               the eigen spectrum (it is an input, not a dynamic state).
// ---------------------------------------------------------------------------
struct Entry {
    const char* name;
    double scale;                                        // FD step size
    std::function<double(const AircraftState&)> get;
    std::function<void(AircraftState&, double)> set;
    bool synthetic = false;
    double nw = 0.0;   // Newton residual weight (0 = use scale). The stiff
                       // filter-history rows need a RELAXED weight: their
                       // absolute residuals are tiny but the FD scale makes
                       // them dominate the max-norm and stall the line search
                       // on the slow energy rows that actually matter.
};

struct Packer {
    std::vector<Entry> entries;
    std::size_t size() const noexcept { return entries.size(); }

    std::vector<double> gather(const AircraftState& s) const {
        std::vector<double> x(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) x[i] = entries[i].get(s);
        return x;
    }
    void scatter(AircraftState& s, const std::vector<double>& x) const {
        for (std::size_t i = 0; i < entries.size(); ++i) entries[i].set(s, x[i]);
    }
    std::vector<double> fd_scales() const {
        std::vector<double> v(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) v[i] = entries[i].scale;
        return v;
    }
};

// ---------------------------------------------------------------------------
// Diagnosis session: everything needed to evaluate the discrete map.
// ---------------------------------------------------------------------------
void applyDerivedConsistency(AircraftState& s, double psi_rad);

struct Session {
    std::unique_ptr<FlightModel> fm;
    std::unique_ptr<AirSteering> ai;      // only when --ai
    Packer packer;
    double dt = kDefaultMajorDt;
    double psi0 = 0.0;
    double groundZ = 0.0;

    double vt_target = 0.0;   // pinned trim speed (ft/s) for the Newton solve
    double alt_target_ft = 0.0;

    // AI targets (trim values when --ai)
    double ai_hdg_rad = 0.0;
    double ai_alt_ft = 0.0;
    double ai_spd_kts = 0.0;

    std::vector<double> step(const std::vector<double>& x) const {
        AircraftState& s = fm->state();
        packer.scatter(s, x);
        applyDerivedConsistency(s, psi0);

        PilotInput pi;
        if (ai) {
            const auto& st = fm->state();
            AirSteering::Input in;
            in.position = f4::geo::WorldPosition(st.kin.y, st.kin.x, -st.kin.z);
            in.heading_rad = to_radians(st.kin.psi);
            in.pitch_rad = to_radians(st.kin.theta);
            in.roll_rad = to_radians(st.kin.phi);
            in.roll_rate_radps = st.kin.p;
            in.pitch_rate_radps = st.kin.q;
            in.vs_fpm = -st.kin.zdot * 60.0;
            in.vcas_kts = st.vcas;
            in.alt_msl_ft = -st.kin.z;
            const auto out = ai->steer(ai_hdg_rad, ai_alt_ft, ai_spd_kts, in);
            pi.pstick = out.pitch_cmd;
            pi.rstick = out.roll_cmd;
            pi.ypedal = out.yaw_cmd;
            pi.throttle = out.throttle_cmd;
            pi.speedBrake = out.speed_brake_cmd;
            // Gear / surfaces stay on the configured condition, not on the
            // AI cruise law (which commands gear up / clean by default).
            pi.gearHandle = gear_down ? 1.0 : -1.0;
            pi.tefCmd = tef_cmd;
            pi.lefCmd = lef_cmd;
            pi.maxRollDeg = out.max_roll_deg;
            pi.maxRollDeltaDeg = out.max_roll_delta_deg;
            pi.validate();
        } else {
            pi.throttle = base_throttle;
            pi.gearHandle = gear_down ? 1.0 : -1.0;
            pi.tefCmd = tef_cmd;
            pi.lefCmd = lef_cmd;
            pi.validate();
        }
        fm->update(dt, pi, groundZ, kFlatNormal);

        std::vector<double> xn = packer.gather(fm->state());
        if (ai) appendAiStates(xn);
        return xn;
    }

    void appendAiStates(std::vector<double>& x) const {
        const auto d = ai->debug_get_integrators();
        x.push_back(std::isfinite(d.speed_integral) ? d.speed_integral : 0.0);
        x.push_back(d.alt_integral);
        x.push_back(d.vs_target);
        x.push_back(std::isfinite(d.prev_alpha_est) ? d.prev_alpha_est : 0.0);
    }
    void setAiStates(const std::vector<double>& x) const {
        if (!ai) return;
        AirSteering::DebugIntegrators d{};
        const std::size_t n = packer.size();
        d.speed_integral = x[n + 0];
        d.alt_integral = x[n + 1];
        d.vs_target = x[n + 2];
        d.prev_alpha_est = x[n + 3];
        d.approach_alt_integral = 0.0;
        ai->debug_set_integrators(d);
    }

    // Pilot input constants for the non-AI map
    double base_throttle = 0.7;
    bool   gear_down = false;
    double tef_cmd = 0.0;
    double lef_cmd = 0.0;
};

Packer makePacker(Session* ses, bool z_stationary) {
    Packer p;
    auto add = [&](const char* name, double scale,
                   std::function<double(const AircraftState&)> g,
                   std::function<void(AircraftState&, double)> st,
                   bool synthetic = false) {
        p.entries.push_back({name, scale, std::move(g), std::move(st), synthetic, 0.0});
    };
    auto addW = [&](const char* name, double scale, double nw,
                    std::function<double(const AircraftState&)> g,
                    std::function<void(AircraftState&, double)> st) {
        p.entries.push_back({name, scale, std::move(g), std::move(st), false, nw});
    };

    // --- Kinematics ---
    add("vt",        1e-3, [](const AircraftState& s){ return s.kin.vt; },
                      [](AircraftState& s, double v){ s.kin.vt = v; });
    add("theta_rad", 1e-5, [](const AircraftState& s){ return to_radians(s.kin.theta); },
                      [](AircraftState& s, double v){ s.kin.theta = angle_from_radians(v); });
    add("phi_rad",   1e-5, [](const AircraftState& s){ return to_radians(s.kin.phi); },
                      [](AircraftState& s, double v){ s.kin.phi = angle_from_radians(v); });
    // z is a dynamic state ONLY when the AI altitude loop is closed (the AI
    // integral can hold it stationary). In plant mode the FCS integrator
    // leak forces a slow gamma/position drift — no exact equilibrium exists
    // with z in the stationarity set, so z is excluded there (documented).

    // --- Aero coefficients the FCS reads one minor frame stale ---
    addW("a_cl", 1e-4, 1e-2, [](const AircraftState& s){ return s.aero.cl; },
                      [](AircraftState& s, double v){ s.aero.cl = v; });
    add("a_clalph0", 1e-3, [](const AircraftState& s){ return s.aero.clalph0; },
                      [](AircraftState& s, double v){ s.aero.clalph0 = v; });
    addW("a_clift0", 1e-3, 1e-2, [](const AircraftState& s){ return s.aero.clift0; },
                      [](AircraftState& s, double v){ s.aero.clift0 = v; });
    addW("a_cnalpha", 1e-2, 1e-1, [](const AircraftState& s){ return s.aero.cnalpha; },
                      [](AircraftState& s, double v){ s.aero.cnalpha = v; });
    add("a_gearPos", 1e-4, [](const AircraftState& s){ return s.aero.gearPos; },
                      [](AircraftState& s, double v){ s.aero.gearPos = v; });
    add("a_tefPos",  1e-4, [](const AircraftState& s){ return s.aero.tefPos; },
                      [](AircraftState& s, double v){ s.aero.tefPos = v; });
    add("a_lefPos",  1e-4, [](const AircraftState& s){ return s.aero.lefPos; },
                      [](AircraftState& s, double v){ s.aero.lefPos = v; });

    // --- FCS filter / integrator histories ---
    addW("f_pitchI_y", 1e-3, 1e-2, [](const AircraftState& s){ return s.fcs.pitchIntegral.output(); },
                            [](AircraftState& s, double v){ s.fcs.pitchIntegral.prime(v, s.fcs.pitchIntegral.prev_input()); });
    addW("f_pitchI_u", 1e-3, 1e-2, [](const AircraftState& s){ return s.fcs.pitchIntegral.prev_input(); },
                            [](AircraftState& s, double v){ s.fcs.pitchIntegral.prime(s.fcs.pitchIntegral.output(), v); });
    addW("f_rateLag_y", 1e-4, 1e-2, [](const AircraftState& s){ return s.fcs.pitchRateLag.output(); },
                             [](AircraftState& s, double v){ s.fcs.pitchRateLag.prime(v, s.fcs.pitchRateLag.prev_input()); });
    addW("f_rateLag_u", 1e-4, 1e-2, [](const AircraftState& s){ return s.fcs.pitchRateLag.prev_input(); },
                             [](AircraftState& s, double v){ s.fcs.pitchRateLag.prime(s.fcs.pitchRateLag.output(), v); });
    addW("f_alphaLag_y1", 1e-3, 1e-1, [](const AircraftState& s){ return s.fcs.pitchAlphaLag.output_prev1(); },
                               [](AircraftState& s, double v){ s.fcs.pitchAlphaLag.prime(v, s.fcs.pitchAlphaLag.output_prev2(), s.fcs.pitchAlphaLag.input_prev1(), s.fcs.pitchAlphaLag.input_prev2()); });
    addW("f_alphaLag_y2", 1e-3, 1e-1, [](const AircraftState& s){ return s.fcs.pitchAlphaLag.output_prev2(); },
                               [](AircraftState& s, double v){ s.fcs.pitchAlphaLag.prime(s.fcs.pitchAlphaLag.output_prev1(), v, s.fcs.pitchAlphaLag.input_prev1(), s.fcs.pitchAlphaLag.input_prev2()); });
    addW("f_alphaLag_u1", 1e-3, 1e-1, [](const AircraftState& s){ return s.fcs.pitchAlphaLag.input_prev1(); },
                               [](AircraftState& s, double v){ s.fcs.pitchAlphaLag.prime(s.fcs.pitchAlphaLag.output_prev1(), s.fcs.pitchAlphaLag.output_prev2(), v, s.fcs.pitchAlphaLag.input_prev2()); });
    addW("f_alphaLag_u2", 1e-3, 1e-1, [](const AircraftState& s){ return s.fcs.pitchAlphaLag.input_prev2(); },
                               [](AircraftState& s, double v){ s.fcs.pitchAlphaLag.prime(s.fcs.pitchAlphaLag.output_prev1(), s.fcs.pitchAlphaLag.output_prev2(), s.fcs.pitchAlphaLag.input_prev1(), v); });
    addW("f_rollLag_y", 1e-3, 1e-1, [](const AircraftState& s){ return s.fcs.rollRateLag.output(); },
                             [](AircraftState& s, double v){ s.fcs.rollRateLag.prime(v, s.fcs.rollRateLag.prev_input()); });
    addW("f_rollLag_u", 1e-3, 1e-1, [](const AircraftState& s){ return s.fcs.rollRateLag.prev_input(); },
                             [](AircraftState& s, double v){ s.fcs.rollRateLag.prime(s.fcs.rollRateLag.output(), v); });

    // --- Speed damper washout state (Task 64) ---
    // Present ONLY when the damper is enabled: with speedDampGain == 0 the
    // state is FROZEN (identity column, lambda_d = 1 → spurious lambda_c = 0
    // pole), not dead (zero column), so dead-state pruning cannot catch it.
    // At trim vt == speedTrimVt, so the fixed point is damper-neutral — the
    // trim value is unchanged.
    if (ses == nullptr || ses->fm->state().fcs.speedDampGain > 0.0) {
        addW("f_speedTrimVt", 1e-2, 1e-1, [](const AircraftState& s){ return s.fcs.speedTrimVt; },
                                  [](AircraftState& s, double v){ s.fcs.speedTrimVt = v; });
    }

    // --- Load factors (FCS reads them one minor frame stale) ---
    add("l_nzcgs", 1e-4, [](const AircraftState& s){ return s.loads.nzcgs; },
                         [](AircraftState& s, double v){ s.loads.nzcgs = v; });
    add("l_nycgw", 1e-4, [](const AircraftState& s){ return s.loads.nycgw; },
                         [](AircraftState& s, double v){ s.loads.nycgw = v; });

    // (Trig cache + alpha: recomputed/slaved within one minor step — see
    //  applyDerivedConsistency. Their one-minor-frame staleness is a
    //  1/360 s transport delay, dynamically negligible at the modes of
    //  interest, and their injected values would otherwise be clobbered
    //  before use, producing zero Jacobian columns.)

    // --- Engine spool lag ---
    addW("e_rpmLag_y", 1e-4, 1e-2, [](const AircraftState& s){ return s.engine.rpmLag.output(); },
                            [](AircraftState& s, double v){ s.engine.rpmLag.prime(v, s.engine.rpmLag.prev_input()); });
    addW("e_rpmLag_u", 1e-4, 1e-2, [](const AircraftState& s){ return s.engine.rpmLag.prev_input(); },
                            [](AircraftState& s, double v){ s.engine.rpmLag.prime(s.engine.rpmLag.output(), v); });

    // --- Position (AI mode only) ---
    if (z_stationary) {
        add("z_ft", 1e-2, [](const AircraftState& s){ return s.kin.z; },
                          [](AircraftState& s, double v){ s.kin.z = v; });
    }

    // --- Synthetic Newton variable: the trim throttle (plant mode only).
    // The map leaves it unchanged (input), but the vt row depends on it —
    // Newton moves it to null the axial residual. Excluded from eigen. ---
    if (ses != nullptr) {
        add("throttle_input", 1e-3,
            [ses](const AircraftState&){ return ses->base_throttle; },
            [ses](AircraftState&, double v){ ses->base_throttle = std::clamp(v, 0.0, 1.5); },
            /*synthetic=*/true);
    }

    return p;
}

// ---------------------------------------------------------------------------
// Derived-state consistency: the values that must agree with the injected
// states for the discrete map to be exactly the in-flight map.
// ---------------------------------------------------------------------------
void applyDerivedConsistency(AircraftState& s, double psi_rad) {
    // Quaternion from euler (ZYX; mirrors eom.cpp's ground block).
    const double phi   = to_radians(s.kin.phi);
    const double theta = to_radians(s.kin.theta);
    const double cr = std::cos(phi * 0.5),   sr = std::sin(phi * 0.5);
    const double cp = std::cos(theta * 0.5), sp = std::sin(theta * 0.5);
    const double cy = std::cos(psi_rad * 0.5), sy = std::sin(psi_rad * 0.5);
    s.kin.quat = f4::math::Quatd(cr * cp * cy + sr * sp * sy,
                                 sr * cp * cy - cr * sp * sy,
                                 cr * sp * cy + sr * cp * sy,
                                 cr * cp * sy - sr * sp * cy).normalized();
    s.kin.psi = angle_from_radians(psi_rad);

    // Full trig cache recompute (mirror of eom.cpp trigonometry()). The
    // cache is treated as instantaneous: it is overwritten by the frame's
    // own trigonometry() one minor step in, and the 1/360 s transport
    // delay is negligible at the modes of interest.
    const double alpha_rad = to_radians(s.aero.alpha);
    const double beta_rad  = to_radians(s.aero.beta);
    s.kin.sinalp = std::sin(alpha_rad);  s.kin.cosalp = std::cos(alpha_rad);
    s.kin.sinbet = std::sin(beta_rad);   s.kin.cosbet = std::cos(beta_rad);
    s.kin.sinthe = std::sin(theta);      s.kin.costhe = std::cos(theta);
    s.kin.sinphi = std::sin(phi);        s.kin.cosphi = std::cos(phi);
    s.kin.gmma  = angle_from_radians(theta - alpha_rad * s.kin.cosphi);
    s.kin.sigma = angle_from_radians(psi_rad + beta_rad * s.kin.costhe);
    s.kin.mu    = s.kin.phi;
    s.kin.singam = std::sin(to_radians(s.kin.gmma));
    s.kin.cosgam = std::cos(to_radians(s.kin.gmma));
    s.kin.sinsig = std::sin(to_radians(s.kin.sigma));
    s.kin.cossig = std::cos(to_radians(s.kin.sigma));
    s.kin.sinmu  = std::sin(to_radians(s.kin.mu));
    s.kin.cosmu  = std::cos(to_radians(s.kin.mu));

    // Body rates consistent with the lag outputs / algebraic yaw
    s.kin.p = s.fcs.rollRateLag.output();
    s.kin.q = s.fcs.pitchRateLag.output();
    const double tempVt = std::max(4.0, std::fabs(s.kin.vt));
    s.kin.r = (s.loads.nycgw + s.kin.cosgam * s.kin.sinmu) * GRAVITY / tempVt;

    // rpm consistent with its lag output
    s.engine.rpm = s.engine.rpmLag.output();

    // World velocities (AI input reads zdot)
    s.kin.xdot = s.kin.vt * s.kin.cosgam * s.kin.cossig + s.windX;
    s.kin.ydot = s.kin.vt * s.kin.cosgam * s.kin.sinsig + s.windY;
    s.kin.zdot = -s.kin.vt * s.kin.singam;

    // Freeze the stall relay at None for linearization
    s.aero.stallState = StallState{0};
    s.aero.stalled = false;
}


// Full state vector (FM + AI)
std::vector<double> fullState(const Session& ses) {
    std::vector<double> x = ses.packer.gather(ses.fm->state());
    if (ses.ai) ses.appendAiStates(x);
    return x;
}
void setFullState(const Session& ses, const std::vector<double>& x) {
    ses.packer.scatter(ses.fm->state(), x);
    ses.setAiStates(x);
}
std::vector<double> fullScales(const Session& ses) {
    std::vector<double> v = ses.packer.fd_scales();
    if (ses.ai) v.insert(v.end(), {1e-3, 1e-1, 1e-1, 1e-4});  // AI integrators
    return v;
}
std::vector<double> newtonWeights(const Session& ses) {
    std::vector<double> v;
    v.reserve(ses.packer.size() + 4);
    for (const auto& e : ses.packer.entries)
        v.push_back(e.nw > 0.0 ? e.nw : e.scale);
    if (ses.ai) v.insert(v.end(), {1e-3, 1e-1, 1e-1, 1e-4});
    return v;
}
// Normalized max residual
double residualMax([[maybe_unused]] const Session& ses,
                   const std::vector<double>& fx,
                   const std::vector<double>& x, const std::vector<double>& W) {
    double rmax = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
        rmax = std::max(rmax, std::fabs(fx[i] - x[i]) / W[i]);
    return rmax;
}

// ---------------------------------------------------------------------------
// FD Jacobian of the major-frame map at x0 (central differences).
// ---------------------------------------------------------------------------
std::vector<double> jacobian(const Session& ses, const std::vector<double>& x0) {
    const std::size_t n = x0.size();
    const std::vector<double> h = fullScales(ses);
    std::vector<double> J(n * n, 0.0);
    for (std::size_t j = 0; j < n; ++j) {
        std::vector<double> xp = x0, xm = x0;
        xp[j] += h[j];
        xm[j] -= h[j];
        const std::vector<double> fp = ses.step(xp);
        const std::vector<double> fm = ses.step(xm);
        for (std::size_t i = 0; i < n; ++i)
            J[i * n + j] = (fp[i] - fm[i]) / (2.0 * h[j]);
    }
    return J;
}

// ---------------------------------------------------------------------------
// Newton fixed-point of the closed map: solve (J - I) dx = -(F(x) - x).
// ---------------------------------------------------------------------------
struct TrimResult {
    bool converged = false;
    double residual = 0.0;
    int iterations = 0;
    std::vector<double> x;
};

TrimResult newtonFixedPoint(const Session& ses, std::vector<double> x0,
                            int max_iter = 12, double tol = 1e-7) {
    // Structure: the map F(x; delta_t) has a one-parameter family of
    // equilibria indexed by the (synthetic) throttle input. We solve
    //   inner: square Newton on the STATE-only stationarity (J_ss - I), the
    //          throttle frozen — nonsingular at an isolated equilibrium;
    //   outer: secant on the throttle to drive the inner equilibrium's vt to
    //          the requested trim speed.
    TrimResult tr;
    const std::size_t n = x0.size();
    const bool dbg = std::getenv("DIAG_POLES_DEBUG") != nullptr;
    const std::vector<double> W = newtonWeights(ses);

    // Names for every x component (packer entries + AI states)
    std::vector<std::string> xnames;
    for (const auto& e : ses.packer.entries) xnames.push_back(e.name);
    if (ses.ai) {
        xnames.push_back("ai_speedI");
        xnames.push_back("ai_altI");
        xnames.push_back("ai_vsTgt");
        xnames.push_back("ai_prevA");
    }
    const std::size_t n_packer = ses.packer.entries.size();

    std::vector<std::size_t> sidx;   // state indices (non-synthetic)
    std::size_t t_idx = n;
    std::size_t vt_idx = n;
    for (std::size_t i = 0; i < n; ++i) {
        const bool is_synth = (i < n_packer) && ses.packer.entries[i].synthetic;
        if (is_synth) t_idx = i;
        else sidx.push_back(i);
        if (xnames[i] == "vt") vt_idx = i;
    }
    const bool has_synth = (t_idx < n);
    const std::size_t ns = sidx.size();
    const double vt_target = ses.vt_target;

    auto residual_of = [&](const std::vector<double>& fx,
                           const std::vector<double>& x) {
        double rmax = 0.0;
        for (std::size_t i = 0; i < ns; ++i) {
            const std::size_t k = sidx[i];
            rmax = std::max(rmax, std::fabs(fx[k] - x[k]) / W[k]);
        }
        return rmax;
    };

    // --- inner: square damped Newton on the state subvector ---
    auto inner = [&](std::vector<double> x) {
        for (int it = 0; it < max_iter; ++it) {
            const std::vector<double> fx = ses.step(x);
            const double rmax = residual_of(fx, x);
            if (dbg) {
                std::printf("    inner it=%d r=%.3e top:", it, rmax);
                std::vector<std::pair<double, std::size_t>> rn(ns);
                for (std::size_t i = 0; i < ns; ++i)
                    rn[i] = {std::fabs(fx[sidx[i]] - x[sidx[i]]) / W[sidx[i]], sidx[i]};
                std::sort(rn.begin(), rn.end(), std::greater<>());
                for (int k = 0; k < 3; ++k)
                    std::printf(" %s=%.2e", xnames[rn[k].second].c_str(), rn[k].first);
                std::printf("\n");
            }
            if (rmax < tol) return std::pair<std::vector<double>, double>{x, rmax};
            const std::vector<double> J = jacobian(ses, x);
            const std::vector<double> S = fullScales(ses);
            std::vector<double> A(ns * ns);
            for (std::size_t r = 0; r < ns; ++r)
                for (std::size_t c = 0; c < ns; ++c)
                    A[r * ns + c] = J[sidx[r] * n + sidx[c]] * S[sidx[c]] / S[sidx[r]];
            for (std::size_t r = 0; r < ns; ++r) A[r * ns + r] -= 1.0;
            std::vector<double> dy(ns);
            for (std::size_t r = 0; r < ns; ++r)
                dy[r] = -(fx[sidx[r]] - x[sidx[r]]) / S[sidx[r]];
            if (!solve_real(A, dy, ns)) break;
            std::vector<double> dx(n, 0.0);
            for (std::size_t r = 0; r < ns; ++r) dx[sidx[r]] = dy[r] * S[sidx[r]];
            double best_r = rmax;
            std::vector<double> best_x = x;
            for (double sc = 1.0; sc >= 0.0078125; sc *= 0.5) {
                std::vector<double> xt = x;
                for (std::size_t r = 0; r < ns; ++r) xt[sidx[r]] += sc * dx[sidx[r]];
                const std::vector<double> fxt = ses.step(xt);
                const double rt = residual_of(fxt, xt);
                if (rt < best_r) { best_r = rt; best_x = xt; }
                if (sc == 1.0 && rt < rmax) break;
            }
            x = best_x;
        }
        const std::vector<double> fx = ses.step(x);
        return std::pair<std::vector<double>, double>{x, residual_of(fx, x)};
    };

    if (!has_synth) {
        auto [x1, r1] = inner(x0);
        tr.x = x1; tr.residual = r1;
        tr.converged = r1 < tol * 100.0;
        tr.iterations = max_iter;
        return tr;
    }

    // --- outer: secant on the throttle ---
    // Template state: the quasi-trimmed snapshot. Each outer pass restarts
    // the inner Newton from a re-settled template at the trial throttle so a
    // diverged pass cannot poison the next one.
    const std::vector<double> x_template = x0;
    // The trim procedure owns `ses` for its duration; the map reads the
    // trial throttle from ses.base_throttle (the synthetic input channel).
    auto& ses_m = const_cast<Session&>(ses);
    auto resetAndSettle = [&](double thr) {
        ses_m.packer.scatter(ses_m.fm->state(), x_template);
        ses_m.base_throttle = thr;
        for (int i = 0; i < 240; ++i) {
            PilotInput pj;
            pj.throttle = thr;
            pj.gearHandle = ses_m.gear_down ? 1.0 : -1.0;
            pj.tefCmd = ses_m.tef_cmd;
            pj.lefCmd = ses_m.lef_cmd;
            pj.validate();
            ses_m.fm->update(ses_m.dt, pj, ses_m.groundZ, kFlatNormal);
            ses_m.fm->state().kin.vt += (vt_target - ses_m.fm->state().kin.vt) * 0.2;
            ses_m.fm->state().kin.z += (-ses_m.alt_target_ft - ses_m.fm->state().kin.z) * 0.2;
        }
        std::vector<double> xs = ses_m.packer.gather(ses_m.fm->state());
        std::vector<double> full = xs;
        if (ses_m.ai) ses_m.appendAiStates(full);
        full[t_idx] = thr;
        return full;
    };
    double thr = x0[t_idx];
    double e_prev = 0.0, thr_prev = 0.0;
    bool have_prev = false;
    std::vector<double> x = x0;
    for (int outer = 0; outer < 14; ++outer) {
        x = resetAndSettle(thr);
        auto [xs, r_in] = inner(x);
        x = xs;
        const double e = xs[vt_idx] - vt_target;
        if (dbg)
            std::printf("  outer it=%d thr=%.4f vt*=%.3f (target %.3f) e=%+.4f inner_r=%.2e\n",
                        outer, thr, xs[vt_idx], vt_target, e, r_in);
        tr.x = x;
        tr.residual = r_in + std::fabs(e) / W[vt_idx];
        if (std::fabs(e) < 1e-4 && r_in < tol * 100.0) { tr.converged = true; break; }
        // The equilibrium-vt sensitivity to throttle (~1.7 ft/s per unit
        // throttle at cruise; the near-unity slow pole amplifies the
        // per-frame 0.32 sensitivity) with a bounded step: a saturated
        // thrust map breaks the secant badly.
        double step;
        if (!have_prev || std::fabs(e - e_prev) < 1e-12) {
            step = e / 1.7;
        } else {
            const double d = (e - e_prev) / (thr - thr_prev);
            step = (std::fabs(d) > 1e-3) ? e / d : e / 1.7;
        }
        thr -= std::clamp(step, -0.15, 0.15);
        thr = std::clamp(thr, 0.0, 1.5);
        e_prev = e; thr_prev = thr; have_prev = true;
    }
    tr.iterations = 14;
    const std::vector<double> fx = ses.step(tr.x);
    tr.residual = residual_of(fx, tr.x);
    const double e_vt = std::fabs(tr.x[vt_idx] - vt_target);
    tr.converged = (e_vt < 0.1);   // residual floor is the physical integrator-leak creep
    return tr;
}

// ---------------------------------------------------------------------------
// Speed stability: S_u = d(T-D)/dV [1/s] from the aero + engine models
// directly (alpha, throttle, config frozen at trim).
// ---------------------------------------------------------------------------
struct SpeedStability {
    double Su = 0.0;             // 1/s
    double omega_n = 0.0;        // g*sqrt(2)/V
    double zeta_pred = 0.0;      // -Su * V / (2*sqrt(2)*g)
    double drag_ftps2 = 0.0;
    double thrust_ftps2 = 0.0;
};

SpeedStability speedStability(const f4::data::AircraftConfig& cfg,
                              double alt_ft, double vt_ftps,
                              double alpha_deg, double throttle,
                              double mass_slugs,
                              double tefPos, double lefPos, double gearPos) {
    const double S = cfg.geometry.area.value();
    Aerodynamics aero(&cfg.aero, &cfg.geometry, &cfg.aux);
    EngineModel engine(&cfg.engine, &cfg.aux);

    auto axial = [&](double V) {
        const auto atm = computeAtmosphere(alt_ft, V, S, mass_slugs);
        AeroState as{};
        as.tefPos = tefPos;
        as.lefPos = lefPos;
        as.gearPos = gearPos;
        as.dbrake = 0.0;
        as.stallState = StallState{0};
        AeroInputs in{};
        in.alpha = angle_from_degrees(alpha_deg);
        in.beta = zero_angle();
        in.mach = atm.mach;
        in.vt_ftps = V;
        in.qbar = atm.qbar;
        in.qsom = atm.qsom;
        in.altitude_ft = alt_ft;
        in.groundZ_ft = 0.0;
        in.z_ft = -alt_ft;
        in.vcas_kts = atm.vcas;
        in.pstick = 0.0;
        aero.update(in, as);

        // Engine spool to steady state at this V
        EngineState es{};
        es.rpm = 0.7;
        es.engLit = true;
        for (int i = 0; i < 400; ++i) {
            const double prev = es.rpm;
            engine.update(0.25, alt_ft, atm.mach, V, mass_slugs, throttle,
                          1.0, false, es);
            if (std::fabs(es.rpm - prev) < 1e-12) break;
        }
        double xprop, yprop, zprop, xsprop, zsprop;
        const double alp = alpha_deg * kD2R;
        EngineModel::bodyForces(es.thrust, std::sin(alp), std::cos(alp),
                                es.nozzlePos, xprop, yprop, zprop, xsprop, zsprop);
        return std::pair<double, double>{as.xsaero + xsprop, as.drag};
    };

    const double hstep = 1.0;  // ft/s
    const auto [ap, dp] = axial(vt_ftps + hstep);
    const auto [am, dm] = axial(vt_ftps - hstep);
    const auto [a0, d0] = axial(vt_ftps);
    SpeedStability ss;
    ss.Su = (ap - am) / (2.0 * hstep);
    ss.omega_n = GRAVITY * std::sqrt(2.0) / vt_ftps;
    ss.zeta_pred = -ss.Su * vt_ftps / (2.0 * std::sqrt(2.0) * GRAVITY);
    ss.drag_ftps2 = d0;
    ss.thrust_ftps2 = a0 + d0;   // reconstruct thrust from ax + drag
    (void)a0; (void)d0;
    return ss;
}

// ---------------------------------------------------------------------------
// Eigen report — over the DYNAMIC submatrix (synthetic Newton variables such
// as the trim throttle are inputs, not dynamics, and are dropped).
// ---------------------------------------------------------------------------
struct ModeRow {
    cdouble lambda_d;
    cdouble lambda_c;
    double zeta = 0.0;
    double omega_n = 0.0;
    double period_s = 0.0;
    double t2x_s = 0.0;
    std::string participants;
};

std::vector<ModeRow> eigenReport(const std::vector<double>& J_full,
                                 const Packer& packer, bool with_ai,
                                 double dt) {
    // Dynamic indices: non-synthetic packer entries + AI states
    std::vector<std::size_t> idx;
    std::vector<std::string> names;
    for (std::size_t i = 0; i < packer.entries.size(); ++i) {
        if (packer.entries[i].synthetic) continue;
        idx.push_back(i);
        names.push_back(packer.entries[i].name);
    }
    if (with_ai) {
        for (std::size_t i = packer.entries.size();
             i < packer.entries.size() + 4; ++i) idx.push_back(i);
        names.push_back("ai_speedI");
        names.push_back("ai_altI");
        names.push_back("ai_vsTgt");
        names.push_back("ai_prevA");
    }
    const std::size_t n = idx.size();
    std::vector<double> J(n * n);
    // Submatrix extraction — J_full is (N x N), N = full state size
    const std::size_t N = packer.entries.size() + (with_ai ? 4 : 0);
    for (std::size_t r = 0; r < n; ++r)
        for (std::size_t c = 0; c < n; ++c)
            J[r * n + c] = J_full[idx[r] * N + idx[c]];

    // Dead-state pruning: a state whose Jacobian COLUMN is exactly zero has
    // no feedback path at this trim (e.g. a_cl is never read by the FCS; a_cnalpha
    // is only read in G-command mode). Keeping it would inject spurious
    // lambda_d = 0 eigenvalues. Drop row+column and report.
    std::vector<char> dead(n, 0);
    // Relative threshold against the MEDIAN column magnitude: stiff filter
    // columns are ~1e3-1e4 while genuinely dead states carry only FD noise.
    {
        std::vector<double> colmax(n, 0.0);
        for (std::size_t c = 0; c < n; ++c)
            for (std::size_t r = 0; r < n; ++r)
                colmax[c] = std::max(colmax[c], std::fabs(J[r * n + c]));
        std::vector<double> sorted = colmax;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[n / 2];
        const double dead_thresh = 1e-6 * median;
        for (std::size_t c = 0; c < n; ++c)
            if (colmax[c] <= dead_thresh) dead[c] = 1;
    }
    for (std::size_t c = 0; c < n; ++c)
        if (dead[c])
            std::printf("  [eigen] dropping dead state '%s' (no feedback path at this trim)\n",
                        names[c].c_str());
    std::vector<std::size_t> live;
    for (std::size_t i = 0; i < n; ++i) if (!dead[i]) live.push_back(i);
    const std::size_t m = live.size();
    std::vector<double> Jl(m * m);
    for (std::size_t r = 0; r < m; ++r)
        for (std::size_t c = 0; c < m; ++c)
            Jl[r * m + c] = J[live[r] * n + live[c]];
    std::vector<std::string> lnames(m);
    for (std::size_t i = 0; i < m; ++i) lnames[i] = names[live[i]];

    const f4::math::EigenResult er = f4::math::eig_real_general(Jl, m);
    std::vector<ModeRow> rows;
    for (std::size_t i = 0; i < er.values.size(); ++i) {
        ModeRow mr;
        mr.lambda_d = er.values[i];
        // Stiff discrete modes (lambda_d < 0): the principal log maps them to
        // |Im| = pi/dt which is an artifact of the major-rate sampling. Their
        // dynamics live at the MINOR rate (dt/6) — convert against the minor
        // step so zeta/wn stay meaningful.
        const bool stiff = (er.values[i].real() < 0.0);
        const double conv_dt = stiff ? dt / 6.0 : dt;
        const auto mi = f4::math::modal_info(er.values[i], conv_dt);
        mr.lambda_c = mi.lambda_c;
        mr.zeta = mi.zeta;
        mr.omega_n = mi.omega_n;
        mr.period_s = mi.period_s;
        mr.t2x_s = mi.time_to_double_s;
        if (stiff) mr.participants = "[minor-rate] ";
        const auto& v = er.vectors[i];
        std::vector<std::pair<double, std::size_t>> mags(v.size());
        for (std::size_t j = 0; j < v.size(); ++j)
            mags[j] = {std::abs(v[j]), j};
        std::sort(mags.begin(), mags.end(), std::greater<>());
        double total = 0.0;
        for (const auto& mg : mags) total += mg.first;
        for (int k = 0; k < 3 && k < (int)mags.size(); ++k) {
            const double frac = (total > 0) ? mags[k].first / total : 0.0;
            if (frac < 0.12) break;
            mr.participants += (k ? "/" : "") +
                lnames[mags[k].second] +
                ":" + std::to_string((int)std::lround(frac * 100.0));
        }
        rows.push_back(mr);
    }
    return rows;
}

void printModes(const std::vector<ModeRow>& rows) {
    std::printf("  %-32s %10s %10s %7s %8s %9s %9s\n",
                "lambda_c (1/s)", "Re", "Im", "zeta", "wn", "T[s]", "t2x[s]");
    for (const auto& m : rows) {
        std::printf("  %-32s %10.5f %10.5f %7.3f %8.4f %9.2f %9.2f  %s\n",
                    m.participants.c_str(),
                    m.lambda_c.real(), m.lambda_c.imag(), m.zeta,
                    m.omega_n, m.period_s, m.t2x_s, "");
    }
}

// ---------------------------------------------------------------------------
// Condition setup
// ---------------------------------------------------------------------------
f4::data::AircraftConfig loadCfg(const std::string& fixture) {
    auto r = f4::data::loadConfig(fixture);
    if (!r.ok) {
        std::fprintf(stderr, "FATAL: cannot load config %s\n", fixture.c_str());
        std::exit(2);
    }
    return r.config;
}

struct CondSpec {
    double alt_ft = 15000.0;
    double kcas = 250.0;
    std::string config = "clean";   // clean | gear | gearflaps
    double fuel_frac = 0.5;
    double dt = kDefaultMajorDt;
    double qdamp_scale = 1.0;
    bool freeze_bias = false;
    bool use_ai = false;
    double ai_gain_scale = 1.0;
    // Task 64: speed damper (F-2 step 1) + Phase E ablation scales.
    double sd_gain = 0.0;      // G per ft/s (0 = damper off)
    double sd_tau = 60.0;      // washout time constant [s]
    double leak_scale = 1.0;   // QIL integrator-leak scale
    double shed_scale = 1.0;   // STAB-E51 shedding-rate scale
    // AI loop-at-a-time scales (F-3 bandwidth separation, L1/L2).
    double altI_gain_scale = 1.0;    // AirSteering::alt_integral_gain
    double vs_gain_scale = 1.0;      // AirSteering::vs_gain
    double thrI_gain_scale = 1.0;    // AirSteering::throttle_integral_gain
    // Phase E limit-cycle suspects (AI pitch path nonlinearities).
    double vs_window_scale = 1.0;    // STAB-E10 vs_corr window (0 = off)
    double alpha_rate_scale = 1.0;   // EXPERIMENT U2 alpha-rate damp (0 = off)
    double vs_slew_scale = 1.0;      // STAB-E29 VS slew rate scale (0 = off)
    // Named cruise-tune selector (Task 66): "default" (class defaults),
    // "nav" (navigation_module's shipping cruise tune), "nav-linband"
    // (nav + the M3 linear-band rescale the nav comment claims but the
    // code never applied), "approach" (the P5.2 gate tune family).
    std::string tune = "default";
};

double kcasToVt(const f4::data::AircraftConfig& cfg, double alt_ft, double kcas) {
    const auto atm0 = computeAtmosphere(alt_ft, 1.0, cfg.geometry.area.value(), 1.0);
    const double sigma = atm0.rsigma;
    return kcas * 1.68781 / std::sqrt(sigma);
}

std::unique_ptr<Session> makeSession(const f4::data::AircraftConfig& cfg,
                                     const CondSpec& cs) {
    auto ses = std::make_unique<Session>();
    ses->dt = cs.dt;
    ses->fm = std::make_unique<FlightModel>();
    const double vt0 = kcasToVt(cfg, cs.alt_ft, cs.kcas);
    ses->fm->init(cfg, cs.alt_ft, vt0, 0.0, /*inAir=*/true);
    ses->fm->set_internal_fuel_lbs(cs.fuel_frac * cfg.geometry.internalFuel.value());
    ses->fm->setGround(0.0, kFlatNormal);
    ses->gear_down = (cs.config != "clean");
    ses->tef_cmd = (cs.config == "gearflaps") ? 1.0 : 0.0;
    ses->lef_cmd = (cs.config == "gearflaps") ? 1.0 : 0.0;
    ses->fm->state().fcs.pitchRateDampGain *= cs.qdamp_scale;
    // Task 64: speed damper + ablation knobs (applied before the actuate /
    // settle loops so the washout seeds and settles at the trim speed).
    ses->fm->state().fcs.speedDampGain = cs.sd_gain;
    ses->fm->state().fcs.speedDampTau = cs.sd_tau;
    ses->fm->state().fcs.debug_leak_scale = cs.leak_scale;
    ses->fm->state().fcs.debug_shed_scale = cs.shed_scale;

    // Actuate config surfaces to their targets (rate-limited)
    PilotInput pi;
    pi.throttle = 0.7;
    pi.gearHandle = ses->gear_down ? 1.0 : -1.0;
    pi.tefCmd = ses->tef_cmd;
    pi.lefCmd = ses->lef_cmd;
    for (int i = 0; i < 900; ++i) ses->fm->update(cs.dt, pi, 0.0, kFlatNormal);

    // Quasi-steady 1-G trim at the current (possibly drifted) state
    ses->fm->state().kin.vt = vt0;
    ses->fm->state().kin.z = -cs.alt_ft;
    if (!ses->fm->trim()) {
        std::fprintf(stderr, "WARN: quasi-steady trim did not converge\n");
    }
    ses->base_throttle = 0.7;

    // --- Quasi-steady throttle solve with the FULL FCS in the loop ---
    // fm->trim() solves alpha/throttle on the aero+engine models alone and
    // discards the throttle. Here we null the per-frame vt residual with the
    // FCS active: reset the kinematic state each pass (so the point stays in
    // the Newton basin), run one major frame, and secant on the throttle
    // using the measured frame sensitivity (~0.32 (ft/s)/frame per unit
    // throttle, measured by the FD Jacobian).
    {
        auto& st = ses->fm->state();
        double thr = ses->base_throttle;
        double e_prev = 0.0, thr_prev = thr;
        for (int it = 0; it < 60; ++it) {
            st.kin.vt = vt0;
            st.kin.z = -cs.alt_ft;
            st.aero.alpha = angle_from_degrees(std::clamp(to_degrees(st.aero.alpha), -2.0, 14.0));
            // gamma = 0 attitude
            st.kin.theta = st.aero.alpha;   // phi = 0
            st.kin.phi = zero_angle();
            st.kin.cosgam = 1.0; st.kin.singam = 0.0;
            st.kin.gmma = zero_angle();
            st.kin.cosmu = 1.0; st.kin.sinmu = 0.0; st.kin.mu = zero_angle();
            const double thr_before = thr;
            PilotInput pj;
            pj.throttle = thr;
            pj.gearHandle = ses->gear_down ? 1.0 : -1.0;
            pj.tefCmd = ses->tef_cmd;
            pj.lefCmd = ses->lef_cmd;
            pj.validate();
            ses->fm->update(cs.dt, pj, 0.0, kFlatNormal);
            const double e = st.kin.vt - vt0;   // frame vt residual
            if (it > 0 && std::fabs(e - e_prev) > 1e-12) {
                // secant on the frame residual vs throttle
                const double d = (e - e_prev) / (thr - thr_prev);
                if (std::fabs(d) > 1e-6) thr -= e / d;
            } else {
                thr -= e / 0.32;
            }
            thr = std::clamp(thr, 0.0, 1.5);
            e_prev = e; thr_prev = thr_before;
            if (std::fabs(e) < 1e-4) break;
        }
        ses->base_throttle = thr;
        // settle filters/attitude at the trimmed throttle (state resets each
        // pass above keep the point near trim; a short free run lets the FCS
        // filters and theta settle)
        for (int i = 0; i < 240; ++i) {
            PilotInput pj;
            pj.throttle = ses->base_throttle;
            pj.gearHandle = ses->gear_down ? 1.0 : -1.0;
            pj.tefCmd = ses->tef_cmd;
            pj.lefCmd = ses->lef_cmd;
            pj.validate();
            ses->fm->update(cs.dt, pj, 0.0, kFlatNormal);
            // hold vt/z near the target so the free run doesn't drift away
            ses->fm->state().kin.vt += (vt0 - ses->fm->state().kin.vt) * 0.2;
            ses->fm->state().kin.z += (-cs.alt_ft - ses->fm->state().kin.z) * 0.2;
        }
    }
    ses->vt_target = vt0;
    ses->alt_target_ft = cs.alt_ft;
    ses->packer = makePacker(ses.get(), cs.use_ai);

    if (cs.use_ai) {
        ses->ai = std::make_unique<AirSteering>();
        // Task 66: named cruise tunes (applied BEFORE the per-knob scales so
        // both compose). "nav" mirrors NavigationModule's constructor;
        // "nav-linband" adds the M3 linear-band rule (path_gain small enough
        // that gamma_corr stays proportional across the +-2,000 fpm phugoid
        // band instead of railing into a relay).
        if (cs.tune == "nav" || cs.tune == "nav-linband") {
            ses->ai->attitude_gain = 1.0;
            ses->ai->vs_gain = 2.5;
            ses->ai->max_vs_fpm = 1500.0;
            ses->ai->roll_gain = 4.0;
            ses->ai->balloon_guard_fpm = 1000000.0;
        }
        if (cs.tune == "nav-linband") {
            ses->ai->path_gain = 0.00005;
            ses->ai->gamma_corr_limit = 0.10;
        }
        if (cs.tune == "approach") {
            ses->ai->bank_gain = 1.2;
            ses->ai->max_bank_rad = 0.44;
            ses->ai->roll_gain = 3.0;
            ses->ai->vs_gain = 3.0;
            ses->ai->max_vs_fpm = 1400.0;
            ses->ai->alt_integral_max = 150.0;
            ses->ai->vs_slew_fpm_per_s = 800.0;
            ses->ai->vs_corr_max_fpm = 300.0;
            ses->ai->path_gain = 0.00006;
            ses->ai->gamma_corr_limit = 0.10;
            ses->ai->attitude_gain = 0.9;
            ses->ai->pitch_rate_damp = 0.5;
        }
        ses->ai->vs_gain *= cs.ai_gain_scale * cs.vs_gain_scale;
        ses->ai->throttle_gain *= cs.ai_gain_scale;
        ses->ai->alt_integral_gain *= cs.altI_gain_scale;
        ses->ai->throttle_integral_gain *= cs.thrI_gain_scale;
        if (cs.vs_window_scale != 1.0) {
            ses->ai->vs_corr_max_fpm = (cs.vs_window_scale <= 0.0)
                ? -1.0  // negative = window disabled (stock clamp path)
                : ses->ai->vs_corr_max_fpm * cs.vs_window_scale;
        }
        ses->ai->alpha_rate_damp *= cs.alpha_rate_scale;
        if (cs.vs_slew_scale != 1.0) {
            ses->ai->vs_slew_fpm_per_s = (cs.vs_slew_scale <= 0.0)
                ? 0.0  // 0 = slew limiter disabled
                : ses->ai->vs_slew_fpm_per_s * cs.vs_slew_scale;
        }
        ses->ai_alt_ft = cs.alt_ft;
        const auto& st = ses->fm->state();
        ses->ai_spd_kts = st.vcas;
        ses->ai_hdg_rad = 0.0;
        const double vs_fpm = -st.kin.zdot * 60.0;
        const double alpha_est = to_radians(st.kin.theta) - st.kin.singam;
        ses->ai->seed_from_state(vs_fpm, alpha_est);
    }
    return ses;
}

// ---------------------------------------------------------------------------
// Time-domain verification: perturb and run, log, estimate period via
// zero crossings of the mean-removed signal.
// ---------------------------------------------------------------------------
void verifyRun(const Session& ses, const std::vector<double>& x0,
               double seconds, const std::string& csv_path,
               double amp_fps = 25.0) {
    std::vector<double> x = x0;
    // Inject amp_fps of speed (a phugoid-energy perturbation).
    // Iterate the PACKER entries only: with the AI loop closed, x0 also
    // carries the 4 trailing AI integrator states, and indexing
    // entries[] with the full-state size reads past the vector (the
    // pre-existing segfault in --ai --verify).
    for (std::size_t i = 0; i < ses.packer.entries.size(); ++i)
        if (std::strcmp(ses.packer.entries[i].name, "vt") == 0) x[i] += amp_fps;
    std::vector<double> vs, vcas, alt;
    std::ofstream f(csv_path);
    f << "t,vs_fpm,vcas_kts,alt_ft,nz,alpha_deg,pstick,throttle\n";
    const double dt = ses.dt;
    const int ticks = (int)(seconds / dt);
    double pstick = 0.0, thr = 0.0;
    for (int i = 0; i < ticks; ++i) {
        // Step with the session's own map (AI or constant inputs)
        AircraftState& s = ses.fm->state();
        ses.packer.scatter(s, x);
        applyDerivedConsistency(s, ses.psi0);
        PilotInput pi;
        if (ses.ai) {
            AirSteering::Input in;
            in.heading_rad = to_radians(s.kin.psi);
            in.pitch_rad = to_radians(s.kin.theta);
            in.roll_rad = to_radians(s.kin.phi);
            in.roll_rate_radps = s.kin.p;
            in.pitch_rate_radps = s.kin.q;
            in.vs_fpm = -s.kin.zdot * 60.0;
            in.vcas_kts = s.vcas;
            in.alt_msl_ft = -s.kin.z;
            const auto out = ses.ai->steer(ses.ai_hdg_rad, ses.ai_alt_ft, ses.ai_spd_kts, in);
            pi.pstick = out.pitch_cmd;
            pi.rstick = out.roll_cmd;
            pi.ypedal = out.yaw_cmd;
            pi.throttle = out.throttle_cmd;
            pi.speedBrake = out.speed_brake_cmd;
            pi.gearHandle = ses.gear_down ? 1.0 : -1.0;
            pi.tefCmd = ses.tef_cmd;
            pi.lefCmd = ses.lef_cmd;
            pstick = pi.pstick; thr = pi.throttle;
        } else {
            pi.throttle = ses.base_throttle;
            pi.gearHandle = ses.gear_down ? 1.0 : -1.0;
            pi.tefCmd = ses.tef_cmd;
            pi.lefCmd = ses.lef_cmd;
            pstick = 0.0; thr = pi.throttle;
        }
        pi.validate();
        ses.fm->update(dt, pi, ses.groundZ, kFlatNormal);
        x = ses.packer.gather(ses.fm->state());
        if (ses.ai) ses.appendAiStates(x);
        const auto& st = ses.fm->state();
        const double t = i * dt;
        vs.push_back(-st.kin.zdot * 60.0);
        vcas.push_back(st.vcas);
        alt.push_back(-st.kin.z);
        f << t << "," << vs.back() << "," << vcas.back() << "," << alt.back()
          << "," << st.loads.nzcgs << "," << to_degrees(st.aero.alpha)
          << "," << pstick << "," << thr << "\n";
    }
    f.close();

    // Zero-crossing period estimate on the mean-removed VS signal
    const double N = (double)vs.size();
    const double mean = std::accumulate(vs.begin(), vs.end(), 0.0) / N;
    int crossings = 0;
    double amp = 0.0;
    for (std::size_t i = 1; i < vs.size(); ++i) {
        if ((vs[i - 1] - mean) * (vs[i] - mean) < 0.0) ++crossings;
        amp = std::max(amp, std::fabs(vs[i] - mean));
    }
    const double period = (crossings > 1)
        ? 2.0 * seconds / (double)(crossings / 2) : 0.0;
    std::printf("  verify: VS mean %.1f fpm, max |dVS| %.1f fpm, est period %.1f s (%d half-crossings)\n",
                mean, amp, period, crossings);
}

void usage() {
    std::printf(
        "diag_poles — pole-based flight-control diagnosis\n"
        "  --fixture <path>          aircraft JSON (default $F4_GENERATED_FIXTURES_DIR/f16.json)\n"
        "  --alt <ft> --kcas <kts> --config clean|gear|gearflaps --fuel <0..1>\n"
        "  --dt <s>                  major frame (default 1/60)\n"
        "  --qdamp-scale <x>         FCS pitch-rate damper gain scale (Phase C L4)\n"
        "  --freeze-bias             freeze the 1-G alpha bias' sensed feedback (Phase C L3)\n"
        "  --sd-gain <x>             FCS speed damper gain, G per ft/s (Task 64; 0=off)\n"
        "  --sd-tau <s>              FCS speed damper washout time constant (default 60)\n"
        "  --leak-scale <x>          QIL integrator-leak scale (Phase E ablation; 0=off)\n"
        "  --shed-scale <x>          STAB-E51 shedding-rate scale (Phase E ablation)\n"
        "  --tune <name>             AI cruise tune: default|nav|nav-linband|approach\n"
        "  --altI-gain-scale <x>     AI altitude-integral gain scale (F-3 L1)\n"
        "  --vs-gain-scale <x>       AI vs_gain scale (F-3 L1)\n"
        "  --thrI-gain-scale <x>     AI throttle-integral gain scale (F-3 L2)\n"
        "  --vs-window-scale <x>     AI STAB-E10 vs_corr window scale; 0=off\n"
        "  --alpha-rate-scale <x>    AI U2 alpha-rate damp scale; 0=off\n"
        "  --vs-slew-scale <x>       AI STAB-E29 VS slew rate scale; 0=off\n"
        "  --ai                      close the AirSteering cruise law (Phase C L1/L2)\n"
        "  --ai-gain-scale <x>       scale vs_gain and throttle_gain\n"
        "  --verify [csv]            180 s time-domain run from trim with +25 ft/s\n"
        "  --verify-amp <ft/s>       verify perturbation amplitude (default 25)\n"
        "  --sweep <csv>             sweep kcas x config, CSV out\n"
        "  --help\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string fixture;
    if (const char* env = std::getenv("F4_GENERATED_FIXTURES_DIR")) {
        fixture = std::string(env) + "/f16.json";
    }
    CondSpec cs;
    std::string sweep_csv, verify_csv;
    bool verify = false;
    double verify_amp = 25.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](void) -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--fixture") fixture = next();
        else if (a == "--alt") cs.alt_ft = std::stod(next());
        else if (a == "--kcas") cs.kcas = std::stod(next());
        else if (a == "--config") cs.config = next();
        else if (a == "--fuel") cs.fuel_frac = std::stod(next());
        else if (a == "--dt") cs.dt = std::stod(next());
        else if (a == "--qdamp-scale") cs.qdamp_scale = std::stod(next());
        else if (a == "--freeze-bias") cs.freeze_bias = true;
        else if (a == "--sd-gain") cs.sd_gain = std::stod(next());
        else if (a == "--sd-tau") cs.sd_tau = std::stod(next());
        else if (a == "--leak-scale") cs.leak_scale = std::stod(next());
        else if (a == "--shed-scale") cs.shed_scale = std::stod(next());
        else if (a == "--tune") cs.tune = next();
        else if (a == "--altI-gain-scale") cs.altI_gain_scale = std::stod(next());
        else if (a == "--vs-gain-scale") cs.vs_gain_scale = std::stod(next());
        else if (a == "--thrI-gain-scale") cs.thrI_gain_scale = std::stod(next());
        else if (a == "--vs-window-scale") cs.vs_window_scale = std::stod(next());
        else if (a == "--alpha-rate-scale") cs.alpha_rate_scale = std::stod(next());
        else if (a == "--vs-slew-scale") cs.vs_slew_scale = std::stod(next());
        else if (a == "--ai") cs.use_ai = true;
        else if (a == "--ai-gain-scale") cs.ai_gain_scale = std::stod(next());
        else if (a == "--verify") { verify = true; verify_csv = next(); }
        else if (a == "--verify-amp") verify_amp = std::stod(next());
        else if (a == "--sweep") sweep_csv = next();
        else if (a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); usage(); return 2; }
    }
    if (fixture.empty()) {
        std::fprintf(stderr, "FATAL: no fixture (set F4_GENERATED_FIXTURES_DIR or --fixture)\n");
        return 2;
    }

    const auto cfg = loadCfg(fixture);

    // ------------------------------------------------------------------
    // Sweep mode: condition grid, one summary CSV row each.
    // ------------------------------------------------------------------
    if (!sweep_csv.empty()) {
        std::ofstream f(sweep_csv);
        f << "alt_ft,kcas,config,ai,sd_gain,sd_tau,leak_scale,shed_scale,converged,trim_residual,"
          "trim_alpha_deg,trim_throttle,trim_alt_ft,trim_vcas_kts,"
          "Su_1_s,zeta_su_pred,omega_n_su,"
          "worst_Re_1s,worst_slow_Re_1s,slow_osc_Re,slow_osc_Im,slow_osc_zeta,slow_osc_T_s,"
          "slow_osc_participants\n";
        const std::vector<double> kcass = {160.0, 200.0, 250.0, 300.0, 350.0, 450.0};
        const std::vector<std::string> cfgs = {"clean", "gear", "gearflaps"};
        for (const std::string& c : cfgs) {
            for (const double kc : kcass) {
                CondSpec c2 = cs;
                c2.kcas = kc;
                c2.config = c;
                auto ses = makeSession(cfg, c2);
                std::vector<double> x0 = fullState(*ses);
                const TrimResult tr = newtonFixedPoint(*ses, x0);
                setFullState(*ses, tr.x);
                const auto& st = ses->fm->state();

                const auto ss = speedStability(cfg, -st.kin.z, st.kin.vt,
                                               to_degrees(st.aero.alpha),
                                               ses->base_throttle,
                                               st.fuel.mass_slugs,
                                               st.aero.tefPos, st.aero.lefPos,
                                               st.aero.gearPos);
                const std::vector<double> J = jacobian(*ses, tr.x);
                const auto rows = eigenReport(J, ses->packer, cs.use_ai,
                                              ses->dt);
                // Worst mode = max Re; worst_slow = max Re among sub-1-rad/s
                // modes (the AI slew limiter's describing-function artifacts
                // sit at Re 1e2-1e3 and pollute `worst` in --ai runs — Task
                // 63 flagged this; worst_slow is the stability metric that
                // matters for the phugoid band). slow_osc = largest |Im| <
                // 1 rad/s.
                double worst = -1e300;
                double worst_slow = -1e300;
                const ModeRow* slow = nullptr;
                for (const auto& m : rows) {
                    worst = std::max(worst, m.lambda_c.real());
                    if (m.lambda_c.real() < 1.0)
                        worst_slow = std::max(worst_slow, m.lambda_c.real());
                    if (std::abs(m.lambda_c.imag()) > 1e-6 &&
                        std::abs(m.lambda_c.imag()) < 1.0) {
                        if (!slow || m.lambda_c.real() > slow->lambda_c.real()) slow = &m;
                    }
                }
                f << c2.alt_ft << "," << kc << "," << c << "," << (cs.use_ai ? 1 : 0)
                  << "," << cs.sd_gain << "," << cs.sd_tau
                  << "," << cs.leak_scale << "," << cs.shed_scale
                  << "," << (tr.converged ? 1 : 0) << "," << tr.residual
                  << "," << to_degrees(st.aero.alpha) << "," << ses->base_throttle
                  << "," << -st.kin.z << "," << st.vcas
                  << "," << ss.Su << "," << ss.zeta_pred << "," << ss.omega_n
                  << "," << worst << "," << worst_slow;
                if (slow) {
                    f << "," << slow->lambda_c.real() << "," << slow->lambda_c.imag()
                      << "," << slow->zeta << "," << slow->period_s
                      << ",\"" << slow->participants << "\"";
                } else {
                    f << ",0,0,0,0,\"\"";
                }
                f << "\n";
                std::printf("[%3.0f kcas %-9s] conv=%d r=%.1e alpha=%.1f thr=%.2f Su=%+.4f 1/s zeta_su=%+.3f worstRe=%+.4f %s\n",
                            kc, c.c_str(), tr.converged ? 1 : 0, tr.residual,
                            to_degrees(st.aero.alpha), ses->base_throttle,
                            ss.Su, ss.zeta_pred, worst,
                            (slow ? slow->participants.c_str() : ""));
                std::fflush(stdout);
            }
        }
        std::printf("sweep written to %s\n", sweep_csv.c_str());
        return 0;
    }

    // ------------------------------------------------------------------
    // Single-condition mode.
    // ------------------------------------------------------------------
    auto ses = makeSession(cfg, cs);
    std::vector<double> x0 = fullState(*ses);
    const TrimResult tr = newtonFixedPoint(*ses, x0);
    setFullState(*ses, tr.x);
    const auto& st = ses->fm->state();

    std::printf("== condition: alt=%.0f ft kcas=%.0f config=%s fuel=%.0f%% dt=%.4f%s%s%s%s%s%s\n",
                cs.alt_ft, cs.kcas, cs.config.c_str(), cs.fuel_frac * 100.0, cs.dt,
                cs.use_ai ? " [AI closed]" : "", cs.freeze_bias ? " [bias frozen]" : "",
                (cs.qdamp_scale != 1.0) ? " [qdamp scaled]" : "",
                (cs.sd_gain != 0.0) ? " [speed damper]" : "",
                (cs.leak_scale != 1.0) ? " [leak scaled]" : "",
                (cs.shed_scale != 1.0) ? " [shed scaled]" : "");
    std::printf("trim: converged=%s residual=%.2e iters=%d alpha=%.2f deg throttle=%.3f alt=%.1f ft vcas=%.1f kt W=%.0f lb\n",
                tr.converged ? "yes" : "NO", tr.residual, tr.iterations,
                to_degrees(st.aero.alpha), ses->base_throttle,
                -st.kin.z, st.vcas, st.fuel.weight_lbs);

    // Optional: freeze the alpha-bias sensed feedback at the TRIM values
    // (Phase C L3) — after the live-bias Newton, so the trim value is
    // unchanged and only the feedback path differs.
    if (cs.freeze_bias) {
        auto& stw = ses->fm->state();
        stw.fcs.debug_bias_cosgam = st.kin.cosgam;
        stw.fcs.debug_bias_cosmu = st.kin.cosmu;
        stw.fcs.debug_bias_qsom = st.qsom;
        stw.fcs.debug_bias_freeze = true;
        const TrimResult tr2 = newtonFixedPoint(*ses, fullState(*ses));
        setFullState(*ses, tr2.x);
        std::printf("trim (frozen bias): converged=%s residual=%.2e alpha=%.2f deg throttle=%.3f\n",
                    tr2.converged ? "yes" : "NO", tr2.residual,
                    to_degrees(ses->fm->state().aero.alpha), ses->base_throttle);
    }

    const auto ss = speedStability(cfg, -st.kin.z, st.kin.vt,
                                   to_degrees(st.aero.alpha),
                                   ses->base_throttle, st.fuel.mass_slugs,
                                   st.aero.tefPos, st.aero.lefPos,
                                   st.aero.gearPos);
    std::printf("speed stability (alpha/throttle frozen): Su=%+.4f 1/s  omega_n=%.4f rad/s (T=%.1f s)  zeta_pred=%+.3f  (drag=%.2f, thrust=%.2f ft/s^2)\n",
                ss.Su, ss.omega_n, 2.0 * kPi / ss.omega_n, ss.zeta_pred,
                ss.drag_ftps2, ss.thrust_ftps2);

    const std::vector<double> J = jacobian(*ses, tr.x);
    const auto rows = eigenReport(J, ses->packer, cs.use_ai, ses->dt);
    std::printf("eigenvalues (continuous equivalent, dt=%.4f s, n=%zu):\n",
                ses->dt, rows.size());
    printModes(rows);

    if (verify) {
        if (verify_csv.empty()) verify_csv = "diag_poles_verify.csv";
        std::printf("verify run (%.0f s) -> %s\n", 180.0, verify_csv.c_str());
        verifyRun(*ses, tr.x, 180.0, verify_csv, verify_amp);
    }
    return tr.converged ? 0 : 1;
}

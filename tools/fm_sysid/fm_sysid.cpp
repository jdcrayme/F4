// tools/fm_sysid/fm_sysid.cpp
//
// PHUG-PLAN Phase 1-3 — flight-model system-identification harness.
// See Docs/LONGITUDINAL_STABILITY_PLAN.md §5 (plant identification),
// §6 (loop margins), §7 (closed-loop bisection).
//
// This tool isolates the longitudinal channel into its constituent loops
// and measures each one. It is the measurement instrument for the
// longitudinal-stability diagnosis; it changes no control-law code.
//
// Modes:
//   alpha-sweep  <alt_ft> <vt_fps> [gear 0|1] [out.csv]
//       Direct Aerodynamics sweeps at fixed V/alt: nzcgs/CL/CD vs alpha.
//       Identifies K_nz(V) = dnzcgs/dalpha (the L0 loop gain) and the
//       drag polar. Uses the FM's own atmosphere for qbar/qsom/mach.
//
//   speed-sweep  <alt_ft> <vt_lo> <vt_hi> <steps> [gear 0|1] [out.csv]
//       K_nz, qbar, qsom, and trim-CL vs V (the speed couplings that own
//       the classical phugoid-adjacent dynamics in this pseudo-model).
//
//   trim-hold    <alt_ft> <vt_fps> <seconds> [out.csv]
//       BISECTION CONFIG A: trim, then hands-off (pstick=0, throttle
//       frozen at the discovered trim value). FCS-internal 1-G hold only.
//       Any oscillation here is owned by L0/L1/L2 (FCS core, q-damper,
//       alpha-bias coupling).
//
//   stick-step   <alt_ft> <vt_fps> <step> <seconds> [out.csv]
//       pstick step at t=2 s: the L0 closed-loop G response.
//
//   throttle-step <alt_ft> <vt_fps> <step> <seconds> [out.csv]
//       throttle step at t=2 s: engine lag + axial dynamics (L4 plant).
//
//   ai-hold      <alt_ft> <vt_fps> <seconds> <alt_loop 0/1> <speed_loop 0/1>
//                [heading_deg] [out.csv]
//       BISECTION CONFIGS C/D/E: AirSteering cascade around the FM.
//         alt_loop=0, speed_loop=0 : pstick=0, throttle frozen   (≈ config A)
//         alt_loop=1, speed_loop=0 : altitude/VS cascade, frozen throttle
//         alt_loop=0, speed_loop=1 : pstick=0, speed PI on throttle
//         alt_loop=1, speed_loop=1 : full steer()                (config E)
//       The pairing (1,1) vs (1,0)/(0,1) is the L3/L4 beat-mode test.
//
//   thrust-map   <alt_ft> [out.csv]
//       Static EngineModel map: thrust accel over (throttle x mach) at
//       fixed altitude. Phase-2 evidence for the "two-timeconstant"
//       throttle-step response: below MIL the thrust is ALGEBRAIC in
//       throttle (engine.cpp MIL branch) — any slow creep in a step test
//       must come from the dThrust/dMach table slope acting through the
//       airframe's speed integration, not from a second engine lag.
//
//   margin <alt_ft> <vt_fps> <case g|g0|C|E> <gear 0|1> [amp] [out.csv]
//       PHUG-PLAN Phase 2: sinusoidal-reference injection + lock-in
//       extraction of the closed-loop frequency response R(jw) per
//       channel, per frequency. Cases:
//         g  : pstick sinusoid, hands-off (L0 loop incl. q-damper L1)
//         g0 : same with the Tranche-42 q-damper zeroed (isolates L1's
//              contribution by difference)
//         C  : altitude-TARGET sinusoid, alt loop on, throttle frozen
//              (isolated L3 cascade; speed_damp cross-term remains — it
//              lives inside the pitch path)
//         E  : altitude- AND speed-target sinusoids (two sub-sweeps),
//              full stack: the 2x2 closed-loop matrix for L3/L4 MIMO
//       Every run measures ALL channels against the injected reference,
//       so cross-couplings come out of the same runs.
//       Output rows: one per (frequency, channel) with the complex R.
//
//   rootlocus <alt_ft> <vt_fps> <gear 0|1> <seconds_per_run> [out.csv]
//       PHUG-PLAN Phase 3 remainder: brute-force one-at-a-time gain sweep
//       of the AirSteering cascade parameters (x0.5 / base / x2) at
//       config E. Time-series CSV at 10 Hz per run; dominant period and
//       damping extracted by scripts/trace_metrics.py. A mode whose
//       period tracks a parameter is owned by that parameter's loop.
//
// All time-series modes discard nothing (record from t=0); the analysis
// tooling applies the warm-up window. One row per 60 Hz major tick
// (rootlocus: 10 Hz decimated).
//
// Usage: fm_sysid <mode> [args...] [out.csv]
// Without out.csv, CSV goes to stdout.

#include "f4/flight/flight_model.hpp"
#include "f4/flight/aerodynamics.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/constants.hpp"
#include "f4/data/config_loader.hpp"
#include "f4/ai/air_steering.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace f4::flight;
using f4::ai::AirSteering;
using f4::math::Vec3d;

namespace {

constexpr const char* kFixturesDir = F4_GENERATED_FIXTURES_DIR;
constexpr double MAJOR_DT = 1.0 / 60.0;
constexpr double GROUND_Z_FLAT = 0.0;
const Vec3d FLAT_NORMAL{0.0, 0.0, -1.0};
constexpr double KT_PER_FPS = 0.592484;
constexpr double FPS_PER_KT = 1.68781;
constexpr double TWO_PI = 6.28318530717958647692;

// Landing/approach configuration for the 4th trim point
// (PHUG-PLAN §5: "160 kts gear+flaps+TEF").
struct ConfigFlags {
    bool gear_down = false;
    bool tef = false;   // trailing-edge flaps (approach config)
    bool lef = false;   // leading-edge flaps
};

bool loadF16Config(f4::data::AircraftConfig& cfg) {
    const std::string path = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(path)) return false;
    auto r = f4::data::loadConfig(path);
    if (!r.ok) return false;
    cfg = r.config;
    return true;
}

// Build a trimmed F16 at (alt, vt). `flags` selects the approach config:
// gear/flap positions are pre-set BEFORE trim so the trim solver sees the
// real drag and landing gains (gear travel takes 3 s at 60 Hz otherwise,
// which would contaminate the first seconds of every run).
std::unique_ptr<FlightModel> makeTrimmedF16(double alt_ft, double vt_ftps,
                                            ConfigFlags flags = {}) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) return nullptr;
    auto fm = std::make_unique<FlightModel>();
    fm->init(cfg, alt_ft, vt_ftps, /*heading_rad=*/0.0, /*inAir=*/true);
    if (flags.gear_down) fm->state().aero.gearPos = 1.0;
    if (flags.tef)       fm->state().aero.tefPos  = 1.0;
    if (flags.lef)       fm->state().aero.lefPos  = 1.0;
    if (!fm->trim()) return nullptr;
    fm->setGround(GROUND_Z_FLAT, FLAT_NORMAL);
    return fm;
}

PilotInput handsOffInput(double throttle, ConfigFlags flags = {}) {
    PilotInput pi;
    pi.pstick = 0.0;
    pi.rstick = 0.0;
    pi.ypedal = 0.0;
    pi.throttle = throttle;
    pi.speedBrake = -1.0;
    pi.gearHandle = flags.gear_down ? 1.0 : -1.0;
    pi.tefCmd = flags.tef ? 1.0 : 0.0;
    pi.lefCmd = flags.lef ? 1.0 : 0.0;
    pi.noseSteerOn = false;
    pi.validate();
    return pi;
}

AirSteering::Input readState(const FlightModel& fm) {
    AirSteering::Input in;
    const auto& s = fm.state();
    in.position = f4::geo::WorldPosition(s.kin.y, s.kin.x, -s.kin.z);
    in.heading_rad = to_radians(s.kin.psi);
    in.pitch_rad    = to_radians(s.kin.theta);
    in.roll_rad     = to_radians(s.kin.phi);
    in.roll_rate_radps = s.kin.p;
    in.pitch_rate_radps = s.kin.q;
    in.vs_fpm       = -s.kin.zdot * 60.0;
    in.vcas_kts     = s.vcas;
    in.alt_msl_ft   = -s.kin.z;
    return in;
}

void write_header(std::ostream& os) {
    os << "t,vt_fps,vcas_kts,alt_msl_ft,vs_fpm,gamma_deg,alpha_deg,theta_deg,"
       << "phi_deg,q_dps,nzcgs,ptcmd,aoacmd_deg,alpha_bias_deg,q_damper_term,"
       << "pi_error,pitch_integral,omega_sp,zp01,tp02,tp03,"
       << "pstick,throttle,thrust_accel,vt_dot,qsom,qbar,stall_state,"
       << "vs_target_fpm,vs_corr_fpm,alt_err_ft,gamma_corr,alpha_est,"
       << "theta_target,speed_err_kt,speed_integral,energy_err_ft,"
       // PHUG-P2: loop-attribution additions (appended; earlier columns
       // keep their Phase-1 positions).
       << "speed_brake,tef_pos,lef_pos,gear_pos,rpm,ai_pitch_cmd\n";
}

void write_row(std::ostream& os, double t, const FlightModel& fm,
               const PilotInput& pi,
               const f4::ai::AirSteerDebug* dbg) {
    const auto& s = fm.state();
    const auto& fcs = s.fcs;
    os << t << ','
       << s.kin.vt << ',' << s.vcas << ',' << -s.kin.z << ','
       << -s.kin.zdot * 60.0 << ','
       << to_degrees(s.kin.gmma) << ','
       << to_degrees(s.aero.alpha) << ','
       << to_degrees(s.kin.theta) << ','
       << to_degrees(s.kin.phi) << ','
       << s.kin.q * 180.0 / 3.14159265358979323846 << ','
       << s.loads.nzcgs << ','
       << fcs.ptcmd << ','
       << to_degrees(fcs.aoacmd) << ','
       << fcs.alphaBiasDeg << ','
       << fcs.qDamperTerm << ','
       << fcs.piError << ','
       << fcs.pitchIntegral.output() << ','
       << fcs.omegaSp << ',' << fcs.zp01 << ',' << fcs.tp02 << ',' << fcs.tp03 << ','
       << pi.pstick << ',' << pi.throttle << ','
       << s.engine.thrust << ',' << s.vtDot << ','
       << s.qsom << ',' << s.qbar << ','
       << static_cast<int>(s.aero.stallState);
    if (dbg) {
        os << ',' << dbg->vs_target_fpm << ',' << dbg->vs_corr_fpm << ','
           << dbg->alt_err_ft << ',' << dbg->gamma_corr_rad << ','
           << dbg->alpha_est_rad << ',' << dbg->theta_target_rad << ','
           << dbg->speed_err_kt << ',' << dbg->speed_integral << ','
           << dbg->energy_err_ft;
    } else {
        os << ",0,0,0,0,0,0,0,0,0";
    }
    os << ',' << pi.speedBrake
       << ',' << s.aero.tefPos << ',' << s.aero.lefPos << ',' << s.aero.gearPos
       << ',' << s.engine.rpm << ',' << (dbg ? dbg->pitch_cmd : 0.0)
       << '\n';
}

// Discover the trim throttle: trim, then let the AirSteering speed PI settle
// for a few seconds; the mean throttle over the settle window is the value
// that holds the target energy state. Used to freeze the throttle for the
// speed-loop-off bisection configs.
// Exact trim throttle via the thrust map. The MIL-branch thrust is LINEAR
// in throttle: T(thr) = ((Tmil − Tidle)·thr + Tidle) / mass — so the
// throttle that produces the trimmed thrust (state().engine.thrust, which
// FlightModel::trim() converged to) is a simple inverse interpolation
// between two static EngineModel evaluations. The previous 8-s AirSteering
// settle was unreliable: with a correct CAS target the speed PI sits at
// throttle_mid with zero error (never discovering the trim value), and
// with any target error its leaky integral does not converge in 8 s.
double discover_trim_throttle(double alt_ft, double vt_ftps, double /*speed_kts*/,
                              ConfigFlags flags = {}) {
    auto fm = makeTrimmedF16(alt_ft, vt_ftps, flags);
    if (!fm) return 0.6;
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) return 0.6;
    const double alt = -fm->state().kin.z;
    const double mach = fm->state().mach;
    const double vt = fm->state().kin.vt;
    const double mass = fm->state().fuel.mass_slugs;
    const double thrust_target = fm->state().engine.thrust;  // ft/s^2, trimmed
    EngineModel engine(&cfg.engine, &cfg.aux);
    auto thrust_at = [&](double thr) {
        EngineState st;
        st.rpm = 0.7;  // RPM_IDLE seed (in-air spawn convention)
        for (int i = 0; i < 50; ++i)
            engine.update(0.1, alt, mach, vt, mass, thr, 1.0, false, st);
        return st.thrust;
    };
    const double t0 = thrust_at(0.0);
    const double t1 = thrust_at(1.0);
    if (std::fabs(t1 - t0) < 1e-9) return 0.6;
    return std::clamp((thrust_target - t0) / (t1 - t0), 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Static sweeps (Phase 1)
// ---------------------------------------------------------------------------
int run_alpha_sweep(double alt_ft, double vt_ftps, ConfigFlags flags,
                    std::ostream& os) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) {
        std::cerr << "f16.json fixture not found at " << kFixturesDir << "\n";
        return 1;
    }
    // Use the FM's own atmosphere: init at (alt, vt) to populate qbar/qsom.
    auto fm = makeTrimmedF16(alt_ft, vt_ftps, flags);
    if (!fm) { std::cerr << "trim failed\n"; return 1; }
    const auto& s0 = fm->state();

    Aerodynamics aero(&cfg.aero, &cfg.geometry, &cfg.aux);
    os << "kind,vt_fps,mach,qbar,qsom,alpha_deg,cl,cd,lift_acc,drag_acc,"
       << "nzcgs,cnalpha,clalph0,tef,lef,gear\n";
    for (double a = -4.0; a <= 24.0; a += 0.25) {
        AeroInputs in;
        in.alpha      = angle_from_degrees(a);
        in.beta       = zero_angle();
        in.mach       = s0.mach;
        in.vt_ftps    = vt_ftps;
        in.qbar       = s0.qbar;
        in.qsom       = s0.qsom;
        in.altitude_ft = alt_ft;
        in.groundZ_ft = 0.0;
        in.z_ft       = -alt_ft;
        in.vcas_kts   = vt_ftps * KT_PER_FPS;
        in.pstick     = 0.0;
        AeroState st;
        st.tefPos = flags.tef ? 1.0 : 0.0;
        st.lefPos = flags.lef ? 1.0 : 0.0;
        st.gearPos = flags.gear_down ? 1.0 : 0.0;
        aero.update(in, st);
        const double nzcgs = -st.zsaero / GRAVITY;
        os << "alpha," << vt_ftps << ',' << s0.mach << ',' << s0.qbar << ','
           << s0.qsom << ',' << a << ',' << st.cl << ',' << st.cd << ','
           << st.lift << ',' << st.drag << ',' << nzcgs << ','
           << st.cnalpha << ',' << st.clalph0 << ','
           << st.tefPos << ',' << st.lefPos << ',' << st.gearPos << '\n';
    }
    return 0;
}

int run_speed_sweep(double alt_ft, double vt_lo, double vt_hi, int steps,
                    ConfigFlags flags, std::ostream& os) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) {
        std::cerr << "f16.json fixture not found at " << kFixturesDir << "\n";
        return 1;
    }
    Aerodynamics aero(&cfg.aero, &cfg.geometry, &cfg.aux);
    os << "kind,alt_ft,vt_fps,vcas_kts,mach,qbar,qsom,nzcgs_at_1g_alpha,"
       << "cl_at_alpha,cd_at_alpha,drag_acc,K_nz_per_deg,trim_ok\n";
    for (int i = 0; i <= steps; ++i) {
        const double vt = vt_lo + (vt_hi - vt_lo) * i / steps;
        auto fm = makeTrimmedF16(alt_ft, vt, flags);
        if (!fm) {
            // Below the 1-G stall speed (or otherwise untrimmable) the trim
            // solver legitimately fails — skip, that region is out of
            // envelope rather than a tool error.
            std::cerr << "note: trim failed at vt=" << vt
                      << " (below 1-G stall speed?) — skipped\n";
            os << "speed," << alt_ft << ',' << vt << ",0,0,0,0,0,0,0,0,0,0\n";
            continue;
        }
        const auto& s = fm->state();
        // Evaluate the aero at the trim alpha (the state after trim()).
        AeroInputs in;
        in.alpha      = s.aero.alpha;
        in.beta       = zero_angle();
        in.mach       = s.mach;
        in.vt_ftps    = vt;
        in.qbar       = s.qbar;
        in.qsom       = s.qsom;
        in.altitude_ft = alt_ft;
        in.groundZ_ft = 0.0;
        in.z_ft       = -alt_ft;
        in.vcas_kts   = s.vcas;
        in.pstick     = 0.0;
        AeroState st;
        st.tefPos = flags.tef ? 1.0 : 0.0;
        st.lefPos = flags.lef ? 1.0 : 0.0;
        st.gearPos = flags.gear_down ? 1.0 : 0.0;
        aero.update(in, st);
        const double nzcgs = -st.zsaero / GRAVITY;
        // K_nz per degree from the FCS's own static-slope convention.
        const double k_nz = st.clalph0 * s.qsom / GRAVITY;
        os << "speed," << alt_ft << ',' << vt << ',' << s.vcas << ','
           << s.mach << ',' << s.qbar << ',' << s.qsom << ',' << nzcgs << ','
           << st.cl << ',' << st.cd << ',' << st.drag << ',' << k_nz << ",1\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Time-series modes (Phase 1 bisection + steps)
// ---------------------------------------------------------------------------
int run_time_series(const std::string& mode, double alt_ft, double vt_ftps,
                    double seconds, bool alt_loop, bool speed_loop,
                    double step, ConfigFlags flags, std::ostream& os) {
    auto fm = makeTrimmedF16(alt_ft, vt_ftps, flags);
    if (!fm) {
        std::cerr << "f16 fixture missing or trim failed\n";
        return 1;
    }

    // Speed target = trim CAS (see discover_trim_throttle note above):
    // steer() regulates CAS, and TAS != CAS at altitude.
    const double target_speed_kts = fm->state().vcas;
    double frozen_throttle = 0.6;
    AirSteering steering;
    bool use_ai = (mode == "ai-hold");
    if (use_ai) {
        frozen_throttle = discover_trim_throttle(alt_ft, vt_ftps, target_speed_kts, flags);
        AirSteering::Input in = readState(*fm);
        const double alpha_est0 = in.pitch_rad - in.vs_fpm / 60.0
            / std::max(100.0, in.vcas_kts * FPS_PER_KT);
        steering.seed_from_state(in.vs_fpm, alpha_est0);
    } else if (mode == "trim-hold" || mode == "stick-step" ||
               mode == "throttle-step") {
        frozen_throttle = discover_trim_throttle(alt_ft, vt_ftps, target_speed_kts, flags);
    }

    write_header(os);
    const int n_ticks = static_cast<int>(seconds / MAJOR_DT);
    const int step_tick = static_cast<int>(2.0 / MAJOR_DT);  // steps at t=2 s

    for (int tick = 0; tick <= n_ticks; ++tick) {
        const double t = tick * MAJOR_DT;
        PilotInput pi = handsOffInput(frozen_throttle, flags);
        const f4::ai::AirSteerDebug* dbg = nullptr;

        if (mode == "stick-step" && tick >= step_tick) {
            pi.pstick = step;
        } else if (mode == "throttle-step" && tick >= step_tick) {
            pi.throttle = std::clamp(frozen_throttle + step, 0.0, 1.5);
        } else if (use_ai) {
            AirSteering::Input in = readState(*fm);
            auto out = steering.steer(0.0, alt_ft, target_speed_kts, in);
            if (!alt_loop) {
                // Altitude loop OFF: FCS holds its own 1-G trim (pstick=0).
                // This also removes speed_damp + gamma_corr + alpha_est
                // (all live inside the pitch path).
                out.pitch_cmd = 0.0;
                out.roll_cmd = 0.0;
                out.yaw_cmd = 0.0;
            }
            if (!speed_loop) {
                // Speed loop OFF: freeze the throttle at the trim value.
                out.throttle_cmd = frozen_throttle;
            }
            pi.pstick = out.pitch_cmd;
            pi.rstick = out.roll_cmd;
            pi.ypedal = out.yaw_cmd;
            pi.throttle = out.throttle_cmd;
            pi.speedBrake = out.speed_brake_cmd;
            dbg = &steering.last_debug();
        }

        fm->update(MAJOR_DT, pi, GROUND_Z_FLAT, FLAT_NORMAL);
        write_row(os, t, *fm, pi, dbg);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// thrust-map (Phase 2 engine evidence)
// ---------------------------------------------------------------------------
int run_thrust_map(double alt_ft, std::ostream& os) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) {
        std::cerr << "f16.json fixture not found\n";
        return 1;
    }
    auto fm = makeTrimmedF16(alt_ft, 422.0);
    if (!fm) { std::cerr << "trim failed\n"; return 1; }
    const double mass_slugs = fm->state().fuel.mass_slugs;
    // Speed of sound at this altitude from the FM's own atmosphere state.
    const double a_sound = fm->state().kin.vt / fm->state().mach;

    EngineModel engine(&cfg.engine, &cfg.aux);
    os << "kind,alt_ft,mach,vt_fps,throttle,thrust_accel,thrust_lbf,rpm,aburnLit\n";
    for (double mach = 0.0; mach <= 1.0001; mach += 0.05) {
        for (double thr = 0.0; thr <= 1.0001; thr += 0.05) {
            // Fresh state: the lightup zone (rpm below the lightup threshold)
            // produces zero thrust, so spool the engine with repeated updates
            // at this (throttle, mach) point before recording. Below MIL the
            // thrust is ALGEBRAIC in throttle (engine.cpp MIL branch), so
            // once out of lightup one more update is deterministic.
            EngineState st;  // engLit defaults true
            // Seed at idle RPM exactly like the FM does for in-air spawns
            // (flight_model.cpp initGearAndEngine): a fresh rpm=0 state is
            // stuck in the lightup zone (thrust=0) — that zone is for
            // ground starts, not for this in-flight map.
            st.rpm = 0.7;  // RPM_IDLE
            const double vt = mach * a_sound;
            for (int i = 0; i < 50; ++i) {  // 5 s simulated rpm settle
                engine.update(0.1, alt_ft, mach, vt, mass_slugs, thr,
                              /*ethrst=*/1.0, /*simplified=*/false, st);
            }
            engine.update(0.1, alt_ft, mach, vt, mass_slugs, thr,
                          /*ethrst=*/1.0, /*simplified=*/false, st);
            os << "thrustmap," << alt_ft << ',' << mach << ',' << vt << ','
               << thr << ',' << st.thrust << ',' << st.thrust * mass_slugs
               << ',' << st.rpm << ',' << (st.aburnLit ? 1 : 0) << '\n';
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// margin mode (Phase 2): sinusoid injection + lock-in extraction
// ---------------------------------------------------------------------------

// Channel extractors for the lock-in. Plain function pointers (no captures).
struct ChanDef {
    const char* name;
    double (*get)(const FlightModel&, const PilotInput&,
                  const f4::ai::AirSteerDebug*);
};

double ch_alt_ft(const FlightModel& fm, const PilotInput&,
                 const f4::ai::AirSteerDebug*) {
    return -fm.state().kin.z;
}
double ch_vcas(const FlightModel& fm, const PilotInput&,
               const f4::ai::AirSteerDebug*) {
    return fm.state().vcas;
}
double ch_vs_fpm(const FlightModel& fm, const PilotInput&,
                 const f4::ai::AirSteerDebug*) {
    return -fm.state().kin.zdot * 60.0;
}
double ch_gamma(const FlightModel& fm, const PilotInput&,
                const f4::ai::AirSteerDebug*) {
    return to_degrees(fm.state().kin.gmma);
}
double ch_alpha(const FlightModel& fm, const PilotInput&,
                const f4::ai::AirSteerDebug*) {
    return to_degrees(fm.state().aero.alpha);
}
double ch_theta(const FlightModel& fm, const PilotInput&,
                const f4::ai::AirSteerDebug*) {
    return to_degrees(fm.state().kin.theta);
}
double ch_q_dps(const FlightModel& fm, const PilotInput&,
                const f4::ai::AirSteerDebug*) {
    return fm.state().kin.q * 180.0 / 3.14159265358979323846;
}
double ch_nzcgs(const FlightModel& fm, const PilotInput&,
                const f4::ai::AirSteerDebug*) {
    return fm.state().loads.nzcgs;
}
double ch_ptcmd(const FlightModel& fm, const PilotInput&,
                const f4::ai::AirSteerDebug*) {
    return fm.state().fcs.ptcmd;
}
double ch_pstick(const FlightModel&, const PilotInput& pi,
                 const f4::ai::AirSteerDebug*) {
    return pi.pstick;
}
double ch_throttle(const FlightModel&, const PilotInput& pi,
                   const f4::ai::AirSteerDebug*) {
    return pi.throttle;
}
double ch_spdbrake(const FlightModel&, const PilotInput& pi,
                   const f4::ai::AirSteerDebug*) {
    return pi.speedBrake;
}
double ch_thrust(const FlightModel& fm, const PilotInput&,
                 const f4::ai::AirSteerDebug*) {
    return fm.state().engine.thrust;
}
double ch_rpm(const FlightModel& fm, const PilotInput&,
              const f4::ai::AirSteerDebug*) {
    return fm.state().engine.rpm;
}

const ChanDef kChannels[] = {
    {"alt_ft", ch_alt_ft},   {"vcas_kts", ch_vcas}, {"vs_fpm", ch_vs_fpm},
    {"gamma_deg", ch_gamma}, {"alpha_deg", ch_alpha}, {"theta_deg", ch_theta},
    {"q_dps", ch_q_dps},     {"nzcgs", ch_nzcgs},   {"ptcmd", ch_ptcmd},
    {"pstick", ch_pstick},   {"throttle", ch_throttle},
    {"speed_brake", ch_spdbrake}, {"thrust_accel", ch_thrust},
    {"rpm", ch_rpm},
};
constexpr int kNChannels = sizeof(kChannels) / sizeof(kChannels[0]);

enum class Inj { Pstick, AltTarget, SpdTarget };

struct LockinRow {
    int run_id;
    const char* case_name;
    const char* inj_name;
    int gear;
    double alt_ft, vt_fps;
    double freq_req, freq_used, amp;
    const char* channel;
    double Xc, Xs;                 // complex amplitude of the channel
    double R_re, R_im;             // channel / reference
    double R_db, R_phase_deg;
};

// One sinusoid run: fresh FM (+ AirSteering when the AI loops are on),
// settle at the constant base input, then measure over an integer number
// of lock-in periods. Lock-in over exactly kLockinPeriodsLow periods with an
// integer ticks-per-period count — zero spectral leakage.
//
// Pstick-injection cases (g/g0) additionally PRE-SETTLE under the AI
// altitude loop for 40 s: the raw trim leaves a residual gamma (PLANT
// IDENTIFICATION F6: theta is not re-set to alpha after trim), which
// otherwise drives a slow climb/acceleration drift whose spectral tail
// contaminates the low-frequency lock-in windows. The AI hold nulls VS and
// gamma and finds the settled trim throttle; the pstick reference then
// injects around that nulled state with the throttle frozen at the AI's
// settled value.
constexpr int kLockinPeriods = 2;
constexpr int kLockinPeriodsLow = 4;   // used at f <= 0.1 rad/s
constexpr double kPresettleS = 40.0;   // AI alt-hold nulling phase (g/g0)

bool run_lockin(double alt_ft, double vt_ftps, ConfigFlags flags,
                int alt_loop, int spd_loop, Inj inj, double amp,
                double freq_req, bool zero_qdamp, int run_id,
                const char* case_name, const char* inj_name,
                std::vector<LockinRow>& rows, int64_t& sim_ticks_budget) {
    auto fm = makeTrimmedF16(alt_ft, vt_ftps, flags);
    if (!fm) {
        std::cerr << "margin: trim failed (vt=" << vt_ftps << ")\n";
        return false;
    }
    if (zero_qdamp) fm->state().fcs.pitchRateDampGain = 0.0;

    AirSteering steering;
    const double target_speed_kts = fm->state().vcas;  // trim CAS, not TAS
    double frozen_throttle = 0.6;
    if (alt_loop || spd_loop) {
        frozen_throttle = discover_trim_throttle(alt_ft, vt_ftps,
                                                 target_speed_kts, flags);
        AirSteering::Input in0 = readState(*fm);
        const double alpha_est0 = in0.pitch_rad - in0.vs_fpm / 60.0
            / std::max(100.0, in0.vcas_kts * FPS_PER_KT);
        steering.seed_from_state(in0.vs_fpm, alpha_est0);
    } else {
        frozen_throttle = discover_trim_throttle(alt_ft, vt_ftps,
                                                 target_speed_kts, flags);
    }

    // --- Timing: settle + measure, integer ticks per period ---
    const double T = TWO_PI / freq_req;
    const int ticks_per_period = std::max(1,
        static_cast<int>(std::lround(T / MAJOR_DT)));
    const double freq_used = TWO_PI / (ticks_per_period * MAJOR_DT);
    const double T_used = ticks_per_period * MAJOR_DT;
    const int lockin_periods = (freq_req <= 0.1) ? kLockinPeriodsLow
                                                 : kLockinPeriods;
    double settle_s = std::min(30.0 + T_used, 400.0);
    double measure_total_s = std::max((lockin_periods + 1) * T_used, 60.0);
    if (settle_s + measure_total_s > 1300.0) {
        measure_total_s = 1300.0 - settle_s;
    }
    const int presettle_ticks = (!alt_loop && !spd_loop)
        ? static_cast<int>(kPresettleS / MAJOR_DT) : 0;
    const int settle_ticks = static_cast<int>(settle_s / MAJOR_DT);
    const int measure_ticks = static_cast<int>(measure_total_s / MAJOR_DT);
    // Fit the lock-in window into the (possibly capped) measurement buffer,
    // always keeping at least one full transient period before it.
    int lockin_periods_eff = lockin_periods;
    while (lockin_periods_eff > 1 &&
           lockin_periods_eff * ticks_per_period >
               measure_ticks + 1 - ticks_per_period) {
        --lockin_periods_eff;
    }
    const int lockin_ticks = lockin_periods_eff * ticks_per_period;
    sim_ticks_budget += presettle_ticks + settle_ticks + measure_ticks;

    // Recording buffers (measure phase only).
    std::vector<std::array<double, kNChannels>> samples(
        static_cast<size_t>(measure_ticks + 1));

    const double base_alt = alt_ft;
    const double base_spd = target_speed_kts;

    // --- Phase 0 (g/g0 only): AI altitude-hold pre-settle. Nulls the
    // residual-gamma climb transient (F6) and finds the settled trim
    // throttle. The steering object runs the FULL altitude cascade here
    // (it is discarded afterwards; its integrator states do not carry
    // into the measurement). ---
    for (int k = 0; k < presettle_ticks; ++k) {
        AirSteering::Input in = readState(*fm);
        auto out = steering.steer(0.0, base_alt, base_spd, in);
        PilotInput pi = handsOffInput(out.throttle_cmd, flags);
        pi.pstick = out.pitch_cmd;
        pi.rstick = out.roll_cmd;
        pi.ypedal = out.yaw_cmd;
        pi.validate();
        fm->update(MAJOR_DT, pi, GROUND_Z_FLAT, FLAT_NORMAL);
    }
    if (presettle_ticks > 0) {
        // Freeze the throttle at the AI's settled (nulled-state) value.
        AirSteering::Input in = readState(*fm);
        auto out = steering.steer(0.0, base_alt, base_spd, in);
        frozen_throttle = out.throttle_cmd;
    }

    for (int k = 0; k <= settle_ticks + measure_ticks; ++k) {
        const bool in_measure = (k >= settle_ticks);
        const int m = k - settle_ticks;          // measure index (0-based)
        const double t_meas = m * MAJOR_DT;      // reference clock (sin starts at 0)
        const double ref = in_measure
            ? amp * std::sin(freq_used * t_meas) : 0.0;

        PilotInput pi = handsOffInput(frozen_throttle, flags);
        const f4::ai::AirSteerDebug* dbg = nullptr;

        if (alt_loop || spd_loop) {
            double alt_target = base_alt;
            double spd_target = base_spd;
            switch (inj) {
                case Inj::AltTarget: alt_target = base_alt + ref; break;
                case Inj::SpdTarget: spd_target = base_spd + ref; break;
                case Inj::Pstick:    break;
            }
            AirSteering::Input in = readState(*fm);
            auto out = steering.steer(0.0, alt_target, spd_target, in);
            if (!alt_loop) {
                out.pitch_cmd = 0.0;
                out.roll_cmd = 0.0;
                out.yaw_cmd = 0.0;
            }
            if (!spd_loop) out.throttle_cmd = frozen_throttle;
            pi.pstick = out.pitch_cmd;
            pi.rstick = out.roll_cmd;
            pi.ypedal = out.yaw_cmd;
            pi.throttle = out.throttle_cmd;
            pi.speedBrake = out.speed_brake_cmd;
            dbg = &steering.last_debug();
        } else {
            if (inj == Inj::Pstick) pi.pstick = ref;
        }

        fm->update(MAJOR_DT, pi, GROUND_Z_FLAT, FLAT_NORMAL);

        if (in_measure) {
            for (int c = 0; c < kNChannels; ++c) {
                samples[static_cast<size_t>(m)][static_cast<size_t>(c)] =
                    kChannels[c].get(*fm, pi, dbg);
            }
        }
    }

    // --- Lock-in over the last kLockinPeriods periods ---
    // Recorded measure indices are m = 0..measure_ticks (m=0 is the sin
    // start, ref=0 there), so the buffer holds measure_ticks+1 samples.
    const int i0 = measure_ticks + 1 - lockin_ticks;
    const double N = static_cast<double>(lockin_ticks);
    // Reference: ideal injected sin at (freq_used, amp) — exact phase.
    // U = -j*amp  (Uc = 0, Us = amp).
    const double Uc = 0.0;
    const double Us = amp;

    LockinRow base{};
    base.run_id = run_id;
    base.case_name = case_name;
    base.inj_name = inj_name;
    base.gear = flags.gear_down ? 1 : 0;
    base.alt_ft = alt_ft;
    base.vt_fps = vt_ftps;
    base.freq_req = freq_req;
    base.freq_used = freq_used;
    base.amp = amp;

    // Emit a U row (reference) for traceability.
    {
        LockinRow r = base;
        r.channel = "REF";
        r.Xc = Uc; r.Xs = Us;
        r.R_re = 1.0; r.R_im = 0.0; r.R_db = 0.0; r.R_phase_deg = 0.0;
        rows.push_back(r);
    }

    for (int c = 0; c < kNChannels; ++c) {
        double sc = 0.0, ss = 0.0;  // Σ x·cos, Σ x·sin
        for (int i = 0; i < lockin_ticks; ++i) {
            const double t_meas = (i0 + i) * MAJOR_DT;
            const double x = samples[static_cast<size_t>(i0 + i)]
                                    [static_cast<size_t>(c)];
            sc += x * std::cos(freq_used * t_meas);
            ss += x * std::sin(freq_used * t_meas);
        }
        const double Xc = 2.0 * sc / N;
        const double Xs = 2.0 * ss / N;
        LockinRow r = base;
        r.channel = kChannels[c].name;
        r.Xc = Xc; r.Xs = Xs;
        // R = X / U with X = (Xc - j*Xs), U = (Uc - j*Us):
        //   R = (Xc - j*Xs) * conj(U) / |U|^2
        //     = [(Xc*Uc + Xs*Us) + j*(Xc*Us - Xs*Uc)] / (Uc^2 + Us^2)
        // (Check: U = -j*amp → R_re = Xs/amp, R_im = Xc/amp — a pure
        //  phase lag of 90° shows up as R = -j, as expected.)
        const double denom = Uc * Uc + Us * Us;
        r.R_re = ( Xc * Uc + Xs * Us) / denom;
        r.R_im = ( Xc * Us - Xs * Uc) / denom;
        r.R_db = 20.0 * std::log10(std::max(1e-15,
            std::sqrt(r.R_re * r.R_re + r.R_im * r.R_im)));
        r.R_phase_deg = std::atan2(r.R_im, r.R_re) * 180.0 / 3.14159265358979323846;
        rows.push_back(r);
    }
    return true;
}

void write_margin_header(std::ostream& os) {
    os << "run_id,case,inj,gear,alt_ft,vt_fps,freq_req,freq_used,amp,"
       << "channel,Xc,Xs,R_re,R_im,R_db,R_phase_deg\n";
}

void write_margin_rows(std::ostream& os, const std::vector<LockinRow>& rows) {
    for (const auto& r : rows) {
        os << r.run_id << ',' << r.case_name << ',' << r.inj_name << ','
           << r.gear << ',' << r.alt_ft << ',' << r.vt_fps << ','
           << r.freq_req << ',' << r.freq_used << ',' << r.amp << ','
           << r.channel << ',' << r.Xc << ',' << r.Xs << ','
           << r.R_re << ',' << r.R_im << ',' << r.R_db << ',' << r.R_phase_deg
           << '\n';
    }
}

// Frequency grids (rad/s). g-cases: the L0 loop lives at 0.1–3 rad/s
// (design ω=0.8); low frequencies degenerate (R→1 makes R/(1−R)
// ill-conditioned and the loop gain → ∞ through the integrator anyway).
// C/E cases: the L3/L4 mode band (observed 16.7 s ≈ 0.376 rad/s) plus the
// integrator slope at 0.02–0.05.
constexpr double kFreqsG[] = {0.1, 0.2, 0.4, 0.8, 1.6, 3.2};
constexpr double kFreqsCE[] = {0.02, 0.05, 0.1, 0.2, 0.4, 0.8, 1.6};

int run_margin(double alt_ft, double vt_ftps, const std::string& case_name,
               bool gear_down, double amp_override, std::ostream& os) {
    ConfigFlags flags;
    flags.gear_down = gear_down;
    flags.tef = gear_down;   // approach config: gear + flaps + TEF
    flags.lef = gear_down;

    std::vector<LockinRow> rows;
    int run_id = 0;
    int64_t budget = 0;

    if (case_name == "g" || case_name == "g0") {
        const double amp = (amp_override > 0.0) ? amp_override : 0.03;
        const bool zero_qdamp = (case_name == "g0");
        for (double f : kFreqsG) {
            std::cerr << "margin " << case_name << ": f=" << f << "\n";
            if (!run_lockin(alt_ft, vt_ftps, flags,
                            /*alt_loop=*/0, /*spd_loop=*/0, Inj::Pstick, amp,
                            f, zero_qdamp, ++run_id, case_name.c_str(), "pstick",
                            rows, budget)) return 1;
        }
    } else if (case_name == "C") {
        const double amp = (amp_override > 0.0) ? amp_override : 30.0;  // ft
        for (double f : kFreqsCE) {
            std::cerr << "margin C: f=" << f << "\n";
            if (!run_lockin(alt_ft, vt_ftps, flags,
                            /*alt_loop=*/1, /*spd_loop=*/0, Inj::AltTarget, amp,
                            f, false, ++run_id, "C", "alt_target",
                            rows, budget)) return 1;
        }
    } else if (case_name == "E") {
        const double amp_alt = (amp_override > 0.0) ? amp_override : 30.0;  // ft
        const double amp_spd = 4.0;                                          // kt
        for (double f : kFreqsCE) {
            std::cerr << "margin E(alt): f=" << f << "\n";
            if (!run_lockin(alt_ft, vt_ftps, flags,
                            /*alt_loop=*/1, /*spd_loop=*/1, Inj::AltTarget,
                            amp_alt, f, false, ++run_id, "E", "alt_target",
                            rows, budget)) return 1;
        }
        for (double f : kFreqsCE) {
            std::cerr << "margin E(spd): f=" << f << "\n";
            if (!run_lockin(alt_ft, vt_ftps, flags,
                            /*alt_loop=*/1, /*spd_loop=*/1, Inj::SpdTarget,
                            amp_spd, f, false, ++run_id, "E", "spd_target",
                            rows, budget)) return 1;
        }
    } else {
        std::cerr << "margin: unknown case '" << case_name
                  << "' (g|g0|C|E)\n";
        return 1;
    }

    write_margin_header(os);
    write_margin_rows(os, rows);
    std::cerr << "margin: " << rows.size() << " rows, "
              << budget << " sim ticks\n";
    return 0;
}

// ---------------------------------------------------------------------------
// rootlocus mode (Phase 3 remainder)
// ---------------------------------------------------------------------------

int run_rootlocus(double alt_ft, double vt_ftps, bool gear_down,
                  double seconds, std::ostream& os) {
    ConfigFlags flags;
    flags.gear_down = gear_down;
    flags.tef = gear_down;
    flags.lef = gear_down;

    // One-at-a-time sweep of the AirSteering cascade gains (public members
    // — no library changes). A mode whose PERIOD tracks a parameter is
    // owned by that parameter's loop (plan §7).
    struct RlParam {
        const char* name;
        double AirSteering::* mem;
    };
    static const RlParam kParams[] = {
        {"attitude_gain",  &AirSteering::attitude_gain},
        {"vs_gain",        &AirSteering::vs_gain},
        {"speed_damp",     &AirSteering::speed_damp_rad_per_kt},
        {"path_gain",      &AirSteering::path_gain},
        {"gamma_corr_lim", &AirSteering::gamma_corr_limit},
        {"alt_int_gain",   &AirSteering::alt_integral_gain},
        {"pitch_rate_damp",&AirSteering::pitch_rate_damp},
        {"max_vs_fpm",     &AirSteering::max_vs_fpm},
    };
    static constexpr double kFactors[] = {0.5, 2.0};

    os << "param,factor,t,vt_fps,vcas_kts,alt_msl_ft,vs_fpm,gamma_deg,"
       << "alpha_deg,theta_deg,q_dps,nzcgs,vs_target_fpm,vs_corr_fpm,"
       << "alt_err_ft,gamma_corr,speed_err_kt,throttle,pstick,energy_err_ft\n";

    const double target_speed_kts = [&]() {
        auto fm0 = makeTrimmedF16(alt_ft, vt_ftps, flags);
        return fm0 ? fm0->state().vcas : vt_ftps * KT_PER_FPS;
    }();  // trim CAS (steer() regulates CAS; TAS != CAS at altitude)
    const int n_ticks = static_cast<int>(seconds / MAJOR_DT);
    const int decim = 6;  // 10 Hz output
    int run_n = 0;

    auto one_run = [&](const char* pname, double factor,
                       const RlParam* p) {
        ++run_n;
        auto fm = makeTrimmedF16(alt_ft, vt_ftps, flags);
        if (!fm) {
            std::cerr << "rootlocus: trim failed\n";
            return;
        }
        AirSteering steering;
        AirSteering::Input in0 = readState(*fm);
        const double alpha_est0 = in0.pitch_rad - in0.vs_fpm / 60.0
            / std::max(100.0, in0.vcas_kts * FPS_PER_KT);
        steering.seed_from_state(in0.vs_fpm, alpha_est0);
        if (p != nullptr) steering.*p->mem = steering.*p->mem * factor;

        for (int tick = 0; tick <= n_ticks; ++tick) {
            const double t = tick * MAJOR_DT;
            AirSteering::Input in = readState(*fm);
            auto out = steering.steer(0.0, alt_ft, target_speed_kts, in);
            PilotInput pi = handsOffInput(out.throttle_cmd, flags);
            pi.pstick = out.pitch_cmd;
            pi.rstick = out.roll_cmd;
            pi.ypedal = out.yaw_cmd;
            pi.throttle = out.throttle_cmd;
            pi.speedBrake = out.speed_brake_cmd;
            pi.validate();
            fm->update(MAJOR_DT, pi, GROUND_Z_FLAT, FLAT_NORMAL);
            if (tick % decim != 0) continue;
            const auto& s = fm->state();
            const auto& dbg = steering.last_debug();
            os << pname << ',' << factor << ',' << t << ','
               << s.kin.vt << ',' << s.vcas << ',' << -s.kin.z << ','
               << -s.kin.zdot * 60.0 << ','
               << to_degrees(s.kin.gmma) << ','
               << to_degrees(s.aero.alpha) << ','
               << to_degrees(s.kin.theta) << ','
               << s.kin.q * 180.0 / 3.14159265358979323846 << ','
               << s.loads.nzcgs << ','
               << dbg.vs_target_fpm << ',' << dbg.vs_corr_fpm << ','
               << dbg.alt_err_ft << ',' << dbg.gamma_corr_rad << ','
               << dbg.speed_err_kt << ',' << pi.throttle << ',' << pi.pstick
               << ',' << dbg.energy_err_ft << '\n';
        }
        std::cerr << "rootlocus run " << run_n << ": " << pname
                  << " x" << factor << " done\n";
    };

    one_run("base", 1.0, nullptr);
    for (const auto& p : kParams) {
        for (double f : kFactors) {
            one_run(p.name, f, &p);
        }
    }
    return 0;
}

void usage() {
    std::cerr <<
        "fm_sysid — PHUG-PLAN Phase 1-3 identification + margin harness\n"
        "usage:\n"
        "  fm_sysid alpha-sweep   <alt_ft> <vt_fps> [gear 0|1] [out.csv]\n"
        "  fm_sysid speed-sweep   <alt_ft> <vt_lo> <vt_hi> <steps> [gear 0|1]"
        " [out.csv]\n"
        "  fm_sysid trim-hold     <alt_ft> <vt_fps> <seconds> [out.csv]\n"
        "  fm_sysid stick-step    <alt_ft> <vt_fps> <step> <seconds> [out.csv]\n"
        "  fm_sysid throttle-step <alt_ft> <vt_fps> <step> <seconds> [out.csv]\n"
        "  fm_sysid ai-hold       <alt_ft> <vt_fps> <seconds> <alt01> <spd01>"
        " [heading_deg] [out.csv]\n"
        "  fm_sysid thrust-map    <alt_ft> [out.csv]\n"
        "  fm_sysid margin        <alt_ft> <vt_fps> <case g|g0|C|E> <gear 0|1>"
        " [amp] [out.csv]\n"
        "  fm_sysid rootlocus     <alt_ft> <vt_fps> <gear 0|1> <sec_per_run>"
        " [out.csv]\n";
}

// Trailing-arg helpers: the last arg is out.csv when it ends in ".csv";
// an optional preceding "0"/"1" is a gear flag.
bool is_csv_arg(const char* a) {
    const std::string s(a);
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".csv") == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    const std::string mode = argv[1];
    std::ofstream file_out;
    auto use_file = [&](const std::string& last_arg) -> std::ostream* {
        static const std::string csv_ext = ".csv";
        if (last_arg.size() > 4 &&
            last_arg.compare(last_arg.size() - 4, 4, csv_ext) == 0) {
            file_out.open(last_arg, std::ios::out | std::ios::trunc);
            if (!file_out) {
                std::cerr << "cannot open " << last_arg << "\n";
                return nullptr;
            }
            return &file_out;
        }
        return &std::cout;
    };

    // --- Phase-1 modes (unchanged interfaces) ---
    if (mode == "alpha-sweep" && argc >= 4) {
        // alpha-sweep <alt> <vt> [gear] [out.csv]
        bool gear = false;
        if (argc >= 5 && !is_csv_arg(argv[4])) gear = (argv[4][0] == '1');
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        ConfigFlags flags; flags.gear_down = gear;
        flags.tef = gear; flags.lef = gear;
        return run_alpha_sweep(std::stod(argv[2]), std::stod(argv[3]),
                               flags, *os);
    }
    if (mode == "speed-sweep" && argc >= 6) {
        // speed-sweep <alt> <lo> <hi> <steps> [gear] [out.csv]
        bool gear = false;
        std::ostream* os = &std::cout;
        if (is_csv_arg(argv[argc - 1])) {
            os = use_file(argv[argc - 1]);
            if (argc >= 8) gear = (argv[6][0] == '1');
        } else if (argc >= 7) {
            gear = (argv[6][0] == '1');
        }
        if (!os) return 1;
        ConfigFlags flags; flags.gear_down = gear;
        flags.tef = gear; flags.lef = gear;
        return run_speed_sweep(std::stod(argv[2]), std::stod(argv[3]),
                               std::stod(argv[4]), std::stoi(argv[5]),
                               flags, *os);
    }
    if (mode == "trim-hold" && argc >= 5) {
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        return run_time_series(mode, std::stod(argv[2]), std::stod(argv[3]),
                               std::stod(argv[4]), false, false, 0.0, {}, *os);
    }
    if (mode == "stick-step" && argc >= 6) {
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        return run_time_series(mode, std::stod(argv[2]), std::stod(argv[3]),
                               std::stod(argv[5]), false, false,
                               std::stod(argv[4]), {}, *os);
    }
    if (mode == "throttle-step" && argc >= 6) {
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        return run_time_series(mode, std::stod(argv[2]), std::stod(argv[3]),
                               std::stod(argv[5]), false, false,
                               std::stod(argv[4]), {}, *os);
    }
    if (mode == "ai-hold" && argc >= 7) {
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        return run_time_series("ai-hold", std::stod(argv[2]),
                               std::stod(argv[3]), std::stod(argv[4]),
                               argv[5][0] == '1', argv[6][0] == '1',
                               0.0, {}, *os);
    }

    // --- Phase-2/3 modes ---
    if (mode == "thrust-map" && argc >= 3) {
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        return run_thrust_map(std::stod(argv[2]), *os);
    }
    if (mode == "margin" && argc >= 6) {
        // margin <alt> <vt> <case> <gear> [amp] [out.csv]
        double amp = 0.0;
        std::ostream* os = &std::cout;
        if (is_csv_arg(argv[argc - 1])) {
            os = use_file(argv[argc - 1]);
            if (argc >= 8) amp = std::stod(argv[6]);
        } else if (argc >= 7) {
            amp = std::stod(argv[6]);
        }
        if (!os) return 1;
        return run_margin(std::stod(argv[2]), std::stod(argv[3]), argv[4],
                          argv[5][0] == '1', amp, *os);
    }
    if (mode == "rootlocus" && argc >= 6) {
        auto* os = use_file(argv[argc - 1]);
        if (!os) return 1;
        return run_rootlocus(std::stod(argv[2]), std::stod(argv[3]),
                             argv[4][0] == '1', std::stod(argv[5]), *os);
    }

    usage();
    return 1;
}

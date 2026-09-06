// f4-flight-model/tests/diag_trajectory.cpp
//
// Standalone diagnostic — dumps CSV trajectory data for the heading-capture
// and altitude-in-turn scenarios. Not a GoogleTest; built as a separate
// executable. Used by the experiment harness to measure before/after
// metrics when applying control-law changes.

#include "f4/flight/flight_model.hpp"
#include "f4/flight/aircraft_state.hpp"
#include "f4/flight/constants.hpp"
#include "f4/data/config_loader.hpp"
#include "f4/ai/air_steering.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace f4::flight;
using f4::ai::AirSteering;
using f4::math::Vec3d;
namespace geo = f4::geo;

namespace {
constexpr const char* kFixturesDir = F4_GENERATED_FIXTURES_DIR;
constexpr double kPI  = 3.14159265358979323846;
constexpr double kD2R = kPI / 180.0;
constexpr double kRTD = 180.0 / kPI;
constexpr double MAJOR_DT = 1.0 / 60.0;
constexpr double GROUND_Z_FLAT = 0.0;
const Vec3d FLAT_NORMAL{0.0, 0.0, -1.0};

bool loadF16Config(f4::data::AircraftConfig& cfg) {
    const std::string path = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(path)) return false;
    auto r = f4::data::loadConfig(path);
    if (!r.ok) return false;
    cfg = r.config;
    return true;
}

std::unique_ptr<FlightModel> makeTrimmedF16(double alt_ft, double vt_ftps,
                                             double heading_rad = 0.0) {
    f4::data::AircraftConfig cfg;
    if (!loadF16Config(cfg)) return nullptr;
    auto fm = std::make_unique<FlightModel>();
    fm->init(cfg, alt_ft, vt_ftps, heading_rad, /*inAir=*/true);
    if (!fm->trim()) return nullptr;
    fm->setGround(GROUND_Z_FLAT, FLAT_NORMAL);
    return fm;
}

AirSteering::Input readState(const FlightModel& fm) {
    AirSteering::Input in;
    const auto& s = fm.state();
    in.position = geo::WorldPosition(s.kin.y, s.kin.x, -s.kin.z);
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

PilotInput mapAI(const f4::ai::AIControlOutput& ai) {
    PilotInput pi;
    pi.pstick     = ai.pitch_cmd;
    pi.rstick     = ai.roll_cmd;
    pi.ypedal     = ai.yaw_cmd;
    pi.throttle   = ai.throttle_cmd;
    pi.speedBrake = ai.speed_brake_cmd;
    pi.gearHandle = ai.gear_handle_down ? 1.0 : -1.0;
    pi.wheelBrakes = ai.wheel_brakes;
    pi.parkingBrake = ai.parking_brake;
    pi.noseSteerOn = true;
    pi.maxRollDeg = ai.max_roll_deg;           // EXPERIMENT S
    pi.maxRollDeltaDeg = ai.max_roll_delta_deg; // EXPERIMENT S
    pi.validate();
    return pi;
}

struct TrajectoryPoint {
    double t;
    double heading_err_deg;
    double phi_deg;
    double alt_ft;
    double vs_fpm;
    double vcas_kts;
    double p_radps;
    double q_radps;
    double rstick;
    double pstick;
    double beta_deg;
    double throttle;
    double speed_brake;
};

void dump_csv(const std::string& path, const std::vector<TrajectoryPoint>& pts) {
    std::ofstream f(path);
    f << "t,hdg_err_deg,phi_deg,alt_ft,vs_fpm,vcas_kts,p_radps,q_radps,rstick,pstick,beta_deg,throttle,speed_brake\n";
    for (const auto& p : pts) {
        f << p.t << "," << p.heading_err_deg << "," << p.phi_deg << ","
          << p.alt_ft << "," << p.vs_fpm << "," << p.vcas_kts << ","
          << p.p_radps << "," << p.q_radps << "," << p.rstick << ","
          << p.pstick << "," << p.beta_deg << "," << p.throttle << ","
          << p.speed_brake << "\n";
    }
}

struct Summary {
    double final_hdg_err_deg;
    int     sign_changes_tail;
    double  max_phi_deg;
    double  alt_drift_ft;
    double  alt_range_ft;
    double  max_vs_fpm;
    int     hdg_sign_changes_tail;
    double  final_vcas;
    double  max_beta_deg;
    double  throttle_range;
    int     speed_brake_cycles;
};

Summary summarize(const std::vector<TrajectoryPoint>& pts) {
    Summary s{};
    s.final_hdg_err_deg = std::fabs(pts.back().heading_err_deg);
    s.max_phi_deg = 0.0;
    s.max_vs_fpm = 0.0;
    s.max_beta_deg = 0.0;
    s.throttle_range = 0.0;
    s.speed_brake_cycles = 0;
    double min_alt = 1e9, max_alt = -1e9;
    double min_thr = 1e9, max_thr = -1e9;
    for (const auto& p : pts) {
        s.max_phi_deg = std::max(s.max_phi_deg, std::fabs(p.phi_deg));
        s.max_vs_fpm = std::max(s.max_vs_fpm, std::fabs(p.vs_fpm));
        s.max_beta_deg = std::max(s.max_beta_deg, std::fabs(p.beta_deg));
        min_alt = std::min(min_alt, p.alt_ft);
        max_alt = std::max(max_alt, p.alt_ft);
        min_thr = std::min(min_thr, p.throttle);
        max_thr = std::max(max_thr, p.throttle);
    }
    s.alt_drift_ft = std::fabs(pts.back().alt_ft - pts.front().alt_ft);
    s.alt_range_ft = max_alt - min_alt;
    s.throttle_range = max_thr - min_thr;
    s.final_vcas = pts.back().vcas_kts;

    // Count speed-brake direction reversals (each reversal = a deploy/retract
    // cycle). Ignore tiny chatter < 0.05.
    int sb_changes = 0;
    int prev_sb_sign = 0;
    for (const auto& p : pts) {
        double delta = p.speed_brake - (-1.0);  // -1 = retracted
        int sgn = (delta > 0.05) ? 1 : (delta < -0.05 ? -1 : 0);
        // Actually count transitions between deployed (>0.1) and retracted (<-0.9)
        int state = (p.speed_brake > -0.5) ? 1 : 0;
        if (prev_sb_sign != 0 && state != prev_sb_sign) ++sb_changes;
        if (state != 0 || prev_sb_sign != 0) prev_sb_sign = state;
    }
    s.speed_brake_cycles = sb_changes;

    // Last 5 seconds of heading error for sign changes (limit-cycle check)
    int tail_n = std::min<int>(300, pts.size());
    auto tail_begin = pts.end() - tail_n;
    int sign_changes = 0;
    int prev_sign = 0;
    for (auto it = tail_begin; it != pts.end(); ++it) {
        double v = it->heading_err_deg;
        int sgn = (v > 0.05) ? 1 : (v < -0.05 ? -1 : 0);
        if (sgn != 0 && prev_sign != 0 && sgn != prev_sign) ++sign_changes;
        if (sgn != 0) prev_sign = sgn;
    }
    s.hdg_sign_changes_tail = sign_changes;

    // Also count sign changes of phi (bank reversal) in tail
    int phi_changes = 0;
    int prev_phi_sign = 0;
    for (auto it = tail_begin; it != pts.end(); ++it) {
        double v = it->phi_deg;
        int sgn = (v > 0.5) ? 1 : (v < -0.5 ? -1 : 0);
        if (sgn != 0 && prev_phi_sign != 0 && sgn != prev_phi_sign) ++phi_changes;
        if (sgn != 0) prev_phi_sign = sgn;
    }
    s.sign_changes_tail = phi_changes;

    return s;
}

void print_summary(const std::string& name, const Summary& s) {
    std::cout << "[" << name << "] "
              << "final_hdg_err=" << s.final_hdg_err_deg << "deg "
              << "max_phi=" << s.max_phi_deg << "deg "
              << "alt_drift=" << s.alt_drift_ft << "ft "
              << "alt_range=" << s.alt_range_ft << "ft "
              << "max_vs=" << s.max_vs_fpm << "fpm "
              << "max_beta=" << s.max_beta_deg << "deg "
              << "thr_range=" << s.throttle_range << " "
              << "sb_cycles=" << s.speed_brake_cycles << " "
              << "hdg_signchg=" << s.hdg_sign_changes_tail << " "
              << "phi_signchg=" << s.sign_changes_tail << " "
              << "final_vcas=" << s.final_vcas << "kts\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = (argc > 1) ? argv[1] : "/tmp/f4_diag";
    std::filesystem::create_directories(out_dir);

    // -------- Scenario 1: 90-deg heading capture, hold altitude --------
    {
        auto fm = makeTrimmedF16(10000.0, 500.0);
        if (!fm) { std::cerr << "trim failed\n"; return 1; }
        AirSteering as;
        const double target_heading = 90.0 * kD2R;
        const double target_alt = 10000.0;
        const double target_speed_kts = fm->state().vcas;

        std::vector<TrajectoryPoint> pts;
        pts.reserve(1500);
        for (int tick = 0; tick < 1500; ++tick) {  // 25 s
            AirSteering::Input in = readState(*fm);
            const auto ai = as.steer(target_heading, target_alt,
                                     target_speed_kts, in);
            fm->update(MAJOR_DT, mapAI(ai), GROUND_Z_FLAT, FLAT_NORMAL);

            const double hdg = to_radians(fm->state().kin.psi);
            double hdg_err = target_heading - hdg;
            while (hdg_err > kPI)  hdg_err -= 2*kPI;
            while (hdg_err < -kPI) hdg_err += 2*kPI;

            TrajectoryPoint p{};
            p.t = tick * MAJOR_DT;
            p.heading_err_deg = hdg_err * kRTD;
            p.phi_deg = to_degrees(fm->state().kin.phi);
            p.alt_ft = -fm->state().kin.z;
            p.vs_fpm = -fm->state().kin.zdot * 60.0;
            p.vcas_kts = fm->state().vcas;
            p.p_radps = fm->state().kin.p;
            p.q_radps = fm->state().kin.q;
            p.rstick = ai.roll_cmd;
            p.pstick = ai.pitch_cmd;
            p.beta_deg = to_degrees(fm->state().aero.beta);
            p.throttle = ai.throttle_cmd;
            p.speed_brake = ai.speed_brake_cmd;
            pts.push_back(p);
        }
        dump_csv(out_dir + "/heading_capture.csv", pts);
        print_summary("HEADING_90", summarize(pts));
    }

    // -------- Scenario 2: 30-deg heading step (smaller turn) --------
    {
        auto fm = makeTrimmedF16(10000.0, 500.0);
        if (!fm) { std::cerr << "trim failed\n"; return 1; }
        AirSteering as;
        const double target_heading = 30.0 * kD2R;
        const double target_alt = 10000.0;
        const double target_speed_kts = fm->state().vcas;

        std::vector<TrajectoryPoint> pts;
        pts.reserve(1200);
        for (int tick = 0; tick < 1200; ++tick) {  // 20 s
            AirSteering::Input in = readState(*fm);
            const auto ai = as.steer(target_heading, target_alt,
                                     target_speed_kts, in);
            fm->update(MAJOR_DT, mapAI(ai), GROUND_Z_FLAT, FLAT_NORMAL);

            const double hdg = to_radians(fm->state().kin.psi);
            double hdg_err = target_heading - hdg;
            while (hdg_err > kPI)  hdg_err -= 2*kPI;
            while (hdg_err < -kPI) hdg_err += 2*kPI;

            TrajectoryPoint p{};
            p.t = tick * MAJOR_DT;
            p.heading_err_deg = hdg_err * kRTD;
            p.phi_deg = to_degrees(fm->state().kin.phi);
            p.alt_ft = -fm->state().kin.z;
            p.vs_fpm = -fm->state().kin.zdot * 60.0;
            p.vcas_kts = fm->state().vcas;
            p.p_radps = fm->state().kin.p;
            p.q_radps = fm->state().kin.q;
            p.rstick = ai.roll_cmd;
            p.pstick = ai.pitch_cmd;
            p.beta_deg = to_degrees(fm->state().aero.beta);
            p.throttle = ai.throttle_cmd;
            p.speed_brake = ai.speed_brake_cmd;
            pts.push_back(p);
        }
        dump_csv(out_dir + "/heading_30.csv", pts);
        print_summary("HEADING_30", summarize(pts));
    }

    // -------- Scenario 3: altitude capture (level off) --------
    {
        auto fm = makeTrimmedF16(10000.0, 500.0);
        if (!fm) { std::cerr << "trim failed\n"; return 1; }
        AirSteering as;
        const double target_alt = 11000.0;
        const double target_heading = to_radians(fm->state().kin.psi);
        const double target_speed_kts = fm->state().vcas;

        std::vector<TrajectoryPoint> pts;
        pts.reserve(1800);
        for (int tick = 0; tick < 1800; ++tick) {  // 30 s
            AirSteering::Input in = readState(*fm);
            const auto ai = as.steer(target_heading, target_alt,
                                     target_speed_kts, in);
            fm->update(MAJOR_DT, mapAI(ai), GROUND_Z_FLAT, FLAT_NORMAL);

            const double hdg = to_radians(fm->state().kin.psi);
            double hdg_err = target_heading - hdg;
            while (hdg_err > kPI)  hdg_err -= 2*kPI;
            while (hdg_err < -kPI) hdg_err += 2*kPI;

            TrajectoryPoint p{};
            p.t = tick * MAJOR_DT;
            p.heading_err_deg = hdg_err * kRTD;
            p.phi_deg = to_degrees(fm->state().kin.phi);
            p.alt_ft = -fm->state().kin.z;
            p.vs_fpm = -fm->state().kin.zdot * 60.0;
            p.vcas_kts = fm->state().vcas;
            p.p_radps = fm->state().kin.p;
            p.q_radps = fm->state().kin.q;
            p.rstick = ai.roll_cmd;
            p.pstick = ai.pitch_cmd;
            p.beta_deg = to_degrees(fm->state().aero.beta);
            p.throttle = ai.throttle_cmd;
            p.speed_brake = ai.speed_brake_cmd;
            pts.push_back(p);
        }
        dump_csv(out_dir + "/alt_capture.csv", pts);
        print_summary("ALT_CAPTURE", summarize(pts));
    }

    // -------- Scenario 4: sustained turn (alt hold while turning) --------
    // Hold a CONSTANT nonzero target heading offset — but since AirSteering
    // turns toward the heading, this is equivalent to a sustained 30° bank.
    // We instead test sustained bank by holding heading target one step
    // ahead: re-command "current heading + 30°" each tick so the aircraft
    // has to bank continuously. This stresses the in-turn altitude hold.
    {
        auto fm = makeTrimmedF16(10000.0, 500.0);
        if (!fm) { std::cerr << "trim failed\n"; return 1; }
        AirSteering as;
        const double target_alt = 10000.0;
        const double target_speed_kts = fm->state().vcas;

        std::vector<TrajectoryPoint> pts;
        pts.reserve(1800);
        for (int tick = 0; tick < 1800; ++tick) {  // 30 s
            // Continuously demand 30° further along — sustains the turn.
            const double target_heading = to_radians(fm->state().kin.psi)
                                          + 30.0 * kD2R;
            AirSteering::Input in = readState(*fm);
            const auto ai = as.steer(target_heading, target_alt,
                                     target_speed_kts, in);
            fm->update(MAJOR_DT, mapAI(ai), GROUND_Z_FLAT, FLAT_NORMAL);

            TrajectoryPoint p{};
            p.t = tick * MAJOR_DT;
            // For this scenario we report the actual bank as the indicator.
            p.heading_err_deg = 30.0;  // the steady-state command
            p.phi_deg = to_degrees(fm->state().kin.phi);
            p.alt_ft = -fm->state().kin.z;
            p.vs_fpm = -fm->state().kin.zdot * 60.0;
            p.vcas_kts = fm->state().vcas;
            p.p_radps = fm->state().kin.p;
            p.q_radps = fm->state().kin.q;
            p.rstick = ai.roll_cmd;
            p.pstick = ai.pitch_cmd;
            p.beta_deg = to_degrees(fm->state().aero.beta);
            p.throttle = ai.throttle_cmd;
            p.speed_brake = ai.speed_brake_cmd;
            pts.push_back(p);
        }
        dump_csv(out_dir + "/sustained_turn.csv", pts);
        // Custom summary: avg/max bank, altitude drift over 30s sustained turn
        double min_alt = 1e9, max_alt = -1e9;
        double avg_phi = 0, max_phi = 0, avg_vs = 0;
        for (const auto& p : pts) {
            min_alt = std::min(min_alt, p.alt_ft);
            max_alt = std::max(max_alt, p.alt_ft);
            avg_phi += std::fabs(p.phi_deg);
            max_phi = std::max(max_phi, std::fabs(p.phi_deg));
            avg_vs += std::fabs(p.vs_fpm);
        }
        avg_phi /= pts.size();
        avg_vs  /= pts.size();
        std::cout << "[SUSTAINED_TURN] "
                  << "avg_phi=" << avg_phi << "deg "
                  << "max_phi=" << max_phi << "deg "
                  << "alt_range=" << (max_alt - min_alt) << "ft "
                  << "alt_drift=" << (pts.front().alt_ft - pts.back().alt_ft) << "ft "
                  << "max_vs=" << std::fabs(std::max_element(pts.begin(), pts.end(),
                        [](const auto&a, const auto&b){return std::fabs(a.vs_fpm)<std::fabs(b.vs_fpm);})->vs_fpm)
                  << "fpm\n";
    }

    // -------- Scenario 5: glideslope tracking (landing configuration) --------
    // Test whether the cruise steer() law can track a glideslope in the
    // landing configuration (gear + flaps, approach speed, throttle floor 0.20).
    // This matches what the landing module does on OnFinal.
    {
        const double gs_angle_rad = 3.0 * kD2R;
        const double start_range_ft = 60760.0;  // 10nm
        const double start_alt_ft = start_range_ft * std::tan(gs_angle_rad);  // ~3183ft
        // Trim at approach speed (160 kts) at the start altitude
        auto fm = makeTrimmedF16(/*alt_ft=*/start_alt_ft,
                                  /*vt_ftps=*/160.0 * 1.68781);
        if (!fm) { std::cerr << "trim failed\n"; return 1; }
        AirSteering as;

        const double target_speed_kts = 160.0;
        const double target_heading = 0.0;
        const double v_fps = target_speed_kts * 1.68781;
        const double gs_vs_fpm = -std::tan(gs_angle_rad) * v_fps * 60.0;  // ~-845 fpm

        std::vector<TrajectoryPoint> pts;
        pts.reserve(7200);  // 120s
        for (int tick = 0; tick < 7200; ++tick) {  // 120 s
            AirSteering::Input in = readState(*fm);
            const double range_ft = std::sqrt(in.position.x * in.position.x
                                             + in.position.y * in.position.y);
            const double gs_target_alt = range_ft * std::tan(gs_angle_rad);
            in.vs_ff_fpm = gs_vs_fpm;

            const auto ai = as.steer(target_heading, gs_target_alt,
                                     target_speed_kts, in,
                                     /*throttle_floor=*/0.20);
            PilotInput pi = mapAI(ai);
            pi.gearHandle = 1.0;   // gear down
            pi.tefCmd = 1.0;      // full flaps
            pi.lefCmd = 1.0;
            fm->update(MAJOR_DT, pi, GROUND_Z_FLAT, FLAT_NORMAL);

            const double hdg = to_radians(fm->state().kin.psi);
            double hdg_err = target_heading - hdg;
            while (hdg_err > kPI)  hdg_err -= 2*kPI;
            while (hdg_err < -kPI) hdg_err += 2*kPI;

            TrajectoryPoint p{};
            p.t = tick * MAJOR_DT;
            p.heading_err_deg = hdg_err * kRTD;
            p.phi_deg = to_degrees(fm->state().kin.phi);
            p.alt_ft = -fm->state().kin.z;
            p.vs_fpm = -fm->state().kin.zdot * 60.0;
            p.vcas_kts = fm->state().vcas;
            p.p_radps = fm->state().kin.p;
            p.q_radps = fm->state().kin.q;
            p.rstick = ai.roll_cmd;
            p.pstick = ai.pitch_cmd;
            p.beta_deg = to_degrees(fm->state().aero.beta);
            p.throttle = ai.throttle_cmd;
            p.speed_brake = ai.speed_brake_cmd;
            pts.push_back(p);

            if (p.alt_ft < 0.0) break;
        }
        dump_csv(out_dir + "/approach.csv", pts);

        double gs_track_err = 0, speed_err = 0;
        int n = 0;
        for (const auto& p : pts) {
            if (p.t > 10.0 && p.alt_ft > 50.0) {
                gs_track_err += std::fabs(p.vs_fpm - gs_vs_fpm);
                speed_err += std::fabs(p.vcas_kts - target_speed_kts);
                ++n;
            }
        }
        gs_track_err = n > 0 ? gs_track_err / n : 0;
        speed_err = n > 0 ? speed_err / n : 0;
        std::cout << "[APPROACH] "
                  << "final_alt=" << pts.back().alt_ft << "ft "
                  << "gs_track_err=" << gs_track_err << "fpm "
                  << "speed_err=" << speed_err << "kts "
                  << "max_beta=" << [&]{
                      double mx = 0; for (const auto& p : pts) mx = std::max(mx, std::fabs(p.beta_deg)); return mx;
                  }() << "deg "
                  << "sb_cycles=" << [&]{
                      int c = 0, prev = 0;
                      for (const auto& p : pts) {
                          int s = (p.speed_brake > -0.5) ? 1 : 0;
                          if (prev != 0 && s != prev) ++c;
                          if (s != 0 || prev != 0) prev = s;
                      }
                      return c;
                  }() << "\n";
    }

    // -------- Scenario 5b: Approach-speed trajectory dump --------
    // Dump detailed trajectory for the F-16 at approach speed (5k/300fps)
    // to diagnose the phugoid that appears at this operating point.
    {
        f4::data::AircraftConfig cfg;
        auto r = f4::data::loadConfig(std::string(kFixturesDir) + "/f16.json");
        if (!r.ok) { std::cerr << "f16 load failed\n"; return 1; }
        cfg = r.config;
        // Trim at approach speed (300 fps = ~273kt)
        auto fm = std::make_unique<FlightModel>();
        fm->init(cfg, 5000.0, 300.0 * 1.68781, 0.0, /*inAir=*/true);
        if (!fm->trim()) { std::cerr << "trim failed\n"; return 1; }
        fm->setGround(GROUND_Z_FLAT, FLAT_NORMAL);
        AirSteering as;
        // Seed integrators from current state to avoid first-tick transient
        {
            AirSteering::Input in = readState(*fm);
            const double v_fps_s = std::max(100.0, in.vcas_kts * 1.68781);
            const double gamma_now = std::asin(std::clamp((in.vs_fpm / 60.0) / v_fps_s, -0.7, 0.7));
            const double alpha_est = std::clamp(in.pitch_rad - gamma_now, -0.06, 0.28);
            as.seed_from_state(in.vs_fpm, alpha_est);
        }

        std::vector<TrajectoryPoint> pts;
        pts.reserve(1800);
        const double target_speed = fm->state().vcas;  // hold trim speed
        for (int tick = 0; tick < 1800; ++tick) {  // 30 s
            AirSteering::Input in = readState(*fm);
            // Test cruise law with landing throttle floor (0.05 = true idle)
            const auto ai = as.steer(0.0, 5000.0,
                                     target_speed, in,
                                     /*throttle_floor=*/0.05);
            fm->update(MAJOR_DT, mapAI(ai), GROUND_Z_FLAT, FLAT_NORMAL);

            TrajectoryPoint p{};
            p.t = tick * MAJOR_DT;
            p.heading_err_deg = 30.0;
            p.phi_deg = to_degrees(fm->state().kin.phi);
            p.alt_ft = -fm->state().kin.z;
            p.vs_fpm = -fm->state().kin.zdot * 60.0;
            p.vcas_kts = fm->state().vcas;
            p.p_radps = fm->state().kin.p;
            p.q_radps = fm->state().kin.q;
            p.rstick = ai.roll_cmd;
            p.pstick = ai.pitch_cmd;
            p.beta_deg = to_degrees(fm->state().aero.beta);
            p.throttle = ai.throttle_cmd;
            p.speed_brake = ai.speed_brake_cmd;
            pts.push_back(p);
        }
        dump_csv(out_dir + "/approach_speed.csv", pts);
        double min_alt = 1e9, max_alt = -1e9, max_vs = 0;
        for (const auto& p : pts) {
            min_alt = std::min(min_alt, p.alt_ft);
            max_alt = std::max(max_alt, p.alt_ft);
            max_vs = std::max(max_vs, std::fabs(p.vs_fpm));
        }
        std::cout << "[APPROACH_SPEED] alt_range=" << (max_alt - min_alt)
                  << "ft max_vs=" << max_vs << "fpm\n";
    }

    // -------- Scenario 6: Envelope validation --------
    // Test the cruise control law at different altitudes, speeds, and
    // aircraft. Each sub-scenario runs a 30° heading step + altitude hold
    // and reports the altitude range + heading error.
    {
        struct EnvelopeTest {
            const char* name;
            const char* aircraft;
            double alt_ft;
            double vt_ftps;
        };
        const EnvelopeTest tests[] = {
            {"F16_10k_500fps", "f16", 10000.0, 500.0},
            {"F16_30k_500fps", "f16", 30000.0, 500.0},
            {"F16_5k_300fps",  "f16",  5000.0, 300.0 * 1.68781},
            {"F16_SL_600fps",  "f16",  1000.0, 600.0 * 1.68781},
            {"F18_10k_500fps", "f18", 10000.0, 500.0},
            {"A10_10k_300kt",  "a10", 10000.0, 300.0 * 1.68781},
        };

        for (const auto& t : tests) {
            f4::data::AircraftConfig cfg;
            const std::string path = std::string(kFixturesDir) + "/" + t.aircraft + ".json";
            if (!std::filesystem::exists(path)) {
                std::cout << "[ENV:" << t.name << "] SKIP (fixture not found)\n";
                continue;
            }
            auto r = f4::data::loadConfig(path);
            if (!r.ok) {
                std::cout << "[ENV:" << t.name << "] SKIP (load failed)\n";
                continue;
            }
            cfg = r.config;
            auto fm = std::make_unique<FlightModel>();
            fm->init(cfg, t.alt_ft, t.vt_ftps, 0.0, /*inAir=*/true);
            if (!fm->trim()) {
                std::cout << "[ENV:" << t.name << "] SKIP (trim failed)\n";
                continue;
            }
            fm->setGround(GROUND_Z_FLAT, FLAT_NORMAL);
            AirSteering as;

            const double target_heading = 30.0 * kD2R;
            const double target_alt = t.alt_ft;
            const double target_speed_kts = fm->state().vcas;

            double min_alt = 1e9, max_alt = -1e9, max_vs = 0;
            for (int tick = 0; tick < 1800; ++tick) {  // 30 s
                AirSteering::Input in = readState(*fm);
                const auto ai = as.steer(target_heading, target_alt,
                                         target_speed_kts, in);
                fm->update(MAJOR_DT, mapAI(ai), GROUND_Z_FLAT, FLAT_NORMAL);

                const double alt = -fm->state().kin.z;
                min_alt = std::min(min_alt, alt);
                max_alt = std::max(max_alt, alt);
                max_vs = std::max(max_vs, std::fabs(-fm->state().kin.zdot * 60.0));
            }
            const double hdg_err = std::fabs(target_heading
                                   - to_radians(fm->state().kin.psi));
            std::cout << "[ENV:" << t.name << "] "
                      << "alt_range=" << (max_alt - min_alt) << "ft "
                      << "max_vs=" << max_vs << "fpm "
                      << "hdg_err=" << (hdg_err * kRTD) << "deg "
                      << "final_vcas=" << fm->state().vcas << "kts\n";
        }
    }

    return 0;
}

// f4-recorder/include/f4/recorder/fcs_trace.hpp
//
// FcsTraceSample — one tick of the COMPLETE observable state of the
// flight-control loop: AI commands, FCS intermediates, body rates,
// kinematics, and navigation intent. Designed for offline plotting
// (the diagnostic workflow defined in FLIGHT_CONTROL_NEXT_STEPS.md §3.1).
//
// Distinct from FlightSnapshot (which is the replay-format unit and is
// intentionally lightweight): FcsTraceSample exposes FCS internals that
// are useful for control-loop diagnosis but irrelevant to replay.
//
// Dependencies: none (POD struct + std::string for AI state names).
// C++20.

#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace f4::recorder {

// ============================================================================
// FcsTraceSample — per-tick control-loop trace record.
// ============================================================================
//
// Column groups (kept in the order they're written to CSV by FcsTraceWriter):
//   1. Timing        — tick, sim_time_s, time_scale
//   2. AI state      — ai_mode, ai_state
//   3. AI commands   — pitch_cmd, roll_cmd, yaw_cmd, throttle_cmd, speed_brake_cmd,
//                       tef_cmd, lef_cmd, gear_down, wheel_brakes, parking_brake
//   4. FCS intermed. — aoacmd_deg, pscmd, pstab, ptcmd, nzcgs, pitch_integral,
//                       betcmd_deg, alpha_deg, beta_deg, yshape, pshape, rshape
//   5. Body rates    — p_dps, q_dps, r_dps  (deg/s)
//   6. Kinematics    — vcas_kts, vt_fps, alt_msl_ft, alt_agl_ft, vs_fpm,
//                       heading_deg, pitch_deg, roll_deg, x_ft, y_ft, mach
//   7. Navigation    — target_alt_ft, target_speed_kts, target_heading_deg,
//                       course_lateral_ft, course_along_ft, localizer_heading_deg
//   8. Ground/Engine — on_ground, gear_pos, engine_rpm, fuel_lbs, nz, nx
//   9. Loop diagnos. — PHUG-PLAN P0.3 (Docs/LONGITUDINAL_STABILITY_PLAN.md):
//                       the plant/FCS/cascade signals needed to attribute an
//                       observed oscillation to its owning loop (L0..L4):
//                       plant group (qsom, qbar, gamma, vtDot, thrust, stall,
//                       flap/brake positions), FCS internals group (alpha
//                       bias, q-damper term, designed bandwidth + gains, PI
//                       error), AirSteering cascade group (alt_err, vs_corr,
//                       vs_target, gamma chain, integrator states, speed
//                       channel cross-coupling).
struct FcsTraceSample {
    // --- Timing ---
    std::uint64_t tick{0};
    double sim_time_s{0.0};
    double time_scale{1.0};

    // --- AI state (human-readable for offline diffing) ---
    std::string ai_mode;          // e.g. "TakeoffMode"
    std::string ai_state;          // e.g. "OnFinal"

    // --- AI commands (normalized) ---
    double pitch_cmd{0.0};          // [-1, +1]
    double roll_cmd{0.0};           // [-1, +1]
    double yaw_cmd{0.0};            // [-1, +1]
    double throttle_cmd{0.0};       // [0, 1.5]
    double speed_brake_cmd{-1.0};   // [-1, +1]
    double tef_cmd{0.0};            // [0, 1]
    double lef_cmd{0.0};            // [0, 1]
    bool   gear_down{false};
    bool   wheel_brakes{false};
    bool   parking_brake{false};

    // --- FCS intermediates ---
    double aoacmd_deg{0.0};         // commanded alpha (deg)
    double pscmd{0.0};              // commanded roll rate (rad/s)
    double pstab{0.0};              // filtered roll rate (rad/s)
    double ptcmd{0.0};              // commanded pitch (G or alpha)
    double nzcgs{0.0};              // stability-axis normal load factor (G)
    double pitch_integral{0.0};    // PI integrator state (deg-equivalent)
    double betcmd_deg{0.0};         // commanded beta (deg)
    double alpha_deg{0.0};          // actual alpha (deg)
    double beta_deg{0.0};           // actual beta (deg)
    double yshape{0.0};             // shaped pedal input
    double pshape{0.0};             // shaped pitch stick input
    double rshape{0.0};             // shaped roll stick input

    // --- Body rates (deg/s for human readability) ---
    double p_dps{0.0};              // roll rate
    double q_dps{0.0};               // pitch rate
    double r_dps{0.0};               // yaw rate

    // --- Kinematics ---
    double vcas_kts{0.0};
    double vt_fps{0.0};
    double alt_msl_ft{0.0};
    double alt_agl_ft{0.0};
    double vs_fpm{0.0};
    double heading_deg{0.0};
    double pitch_deg{0.0};
    double roll_deg{0.0};
    double x_ft{0.0};               // ENU east
    double y_ft{0.0};               // ENU north
    double mach{0.0};

    // --- Navigation intent (what the AI is steering toward) ---
    double target_alt_ft{0.0};
    double target_speed_kts{0.0};
    double target_heading_deg{0.0};
    double course_lateral_ft{0.0};  // cross-track (right of centerline > 0)
    double course_along_ft{0.0};    // along-track (past threshold > 0)
    double localizer_heading_deg{0.0};

    // --- Ground / engine ---
    bool   on_ground{true};
    double gear_pos{0.0};           // 0 = up, 1 = down
    double engine_rpm{0.0};         // 0..1+ (1 = MIL)
    double fuel_lbs{0.0};
    double nz{0.0};                  // normal load factor (G)
    double nx{0.0};                  // axial load factor

    // --- Loop diagnostics: plant group (PHUG-PLAN P0.3) ---
    double qsom{0.0};               // q*S/m — sets the L0 loop gain K_nz
    double qbar{0.0};               // dynamic pressure (lb/ft²)
    double gamma_deg{0.0};          // flight-path angle (the phugoid variable)
    double vt_dot{0.0};             // dVt/dt (ft/s²) — axial dynamics
    double thrust_accel{0.0};       // engine thrust acceleration (ft/s²)
    int    stall_state{0};          // StallState enum value (mode filter)
    double tef_pos{0.0};            // actual TEF position 0..1 (actuator lag)
    double lef_pos{0.0};            // actual LEF position 0..1
    double dbrake_pos{0.0};         // actual speed-brake position 0..1

    // --- Loop diagnostics: FCS internals group (PHUG-PLAN P0.3) ---
    double alpha_bias_deg{0.0};     // 1-G trim feedforward (L2: V² coupling)
    double q_damper_term{0.0};      // Tranche 42/45/46 term subtracted from ptcmd (L1)
    double omega_sp{0.0};           // designed inner-loop bandwidth (rad/s)
    double zp01{0.0};               // designed pitch damping ratio
    double tp02{0.0};               // lead-lag lag constant (s)
    double tp03{0.0};               // lead-lag lag constant (s)
    double pi_error{0.0};           // post-kp05 G error into the pitch PI

    // --- Loop diagnostics: AirSteering cascade group (PHUG-PLAN P0.3) ---
    double ai_alt_err_ft{0.0};      // altitude error driving the cascade
    double ai_vs_corr_fpm{0.0};     // P + leaky-I VS correction (windowed)
    double ai_vs_target_fpm{0.0};   // VS command after the slew limiter
    double ai_vs_ff_fpm{0.0};       // path feedforward VS
    double ai_gamma_now_rad{0.0};   // asin(vs/v) from ACTUAL VS
    double ai_gamma_ff_rad{0.0};    // path angle from vs_target
    double ai_gamma_corr_rad{0.0};  // VS-error damping term (Q3-clamped)
    double ai_alpha_est_rad{0.0};   // pitch - gamma_now (STAB-E17 clamped)
    double ai_theta_target_rad{0.0};// final pitch-attitude target
    double ai_alt_integral_fpm{0.0};// alt leaky-integrator state
    double ai_hdg_err_rad{0.0};     // wrapped heading error
    double ai_bank_target_rad{0.0}; // commanded bank (0 = beam ride)
    double ai_speed_err_kt{0.0};    // speed error (L4 input)
    double ai_speed_integral{0.0};  // speed leaky-integrator state
    double ai_energy_err_ft{0.0};   // V2 total-energy error (ft)
};

// ============================================================================
// FcsTraceWriter — accumulates samples and writes them to CSV.
// ============================================================================
class FcsTraceWriter {
public:
    // Append one sample. O(1) amortized.
    void record(const FcsTraceSample& sample) {
        samples_.push_back(sample);
    }

    // Write the accumulated samples to a CSV file at `path`.
    // The first row is a header naming every column. Throws std::runtime_error
    // if the file cannot be opened.
    void write_csv(const std::string& path) const;

    // Write to an output stream (used by tests + write_csv).
    void write_csv(std::ostream& os) const;

    // Direct access to the samples.
    [[nodiscard]] const std::vector<FcsTraceSample>& samples() const noexcept {
        return samples_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

    // Clear all samples (does not affect any file already written).
    void clear() noexcept { samples_.clear(); }

private:
    std::vector<FcsTraceSample> samples_;
};

} // namespace f4::recorder

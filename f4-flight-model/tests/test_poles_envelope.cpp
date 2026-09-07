// test_poles_envelope.cpp — CI golden-pole regression (plan §8.4)
//
// FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md §8 acceptance: "a PR that moves any
// pole more than 5% must update the golden file AND the worklog entry
// explains why. This is the machine-checkable version of 'don't destabilize
// what you didn't mean to touch.'"
//
// Implementation note: this test drives the `diag_poles` diagnostic binary
// (same build tree) end-to-end rather than duplicating its trim/linearize/
// eigen machinery here. The binary is deterministic (same fixture, same
// Newton start -> identical poles run-to-run; verified 0.0% spread across
// the 18-condition plant grid between Task 63 and Task 64 builds).
//
// Golden values (re-derived on the PHUG-merged tree, Task 66 — the prior
// Task-64 goldens were measured on the pre-P4.1 FCS and were superseded by
// the intended P4.1 inner-loop changes; see worklog Task 66):
//
//   plant 15000 ft / 250 KCAS clean: the G-hold aperiodic speed mode is now
//   +0.00115 (was +0.02106 pre-P4.1). The P4.1 corrections (kp05 = 1/K_nz
//   plant inverse, real integrator) reduced it 26x — the pole gate now
//   asserts the STABILITY acceptance (<= +0.005) instead of pinning the old
//   unstable value.
//
//   AI-closed, nav cruise tune (what NavigationModule actually flies):
//   worst sub-1-rad/s mode +0.01656 at 15k/250 (STAB-P1 alt_integral_gain
//   0.6 is load-bearing: with the pre-STAB-P1 1.2 the same point measures
//   +0.2196). The cascade remains marginally unstable across the cruise
//   envelope on BOTH tunes (default +0.23..+0.58, nav +0.02..+0.53) — the
//   structural fix is the F-2/P4.2 TECS margin campaign (parked upstream);
//   gain re-hunting is refused by measurement (razor-non-monotonic
//   landscape). Gates below therefore (a) pin the nav-tune linear value at
//   its measured operating point, (b) assert TIME-DOMAIN boundedness of
//   the nav-tune closed loop (the robust acceptance, immune to the
//   clamp-kink Jacobian artifacts that pollute the AI-closed linear scan:
//   spurious +8..+173/s modes at the slew-limiter kinks), and (c) bound
//   the default-tune linear regression until the TECS work lands.
//
// Evidence CSVs: Docs/diagnostics/phaseB_plant_poles.csv (Task 63),
// phaseB2_plant_default_poles.csv + phaseB2_ai_default_poles.csv +
// phaseE_ablation.csv (Task 64).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>

namespace {

#ifndef DIAG_POLES_BINARY
#define DIAG_POLES_BINARY "diag_poles"
#endif

struct PolesResult {
    int exit_code = 0;
    double worst_all = -1e300;   // max Re over all reported modes
    double worst_slow = -1e300;  // max Re over modes with Re < 1.0 (sub-1-rad/s band)
    std::string output;
};

// Runs diag_poles with the given args and extracts the worst eigenvalue
// metrics from its human-readable output. Mirrors scripts/worst.sh.
PolesResult runDiagPoles(const std::string& args) {
    const std::string fixture = F4_GENERATED_FIXTURES_DIR "/f16.json";
    const std::string cmd =
        std::string(DIAG_POLES_BINARY) + " --fixture " + fixture + " " + args;
    // diag_poles returns 1 when the Newton trim does not fully converge —
    // that is diagnostic information, not a harness failure. Capture output
    // regardless; assertion-level decisions are made on the poles.
    FILE* pipe = ::popen(cmd.c_str(), "r");
    PolesResult r;
    if (pipe == nullptr) {
        r.output = "popen failed for: " + cmd;
        r.exit_code = -1;
        return r;
    }
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) r.output.append(buf, n);
    r.exit_code = ::pclose(pipe);

    // Eigen lines: "  <name>  Re  Im  zeta  wn  T  t2x" with the last six
    // numeric fields (T/t2x may be inf). Names may contain spaces/brackets.
    static const std::string num = "(-?(?:\\d+\\.?\\d*|inf|nan))";
    static const std::regex eigen_re(
        "^[ ]+[a-z].*?" + num + "[ ]+" + num + "[ ]+" + num + "[ ]+" + num +
        "[ ]+" + num + "[ ]+" + num + "[ ]*$");
    std::istringstream in(r.output);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_match(line, m, eigen_re)) continue;
        try {
            const double re = std::stod(m[1]);
            r.worst_all = std::max(r.worst_all, re);
            if (re < 1.0) r.worst_slow = std::max(r.worst_slow, re);
        } catch (const std::exception&) {
            // unparseable field — skip the line
        }
    }
    return r;
}

double relDiff(double a, double b) {
    return std::fabs(a - b) / std::max(std::fabs(a), 1e-9);
}

}  // namespace

// ---------------------------------------------------------------------------
// Plant baseline (no AI): the G-hold aperiodic speed instability must stay
// exactly where Task 63 measured it until the F-2 TECS fix lands.
// ---------------------------------------------------------------------------
TEST(PolesEnvelope, PlantCruiseAperiodicModeGolden) {
    const PolesResult r = runDiagPoles("--alt 15000 --kcas 250 --config clean");
    ASSERT_GT(r.worst_all, -1e299) << "no eigenvalues parsed:\n" << r.output;
    // P4.1 moved the golden +0.02106 -> +0.00115 (intended, documented).
    // The plant+FCS closed map now passes the plan §8.1 stability
    // acceptance, so this is a GATE (not a golden pin): anything above
    // +0.005 re-grows the aperiodic speed mode and must justify itself.
    EXPECT_LE(r.worst_all, 0.005)
        << "plant cruise aperiodic mode regressed past +0.005 (P4.1-era "
           "measured +0.00115):\n"
        << r.output;
}

// ---------------------------------------------------------------------------
// AI-closed, NAV cruise tune (what NavigationModule actually flies):
// STAB-P1 (alt_integral_gain 0.6) is load-bearing against the P4.1 inner
// loop — measured 15k/250: +0.0166 (gain 0.6) vs +0.2196 (pre-STAB-P1 1.2).
// ---------------------------------------------------------------------------
TEST(PolesEnvelope, AiCruiseNavTuneSlowModeGolden) {
    const PolesResult r = runDiagPoles(
        "--alt 15000 --kcas 250 --config clean --ai --tune nav");
    ASSERT_GT(r.worst_slow, -1e299) << "no eigenvalues parsed:\n" << r.output;
    // Golden: +0.01656 (5% rule + 5e-3 absolute floor for near-zero poles).
    // A move beyond this band must update the golden AND explain why in the
    // worklog (plan §8.4).
    EXPECT_NEAR(r.worst_slow, 0.01656, std::max(0.05 * 0.01656, 5e-3))
        << "AI-cruise (nav tune) worst slow pole moved — update the golden "
           "value in test_poles_envelope.cpp AND add a worklog entry "
           "explaining why.\n"
        << r.output;
}

TEST(PolesEnvelope, AiCruiseNavTuneSlowModeGate) {
    const PolesResult r = runDiagPoles(
        "--alt 15000 --kcas 250 --config clean --ai --tune nav");
    ASSERT_GT(r.worst_slow, -1e299) << "no eigenvalues parsed:\n" << r.output;
    // Regression gate: measured envelope under the nav tune at this point
    // is +0.0166; the gate trips at 3x — big enough to ignore trim/FD noise,
    // tight enough to catch a re-grown L1/L3 loop.
    EXPECT_LE(r.worst_slow, 0.05)
        << "AI-cruise (nav tune) slow mode regressed past +0.05 (measured "
           "+0.0166 at the PHUG merge; pre-STAB-P1 was +0.2196): "
        << r.worst_slow << "\n"
        << r.output;
}

// ---------------------------------------------------------------------------
// Time-domain boundedness (the robust acceptance): the AI-closed linear
// scan at cruise is polluted by clamp-kink Jacobian artifacts (spurious
// +8..+173 /s modes at the slew-limiter/authority clamps), so the durable
// gate is the nonlinear response: a +25 ft/s speed kick at trim must stay
// bounded through the nav-tune cascade. Measured on the PHUG-merged tree:
// max |dVS| 731 fpm, mean VS -3 fpm over 180 s (diag_poles --verify).
// ---------------------------------------------------------------------------
TEST(PolesEnvelope, AiCruiseNavTuneBoundedness) {
    const PolesResult r = runDiagPoles(
        "--alt 15000 --kcas 250 --config clean --ai --tune nav "
        "--verify-amp 25 --verify");
    // Parse the verify summary line: "verify: VS mean <x> fpm, max |dVS|
    // <y> fpm, est period <z> s".
    static const std::regex vm_re("max \\|dVS\\|\\s*(-?(?:\\d+\\.?\\d*))");
    std::smatch m;
    ASSERT_TRUE(std::regex_search(r.output, m, vm_re))
        << "no verify summary in output:\n" << r.output;
    const double max_dvs = std::stod(m[1]);
    // Gate: bounded at 2x the measured excursion. A divergence (or a new
    // limit cycle) pushes this orders of magnitude past the band.
    EXPECT_LT(max_dvs, 1500.0)
        << "nav-tune cruise closed loop diverged from a +25 ft/s kick: max "
           "|dVS| " << max_dvs << " fpm (measured 731 on the PHUG merge)\n"
        << r.output;
}

// ---------------------------------------------------------------------------
// Default-tune regression bound (until the F-2/P4.2 TECS margin campaign
// lands): wingman/BVR/WVR/missile/collision-avoid/ground-avoid fly the
// class defaults. The cascade is marginally unstable there (measured
// +0.23..+0.58 across the cruise envelope) — this gate only prevents it
// from getting WORSE without a worklog justification.
// ---------------------------------------------------------------------------
TEST(PolesEnvelope, AiCruiseDefaultTuneBound) {
    const PolesResult r = runDiagPoles("--alt 15000 --kcas 250 --config clean --ai");
    ASSERT_GT(r.worst_slow, -1e299) << "no eigenvalues parsed:\n" << r.output;
    EXPECT_LE(r.worst_slow, 0.70)
        << "default-tune AI-cruise slow mode regressed past +0.70 (measured "
           "+0.2306 at the PHUG merge; envelope +0.23..+0.58): "
        << r.worst_slow << "\n"
        << r.output;
}

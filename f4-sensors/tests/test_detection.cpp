// test_detection.cpp — the pure radar detection model: aspect lobes,
// fourth-root RCS scaling, closure modifier, probability ramp, scan-volume
// containment (including the wrap-around at north).

#include <f4/sensors/detection.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::sensors;

namespace {

constexpr double kPi = 3.14159265358979323846;

RadarParameters default_radar() {
    return RadarParameters{};  // 40 NM vs 5 m^2 head-on
}

} // namespace

// ============================================================================
// Aspect lobes
// ============================================================================

TEST(AspectLobe, NoseOnIsStrongest) {
    const double nose = aspect_lobe_factor(0.0);
    const double beam = aspect_lobe_factor(kPi / 2.0);
    const double tail = aspect_lobe_factor(kPi);
    EXPECT_DOUBLE_EQ(nose, 1.0);
    EXPECT_LT(beam, nose);
    EXPECT_LT(tail, nose);
    EXPECT_GT(tail, beam);  // tail between nose and beam
}

TEST(AspectLobe, ClampedToValidRange) {
    // Negative aspect mirrors; beyond pi clamps to tail-on.
    EXPECT_DOUBLE_EQ(aspect_lobe_factor(-0.5), aspect_lobe_factor(0.5));
    EXPECT_DOUBLE_EQ(aspect_lobe_factor(4.0 * kPi), aspect_lobe_factor(kPi));
}

// ============================================================================
// Fourth-root RCS scaling + closure
// ============================================================================

TEST(DetectionRange, FourthRootRcsScaling) {
    const auto params = default_radar();
    // 2x the RCS => 2^(1/4) x the range (radar equation).
    const double r_ref = detection_range_nm(params, 5.0, 0.0, 0.0);
    const double r_double = detection_range_nm(params, 10.0, 0.0, 0.0);
    EXPECT_NEAR(r_double / r_ref, std::pow(2.0, 0.25), 1e-9);
    // Head-on reference target sees exactly the reference range.
    EXPECT_NEAR(r_ref, 40.0, 1e-9);
}

TEST(DetectionRange, AspectShrinksRange) {
    const auto params = default_radar();
    const double nose = detection_range_nm(params, 5.0, 0.0, 0.0);
    const double beam = detection_range_nm(params, 5.0, kPi / 2.0, 0.0);
    EXPECT_LT(beam, nose);
    // Beam lobe is 0.30 => range scales by 0.30^(1/4).
    EXPECT_NEAR(beam / nose, std::pow(0.30, 0.25), 1e-9);
}

TEST(DetectionRange, ClosureExtendsOpeningShrinks) {
    const auto params = default_radar();
    const double nominal = detection_range_nm(params, 5.0, 0.0, 0.0);
    // +2000 fps closure => +25% range (the modifier cap).
    const double closing = detection_range_nm(params, 5.0, 0.0, 2000.0);
    const double opening = detection_range_nm(params, 5.0, 0.0, -2000.0);
    EXPECT_NEAR(closing, nominal * 1.25, 1e-9);
    EXPECT_NEAR(opening, nominal * 0.75, 1e-9);
    // Effect saturates at the cap.
    EXPECT_DOUBLE_EQ(detection_range_nm(params, 5.0, 0.0, 5000.0), closing);
}

// ============================================================================
// Probability ramp
// ============================================================================

TEST(DetectionProbability, SureThingInsideKnee) {
    const auto params = default_radar();
    const TargetSignature sig;  // reference target, head-on, no closure
    EXPECT_DOUBLE_EQ(detection_probability(params, sig, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(detection_probability(params, sig, 0.75 * 40.0 - 1e-9), 1.0);
}

TEST(DetectionProbability, LinearRampToZeroAtMaxRange) {
    const auto params = default_radar();
    const TargetSignature sig;
    // Halfway between knee (0.75 R) and R: Pd = 0.5.
    const double mid = (0.75 * 40.0 + 40.0) / 2.0;
    EXPECT_NEAR(detection_probability(params, sig, mid), 0.5, 1e-9);
    // Beyond R_det: zero.
    EXPECT_DOUBLE_EQ(detection_probability(params, sig, 40.0 + 1e-9), 0.0);
    EXPECT_DOUBLE_EQ(detection_probability(params, sig, 100.0), 0.0);
}

TEST(DetectionProbability, BeamTargetDropsEarlier) {
    const auto params = default_radar();
    const TargetSignature nose_on;
    const TargetSignature beaming{5.0, kPi / 2.0, 0.0};  // rcs, aspect, closure
    // 27 NM: inside the nose-on sure-thing zone (knee = 30 NM) but inside
    // the beam target's ramp (its R_det = 40 * 0.30^(1/4) ~ 29.5 NM).
    const double probe_nm = 27.0;
    EXPECT_DOUBLE_EQ(detection_probability(params, nose_on, probe_nm), 1.0);
    EXPECT_GT(detection_probability(params, beaming, probe_nm), 0.0);
    EXPECT_LT(detection_probability(params, beaming, probe_nm), 1.0);
}

// ============================================================================
// ScanVolume containment
// ============================================================================

TEST(ScanVolume, ContainsInsideBar) {
    const ScanVolume v;  // centered north, 60 deg half-width
    EXPECT_TRUE(v.contains(0.0, 0.0, 100.0));
    EXPECT_TRUE(v.contains(0.5, 0.1, 100.0));       // inside azimuth + elevation
    EXPECT_TRUE(v.contains(-0.5, -0.2, 100.0));
}

TEST(ScanVolume, RejectsOutsideBar) {
    const ScanVolume v;
    EXPECT_FALSE(v.contains(kPi, 0.0, 100.0));            // due south
    EXPECT_FALSE(v.contains(1.2, 0.0, 100.0));            // beyond half-width
    EXPECT_FALSE(v.contains(0.0, kPi / 3.0, 100.0));      // too high
    EXPECT_FALSE(v.contains(0.0, -kPi / 3.0, 100.0));     // too low
    EXPECT_FALSE(v.contains(0.0, 0.0, 200.0));            // beyond range scale
}

TEST(ScanVolume, WrapAroundAtNorth) {
    // A bar centered on north (0) must contain bearings just below 2*pi.
    const ScanVolume v;
    EXPECT_TRUE(v.contains(2.0 * kPi - 0.1, 0.0, 100.0));
    // A bar centered on south covers 120..240 deg — and must NOT reach
    // across the north seam (its shortest-way geometry never wraps from
    // south onto the 0/2pi boundary within a 60 deg half-width).
    ScanVolume south;
    south.azimuth_center_rad = kPi;
    EXPECT_TRUE(south.contains(kPi - 0.1, 0.0, 100.0));
    EXPECT_TRUE(south.contains(kPi + 0.1, 0.0, 100.0));
    EXPECT_FALSE(south.contains(0.1, 0.0, 100.0));
    EXPECT_FALSE(south.contains(2.0 * kPi - 0.1, 0.0, 100.0));
}

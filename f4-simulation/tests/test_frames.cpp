// f4-simulation/tests/test_frames.cpp
//
// Convention tests for NED<->ENU orientation conversions. These encode the
// proof that fixed the "models render upside down / mirrored" bug: the
// FM's ZYX quaternion is NED (compass yaw positive about DOWN); storing it
// as ENU mirrors heading and scrambles pitch/roll. The fix conjugates it:
// (w,x,y,z)_ned -> (w,y,x,-z)_enu.
//
// Every case checks where a known BODY vector points in the WORLD after
// conversion — body nose (+x_ned) and body up (-z_ned), rotated into ENU
// and compared against the expected world direction.

#include <gtest/gtest.h>

#include "f4/simulation/frames.hpp"

#include <cmath>

using f4::simulation::QuatD;
using f4::simulation::quat_rotate;
using f4::simulation::ned_quat_to_enu;
using f4::simulation::enu_quat_from_compass;

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;
constexpr double TOL = 1e-9;

// The FM/EOM's ZYX (psi, theta, phi) Hamilton quaternion in NED.
// psi = compass heading (positive about z=DOWN), theta = pitch up positive
// (about +y_ned = right wing), phi = roll right positive (about +x_ned =
// nose).
QuatD ned_zyx(double psi, double theta, double phi) {
    const double hp = psi * 0.5, ht = theta * 0.5, hr = phi * 0.5;
    const double cpsi = std::cos(hp), spsi = std::sin(hp);
    const double cth  = std::cos(ht), sth  = std::sin(ht);
    const double cph  = std::cos(hr), sph  = std::sin(hr);
    return {
        cph * cth * cpsi + sph * sth * spsi,
        sph * cth * cpsi - cph * sth * spsi,
        cph * sth * cpsi + sph * cth * spsi,
        cph * cth * spsi - sph * sth * cpsi,
    };
}

struct Vec3 { double x, y, z; };

Vec3 body_vector_in_enu(const QuatD& q_ned, double bx, double by, double bz) {
    const QuatD q_enu = ned_quat_to_enu(q_ned);
    // Body axes in NED (x=nose, y=right wing, z=belly) map to ENU as:
    //   nose  (1,0,0)_ned -> (0,1,0)_enu  (north at heading 0)
    //   right (0,1,0)_ned -> (1,0,0)_enu  (east)
    //   belly (0,0,1)_ned -> (0,0,-1)_enu (down)
    double vx = by, vy = bx, vz = -bz;
    quat_rotate(q_enu, vx, vy, vz);
    return {vx, vy, vz};
}

} // anonymous namespace

// ============================================================================
// Rotation helper sanity
// ============================================================================

TEST(FramesQuat, IdentityRotationIsIdentity) {
    double x = 1, y = 2, z = 3;
    quat_rotate(QuatD{1, 0, 0, 0}, x, y, z);
    EXPECT_NEAR(x, 1, TOL); EXPECT_NEAR(y, 2, TOL); EXPECT_NEAR(z, 3, TOL);
}

TEST(FramesQuat, KnownNinetyAboutZ) {
    // (w=cos45, z=sin45): +90 deg about +z maps +x -> +y.
    QuatD q{std::cos(PI / 4), 0, 0, std::sin(PI / 4)};
    double x = 1, y = 0, z = 0;
    quat_rotate(q, x, y, z);
    EXPECT_NEAR(x, 0, TOL); EXPECT_NEAR(y, 1, TOL); EXPECT_NEAR(z, 0, TOL);
}

// ============================================================================
// Heading (the mirror bug)
// ============================================================================

TEST(FramesHeading, CompassEastTurnsNoseEast) {
    // psi = +90 (east). Body nose must point EAST (+x) in ENU, not west.
    const auto v = body_vector_in_enu(ned_zyx(90 * D2R, 0, 0), /*nose*/1, 0, 0);
    EXPECT_NEAR(v.x,  1, TOL);
    EXPECT_NEAR(v.y,  0, TOL);
    EXPECT_NEAR(v.z,  0, TOL);
}

TEST(FramesHeading, CompassWestTurnsNoseWest) {
    const auto v = body_vector_in_enu(ned_zyx(-90 * D2R, 0, 0), 1, 0, 0);
    EXPECT_NEAR(v.x, -1, TOL);
    EXPECT_NEAR(v.y,  0, TOL);
}

TEST(FramesHeading, NorthIsUnchanged) {
    const auto v = body_vector_in_enu(ned_zyx(0, 0, 0), 1, 0, 0);
    EXPECT_NEAR(v.x, 0, TOL);
    EXPECT_NEAR(v.y, 1, TOL);
}

// ============================================================================
// Pitch (the upside-down ingredient)
// ============================================================================

TEST(FramesPitch, PitchUpRaisesNoseWhileHeadingNorth) {
    // theta = +10 deg: nose rises above the horizon.
    const auto v = body_vector_in_enu(ned_zyx(0, 10 * D2R, 0), 1, 0, 0);
    EXPECT_NEAR(v.x, 0, TOL);
    EXPECT_NEAR(v.y, std::cos(10 * D2R), TOL);
    EXPECT_NEAR(v.z, std::sin(10 * D2R), TOL);
}

TEST(FramesPitch, PitchUpRaisesNoseWhileHeadingEast) {
    const auto v = body_vector_in_enu(ned_zyx(90 * D2R, 10 * D2R, 0), 1, 0, 0);
    EXPECT_NEAR(v.x, std::cos(10 * D2R), TOL);
    EXPECT_NEAR(v.y, 0, TOL);
    EXPECT_NEAR(v.z, std::sin(10 * D2R), TOL);
}

// ============================================================================
// Roll
// ============================================================================

TEST(FramesRoll, RollRightDropsRightWing) {
    // phi = +30 (right): the right-wing body vector (+y_ned = east at
    // heading north) must point DOWN in ENU.
    const auto v = body_vector_in_enu(ned_zyx(0, 0, 30 * D2R), /*right wing*/0, 1, 0);
    EXPECT_NEAR(v.x, std::cos(30 * D2R), TOL);
    EXPECT_NEAR(v.y, 0, TOL);
    EXPECT_NEAR(v.z, -std::sin(30 * D2R), TOL);
}

TEST(FramesRoll, BellyStaysDownLevel) {
    const auto v = body_vector_in_enu(ned_zyx(0, 0, 0), /*belly*/0, 0, 1);
    EXPECT_NEAR(v.z, -1, TOL);
}

// ============================================================================
// Combined attitude (the "upside down in flight" case)
// ============================================================================

TEST(FramesCombined, ClimbingRightBankKeepsCanopyUp) {
    // psi=0, theta=+15 climb, phi=+45 right bank: the canopy (-z body)
    // must stay ABOVE the horizon (positive z_enu component).
    const auto v = body_vector_in_enu(ned_zyx(0, 15 * D2R, 45 * D2R),
                                      /*canopy*/0, 0, -1);
    EXPECT_GT(v.z, 0.5) << "canopy must point up in a sane climbing bank";
}

// ============================================================================
// enu_quat_from_compass (spawn quaternions)
// ============================================================================

TEST(FramesSpawnQuat, CompassEastFacesEast) {
    const QuatD q = enu_quat_from_compass(90 * D2R);
    double x = 0, y = 1, z = 0;  // nose at heading 0 faces north
    quat_rotate(q, x, y, z);
    EXPECT_NEAR(x, 1, TOL);  // ... and after the east heading: east
    EXPECT_NEAR(y, 0, TOL);
}

TEST(FramesSpawnQuat, NorthIsIdentity) {
    const QuatD q = enu_quat_from_compass(0.0);
    EXPECT_NEAR(q.w, 1, TOL);
    EXPECT_NEAR(q.x, 0, TOL);
    EXPECT_NEAR(q.y, 0, TOL);
    EXPECT_NEAR(q.z, 0, TOL);
}

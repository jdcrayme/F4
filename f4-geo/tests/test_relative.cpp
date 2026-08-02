// test_relative.cpp — BRA and BullseyeOffset (Category B: relative/reference
// based position representations).

#include <gtest/gtest.h>
#include <f4/geo/f4_geo.hpp>

using namespace f4::geo;

// ============================================================================
// BRA
// ============================================================================

TEST(Bra, DueNorthOneNm) {
    WorldPosition from(0.0, 0.0, 0.0);
    WorldPosition target(0.0, 6076.1154855643, 0.0);   // 1 NM north
    BRA bra = to_bra(from, target);
    EXPECT_NEAR(bra.bearing_rad, 0.0, 1e-12);
    EXPECT_NEAR(bra.range_nm(), 1.0, 1e-9);
    EXPECT_NEAR(bra.altitude_ft, 0.0, 1e-9);
}

TEST(Bra, DueEast) {
    WorldPosition from(0.0, 0.0, 0.0);
    WorldPosition target(1000.0, 0.0, 0.0);   // east
    BRA bra = to_bra(from, target);
    EXPECT_NEAR(bra.bearing_rad, PI / 2.0, 1e-12);
    EXPECT_NEAR(bra.bearing_deg(), 90.0, 1e-9);
}

TEST(Bra, DueSouthBearingIsPi) {
    WorldPosition from(0.0, 0.0, 0.0);
    WorldPosition target(0.0, -1000.0, 0.0);
    BRA bra = to_bra(from, target);
    EXPECT_NEAR(bra.bearing_rad, PI, 1e-12);
}

TEST(Bra, DueWestBearingIsThreeHalvesPi) {
    WorldPosition from(0.0, 0.0, 0.0);
    WorldPosition target(-1000.0, 0.0, 0.0);
    BRA bra = to_bra(from, target);
    EXPECT_NEAR(bra.bearing_rad, 3.0 * PI / 2.0, 1e-12);
}

TEST(Bra, SlantRangeIncludesAltitude) {
    WorldPosition from(0.0, 0.0, 0.0);
    WorldPosition target(0.0, 3000.0, 4000.0);   // 3-4-5 -> 5000
    BRA bra = to_bra(from, target);
    EXPECT_NEAR(bra.range_ft, 5000.0, 1e-6);
    EXPECT_NEAR(bra.altitude_ft, 4000.0, 1e-9);
}

// ============================================================================
// BullseyeOffset
// ============================================================================

TEST(Bullseye, OffsetAt45DegForOneNm) {
    WorldPosition bullseye(0.0, 0.0, 0.0);
    // 45 deg from north, 1 NM. North = cos(45)*R, East = sin(45)*R.
    const double r = 6076.1154855643;
    WorldPosition target(r * std::sin(PI / 4.0), r * std::cos(PI / 4.0), 0.0);
    BullseyeOffset off = to_bullseye(bullseye, target);
    EXPECT_NEAR(off.bearing_deg(), 45.0, 1e-9);
    EXPECT_NEAR(off.range_nm(), 1.0, 1e-9);
}

TEST(Bullseye, RoundTripReconstructsTarget) {
    WorldPosition bullseye(12345.0, -6789.0, 0.0);
    BullseyeOffset off(PI / 3.0, 25000.0);   // 060 for ~4.1 NM
    WorldPosition reconstructed = from_bullseye(bullseye, off);
    BullseyeOffset back = to_bullseye(bullseye, reconstructed);
    EXPECT_NEAR(back.bearing_rad, off.bearing_rad, 1e-9);
    EXPECT_NEAR(back.range_ft, off.range_ft, 1e-6);
}

TEST(Bullseye, AltitudeTakenFromBullseyeOnReconstruction) {
    // Bullseye offsets are planar; reconstruction inherits the bullseye's
    // altitude (callers set target altitude separately if needed).
    WorldPosition bullseye(0.0, 0.0, 25000.0);
    BullseyeOffset off(0.0, 5000.0);
    WorldPosition t = from_bullseye(bullseye, off);
    EXPECT_DOUBLE_EQ(t.z, 25000.0);
}

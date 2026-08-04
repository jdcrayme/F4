// f4-data/tests/test_engine_rpm_schedule.cpp
//
// Tests for the data-driven per-engine-type RPM schedule.
//
// The schedule was lifted out of f4-flight-model/src/engine.cpp::engineRpmMods()
// (a 40-line if/else chain of magic Mach/altitude breakpoints). The primary
// regression gate is a reference oracle that re-implements the ORIGINAL
// if/else chain verbatim, then verifies that the new data-driven schedule
// produces byte-identical output across a dense grid of flight conditions
// for both engine families.

#include "f4/data/engine_rpm_schedule.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace f4::data {

// ---------------------------------------------------------------------------
// Reference oracle — the ORIGINAL engineRpmMods() if/else chain, ported
// verbatim. Used as a regression gate against the new data-driven schedule.
//
// If this function ever diverges from the data-driven schedule, the
// `ScheduleMatchesOriginal_*` tests will fail and pinpoint the regression.
// ---------------------------------------------------------------------------
static double reference_engineRpmMods(double rpmCmd, double alt_ft,
                                       double mach, double vcas,
                                       int typeEngine) {
    if (typeEngine == 1 || typeEngine == 2) {
        // PW-100 / PW-220
        if (mach >= 0.84 && mach <= 1.4) {
            rpmCmd = std::max(rpmCmd, mach / 1.4);
        }
        if (mach > 1.4) {
            rpmCmd = std::max(rpmCmd, 0.99);
        }
        if (alt_ft > 10000.0) {
            rpmCmd = std::max(rpmCmd, (alt_ft / 10000.0) / 30.0 + 0.7);
        }
        if (alt_ft >= 35000.0 && alt_ft <= 45000.0 && mach >= 0.4 && mach <= 0.8) {
            rpmCmd = std::min(rpmCmd, 1.025);
        }
        if (alt_ft > 45000.0 && alt_ft <= 55000.0 && mach >= 0.4 && mach <= 0.95) {
            rpmCmd = std::min(rpmCmd, 1.01);
        }
        if (alt_ft > 55000.0 || mach <= 0.4) {
            rpmCmd = std::min(rpmCmd, 0.99);
        }
    } else {
        // PW-229 / GE-110 / GE-129 (types 3, 4, 5)
        if (mach > 0.55 && mach < 1.1) {
            rpmCmd = std::max(rpmCmd, 0.79);
        }
        if (mach >= 1.1 && mach <= 1.4) {
            rpmCmd = std::max(rpmCmd, mach / 1.4);
        }
        if (alt_ft > 50000.0 && vcas < 250.0) {
            rpmCmd = std::min(rpmCmd, 0.99);
        }
    }
    return rpmCmd;
}

// ============================================================================
// Schedule lookup
// ============================================================================

TEST(EngineRpmSchedule, BuiltinReturnsSameInstanceForSameType) {
    const auto& s1 = EngineRpmSchedule::builtin(2);
    const auto& s2 = EngineRpmSchedule::builtin(2);
    EXPECT_EQ(&s1, &s2) << "builtin() must return a static reference, not a copy";
}

TEST(EngineRpmSchedule, BuiltinReturnsPw100FamilyForTypes1And2) {
    const auto& s1 = EngineRpmSchedule::builtin(1);
    const auto& s2 = EngineRpmSchedule::builtin(2);
    EXPECT_EQ(&s1, &s2) << "types 1 and 2 should share the PW-100/220 schedule";
    EXPECT_EQ(s1.rules.size(), 7u)
        << "PW-100/220 family should have 7 rules (5 simple + 2 from the OR-split of rule 6)";
}

TEST(EngineRpmSchedule, BuiltinReturnsPw229FamilyForTypes3Through5) {
    const auto& s3 = EngineRpmSchedule::builtin(3);
    const auto& s4 = EngineRpmSchedule::builtin(4);
    const auto& s5 = EngineRpmSchedule::builtin(5);
    EXPECT_EQ(&s3, &s4);
    EXPECT_EQ(&s4, &s5);
    EXPECT_EQ(s3.rules.size(), 3u)
        << "PW-229/GE-110/129 family should have 3 rules";
}

TEST(EngineRpmSchedule, BuiltinDefaultsUnknownTypeToPw220) {
    const auto& s_default = EngineRpmSchedule::builtin(99);
    const auto& s_pw220   = EngineRpmSchedule::builtin(2);
    EXPECT_EQ(&s_default, &s_pw220)
        << "unknown typeEngine should fall back to the PW-220 family";
}

TEST(EngineRpmSchedule, BuiltinHandlesZeroAndNegativeTypes) {
    const auto& s0  = EngineRpmSchedule::builtin(0);
    const auto& sn1 = EngineRpmSchedule::builtin(-1);
    const auto& s2  = EngineRpmSchedule::builtin(2);
    EXPECT_EQ(&s0,  &s2);
    EXPECT_EQ(&sn1, &s2);
}

// ============================================================================
// Rule matching — basic boundary semantics
// ============================================================================

TEST(EngineRpmRule, MatchesWhenAllConditionsInRange) {
    EngineRpmRule r;
    r.machLo = 0.5; r.machHi = 1.0;
    r.altLo_ft = 10000.0; r.altHi_ft = 40000.0;
    EXPECT_TRUE(r.matches(0.7, 25000.0, 0.0));
}

TEST(EngineRpmRule, MatchesWhenNanBoundsAreUnconstrained) {
    EngineRpmRule r;
    // Default-constructed rule has all-NaN bounds — matches everything.
    EXPECT_TRUE(r.matches(0.0,    0.0,    0.0));
    EXPECT_TRUE(r.matches(2.0,    80000.0, 1000.0));
}

TEST(EngineRpmRule, DoesNotMatchBelowLowerBound) {
    EngineRpmRule r;
    r.machLo = 0.5;
    EXPECT_FALSE(r.matches(0.4, 0.0, 0.0));
    EXPECT_TRUE (r.matches(0.5, 0.0, 0.0));  // inclusive lower bound
}

TEST(EngineRpmRule, DoesNotMatchAboveUpperBound) {
    EngineRpmRule r;
    r.machHi = 1.0;
    EXPECT_FALSE(r.matches(1.1, 0.0, 0.0));
    EXPECT_TRUE (r.matches(1.0, 0.0, 0.0));  // inclusive upper bound
}

TEST(EngineRpmRule, StrictLowerBoundExcludesBoundary) {
    EngineRpmRule r;
    r.machLo = 0.5; r.machLoStrict = true;  // strict >
    EXPECT_FALSE(r.matches(0.5, 0.0, 0.0));  // boundary excluded
    EXPECT_TRUE (r.matches(0.5001, 0.0, 0.0));
    EXPECT_FALSE(r.matches(0.4, 0.0, 0.0));
}

TEST(EngineRpmRule, StrictUpperBoundExcludesBoundary) {
    EngineRpmRule r;
    r.machHi = 1.0; r.machHiStrict = true;  // strict <
    EXPECT_FALSE(r.matches(1.0, 0.0, 0.0));  // boundary excluded
    EXPECT_TRUE (r.matches(0.999, 0.0, 0.0));
    EXPECT_FALSE(r.matches(1.1, 0.0, 0.0));
}

TEST(EngineRpmRule, StrictBoundsOnAllCoords) {
    EngineRpmRule r;
    r.altLo_ft = 10000.0; r.altLoStrict = true;  // alt > 10000
    r.vcasHi   = 250.0;   r.vcasHiStrict = true;  // vcas < 250
    EXPECT_FALSE(r.matches(0.0, 10000.0, 200.0));  // alt at boundary
    EXPECT_TRUE (r.matches(0.0, 10001.0, 200.0));  // alt above boundary
    EXPECT_FALSE(r.matches(0.0, 10001.0, 250.0));  // vcas at boundary
    EXPECT_TRUE (r.matches(0.0, 10001.0, 249.0));  // vcas below boundary
}

TEST(EngineRpmRule, DoesNotMatchWhenAnyBoundViolated) {
    EngineRpmRule r;
    r.machLo = 0.5; r.machHi = 1.0;
    r.altLo_ft = 10000.0; r.altHi_ft = 40000.0;
    r.vcasLo = 100.0;    r.vcasHi = 500.0;
    // Each of these violates exactly one bound.
    EXPECT_FALSE(r.matches(0.4, 25000.0, 300.0));  // mach too low
    EXPECT_FALSE(r.matches(1.1, 25000.0, 300.0));  // mach too high
    EXPECT_FALSE(r.matches(0.7,  9000.0, 300.0));  // alt too low
    EXPECT_FALSE(r.matches(0.7, 41000.0, 300.0));  // alt too high
    EXPECT_FALSE(r.matches(0.7, 25000.0,  99.0));  // vcas too low
    EXPECT_FALSE(r.matches(0.7, 25000.0, 501.0));  // vcas too high
}

// ============================================================================
// Rule value evaluation — constant vs linear
// ============================================================================

TEST(EngineRpmRule, ConstantValueIgnoresCoordinates) {
    EngineRpmRule r;
    r.valueKind = EngineRpmRule::ValueKind::Constant;
    r.valueOffset = 0.99;
    EXPECT_DOUBLE_EQ(r.evaluate(0.0,    0.0,    0.0),   0.99);
    EXPECT_DOUBLE_EQ(r.evaluate(2.0,    60000.0, 800.0), 0.99);
}

TEST(EngineRpmRule, LinearValueInMach) {
    EngineRpmRule r;
    r.valueKind = EngineRpmRule::ValueKind::Linear;
    r.valueScale = 1.0 / 1.4;
    r.valueOffset = 0.0;
    r.valueCoord = EngineRpmRule::Coord::Mach;
    EXPECT_NEAR(r.evaluate(0.7,  0.0, 0.0), 0.7 / 1.4, 1e-12);
    EXPECT_NEAR(r.evaluate(1.4,  0.0, 0.0), 1.0,       1e-12);
}

TEST(EngineRpmRule, LinearValueInAltitude) {
    EngineRpmRule r;
    r.valueKind = EngineRpmRule::ValueKind::Linear;
    r.valueScale = 1.0 / 300000.0;
    r.valueOffset = 0.7;
    r.valueCoord = EngineRpmRule::Coord::Altitude_ft;
    // At alt=30000, value = 30000/300000 + 0.7 = 0.1 + 0.7 = 0.8
    EXPECT_NEAR(r.evaluate(0.0, 30000.0, 0.0), 0.8, 1e-12);
    // At alt=60000, value = 60000/300000 + 0.7 = 0.2 + 0.7 = 0.9
    EXPECT_NEAR(r.evaluate(0.0, 60000.0, 0.0), 0.9, 1e-12);
}

TEST(EngineRpmRule, LinearValueInVcas) {
    EngineRpmRule r;
    r.valueKind = EngineRpmRule::ValueKind::Linear;
    r.valueScale = 0.001;  // 1 per 1000 kts (synthetic test)
    r.valueOffset = 0.5;
    r.valueCoord = EngineRpmRule::Coord::Vcas_kts;
    EXPECT_NEAR(r.evaluate(0.0, 0.0,   0.0), 0.5,   1e-12);
    EXPECT_NEAR(r.evaluate(0.0, 0.0, 250.0), 0.75, 1e-12);
}

// ============================================================================
// Schedule.apply — Floor/Ceiling semantics
// ============================================================================

TEST(EngineRpmSchedule, FloorRuleRaisesLowCommand) {
    EngineRpmSchedule s;
    EngineRpmRule r;
    r.kind = EngineRpmRule::Kind::Floor;
    r.valueKind = EngineRpmRule::ValueKind::Constant;
    r.valueOffset = 0.79;
    s.rules.push_back(r);

    // rpmCmd below the floor → raised to floor
    EXPECT_DOUBLE_EQ(s.apply(0.5, 0.0, 0.0, 0.0), 0.79);
    // rpmCmd at the floor → unchanged
    EXPECT_DOUBLE_EQ(s.apply(0.79, 0.0, 0.0, 0.0), 0.79);
    // rpmCmd above the floor → unchanged
    EXPECT_DOUBLE_EQ(s.apply(0.95, 0.0, 0.0, 0.0), 0.95);
}

TEST(EngineRpmSchedule, CeilingRuleLowersHighCommand) {
    EngineRpmSchedule s;
    EngineRpmRule r;
    r.kind = EngineRpmRule::Kind::Ceiling;
    r.valueKind = EngineRpmRule::ValueKind::Constant;
    r.valueOffset = 0.99;
    s.rules.push_back(r);

    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.0, 0.0, 0.0), 0.99);
    EXPECT_DOUBLE_EQ(s.apply(0.99, 0.0, 0.0, 0.0), 0.99);
    EXPECT_DOUBLE_EQ(s.apply(0.85, 0.0, 0.0, 0.0), 0.85);
}

TEST(EngineRpmSchedule, RulesApplyInOrderAndCompose) {
    // A floor at 0.79 followed by a ceiling at 0.99 should clamp to [0.79, 0.99].
    EngineRpmSchedule s;
    EngineRpmRule floor;
    floor.kind = EngineRpmRule::Kind::Floor;
    floor.valueOffset = 0.79;
    EngineRpmRule ceiling;
    ceiling.kind = EngineRpmRule::Kind::Ceiling;
    ceiling.valueOffset = 0.99;
    s.rules.push_back(floor);
    s.rules.push_back(ceiling);

    EXPECT_DOUBLE_EQ(s.apply(0.50, 0.0, 0.0, 0.0), 0.79);  // floored
    EXPECT_DOUBLE_EQ(s.apply(0.85, 0.0, 0.0, 0.0), 0.85);  // in range, unchanged
    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.0, 0.0, 0.0), 0.99);  // ceilinged
}

TEST(EngineRpmSchedule, NonMatchingRulesAreSkipped) {
    EngineRpmSchedule s;
    EngineRpmRule r;
    r.kind = EngineRpmRule::Kind::Floor;
    r.valueOffset = 0.99;
    r.machLo = 0.8; r.machHi = 1.0;  // only matches mach in [0.8, 1.0]
    s.rules.push_back(r);

    // mach outside the rule's box → rule skipped, rpmCmd unchanged
    EXPECT_DOUBLE_EQ(s.apply(0.5, 0.0, 0.0, 0.0), 0.5);
    // mach inside the box → rule fires
    EXPECT_DOUBLE_EQ(s.apply(0.5, 0.9, 0.0, 0.0), 0.99);
}

TEST(EngineRpmSchedule, EmptyScheduleReturnsInput) {
    EngineRpmSchedule s;
    EXPECT_DOUBLE_EQ(s.apply(0.834, 0.7, 25000.0, 300.0), 0.834);
}

// ============================================================================
// REGRESSION GATE — data-driven schedule must match the original if/else
// across a dense grid of flight conditions.
//
// If this test fails, the data-driven schedule diverges from the original
// if/else chain. The failure point will pinpoint which rule was mistranslated.
// ============================================================================

TEST(EngineRpmSchedule, ScheduleMatchesOriginalForPw100FamilyAcrossGrid) {
    const auto& schedule = EngineRpmSchedule::builtin(2);  // PW-220

    // Grid: mach × alt × vcas × rpmCmd
    // The PW-100/220 family doesn't use vcas, but pass a range anyway to
    // verify it's ignored.
    const double machs[]  = {0.0, 0.3, 0.4, 0.55, 0.7, 0.84, 0.95, 1.0, 1.1, 1.4, 1.5, 2.0};
    const double alts[]   = {0.0, 5000.0, 10000.0, 20000.0, 35000.0, 40000.0, 45000.0,
                             50000.0, 55000.0, 60000.0};
    const double vcass[]  = {0.0, 100.0, 250.0, 400.0, 800.0};
    const double rpmCmds[] = {0.5, 0.7, 0.85, 0.95, 0.99, 1.0, 1.025, 1.05, 1.1};

    int checks = 0;
    for (double mach : machs) {
        for (double alt : alts) {
            for (double vcas : vcass) {
                for (double rpmCmd : rpmCmds) {
                    const double expected = reference_engineRpmMods(rpmCmd, alt, mach, vcas, 2);
                    const double actual   = schedule.apply(rpmCmd, mach, alt, vcas);
                    EXPECT_NEAR(actual, expected, 1e-9)
                        << "divergence at mach=" << mach << " alt=" << alt
                        << " vcas=" << vcas << " rpmCmd=" << rpmCmd
                        << " (expected=" << expected << " actual=" << actual << ")";
                    ++checks;
                }
            }
        }
    }
    EXPECT_GT(checks, 1000) << "regression grid should cover >1000 cases";
}

TEST(EngineRpmSchedule, ScheduleMatchesOriginalForPw229FamilyAcrossGrid) {
    const auto& schedule = EngineRpmSchedule::builtin(3);  // PW-229

    const double machs[]  = {0.0, 0.5, 0.55, 0.7, 0.9, 1.0, 1.1, 1.2, 1.4, 1.5, 2.0};
    const double alts[]   = {0.0, 10000.0, 30000.0, 50000.0, 55000.0, 60000.0};
    const double vcass[]  = {0.0, 100.0, 249.0, 250.0, 251.0, 400.0};
    const double rpmCmds[] = {0.5, 0.7, 0.79, 0.85, 0.95, 0.99, 1.0, 1.05};

    int checks = 0;
    for (double mach : machs) {
        for (double alt : alts) {
            for (double vcas : vcass) {
                for (double rpmCmd : rpmCmds) {
                    const double expected = reference_engineRpmMods(rpmCmd, alt, mach, vcas, 3);
                    const double actual   = schedule.apply(rpmCmd, mach, alt, vcas);
                    EXPECT_NEAR(actual, expected, 1e-9)
                        << "divergence at mach=" << mach << " alt=" << alt
                        << " vcas=" << vcas << " rpmCmd=" << rpmCmd
                        << " (expected=" << expected << " actual=" << actual << ")";
                    ++checks;
                }
            }
        }
    }
    EXPECT_GT(checks, 1000) << "regression grid should cover >1000 cases";
}

// Spot-check specific conditions that exercise each rule in isolation.
// These make failure debugging easier than the grid tests above.

TEST(EngineRpmSchedule, Pw100_MachBandFloorMatches) {
    const auto& s = EngineRpmSchedule::builtin(2);
    // mach=1.0 in [0.84, 1.4] → floor at 1.0/1.4 = 0.714
    EXPECT_NEAR(s.apply(0.5, 1.0, 0.0, 0.0), 1.0/1.4, 1e-9);
}

TEST(EngineRpmSchedule, Pw100_AltFloorMatches) {
    const auto& s = EngineRpmSchedule::builtin(2);
    // alt=30000 (>10000) → floor at 30000/300000 + 0.7 = 0.8
    EXPECT_NEAR(s.apply(0.5, 0.0, 30000.0, 0.0), 0.8, 1e-9);
}

TEST(EngineRpmSchedule, Pw100_HighAltCeilingMatches) {
    const auto& s = EngineRpmSchedule::builtin(2);
    // alt=40000 in [35000, 45000] AND mach=0.6 in [0.4, 0.8] → ceiling at 1.025
    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.6, 40000.0, 0.0), 1.025);
}

TEST(EngineRpmSchedule, Pw100_Above55000CeilingMatches) {
    const auto& s = EngineRpmSchedule::builtin(2);
    // alt=60000 > 55000 → ceiling at 0.99
    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.9, 60000.0, 0.0), 0.99);
}

TEST(EngineRpmSchedule, Pw100_LowMachCeilingMatches) {
    const auto& s = EngineRpmSchedule::builtin(2);
    // mach=0.3 <= 0.4 → ceiling at 0.99
    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.3, 0.0, 0.0), 0.99);
}

TEST(EngineRpmSchedule, Pw229_LowMachFloorMatches) {
    const auto& s = EngineRpmSchedule::builtin(3);
    // mach=0.7 in (0.55, 1.1) → floor at 0.79
    EXPECT_DOUBLE_EQ(s.apply(0.5, 0.7, 0.0, 0.0), 0.79);
}

TEST(EngineRpmSchedule, Pw229_SupersonicFloorMatches) {
    const auto& s = EngineRpmSchedule::builtin(3);
    // mach=1.2 in [1.1, 1.4] → floor at 1.2/1.4
    EXPECT_NEAR(s.apply(0.5, 1.2, 0.0, 0.0), 1.2/1.4, 1e-9);
}

TEST(EngineRpmSchedule, Pw229_AbNoLightZoneMatches) {
    const auto& s = EngineRpmSchedule::builtin(3);
    // alt=55000 > 50000 AND vcas=200 < 250 → ceiling at 0.99
    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.9, 55000.0, 200.0), 0.99);
    // vcas=251 > 250 → rule does NOT fire, rpmCmd unchanged
    EXPECT_DOUBLE_EQ(s.apply(1.10, 0.9, 55000.0, 251.0), 1.10);
}

}  // namespace f4::data

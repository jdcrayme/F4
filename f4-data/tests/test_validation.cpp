#include "f4/data/aircraft_config.hpp"

#include <gtest/gtest.h>

using namespace f4::data;

// ============================================================================
// Helper: build a minimal valid config for tests that need a starting point.
// ============================================================================

namespace {

AircraftConfig makeValidConfig() {
    AircraftConfig c;
    c.geometry.emptyWeight  = f4::Quantity<f4::Pounds>(19900.0);
    c.geometry.area         = f4::Quantity<f4::SquareFeet>(300.0);
    c.geometry.internalFuel = f4::Quantity<f4::Pounds>(7162.0);
    c.geometry.maxFuel      = f4::Quantity<f4::Pounds>(7162.0);
    c.geometry.aoaMax       = f4::Quantity<f4::Degrees>(35.0).to<f4::Radians>();
    c.geometry.aoaMin       = f4::Quantity<f4::Degrees>(-8.0).to<f4::Radians>();
    c.geometry.betaMax      = f4::Quantity<f4::Degrees>(30.0).to<f4::Radians>();
    c.geometry.betaMin      = f4::Quantity<f4::Degrees>(-30.0).to<f4::Radians>();
    c.geometry.maxGs            = 9.0;
    c.geometry.maxRoll      = f4::Quantity<f4::Degrees>(190.0).to<f4::Radians>();
    c.geometry.minVcas      = f4::Quantity<f4::CASKnots>(250.0);
    c.geometry.maxVcas      = f4::Quantity<f4::CASKnots>(850.0);
    c.geometry.cornerVcas   = f4::Quantity<f4::CASKnots>(420.0);
    c.geometry.thetaMax     = f4::Quantity<f4::Radians>(0.6);
    c.geometry.cgLoc         = f4::Quantity<f4::Feet>(27.0);
    c.geometry.length        = f4::Quantity<f4::Feet>(47.0);
    c.geometry.span          = f4::Quantity<f4::Feet>(32.0);
    c.geometry.fusRadius     = f4::Quantity<f4::Feet>(2.5);
    c.geometry.tailHt        = f4::Quantity<f4::Feet>(4.5);

    c.aero.mach      = {0.0, 0.5, 1.0};
    c.aero.alpha_deg = {0.0, 5.0, 10.0, 15.0};
    c.aero.clift     = std::vector<double>(12, 0.5);  // 3*4
    c.aero.cdrag     = std::vector<double>(12, 0.05);
    c.aero.cy        = std::vector<double>(12, 0.0);

    c.engine.alt_ft      = {0.0, 10000.0, 30000.0, 50000.0};
    c.engine.mach        = {0.0, 0.4, 0.8, 1.2, 2.0};
    c.engine.thrust_idle = std::vector<double>(20, 1000.0);
    c.engine.thrust_mil  = std::vector<double>(20, 10000.0);
    c.engine.thrust_ab   = std::vector<double>(20, 20000.0);

    c.rollCmd.alpha_deg = {0.0, 10.0, 20.0};
    c.rollCmd.qbar      = {0.0, 100.0, 500.0, 1000.0};
    c.rollCmd.rollRate  = std::vector<double>(12, 30.0);

    c.aux.normSpoolRate        = 3.0;
    c.aux.abSpoolRate          = 10.0;
    c.aux.fuelFlowFactorNormal = 0.87;
    c.aux.nEngines             = 1;
    return c;
}

} // namespace

// ============================================================================
// Valid config passes validation
// ============================================================================

TEST(ValidationTest, ValidConfigPasses) {
    auto c = makeValidConfig();
    auto r = c.validate();
    EXPECT_TRUE(r.ok()) << r.format();
    EXPECT_EQ(r.errorCount(), 0u);
}

TEST(ValidationTest, EmptyConfigFails) {
    AircraftConfig c;
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
    EXPECT_GT(r.errorCount(), 0u);
}

// ============================================================================
// Aero table size mismatches
// ============================================================================

TEST(ValidationTest, AeroCliftSizeMismatchDetected) {
    auto c = makeValidConfig();
    c.aero.clift.push_back(99.0);  // wrong size now
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
    bool found = false;
    for (auto const& i : r.issues) {
        if (std::string(i.field).find("clift") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ValidationTest, EmptyAeroTablesDetected) {
    auto c = makeValidConfig();
    c.aero.clift.clear();
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

TEST(ValidationTest, NonAscendingMachBreakpointsWarns) {
    auto c = makeValidConfig();
    c.aero.mach = {0.0, 0.5, 0.3, 1.0};  // not ascending at index 2
    auto r = c.validate();
    bool found = false;
    for (auto const& i : r.issues) {
        if (std::string(i.field) == "aero.mach") found = true;
    }
    EXPECT_TRUE(found);
}

// ============================================================================
// Engine table validation
// ============================================================================

TEST(ValidationTest, EmptyEngineTablesDetected) {
    auto c = makeValidConfig();
    c.engine.thrust_idle.clear();
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

TEST(ValidationTest, ThrustTableSizeMismatchDetected) {
    auto c = makeValidConfig();
    c.engine.thrust_mil.push_back(99.0);
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

// ============================================================================
// Geometry validation
// ============================================================================

TEST(ValidationTest, NegativeEmptyWeightDetected) {
    auto c = makeValidConfig();
    c.geometry.emptyWeight = f4::Quantity<f4::Pounds>(-1000.0);
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

TEST(ValidationTest, AoaMaxLessThanAoaMinDetected) {
    auto c = makeValidConfig();
    c.geometry.aoaMax = f4::Quantity<f4::Degrees>(5.0).to<f4::Radians>();
    c.geometry.aoaMin = f4::Quantity<f4::Degrees>(10.0).to<f4::Radians>();
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

TEST(ValidationTest, MaxVcasNotGreaterThanMinVcasDetected) {
    auto c = makeValidConfig();
    c.geometry.maxVcas = f4::Quantity<f4::CASKnots>(200.0);
    c.geometry.minVcas = f4::Quantity<f4::CASKnots>(300.0);
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

TEST(ValidationTest, NaNInGeometryDetected) {
    auto c = makeValidConfig();
    c.geometry.emptyWeight = f4::Quantity<f4::Pounds>(std::numeric_limits<double>::quiet_NaN());
    auto r = c.validate();
    EXPECT_FALSE(r.ok());
}

TEST(ValidationTest, UnusualAoaMaxWarns) {
    auto c = makeValidConfig();
    c.geometry.aoaMax = f4::Quantity<f4::Degrees>(0.5).to<f4::Radians>();  // outside typical 1..90 range
    auto r = c.validate();
    EXPECT_TRUE(r.hasWarnings());
}

// ============================================================================
// Limiter evaluation
// ============================================================================

TEST(LimiterTest, LineTypeEvaluatesCorrectly) {
    Limiter l{LimiterType::Line, 0.0, 0.0, 10.0, 100.0, 0, 0};
    // Line from (0,0) to (10,100): y = 10*x
    EXPECT_NEAR(l.limit(5.0),  50.0, 1e-9);
    EXPECT_NEAR(l.limit(0.0),   0.0, 1e-9);
    EXPECT_NEAR(l.limit(10.0), 100.0, 1e-9);
}

TEST(LimiterTest, LineTypeClampsOutOfRange) {
    Limiter l{LimiterType::Line, 0.0, 0.0, 10.0, 100.0, 0, 0};
    EXPECT_NEAR(l.limit(-5.0),   0.0, 1e-9);  // clamps to y1
    EXPECT_NEAR(l.limit(15.0), 100.0, 1e-9);  // clamps to y2
}

TEST(LimiterTest, ValueTypeReturnsConstant) {
    Limiter l{LimiterType::Value, 42.0, 0, 0, 0, 0, 0};
    EXPECT_DOUBLE_EQ(l.limit(0.0), 42.0);
    EXPECT_DOUBLE_EQ(l.limit(99.0), 42.0);
}

TEST(LimiterTest, PercentTypeMultipliesInput) {
    Limiter l{LimiterType::Percent, 0.5, 0, 0, 0, 0, 0};
    EXPECT_NEAR(l.limit(10.0), 5.0, 1e-9);
    EXPECT_NEAR(l.limit(20.0), 10.0, 1e-9);
}

TEST(LimiterTest, ThreePointTypePiecewiseLinear) {
    // (0,0) - (5,10) - (10,5)
    Limiter l{LimiterType::ThreePoint, 0, 0, 0, 0, 0.0, 0.0};
    l.x0 = 0.0; l.y0 = 0.0;
    l.x1 = 5.0; l.y1 = 10.0;
    l.x2 = 10.0; l.y2 = 5.0;
    EXPECT_NEAR(l.limit(2.5), 5.0, 1e-9);   // first segment midpoint
    EXPECT_NEAR(l.limit(7.5), 7.5, 1e-9);  // second segment midpoint
}

TEST(LimiterTest, MinMaxTypeClamps) {
    Limiter l{LimiterType::MinMax, 0.0, 0.0, 10.0, 0.0, 0, 0};
    // MinMax: x1 = min, x2 = max
    l.x1 = 5.0; l.x2 = 15.0;
    EXPECT_NEAR(l.limit(0.0),  5.0, 1e-9);
    EXPECT_NEAR(l.limit(10.0), 10.0, 1e-9);
    EXPECT_NEAR(l.limit(20.0), 15.0, 1e-9);
}

// ============================================================================
// ConfigValidationReport formatting
// ============================================================================

TEST(ValidationReportTest, FormatProducesReadableString) {
    ConfigValidationReport r;
    r.issues.push_back({ConfigValidationReport::Severity::Error, "field1", "error message"});
    r.issues.push_back({ConfigValidationReport::Severity::Warning, "field2", "warning message"});
    std::string s = r.format();
    EXPECT_NE(s.find("E: [field1] error message"), std::string::npos);
    EXPECT_NE(s.find("W: [field2] warning message"), std::string::npos);
}

TEST(ValidationReportTest, OkAndWarningCountsAreCorrect) {
    ConfigValidationReport r;
    r.issues.push_back({ConfigValidationReport::Severity::Error,   "e1", ""});
    r.issues.push_back({ConfigValidationReport::Severity::Warning, "w1", ""});
    r.issues.push_back({ConfigValidationReport::Severity::Warning, "w2", ""});
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.errorCount(), 1u);
    EXPECT_EQ(r.warningCount(), 2u);
    EXPECT_TRUE(r.hasWarnings());
}

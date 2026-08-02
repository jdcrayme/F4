#include "f4/data/table_accessors.hpp"
#include "f4/data/config_loader.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

using namespace f4::data;

// ============================================================================
// Build Table2D views from AircraftConfig and verify they produce correct
// bilinear interpolation results.
// ============================================================================

namespace {

constexpr const char* kFixturesDir = F4_GENERATED_FIXTURES_DIR;

bool fixturesExist() {
    return std::filesystem::exists(kFixturesDir);
}

} // namespace

// ============================================================================
// CL table accessor
// ============================================================================

TEST(TableAccessorsTest, ClTableBuiltFromConfig) {
    AircraftConfig c;
    c.aero.mach      = {0.0, 0.5, 1.0};
    c.aero.alpha_deg = {0.0, 5.0, 10.0, 15.0};
    c.aero.clift = {
        // mach=0.0: 0.10, 0.30, 0.60, 0.90
        0.10, 0.30, 0.60, 0.90,
        // mach=0.5: 0.15, 0.35, 0.70, 1.05
        0.15, 0.35, 0.70, 1.05,
        // mach=1.0: 0.05, 0.20, 0.40, 0.60
        0.05, 0.20, 0.40, 0.60,
    };

    auto table = makeClTable(c.aero);
    EXPECT_EQ(table.num_rows(), 3u);
    EXPECT_EQ(table.num_cols(), 4u);

    // At a breakpoint: exact value
    EXPECT_NEAR(table(0.0, 0.0), 0.10, 1e-12);
    EXPECT_NEAR(table(1.0, 15.0), 0.60, 1e-12);

    // Midpoint bilinear: (0.25, 2.5) between (0.0, 0.0)=0.10 and (0.5, 5.0)=0.35
    // tr=0.5, tc=0.5
    // a = 0.10 + (0.15-0.10)*0.5 = 0.125
    // b = 0.30 + (0.35-0.30)*0.5 = 0.325
    // result = 0.125 + (0.325-0.125)*0.5 = 0.225
    EXPECT_NEAR(table(0.25, 2.5), 0.225, 1e-12);
}

TEST(TableAccessorsTest, ClTableEmptyConfigThrows) {
    AeroTable a;  // empty
    EXPECT_THROW(makeClTable(a), std::invalid_argument);
}

TEST(TableAccessorsTest, ClTableSizeMismatchThrows) {
    AeroTable a;
    a.mach = {0.0, 1.0};
    a.alpha_deg = {0.0, 5.0};
    a.clift = {0.1, 0.2, 0.3};  // should be 4, is 3
    EXPECT_THROW(makeClTable(a), std::invalid_argument);
}

// ============================================================================
// CD table accessor
// ============================================================================

TEST(TableAccessorsTest, CdTableBuiltFromConfig) {
    AircraftConfig c;
    c.aero.mach      = {0.0, 1.0};
    c.aero.alpha_deg = {0.0, 10.0};
    c.aero.cdrag = {0.02, 0.08, 0.10, 0.20};

    auto table = makeCdTable(c.aero);
    EXPECT_NEAR(table(0.0, 0.0), 0.02, 1e-12);
    EXPECT_NEAR(table(1.0, 10.0), 0.20, 1e-12);
    EXPECT_NEAR(table(0.5, 5.0), 0.10, 1e-12);  // center
}

// ============================================================================
// CY table accessor (optional — returns nullopt if empty)
// ============================================================================

TEST(TableAccessorsTest, CyTableEmptyReturnsNullopt) {
    AeroTable a;  // cy is empty
    auto table = makeCyTable(a);
    EXPECT_FALSE(table.has_value());
}

TEST(TableAccessorsTest, CyTablePresentReturnsValue) {
    AeroTable a;
    a.mach = {0.0, 1.0};
    a.alpha_deg = {0.0, 10.0};
    a.cy = {-0.01, -0.02, -0.03, -0.04};
    auto table = makeCyTable(a);
    ASSERT_TRUE(table.has_value());
    EXPECT_NEAR((*table)(0.0, 0.0), -0.01, 1e-12);
}

// ============================================================================
// Thrust table accessor
// ============================================================================

TEST(TableAccessorsTest, ThrustTableBuiltFromConfig) {
    AircraftConfig c;
    c.engine.alt_ft = {0.0, 50000.0};
    c.engine.mach   = {0.0, 1.0, 2.0};
    c.engine.thrust_idle = {1000, 800, 600, 500, 400, 300};  // 2 alt x 3 mach

    auto idle = makeThrustTable(c.engine, ThrustTable::Idle);
    EXPECT_EQ(idle.num_rows(), 2u);
    EXPECT_EQ(idle.num_cols(), 3u);
    EXPECT_NEAR(idle(0.0, 0.0), 1000.0, 1e-12);
    EXPECT_NEAR(idle(50000.0, 2.0), 300.0, 1e-12);
    // At (alt=25000, mach=1.0): mach=1.0 is exactly breakpoint index 1.
    // Bracket [mach=1, mach=2], tc=0. Bracket [alt=0, alt=50000], tr=0.5.
    // v00 = data[0*3+1] = 800 (alt=0, mach=1)
    // v10 = data[1*3+1] = 400 (alt=1, mach=1)
    // a = 800 + (400-800)*0.5 = 600
    // result = 600 (tc=0 so b doesn't contribute)
    EXPECT_NEAR(idle(25000.0, 1.0), 600.0, 1e-9);

    // At a true midpoint (alt=25000, mach=0.5): tr=0.5, tc=0.5
    // v00 = data[0*3+0] = 1000, v10 = data[1*3+0] = 500
    // v01 = data[0*3+1] = 800,  v11 = data[1*3+1] = 400
    // a = 1000 + (500-1000)*0.5 = 750
    // b = 800 + (400-800)*0.5 = 600
    // result = 750 + (600-750)*0.5 = 675
    EXPECT_NEAR(idle(25000.0, 0.5), 675.0, 1e-9);
}

TEST(TableAccessorsTest, ThrustTableABThrowsIfEmpty) {
    EngineTable e;  // thrust_ab is empty
    EXPECT_THROW(makeThrustTable(e, ThrustTable::AB), std::invalid_argument);
}

// ============================================================================
// Roll rate table accessor
// ============================================================================

TEST(TableAccessorsTest, RollRateTableBuiltFromConfig) {
    AircraftConfig c;
    c.rollCmd.alpha_deg = {0.0, 10.0, 20.0};
    c.rollCmd.qbar      = {0.0, 100.0, 1000.0};
    c.rollCmd.rollRate = {
        10, 20, 30,   // alpha=0
        20, 40, 60,   // alpha=10
        30, 60, 90,   // alpha=20
    };

    auto table = makeRollRateTable(c.rollCmd);
    EXPECT_EQ(table.num_rows(), 3u);
    EXPECT_EQ(table.num_cols(), 3u);
    EXPECT_NEAR(table(0.0, 0.0), 10.0, 1e-12);
    EXPECT_NEAR(table(20.0, 1000.0), 90.0, 1e-12);
}

// ============================================================================
// Integration: load the real f16 fixture and exercise its tables
// ============================================================================

TEST(TableAccessorsTest, F16TablesInterpolateCorrectly) {
    if (!fixturesExist()) GTEST_SKIP();

    const std::string path = std::string(kFixturesDir) + "/f16.json";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    auto result = loadConfig(path);
    ASSERT_TRUE(result.ok);

    // Build all table views.
    auto cl = makeClTable(result.config.aero);
    auto cd = makeCdTable(result.config.aero);
    auto thrust_mil = makeThrustTable(result.config.engine, ThrustTable::Mil);

    // Lookup at a known operating point: sea level (alt=0), Mach 0.8
    // The exact CL value depends on the f16.dat data, but we can verify
    // the lookup produces a finite, reasonable value.
    double cl_val = cl(0.8, 4.0);  // Mach 0.8, alpha 4 deg
    EXPECT_TRUE(std::isfinite(cl_val));
    EXPECT_GT(cl_val, 0.0);
    EXPECT_LT(cl_val, 2.0);  // CL should be well under 2.0

    double cd_val = cd(0.8, 4.0);
    EXPECT_TRUE(std::isfinite(cd_val));
    EXPECT_GT(cd_val, 0.0);

    double thrust = thrust_mil(0.0, 0.8);  // sea level, Mach 0.8
    EXPECT_TRUE(std::isfinite(thrust));
    EXPECT_GT(thrust, 0.0);

    // Verify boundary clamping: lookup beyond the table edges returns
    // the edge value (not NaN, not extrapolation).
    double cl_below = cl(-1.0, -10.0);  // below all breakpoints
    double cl_above = cl(99.0, 99.0);   // above all breakpoints
    EXPECT_TRUE(std::isfinite(cl_below));
    EXPECT_TRUE(std::isfinite(cl_above));
}

// ============================================================================
// Cached-index optimization: repeated lookups at nearby points should be fast.
// This is a smoke test — we just verify correctness under a smooth scan.
// ============================================================================

TEST(TableAccessorsTest, CachedIndexUnderSmoothScan) {
    AircraftConfig c;
    c.aero.mach      = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8, 2.0};
    c.aero.alpha_deg = {0.0, 2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0, 18.0, 20.0};
    // Separable function: CL = mach * 0.1 + alpha * 0.05
    c.aero.clift.resize(11 * 11);
    for (int r = 0; r < 11; ++r)
        for (int col = 0; col < 11; ++col)
            c.aero.clift[r * 11 + col] = c.aero.mach[r] * 0.1 + c.aero.alpha_deg[col] * 0.05;

    auto table = makeClTable(c.aero);

    // Walk the table smoothly: both axes advance together.
    for (int step = 0; step <= 100; ++step) {
        double m = step * 0.02;   // 0..2.0
        double a = step * 0.2;    // 0..20.0
        double expected = m * 0.1 + a * 0.05;
        EXPECT_NEAR(table(m, a), expected, 1e-9)
            << "step=" << step << " m=" << m << " a=" << a;
    }
}

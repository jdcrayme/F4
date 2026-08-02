#include <f4/math/table.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace f4::math;

// ============================================================================
// Table1D — construction & validation
// ============================================================================

TEST(Table1DTest, ConstructsFromEqualSizedVectors) {
    Table1D<double, double> t({0.0, 1.0, 2.0}, {10.0, 20.0, 30.0});
    EXPECT_EQ(t.size(), 3u);
}

TEST(Table1DTest, RejectsSizeMismatch) {
    EXPECT_THROW((Table1D<double, double>({0.0, 1.0, 2.0}, {10.0, 20.0})),
                 std::invalid_argument);
}

TEST(Table1DTest, RejectsTooFewBreakpoints) {
    EXPECT_THROW((Table1D<double, double>({1.0}, {10.0})),
                 std::invalid_argument);
}

TEST(Table1DTest, RejectsUnsortedBreakpoints) {
    EXPECT_THROW((Table1D<double, double>({0.0, 2.0, 1.0}, {10.0, 20.0, 30.0})),
                 std::invalid_argument);
}

// ============================================================================
// Table1D — interpolation correctness
// ============================================================================

TEST(Table1DTest, LookupAtBreakpointReturnsExactValue) {
    Table1D<double, double> t({0.0, 1.0, 2.0}, {10.0, 20.0, 30.0});
    EXPECT_NEAR(t(0.0), 10.0, 1e-12);
    EXPECT_NEAR(t(1.0), 20.0, 1e-12);
    EXPECT_NEAR(t(2.0), 30.0, 1e-12);
}

TEST(Table1DTest, LookupMidpointIsBilinear) {
    Table1D<double, double> t({0.0, 1.0, 2.0}, {10.0, 20.0, 30.0});
    EXPECT_NEAR(t(0.5), 15.0, 1e-12);
    EXPECT_NEAR(t(1.5), 25.0, 1e-12);
}

TEST(Table1DTest, LookupAtQuarterPoint) {
    Table1D<double, double> t({0.0, 1.0}, {0.0, 100.0});
    EXPECT_NEAR(t(0.25), 25.0, 1e-12);
    EXPECT_NEAR(t(0.75), 75.0, 1e-12);
}

TEST(Table1DTest, NonMonotonicValuesAreAllowed) {
    // The *breakpoints* must be sorted, but the *values* need not be.
    Table1D<double, double> t({0.0, 1.0, 2.0, 3.0}, {0.0, 10.0, 5.0, 15.0});
    EXPECT_NEAR(t(1.5), 7.5, 1e-12);
}

// ============================================================================
// Table1D — boundary modes
// ============================================================================

TEST(Table1DTest, ClampModeBelowReturnsFirst) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Clamp);
    EXPECT_NEAR(t(-5.0), 10.0, 1e-12);
}

TEST(Table1DTest, ClampModeAboveReturnsLast) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Clamp);
    EXPECT_NEAR(t(5.0), 20.0, 1e-12);
}

TEST(Table1DTest, ErrorModeBelowThrows) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Error);
    EXPECT_THROW(t(-0.001), std::out_of_range);
}

TEST(Table1DTest, ErrorModeAboveThrows) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Error);
    EXPECT_THROW(t(1.001), std::out_of_range);
}

TEST(Table1DTest, ErrorModeAtBoundaryDoesNotThrow) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Error);
    EXPECT_NO_THROW(t(0.0));
    EXPECT_NO_THROW(t(1.0));
}

TEST(Table1DTest, ExtrapolateModeBelowExtendsLinearly) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Extrapolate);
    // slope is 10/unit, so at x=-1, y should be 0
    EXPECT_NEAR(t(-1.0), 0.0, 1e-12);
    EXPECT_NEAR(t(-2.0), -10.0, 1e-12);
}

TEST(Table1DTest, ExtrapolateModeAboveExtendsLinearly) {
    Table1D<double, double> t({0.0, 1.0}, {10.0, 20.0}, BoundaryMode::Extrapolate);
    EXPECT_NEAR(t(2.0), 30.0, 1e-12);
    EXPECT_NEAR(t(3.0), 40.0, 1e-12);
}

// ============================================================================
// Table1D — cached-index optimization (smooth-frame access pattern)
// ============================================================================

TEST(Table1DTest, CachedIndexHandlesForwardScan) {
    // Walk the table forward step-by-step; cached index should make every
    // lookup O(1). Verify correctness along the way.
    std::vector<double> x;
    std::vector<double> y;
    for (int i = 0; i <= 100; ++i) {
        x.push_back(i * 0.1);
        y.push_back(i * 0.1 * 2.0);  // y = 2x
    }
    Table1D<double, double> t(x, y);
    for (int i = 0; i <= 1000; ++i) {
        double q = i * 0.01;
        EXPECT_NEAR(t(q), 2.0 * q, 1e-9) << "q=" << q;
    }
}

TEST(Table1DTest, CachedIndexHandlesBackwardScan) {
    std::vector<double> x;
    std::vector<double> y;
    for (int i = 0; i <= 100; ++i) {
        x.push_back(i * 0.1);
        y.push_back(i * 0.1 * 2.0);
    }
    Table1D<double, double> t(x, y);
    for (int i = 1000; i >= 0; --i) {
        double q = i * 0.01;
        EXPECT_NEAR(t(q), 2.0 * q, 1e-9) << "q=" << q;
    }
}

TEST(Table1DTest, CachedIndexHandlesRandomJump) {
    // Random jumps should still produce correct results; the cached index
    // only affects performance, not correctness.
    Table1D<double, double> t({0.0, 1.0, 2.0, 3.0, 4.0, 5.0},
                              {0.0, 10.0, 20.0, 30.0, 40.0, 50.0});
    EXPECT_NEAR(t(0.5), 5.0, 1e-12);
    EXPECT_NEAR(t(4.5), 45.0, 1e-12);
    EXPECT_NEAR(t(2.5), 25.0, 1e-12);
    EXPECT_NEAR(t(1.5), 15.0, 1e-12);
}

// ============================================================================
// Table1D — float & int element types
// ============================================================================

TEST(Table1DTest, FloatBreakpointsAndValues) {
    Table1D<float, float> t({0.0f, 1.0f, 2.0f}, {10.0f, 20.0f, 30.0f});
    EXPECT_NEAR(t(0.5f), 15.0f, 1e-5f);
}

TEST(Table1DTest, IntegerBreakpoints) {
    Table1D<int, double> t({0, 10, 20}, {0.0, 100.0, 200.0});
    EXPECT_NEAR(t(5), 50.0, 1e-12);
    EXPECT_NEAR(t(15), 150.0, 1e-12);
}

// ============================================================================
// Table2D — construction & validation
// ============================================================================

TEST(Table2DTest, ConstructsFromNestedVectors) {
    Table2D<double, double, double> t(
        {0.0, 1.0, 2.0},                          // rows
        {0.0, 1.0},                                // cols
        {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}});     // 3x2 data
    EXPECT_EQ(t.num_rows(), 3u);
    EXPECT_EQ(t.num_cols(), 2u);
}

TEST(Table2DTest, RejectsRowCountMismatch) {
    EXPECT_THROW((Table2D<double, double, double>(
            {0.0, 1.0, 2.0}, {0.0, 1.0},
            {{1.0, 2.0}, {3.0, 4.0}})),  // only 2 rows, expected 3
                 std::invalid_argument);
}

TEST(Table2DTest, RejectsColCountMismatch) {
    EXPECT_THROW((Table2D<double, double, double>(
            {0.0, 1.0}, {0.0, 1.0, 2.0},
            {{1.0, 2.0}, {3.0, 4.0}})),  // each row has 2, expected 3
                 std::invalid_argument);
}

TEST(Table2DTest, RejectsUnsortedRows) {
    EXPECT_THROW((Table2D<double, double, double>(
            {0.0, 2.0, 1.0}, {0.0, 1.0},
            {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}})),
                 std::invalid_argument);
}

TEST(Table2DTest, ConstructsFromFlatData) {
    using FlatTag = Table2D<double, double, double>::FlatDataTag;
    Table2D<double, double, double> t(
        {0.0, 1.0, 2.0}, {0.0, 1.0},
        {1.0, 2.0, 3.0, 4.0, 5.0, 6.0},
        FlatTag{}, BoundaryMode::Clamp);
    EXPECT_EQ(t.num_rows(), 3u);
    EXPECT_EQ(t.num_cols(), 2u);
    EXPECT_NEAR(t(0.0, 0.0), 1.0, 1e-12);
    EXPECT_NEAR(t(2.0, 1.0), 6.0, 1e-12);
}

// ============================================================================
// Table2D — bilinear interpolation correctness
// ============================================================================

TEST(Table2DTest, LookupAtCornerReturnsCornerValue) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{1.0, 2.0}, {3.0, 4.0}});
    EXPECT_NEAR(t(0.0, 0.0), 1.0, 1e-12);
    EXPECT_NEAR(t(0.0, 1.0), 2.0, 1e-12);
    EXPECT_NEAR(t(1.0, 0.0), 3.0, 1e-12);
    EXPECT_NEAR(t(1.0, 1.0), 4.0, 1e-12);
}

TEST(Table2DTest, LookupAtCenterIsAverageOfFourCorners) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{1.0, 2.0}, {3.0, 4.0}});
    EXPECT_NEAR(t(0.5, 0.5), 2.5, 1e-12);  // (1+2+3+4)/4
}

TEST(Table2DTest, LookupAtMidpointOfEdge) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{1.0, 2.0}, {3.0, 4.0}});
    EXPECT_NEAR(t(0.5, 0.0), 2.0, 1e-12);  // (1+3)/2
    EXPECT_NEAR(t(0.5, 1.0), 3.0, 1e-12);  // (2+4)/2
    EXPECT_NEAR(t(0.0, 0.5), 1.5, 1e-12);  // (1+2)/2
    EXPECT_NEAR(t(1.0, 0.5), 3.5, 1e-12);  // (3+4)/2
}

TEST(Table2DTest, LookupOffCenter) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{0.0, 10.0}, {100.0, 1000.0}});
    // At (0.25, 0.75): the bilinear blend of (0,0)=0, (0,1)=10, (1,0)=100, (1,1)=1000
    // tr=0.25, tc=0.75
    // a = 0  + (100  - 0  ) * 0.25 = 25
    // b = 10 + (1000 - 10) * 0.25 = 257.5
    // result = 25 + (257.5 - 25) * 0.75 = 25 + 174.375 = 199.375
    EXPECT_NEAR(t(0.25, 0.75), 199.375, 1e-9);
}

TEST(Table2DTest, BilinearCorrectnessOnRealAeroTable) {
    // Synthetic CL(Mach, alpha) table — small but realistic shape.
    std::vector<double> mach  = {0.0, 0.5, 1.0};
    std::vector<double> alpha = {0.0, 5.0, 10.0};
    std::vector<std::vector<double>> cl = {
        // alpha=0   5    10
        { 0.00, 0.30, 0.60 },  // M=0.0
        { 0.00, 0.35, 0.70 },  // M=0.5
        { 0.00, 0.20, 0.40 },  // M=1.0 (transonic drop)
    };
    Table2D<double, double, double> t(mach, alpha, cl);

    // At M=0.25, alpha=2.5:
    // bracket [M=0, M=0.5] tr=0.5; bracket [a=0, a=5] tc=0.5
    // v00=0.00, v10=0.00, v01=0.30, v11=0.35
    // a = 0 + (0-0)*0.5 = 0
    // b = 0.30 + (0.35-0.30)*0.5 = 0.325
    // result = 0 + (0.325-0)*0.5 = 0.1625
    EXPECT_NEAR(t(0.25, 2.5), 0.1625, 1e-12);
}

// ============================================================================
// Table2D — boundary modes
// ============================================================================

TEST(Table2DTest, ClampModeReturnsCornerWhenOffAnyEdge) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{1.0, 2.0}, {3.0, 4.0}},
        BoundaryMode::Clamp);
    EXPECT_NEAR(t(-1.0, -1.0), 1.0, 1e-12);
    EXPECT_NEAR(t(2.0, 2.0), 4.0, 1e-12);
    EXPECT_NEAR(t(-1.0, 2.0), 2.0, 1e-12);
    EXPECT_NEAR(t(2.0, -1.0), 3.0, 1e-12);
}

TEST(Table2DTest, ErrorModeThrowsWhenOffEdge) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{1.0, 2.0}, {3.0, 4.0}},
        BoundaryMode::Error);
    EXPECT_THROW(t(-0.001, 0.5), std::out_of_range);
    EXPECT_THROW(t(1.001, 0.5), std::out_of_range);
    EXPECT_THROW(t(0.5, -0.001), std::out_of_range);
    EXPECT_THROW(t(0.5, 1.001), std::out_of_range);
    EXPECT_NO_THROW(t(0.0, 0.0));
    EXPECT_NO_THROW(t(1.0, 1.0));
}

TEST(Table2DTest, ExtrapolateModeExtendsSlope) {
    Table2D<double, double, double> t(
        {0.0, 1.0}, {0.0, 1.0},
        {{1.0, 2.0}, {3.0, 4.0}},
        BoundaryMode::Extrapolate);
    // At (-1, 0): slope in row direction is (3-1)/(1-0) = 2 per unit
    //            extrapolate: 1 + 2*(-1) = -1
    EXPECT_NEAR(t(-1.0, 0.0), -1.0, 1e-12);
    // At (2, 0): 1 + 2*2 = 5
    EXPECT_NEAR(t(2.0, 0.0), 5.0, 1e-12);
}

// ============================================================================
// Table2D — single-point degenerate cases
// ============================================================================

TEST(Table2DTest, SingleRowTableBehavesLikeTable1D) {
    Table2D<double, double, double> t(
        {0.0}, {0.0, 1.0, 2.0},
        {{10.0, 20.0, 30.0}});
    EXPECT_NEAR(t(0.0, 1.0), 20.0, 1e-12);
    EXPECT_NEAR(t(0.0, 0.5), 15.0, 1e-12);
}

TEST(Table2DTest, SinglePointTableReturnsThePoint) {
    Table2D<double, double, double> t(
        {0.0}, {0.0},
        {{42.0}});
    EXPECT_NEAR(t(0.0, 0.0), 42.0, 1e-12);
}

// ============================================================================
// Table2D — cached-index optimization under smooth access
// ============================================================================

TEST(Table2DTest, CachedIndexUnderSmoothScan) {
    std::vector<double> mach;
    std::vector<double> alpha;
    for (int i = 0; i <= 20; ++i) mach.push_back(i * 0.1);     // 0..2.0
    for (int i = 0; i <= 20; ++i) alpha.push_back(i * 1.0);    // 0..20
    std::vector<std::vector<double>> cl(21, std::vector<double>(21));
    for (int r = 0; r < 21; ++r)
        for (int c = 0; c < 21; ++c)
            cl[r][c] = mach[r] * 10.0 + alpha[c] * 0.1;  // separable function

    Table2D<double, double, double> t(mach, alpha, cl);

    // Walk the table smoothly: both axes advance together.
    for (int step = 0; step <= 100; ++step) {
        double m = step * 0.02;          // 0..2.0
        double a = step * 0.2;           // 0..20
        double expected = m * 10.0 + a * 0.1;
        EXPECT_NEAR(t(m, a), expected, 1e-9) << "step=" << step;
    }
}

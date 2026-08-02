#include <f4/math/vec3.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::math;

// ============================================================================
// Construction and element access
// ============================================================================

TEST(Vec3Test, DefaultConstructIsZero) {
    Vec3d v;
    EXPECT_DOUBLE_EQ(v.x, 0.0);
    EXPECT_DOUBLE_EQ(v.y, 0.0);
    EXPECT_DOUBLE_EQ(v.z, 0.0);
}

TEST(Vec3Test, BraceInit) {
    Vec3d v{1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(v.x, 1.0);
    EXPECT_DOUBLE_EQ(v.y, 2.0);
    EXPECT_DOUBLE_EQ(v.z, 3.0);
}

TEST(Vec3Test, IndexAccess) {
    Vec3d v{10.0, 20.0, 30.0};
    EXPECT_DOUBLE_EQ(v[0], 10.0);
    EXPECT_DOUBLE_EQ(v[1], 20.0);
    EXPECT_DOUBLE_EQ(v[2], 30.0);
    v[1] = 99.0;
    EXPECT_DOUBLE_EQ(v.y, 99.0);
}

TEST(Vec3Test, FloatSpecialization) {
    Vec3f v{1.0f, 2.0f, 3.0f};
    EXPECT_FLOAT_EQ(v.x, 1.0f);
    EXPECT_FLOAT_EQ(v.length(), std::sqrt(14.0f));
}

// ============================================================================
// Arithmetic
// ============================================================================

TEST(Vec3Test, Addition) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b{4.0, 5.0, 6.0};
    Vec3d c = a + b;
    EXPECT_DOUBLE_EQ(c.x, 5.0);
    EXPECT_DOUBLE_EQ(c.y, 7.0);
    EXPECT_DOUBLE_EQ(c.z, 9.0);
}

TEST(Vec3Test, Subtraction) {
    Vec3d a{4.0, 5.0, 6.0};
    Vec3d b{1.0, 2.0, 3.0};
    Vec3d c = a - b;
    EXPECT_DOUBLE_EQ(c.x, 3.0);
    EXPECT_DOUBLE_EQ(c.y, 3.0);
    EXPECT_DOUBLE_EQ(c.z, 3.0);
}

TEST(Vec3Test, Negation) {
    Vec3d a{1.0, -2.0, 3.0};
    Vec3d b = -a;
    EXPECT_DOUBLE_EQ(b.x, -1.0);
    EXPECT_DOUBLE_EQ(b.y,  2.0);
    EXPECT_DOUBLE_EQ(b.z, -3.0);
}

TEST(Vec3Test, InPlaceAdd) {
    Vec3d a{1.0, 2.0, 3.0};
    a += Vec3d{10.0, 20.0, 30.0};
    EXPECT_DOUBLE_EQ(a.x, 11.0);
    EXPECT_DOUBLE_EQ(a.y, 22.0);
    EXPECT_DOUBLE_EQ(a.z, 33.0);
}

TEST(Vec3Test, ScalarMultiply) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b = a * 2.0;
    EXPECT_DOUBLE_EQ(b.x, 2.0);
    EXPECT_DOUBLE_EQ(b.y, 4.0);
    EXPECT_DOUBLE_EQ(b.z, 6.0);
}

TEST(Vec3Test, ScalarMultiplyCommutative) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b = 2.0 * a;
    EXPECT_DOUBLE_EQ(b.x, 2.0);
    EXPECT_DOUBLE_EQ(b.y, 4.0);
    EXPECT_DOUBLE_EQ(b.z, 6.0);
}

TEST(Vec3Test, ScalarDivide) {
    Vec3d a{2.0, 4.0, 6.0};
    Vec3d b = a / 2.0;
    EXPECT_DOUBLE_EQ(b.x, 1.0);
    EXPECT_DOUBLE_EQ(b.y, 2.0);
    EXPECT_DOUBLE_EQ(b.z, 3.0);
}

// ============================================================================
// Products
// ============================================================================

TEST(Vec3Test, DotProduct) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b{4.0, 5.0, 6.0};
    EXPECT_DOUBLE_EQ(a.dot(b), 32.0);  // 4 + 10 + 18
}

TEST(Vec3Test, DotProductOrthogonal) {
    Vec3d a{1.0, 0.0, 0.0};
    Vec3d b{0.0, 1.0, 0.0};
    EXPECT_DOUBLE_EQ(a.dot(b), 0.0);
}

TEST(Vec3Test, CrossProduct) {
    Vec3d a{1.0, 0.0, 0.0};
    Vec3d b{0.0, 1.0, 0.0};
    Vec3d c = a.cross(b);
    EXPECT_DOUBLE_EQ(c.x, 0.0);
    EXPECT_DOUBLE_EQ(c.y, 0.0);
    EXPECT_DOUBLE_EQ(c.z, 1.0);
}

TEST(Vec3Test, CrossProductAntiCommutes) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b{4.0, 5.0, 6.0};
    Vec3d ab = a.cross(b);
    Vec3d ba = b.cross(a);
    EXPECT_DOUBLE_EQ(ab.x, -ba.x);
    EXPECT_DOUBLE_EQ(ab.y, -ba.y);
    EXPECT_DOUBLE_EQ(ab.z, -ba.z);
}

TEST(Vec3Test, CrossProductRightHandRule) {
    Vec3d i{1.0, 0.0, 0.0};
    Vec3d j{0.0, 1.0, 0.0};
    Vec3d k{0.0, 0.0, 1.0};
    EXPECT_EQ(i.cross(j), k);
    EXPECT_EQ(j.cross(k), i);
    EXPECT_EQ(k.cross(i), j);
}

TEST(Vec3Test, HadamardProduct) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b{4.0, 5.0, 6.0};
    Vec3d c = a.hadamard(b);
    EXPECT_DOUBLE_EQ(c.x, 4.0);
    EXPECT_DOUBLE_EQ(c.y, 10.0);
    EXPECT_DOUBLE_EQ(c.z, 18.0);
}

// ============================================================================
// Length and normalization
// ============================================================================

TEST(Vec3Test, LengthSquared) {
    Vec3d a{3.0, 4.0, 0.0};
    EXPECT_DOUBLE_EQ(a.length_squared(), 25.0);
}

TEST(Vec3Test, Length) {
    Vec3d a{3.0, 4.0, 0.0};
    EXPECT_DOUBLE_EQ(a.length(), 5.0);
}

TEST(Vec3Test, LengthOfZeroIsZero) {
    Vec3d a{0.0, 0.0, 0.0};
    EXPECT_DOUBLE_EQ(a.length(), 0.0);
}

TEST(Vec3Test, NormalizeUnitVector) {
    Vec3d a{3.0, 4.0, 0.0};
    Vec3d n = a.normalized();
    EXPECT_NEAR(n.length(), 1.0, 1e-12);
    EXPECT_NEAR(n.x, 0.6, 1e-12);
    EXPECT_NEAR(n.y, 0.8, 1e-12);
    EXPECT_NEAR(n.z, 0.0, 1e-12);
}

TEST(Vec3Test, NormalizeZeroVectorReturnsZero) {
    // Degenerate case: normalize of zero vector returns the zero vector
    // (no NaN propagation).
    Vec3d a{0.0, 0.0, 0.0};
    Vec3d n = a.normalized();
    EXPECT_DOUBLE_EQ(n.x, 0.0);
    EXPECT_DOUBLE_EQ(n.y, 0.0);
    EXPECT_DOUBLE_EQ(n.z, 0.0);
}

TEST(Vec3Test, NormalizeDoesNotMutateOriginal) {
    Vec3d a{3.0, 4.0, 0.0};
    [[maybe_unused]] Vec3d n = a.normalized();
    EXPECT_DOUBLE_EQ(a.x, 3.0);
    EXPECT_DOUBLE_EQ(a.y, 4.0);
}

// ============================================================================
// Comparison
// ============================================================================

TEST(Vec3Test, Equality) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b{1.0, 2.0, 3.0};
    Vec3d c{1.0, 2.0, 4.0};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// ============================================================================
// Free-function aliases
// ============================================================================

TEST(Vec3Test, FreeFunctionDot) {
    Vec3d a{1.0, 2.0, 3.0};
    Vec3d b{4.0, 5.0, 6.0};
    EXPECT_DOUBLE_EQ(dot(a, b), 32.0);
}

TEST(Vec3Test, FreeFunctionCross) {
    Vec3d a{1.0, 0.0, 0.0};
    Vec3d b{0.0, 1.0, 0.0};
    EXPECT_EQ(cross(a, b), (Vec3d{0.0, 0.0, 1.0}));
}

TEST(Vec3Test, FreeFunctionLength) {
    Vec3d a{3.0, 4.0, 0.0};
    EXPECT_DOUBLE_EQ(length(a), 5.0);
}

TEST(Vec3Test, FreeFunctionNormalize) {
    Vec3d a{3.0, 4.0, 0.0};
    EXPECT_NEAR(length(normalize(a)), 1.0, 1e-12);
}

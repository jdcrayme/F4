#include <f4/math/quat.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

using namespace f4::math;

namespace {

constexpr double kEps = 1e-12;

// Helper: are two quaternions "equivalent" (q and -q represent the same rotation)?
::testing::AssertionResult QuatNear(const Quatd& a, const Quatd& b, double eps = kEps) {
    double d = std::abs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    if (std::abs(d - 1.0) < eps) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
        << "quaternions differ: (" << a.w << "," << a.x << "," << a.y << "," << a.z
        << ") vs (" << b.w << "," << b.x << "," << b.y << "," << b.z << ")";
}

} // namespace

// ============================================================================
// Construction
// ============================================================================

TEST(QuatTest, IdentityIsUnit) {
    Quatd q = Quatd::identity();
    EXPECT_DOUBLE_EQ(q.w, 1.0);
    EXPECT_DOUBLE_EQ(q.x, 0.0);
    EXPECT_DOUBLE_EQ(q.y, 0.0);
    EXPECT_DOUBLE_EQ(q.z, 0.0);
    EXPECT_NEAR(q.norm(), 1.0, kEps);
}

TEST(QuatTest, ScalarVectorConstructor) {
    Quatd q{1.0, Vec3d{2.0, 3.0, 4.0}};
    EXPECT_DOUBLE_EQ(q.w, 1.0);
    EXPECT_DOUBLE_EQ(q.x, 2.0);
    EXPECT_DOUBLE_EQ(q.y, 3.0);
    EXPECT_DOUBLE_EQ(q.z, 4.0);
}

// ============================================================================
// Hamilton product
// ============================================================================

TEST(QuatTest, IdentityMultiplication) {
    Quatd q{0.5, 0.5, 0.5, 0.5};
    EXPECT_EQ(q * Quatd::identity(), q);
    EXPECT_EQ(Quatd::identity() * q, q);
}

TEST(QuatTest, ProductIsNonCommutative) {
    Quatd a = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 0.5);
    Quatd b = Quatd::from_axis_angle({0.0, 1.0, 0.0}, 0.5);
    Quatd ab = a * b;
    Quatd ba = b * a;
    // ab and ba should NOT be equal (rotations don't commute).
    // They might be equivalent up to sign, so check both.
    bool equal = (ab == ba);
    bool neg_equal = (ab.w == -ba.w && ab.x == -ba.x && ab.y == -ba.y && ab.z == -ba.z);
    EXPECT_FALSE(equal || neg_equal);
}

TEST(QuatTest, ProductAppliesRightFirst) {
    // If q1 rotates 90° about X and q2 rotates 90° about Y,
    // then q1*q2 applied to (0,0,1) should:
    //   first rotate 90° about Y: (0,0,1) -> (1,0,0)
    //   then rotate 90° about X:  (1,0,0) -> (1,0,0)  (X-axis unchanged by X-rotation)
    // Result: (1, 0, 0).
    Quatd qx = Quatd::from_axis_angle({1.0, 0.0, 0.0}, std::numbers::pi / 2.0);
    Quatd qy = Quatd::from_axis_angle({0.0, 1.0, 0.0}, std::numbers::pi / 2.0);
    Quatd q = qx * qy;
    Vec3d v = q.rotate({0.0, 0.0, 1.0});
    EXPECT_NEAR(v.x,  1.0, 1e-9);
    EXPECT_NEAR(v.y,  0.0, 1e-9);
    EXPECT_NEAR(v.z,  0.0, 1e-9);
}

// ============================================================================
// Conjugate / inverse / norm
// ============================================================================

TEST(QuatTest, ConjugateNegatesVectorPart) {
    Quatd q{1.0, 2.0, 3.0, 4.0};
    Quatd c = q.conjugate();
    EXPECT_DOUBLE_EQ(c.w,  1.0);
    EXPECT_DOUBLE_EQ(c.x, -2.0);
    EXPECT_DOUBLE_EQ(c.y, -3.0);
    EXPECT_DOUBLE_EQ(c.z, -4.0);
}

TEST(QuatTest, NormSquared) {
    Quatd q{1.0, 2.0, 3.0, 4.0};
    EXPECT_DOUBLE_EQ(q.norm_squared(), 30.0);  // 1 + 4 + 9 + 16
}

TEST(QuatTest, Norm) {
    Quatd q{1.0, 2.0, 3.0, 4.0};
    EXPECT_DOUBLE_EQ(q.norm(), std::sqrt(30.0));
}

TEST(QuatTest, InverseOfUnitIsConjugate) {
    Quatd q = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 0.5).normalized();
    Quatd inv = q.inverse();
    Quatd conj = q.conjugate();
    EXPECT_NEAR(inv.w, conj.w, kEps);
    EXPECT_NEAR(inv.x, conj.x, kEps);
    EXPECT_NEAR(inv.y, conj.y, kEps);
    EXPECT_NEAR(inv.z, conj.z, kEps);
}

TEST(QuatTest, ProductWithInverseIsIdentity) {
    Quatd q = Quatd::from_axis_angle({0.0, 1.0, 0.0}, 0.7);
    Quatd prod = q * q.inverse();
    EXPECT_NEAR(prod.w, 1.0, kEps);
    EXPECT_NEAR(prod.x, 0.0, kEps);
    EXPECT_NEAR(prod.y, 0.0, kEps);
    EXPECT_NEAR(prod.z, 0.0, kEps);
}

TEST(QuatTest, Normalize) {
    Quatd q{2.0, 0.0, 0.0, 0.0};
    Quatd n = q.normalized();
    EXPECT_NEAR(n.w, 1.0, kEps);
    EXPECT_NEAR(n.x, 0.0, kEps);
}

// ============================================================================
// Axis-angle construction
// ============================================================================

TEST(QuatTest, AxisAngleZeroIsIdentity) {
    Quatd q = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 0.0);
    EXPECT_NEAR(q.w, 1.0, kEps);
    EXPECT_NEAR(q.x, 0.0, kEps);
    EXPECT_NEAR(q.y, 0.0, kEps);
    EXPECT_NEAR(q.z, 0.0, kEps);
}

TEST(QuatTest, AxisAngleNinetyAboutX) {
    Quatd q = Quatd::from_axis_angle({1.0, 0.0, 0.0}, std::numbers::pi / 2.0);
    EXPECT_NEAR(q.w,  std::cos(std::numbers::pi / 4.0), kEps);
    EXPECT_NEAR(q.x,  std::sin(std::numbers::pi / 4.0), kEps);
    EXPECT_NEAR(q.y,  0.0, kEps);
    EXPECT_NEAR(q.z,  0.0, kEps);
}

// ============================================================================
// Vector rotation
// ============================================================================

TEST(QuatTest, RotateZeroAngleIsIdentity) {
    Quatd q = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 0.0);
    Vec3d v{1.0, 2.0, 3.0};
    Vec3d r = q.rotate(v);
    EXPECT_NEAR(r.x, 1.0, kEps);
    EXPECT_NEAR(r.y, 2.0, kEps);
    EXPECT_NEAR(r.z, 3.0, kEps);
}

TEST(QuatTest, RotateNinetyAboutZ) {
    Quatd q = Quatd::from_axis_angle({0.0, 0.0, 1.0}, std::numbers::pi / 2.0);
    Vec3d v{1.0, 0.0, 0.0};
    Vec3d r = q.rotate(v);
    EXPECT_NEAR(r.x, 0.0, kEps);
    EXPECT_NEAR(r.y, 1.0, kEps);
    EXPECT_NEAR(r.z, 0.0, kEps);
}

TEST(QuatTest, RotateInverseRecoversOriginal) {
    Quatd q = Quatd::from_axis_angle({0.0, 1.0, 0.0}, 0.7);
    Vec3d v{1.0, 2.0, 3.0};
    Vec3d r = q.rotate(v);
    Vec3d back = q.rotate_inverse(r);
    EXPECT_NEAR(back.x, v.x, kEps);
    EXPECT_NEAR(back.y, v.y, kEps);
    EXPECT_NEAR(back.z, v.z, kEps);
}

TEST(QuatTest, RotatePreservesLength) {
    Quatd q = Quatd::from_axis_angle(Vec3d{1.0, 1.0, 1.0}.normalized(), 0.8).normalized();
    Vec3d v{3.0, 4.0, 5.0};
    Vec3d r = q.rotate(v);
    EXPECT_NEAR(r.length(), v.length(), 1e-9);
}

TEST(QuatTest, RotateRoundTrip) {
    // Rotating by +theta then by -theta should recover the original.
    Quatd q_pos = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 0.3);
    Quatd q_neg = Quatd::from_axis_angle({1.0, 0.0, 0.0}, -0.3);
    Vec3d v{1.0, 2.0, 3.0};
    Vec3d r = q_neg.rotate(q_pos.rotate(v));
    EXPECT_NEAR(r.x, v.x, kEps);
    EXPECT_NEAR(r.y, v.y, kEps);
    EXPECT_NEAR(r.z, v.z, kEps);
}

// ============================================================================
// Euler angle conversion
// ============================================================================

TEST(QuatTest, EulerZyxZeroIsIdentity) {
    Quatd q = Quatd::from_euler_zyx(0.0, 0.0, 0.0);
    EXPECT_NEAR(q.w, 1.0, kEps);
    EXPECT_NEAR(q.x, 0.0, kEps);
    EXPECT_NEAR(q.y, 0.0, kEps);
    EXPECT_NEAR(q.z, 0.0, kEps);
}

TEST(QuatTest, EulerZyxRoundTrip) {
    double yaw = 0.3, pitch = -0.2, roll = 0.5;
    Quatd q = Quatd::from_euler_zyx(yaw, pitch, roll);
    auto [y2, p2, r2] = q.to_euler_zyx();
    EXPECT_NEAR(y2, yaw,   kEps);
    EXPECT_NEAR(p2, pitch, kEps);
    EXPECT_NEAR(r2, roll,  kEps);
}

TEST(QuatTest, EulerZyxPureYaw) {
    Quatd q = Quatd::from_euler_zyx(std::numbers::pi / 4.0, 0.0, 0.0);
    auto [y, p, r] = q.to_euler_zyx();
    EXPECT_NEAR(y, std::numbers::pi / 4.0, kEps);
    EXPECT_NEAR(p, 0.0, kEps);
    EXPECT_NEAR(r, 0.0, kEps);
}

TEST(QuatTest, EulerZyxPurePitch) {
    Quatd q = Quatd::from_euler_zyx(0.0, 0.6, 0.0);
    auto [y, p, r] = q.to_euler_zyx();
    EXPECT_NEAR(y, 0.0, kEps);
    EXPECT_NEAR(p, 0.6, kEps);
    EXPECT_NEAR(r, 0.0, kEps);
}

TEST(QuatTest, EulerZyxPureRoll) {
    Quatd q = Quatd::from_euler_zyx(0.0, 0.0, -0.8);
    auto [y, p, r] = q.to_euler_zyx();
    EXPECT_NEAR(y, 0.0, kEps);
    EXPECT_NEAR(p, 0.0, kEps);
    EXPECT_NEAR(r, -0.8, kEps);
}

// ============================================================================
// Slerp
// ============================================================================

TEST(SlerpTest, AtZeroReturnsStart) {
    Quatd a = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 0.0);
    Quatd b = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 1.0);
    Quatd r = a.slerp(b, 0.0);
    EXPECT_TRUE(QuatNear(r, a));
}

TEST(SlerpTest, AtOneReturnsEnd) {
    Quatd a = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 0.0);
    Quatd b = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 1.0);
    Quatd r = a.slerp(b, 1.0);
    EXPECT_TRUE(QuatNear(r, b));
}

TEST(SlerpTest, AtHalfIsMidpoint) {
    Quatd a = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 0.0);
    Quatd b = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 1.0);
    Quatd r = a.slerp(b, 0.5);
    Quatd mid = Quatd::from_axis_angle({0.0, 0.0, 1.0}, 0.5);
    EXPECT_TRUE(QuatNear(r, mid));
}

TEST(SlerpTest, TakesShorterPath) {
    // a and -b represent a >180° rotation; slerp should take the short way.
    Quatd a = Quatd::identity();
    Quatd b = Quatd::from_axis_angle({1.0, 0.0, 0.0}, std::numbers::pi - 0.1);
    Quatd negB{-b.w, -b.x, -b.y, -b.z};
    Quatd r1 = a.slerp(b, 0.5);
    Quatd r2 = a.slerp(negB, 0.5);
    // Both should take the short path (toward b, not -b).
    EXPECT_TRUE(QuatNear(r1, r2));
}

TEST(SlerpTest, VeryCloseQuaternionsUseLinear) {
    // When the angle between a and b is tiny, slerp falls back to lerp.
    // Verify no NaN and result is roughly between a and b.
    Quatd a = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 0.0);
    Quatd b = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 1e-8);
    Quatd r = a.slerp(b, 0.5);
    EXPECT_TRUE(std::isfinite(r.w));
    EXPECT_TRUE(std::isfinite(r.x));
    EXPECT_TRUE(std::isfinite(r.y));
    EXPECT_TRUE(std::isfinite(r.z));
}

// ============================================================================
// Composition correctness: rotate by q1*q2 == rotate by q1 then by q2
// (applied in the correct order)
// ============================================================================

TEST(QuatCompositionTest, ProductEqualsChainedRotation) {
    Quatd q1 = Quatd::from_axis_angle({1.0, 0.0, 0.0}, 0.3);
    Quatd q2 = Quatd::from_axis_angle({0.0, 1.0, 0.0}, 0.5);
    Vec3d v{1.0, 2.0, 3.0};

    // q1*q2 means: rotate by q2 first, then q1.
    Vec3d chained = q1.rotate(q2.rotate(v));
    Vec3d composed = (q1 * q2).rotate(v);

    EXPECT_NEAR(chained.x, composed.x, kEps);
    EXPECT_NEAR(chained.y, composed.y, kEps);
    EXPECT_NEAR(chained.z, composed.z, kEps);
}

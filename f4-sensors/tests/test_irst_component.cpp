// test_irst_component.cpp — IrstComponent in a bare EntityWorld: the
// SimData IRST card driving real detection behavior. Nominal-range
// scaling by the target's IR signature, the sqrt law, gimbal gates
// (azimuth off the nose, elevation), the ground factor, ground-clutter
// rejection, hold-time drops, scan pacing, and seeded determinism.

#include <f4/sensors/irst_component.hpp>
#include <f4/sensors/signature.hpp>

#include <f4/data/signature_data.hpp>

#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace f4::sensors;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kFeetPerNm = 6076.11548;

struct World {
    entities::EntityWorld world;
    messaging::MessageBus bus;
};

/// IRST at origin flying north (+y); target due north at `range_ft`,
/// 20,000 ft, flying south (head-on — the IR grids' aspect then reads
/// nose-on... the target's aspect to the SENSOR: flying south toward a
/// sensor behind it = tail aspect — the hot end. The generic card has
/// no signature data on the target, so the aspect is irrelevant unless
/// the test attaches one; default 1.0 either way).
struct IrstRig {
    World w;
    entities::EntityHandle sensor;
    entities::EntityHandle target;

    explicit IrstRig(double range_ft) {
        sensor = w.world.create();
        auto& stf = sensor.add<entities::TransformComponent>();
        stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        stf.vy = 400.0;   // flying north
        auto& ir = sensor.add<IrstComponent>();
        ir.scan_interval_s = 1.0;

        target = w.world.create();
        auto& tf = target.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, range_ft, 20000.0};
        tf.vy = -400.0;   // flying south (head-on, closing)
    }

    IrstComponent& ir() { return *sensor.get<IrstComponent>(); }

    void run(double seconds, double tick = 0.2) {
        for (double i = 0; i < seconds; i += tick) {
            IrstComponent::set_sim_time(IrstComponent::sim_time() + tick);
            w.world.update_all(tick, w.bus);
        }
    }
};

/// A flat IR grid of the given ratio at every aspect (the test's
/// stand-in for a SIGDATA IR band).
f4::data::SignatureGrid flatIrGrid(double v) {
    f4::data::SignatureGrid g;
    g.azimuth_deg = {0.0, 90.0, 180.0};
    g.elevation_deg = {-90.0, 0.0, 90.0};
    g.values = {{v, v, v}, {v, v, v}, {v, v, v}};
    return g;
}

} // namespace

// ============================================================================
// The pure model
// ============================================================================
TEST(IrstModel, RangeScalesWithSqrtOfSignature) {
    IrstParameters p;   // generic card: 10 NM nominal
    // Reference signature: the nominal range exactly.
    EXPECT_DOUBLE_EQ(irst_detection_range_nm(p, 1.0, false), 10.0);
    // 4x the signature: sqrt law doubles the range (one-way flux).
    EXPECT_DOUBLE_EQ(irst_detection_range_nm(p, 4.0, false), 20.0);
    // 1/4: half.
    EXPECT_DOUBLE_EQ(irst_detection_range_nm(p, 0.25, false), 5.0);
    // Negative signature clamps to zero contribution.
    EXPECT_DOUBLE_EQ(irst_detection_range_nm(p, -1.0, false), 0.0);
}

TEST(IrstModel, GroundFactorAppliesOnlyToGroundTargets) {
    IrstParameters p;
    p.ground_factor = 0.001;   // the seeker cards' shape
    EXPECT_NEAR(irst_detection_range_nm(p, 1.0, true), 0.01, 1e-12);
    EXPECT_DOUBLE_EQ(irst_detection_range_nm(p, 1.0, false), 10.0);
}

TEST(IrstModel, ProbabilityIsTheRadarKneeShape) {
    // Sure thing inside 75%, ramp to 0 at the edge.
    EXPECT_DOUBLE_EQ(irst_detection_probability(5.0, 10.0), 1.0);
    EXPECT_DOUBLE_EQ(irst_detection_probability(7.5, 10.0), 1.0);
    EXPECT_DOUBLE_EQ(irst_detection_probability(10.0, 10.0), 0.0);
    // The midpoint of the ramp: 50%.
    EXPECT_NEAR(irst_detection_probability(8.75, 10.0), 0.5, 1e-12);
}

// ============================================================================
// The component
// ============================================================================
TEST(IrstComponent, DetectsTargetInsideNominalRange) {
    IrstRig s{5.0 * kFeetPerNm};   // 5 NM: inside the 0.75 knee of 10 NM
    s.run(2.5);                    // scans at t=1.0 and t=2.0

    EXPECT_EQ(s.ir().scans_performed(), 2u);
    const auto* c = s.ir().find(s.target.id().value);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->detections, 2u);
    EXPECT_DOUBLE_EQ(c->position.y, 5.0 * kFeetPerNm);
}

TEST(IrstComponent, HotTargetSeenFartherThanColdOne) {
    World w;
    // Two targets at 12 NM, one with a 4x IR grid, one with 0.25x.
    auto make_target = [&](double sig) {
        auto t = w.world.create();
        auto& tf = t.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, 12.0 * kFeetPerNm, 20000.0};
        tf.vy = -400.0;
        auto& sig_comp = t.add<SignatureComponent>();
        // sig_data carries all five grids; build a record of flat grids.
        // The static vector is reserved up front so the stored pointers
        // stay valid across the pushes below.
        static std::vector<f4::data::AircraftSignatureData> records;
        records.reserve(2);
        records.push_back(f4::data::AircraftSignatureData{});
        records.back().name = "test";
        records.back().ir0 = flatIrGrid(sig);
        records.back().ir1 = flatIrGrid(sig);
        records.back().ir2 = flatIrGrid(sig);
        records.back().visual = flatIrGrid(1.0);
        sig_comp.sig_data = &records.back();
        return t;
    };
    auto hot = make_target(4.0);     // R_det = 20 NM
    auto cold = make_target(0.25);   // R_det = 5 NM

    auto sensor = w.world.create();
    auto& stf = sensor.add<entities::TransformComponent>();
    stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    stf.vy = 400.0;
    auto& ir = sensor.add<IrstComponent>();
    ir.scan_interval_s = 1.0;

    for (double i = 0; i < 2.5; i += 0.2) {
        IrstComponent::set_sim_time(IrstComponent::sim_time() + 0.2);
        w.world.update_all(0.2, w.bus);
    }

    // 12 NM: inside the hot target's knee (15 NM), past the cold one's
    // edge (5 NM).
    EXPECT_NE(ir.find(hot.id().value), nullptr);
    EXPECT_EQ(ir.find(cold.id().value), nullptr);
}

TEST(IrstComponent, AzimuthGateDropsTargetBehind) {
    IrstRig s{5.0 * kFeetPerNm};
    // The card's 120-deg azimuth limit: a target 150 deg off the nose is
    // outside. Turn the SENSOR east: the target due north is now 90 deg
    // off... still inside 120. Put it due west relative to flight:
    // rotate the sensor's velocity to fly south — target north = 180 deg
    // off the nose, outside any forward cone.
    s.sensor.get<entities::TransformComponent>()->vy = -400.0;
    s.run(2.5);
    EXPECT_EQ(s.ir().find(s.target.id().value), nullptr);
}

TEST(IrstComponent, ElevationGateDropsHighTarget) {
    IrstRig s{5.0 * kFeetPerNm};
    // The card's 60-deg elevation limit: put the target 80 deg up
    // (5 NM horizontal, ~28.4 NM up -> atan(28.4/5) = 80 deg).
    s.target.get<entities::TransformComponent>()->position.z =
        20000.0 + 28.4 * kFeetPerNm;
    s.run(2.5);
    EXPECT_EQ(s.ir().find(s.target.id().value), nullptr);
}

TEST(IrstComponent, GroundClutterSkippedUnlessTracked) {
    World w;
    auto sensor = w.world.create();
    auto& stf = sensor.add<entities::TransformComponent>();
    stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    stf.vy = 400.0;
    auto& ir = sensor.add<IrstComponent>();
    ir.scan_interval_s = 1.0;

    // A parked ramp target 2 NM north (stationary, at the surface —
    // is_ground_clutter's shape).
    auto parked = w.world.create();
    parked.add<entities::TransformComponent>().position =
        f4::geo::WorldPosition{0.0, 2.0 * kFeetPerNm, 0.0};

    for (double i = 0; i < 2.5; i += 0.2) {
        IrstComponent::set_sim_time(IrstComponent::sim_time() + 0.2);
        w.world.update_all(0.2, w.bus);
    }
    EXPECT_EQ(ir.find(parked.id().value), nullptr);

    // The A/G sensor card tracks clutter (the Maverick's job).
    ir.params.track_ground_clutter = true;
    for (double i = 0; i < 2.5; i += 0.2) {
        IrstComponent::set_sim_time(IrstComponent::sim_time() + 0.2);
        w.world.update_all(0.2, w.bus);
    }
    // 2 NM horizontal at ground factor 1.0 (the airframe card): inside
    // the knee — seen.
    EXPECT_NE(ir.find(parked.id().value), nullptr);
}

TEST(IrstComponent, ContactsDropAfterHoldExpires) {
    IrstRig s{5.0 * kFeetPerNm};
    s.run(2.5);   // two scans, contact established
    EXPECT_NE(s.ir().find(s.target.id().value), nullptr);

    // Remove the target from the picture: teleport it far beyond any
    // detection range. The contact holds for contact_hold_s (3 s) past
    // its last sighting, then decays.
    s.target.get<entities::TransformComponent>()->position =
        f4::geo::WorldPosition{0.0, 80.0 * kFeetPerNm, 20000.0};
    s.run(4.0);   // 3 s hold + one more scan tick
    EXPECT_EQ(s.ir().find(s.target.id().value), nullptr);
}

TEST(IrstComponent, SeededRunsAreReproducible) {
    auto run_once = []() {
        World w;
        auto sensor = w.world.create();
        auto& stf = sensor.add<entities::TransformComponent>();
        stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        stf.vy = 400.0;
        auto& ir = sensor.add<IrstComponent>();
        ir.scan_interval_s = 1.0;
        ir.rng_seed = 0xC0FFEE;

        // A ring of marginal targets: 9 NM out, right at the ramp.
        for (int k = 0; k < 8; ++k) {
            const double bearing = k * (M_PI / 4.0);
            auto t = w.world.create();
            auto& tf = t.add<entities::TransformComponent>();
            tf.position = f4::geo::WorldPosition{
                std::sin(bearing) * 9.0 * kFeetPerNm,
                std::cos(bearing) * 9.0 * kFeetPerNm, 20000.0};
            tf.vy = 400.0;   // flying north: beam aspect for E/W targets
        }
        for (double i = 0; i < 6.0; i += 0.2) {
            IrstComponent::set_sim_time(IrstComponent::sim_time() + 0.2);
            w.world.update_all(0.2, w.bus);
        }
        std::size_t n = 0;
        for (auto it = ir.contacts().begin(); it != ir.contacts().end(); ++it) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(run_once(), run_once());
}

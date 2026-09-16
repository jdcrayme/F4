// test_visual_component.cpp — VisualComponent in a bare EntityWorld:
// the original signal law (gain x sig / range^2 >= 1) driving
// deterministic detection. Threshold crossings at the card's implied
// range, the signature scaling (bigger target seen farther), the
// gimbal gates, parked targets being visible (no clutter rejection),
// hold-time drops, and scan pacing.

#include <f4/sensors/visual_component.hpp>
#include <f4/sensors/signature.hpp>

#include <f4/data/signature_data.hpp>

#include <f4/entities/entity.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::sensors;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif // !M_PI

namespace {

constexpr double kFeetPerNm = 6076.11548;

struct World {
    entities::EntityWorld world;
    messaging::MessageBus bus;
};

/// Sensor at origin flying north (+y); target due north at `range_ft`.
struct VisualRig {
    World w;
    entities::EntityHandle sensor;
    entities::EntityHandle target;

    explicit VisualRig(double range_ft, double target_speed = 0.0) {
        sensor = w.world.create();
        auto& stf = sensor.add<entities::TransformComponent>();
        stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        stf.vy = 400.0;   // flying north
        auto& vis = sensor.add<VisualComponent>();
        vis.scan_interval_s = 1.0;

        target = w.world.create();
        auto& tf = target.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, range_ft, 20000.0};
        tf.vy = target_speed;
    }

    VisualComponent& vis() { return *sensor.get<VisualComponent>(); }

    void run(double seconds, double tick = 0.2) {
        for (double i = 0; i < seconds; i += tick) {
            VisualComponent::set_sim_time(VisualComponent::sim_time() + tick);
            w.world.update_all(tick, w.bus);
        }
    }
};

} // namespace

// ============================================================================
// The pure model
// ============================================================================
TEST(VisualModel, SignalIsTheOriginalInverseSquareLaw) {
    // generic.vss: gain 3.7e9 -> threshold range sqrt(3.7e9) =
    // 60821.1 ft = 10.0109 NM (the shipped JSON's own implied nominal —
    // 3.7e9 is a rounded authoring value, not exactly (10 NM in ft)^2).
    const double r10 = 10.0 * kFeetPerNm;
    EXPECT_NEAR(visual_signal(3.7e9, 1.0, r10), 1.0, 0.003);

    // Half the range: 4x the signal (inverse square).
    EXPECT_NEAR(visual_signal(3.7e9, 1.0, r10 / 2.0), 4.0, 0.04);

    // Zero range clamps to a sane division.
    EXPECT_NEAR(visual_signal(3.7e9, 1.0, 0.0), 3.7e9, 1e3);
}

TEST(VisualModel, DetectionRangeIsSqrtOfGain) {
    EXPECT_NEAR(visual_detection_range_ft(3.7e9, 1.0) / kFeetPerNm,
                10.0, 0.02);
    // A 4x signature doubles the threshold range.
    EXPECT_NEAR(visual_detection_range_ft(3.7e9, 4.0) / kFeetPerNm,
                20.0, 0.03);
}

// ============================================================================
// The component
// ============================================================================
TEST(VisualComponent, DetectsTargetInsideNominalRange) {
    VisualRig s{5.0 * kFeetPerNm, -400.0};   // 5 NM: signal 4.0
    s.run(2.5);

    EXPECT_EQ(s.vis().scans_performed(), 2u);
    const auto* c = s.vis().find(s.target.id().value);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->detections, 2u);
    EXPECT_DOUBLE_EQ(c->position.y, 5.0 * kFeetPerNm);
}

TEST(VisualComponent, NoDetectionBeyondThresholdRange) {
    // 11 NM: past the 10-NM threshold (signal ~0.83), nothing.
    VisualRig s{11.0 * kFeetPerNm, -400.0};
    s.run(2.5);
    EXPECT_EQ(s.vis().find(s.target.id().value), nullptr);
}

TEST(VisualComponent, BiggerSignatureSeenFarther) {
    World w;
    // Two targets at 11 NM: a reference fighter (vis 1.0) and a 4x
    // bomber shape. Only the big one crosses the threshold.
    auto make_target = [&](double vis) {
        auto t = w.world.create();
        auto& tf = t.add<entities::TransformComponent>();
        tf.position = f4::geo::WorldPosition{0.0, 11.0 * kFeetPerNm, 20000.0};
        tf.vy = -400.0;
        auto& sig_comp = t.add<SignatureComponent>();
        static std::vector<f4::data::AircraftSignatureData> records;
        records.reserve(2);
        records.push_back(f4::data::AircraftSignatureData{});
        records.back().name = "test";
        records.back().ir0 = records.back().ir1 = records.back().ir2 =
            []() {
                f4::data::SignatureGrid g;
                g.azimuth_deg = {0.0, 90.0, 180.0};
                g.elevation_deg = {-90.0, 0.0, 90.0};
                g.values = {{1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}};
                return g;
            }();
        records.back().visual =
            [vis]() {
                f4::data::SignatureGrid g;
                g.azimuth_deg = {0.0, 90.0, 180.0};
                g.elevation_deg = {-90.0, 0.0, 90.0};
                g.values = {{vis, vis, vis}, {vis, vis, vis}, {vis, vis, vis}};
                return g;
            }();
        sig_comp.sig_data = &records.back();
        return t;
    };
    auto fighter = make_target(1.0);   // threshold 10 NM
    auto bomber = make_target(4.0);    // threshold 20 NM

    auto sensor = w.world.create();
    auto& stf = sensor.add<entities::TransformComponent>();
    stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    stf.vy = 400.0;
    auto& vis = sensor.add<VisualComponent>();
    vis.scan_interval_s = 1.0;

    for (double i = 0; i < 2.5; i += 0.2) {
        VisualComponent::set_sim_time(VisualComponent::sim_time() + 0.2);
        w.world.update_all(0.2, w.bus);
    }

    EXPECT_EQ(vis.find(fighter.id().value), nullptr);
    EXPECT_NE(vis.find(bomber.id().value), nullptr);
}

TEST(VisualComponent, ParkedTargetsAreVisible) {
    // No clutter rejection: an eyeball sees the ramp. A parked target
    // (stationary, on the deck) 2 NM north of an airborne sensor.
    World w;
    auto sensor = w.world.create();
    auto& stf = sensor.add<entities::TransformComponent>();
    stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
    stf.vy = 400.0;
    auto& vis = sensor.add<VisualComponent>();
    vis.scan_interval_s = 1.0;

    auto parked = w.world.create();
    parked.add<entities::TransformComponent>().position =
        f4::geo::WorldPosition{0.0, 2.0 * kFeetPerNm, 0.0};

    for (double i = 0; i < 2.5; i += 0.2) {
        VisualComponent::set_sim_time(VisualComponent::sim_time() + 0.2);
        w.world.update_all(0.2, w.bus);
    }
    EXPECT_NE(vis.find(parked.id().value), nullptr);
}

TEST(VisualComponent, AzimuthGateDropsTargetBehind) {
    VisualRig s{5.0 * kFeetPerNm, -400.0};
    // Fly SOUTH: the target due north sits 180 deg off the nose —
    // outside even the card's 181-deg limit (it wraps to 179... no:
    // |diff| = 180 deg > 181 is FALSE — 180 < 181 IS inside). Use the
    // tight mav-style cone instead: 5-deg azimuth limit.
    s.vis().params.az_limit_deg = 5.0;
    s.run(2.5);
    // Target dead ahead: 0 deg off the nose — still inside 5 deg.
    EXPECT_NE(s.vis().find(s.target.id().value), nullptr);

    // Now 90 deg off: sensor flies east, target north. Run past the
    // 3-s hold so the stale contact decays, proving no RE-detection.
    s.sensor.get<entities::TransformComponent>()->vy = 0.0;
    s.sensor.get<entities::TransformComponent>()->vx = 400.0;
    s.run(4.0);
    EXPECT_EQ(s.vis().find(s.target.id().value), nullptr);
}

TEST(VisualComponent, ContactsDropAfterHoldExpires) {
    VisualRig s{5.0 * kFeetPerNm, -400.0};
    s.run(2.5);
    EXPECT_NE(s.vis().find(s.target.id().value), nullptr);

    s.target.get<entities::TransformComponent>()->position =
        f4::geo::WorldPosition{0.0, 80.0 * kFeetPerNm, 20000.0};
    s.run(4.0);
    EXPECT_EQ(s.vis().find(s.target.id().value), nullptr);
}

TEST(VisualComponent, DeterministicAcrossIdenticalRuns) {
    auto run_once = []() {
        World w;
        auto sensor = w.world.create();
        auto& stf = sensor.add<entities::TransformComponent>();
        stf.position = f4::geo::WorldPosition{0.0, 0.0, 20000.0};
        stf.vy = 400.0;
        auto& vis = sensor.add<VisualComponent>();
        vis.scan_interval_s = 1.0;
        for (int k = 0; k < 6; ++k) {
            const double bearing = k * (M_PI / 3.0);
            auto t = w.world.create();
            auto& tf = t.add<entities::TransformComponent>();
            tf.position = f4::geo::WorldPosition{
                std::sin(bearing) * 9.0 * kFeetPerNm,
                std::cos(bearing) * 9.0 * kFeetPerNm, 20000.0};
        }
        for (double i = 0; i < 3.0; i += 0.2) {
            VisualComponent::set_sim_time(VisualComponent::sim_time() + 0.2);
            w.world.update_all(0.2, w.bus);
        }
        std::size_t n = 0;
        for (auto it = vis.contacts().begin(); it != vis.contacts().end();
             ++it) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(run_once(), run_once());
    EXPECT_GT(run_once(), 0u);
}

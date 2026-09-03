// f4-sensors/tests/test_simdata_sensors.cpp
//
// The SimData consumer side of wave 2 — proof that the SHIPPED data
// changes behavior end to end:
//
//   1. SIGDATA/RCSDAT grids: SignatureComponent carries a grid
//      (non-owning pointer, like BrainComponent's archetype), the radar
//      detection model reads value_at(aspect, elevation) DIRECTLY (the
//      grid IS the lobe shape — detection.hpp's documented "RCD data
//      lands" moment), and data-free targets take the exact
//      pre-SimData path.
//   2. SENSDATA/RWR parameters: RwrConfig's fields ARE generic.rwr's
//      values (the default is the shipped file), sensitivity scales
//      the receiver's effective range (harm.rwr = 2.0), and the FOV
//      limits gate emitters.
//
// The grids are built in-code (they are f4-data values, not files) —
// f4-data's loader tests cover the JSON side.

#include <f4/sensors/detection.hpp>
#include <f4/sensors/rwr.hpp>
#include <f4/sensors/signature.hpp>

#include <f4/data/signature_data.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace f4::sensors;
using namespace f4::data;

namespace entities = f4::entities;
namespace messaging = f4::messaging;

namespace {

constexpr double kFeetPerNm = 6076.11548;

/// The shipped GENERIC.RCS shape: -180/0/180 x -90/0/90, all 10 m^2.
SignatureGrid flatTenM2Grid() {
    SignatureGrid g;
    g.azimuth_deg = {-180.0, 0.0, 180.0};
    g.elevation_deg = {-90.0, 0.0, 90.0};
    g.values = {{10.0, 10.0, 10.0},
                {10.0, 10.0, 10.0},
                {10.0, 10.0, 10.0}};
    return g;
}

/// A lobe-shaped grid: 5 m^2 nose (az 0), 20 m^2 beam (az 90), 8 m^2
/// tail (az 180) at every elevation.
SignatureGrid lobeGrid() {
    SignatureGrid g;
    g.azimuth_deg = {0.0, 90.0, 180.0};
    g.elevation_deg = {-90.0, 0.0, 90.0};
    g.values = {{5.0, 20.0, 8.0},
                {5.0, 20.0, 8.0},
                {5.0, 20.0, 8.0}};
    return g;
}

} // namespace

// ============================================================================
// RCS grids -> detection
// ============================================================================
TEST(SimDataSensors, GridPathReplacesThePlaceholderLobe) {
    const RadarParameters params;   // reference 5 m^2 @ 40 NM
    const auto grid = lobeGrid();

    // Scalar path (placeholder lobe): 5 m^2 head-on, lobe factor 1.0.
    TargetSignature scalar;
    scalar.rcs_m2 = 5.0;
    scalar.aspect_rad = 0.0;
    const double r_scalar = detection_range_nm(params, scalar);

    // Grid path: beam-on (aspect 90 deg) reads 20 m^2 DIRECTLY — no
    // placeholder lobe factor stacks on top.
    TargetSignature gridded;
    gridded.rcs_grid = &grid;
    gridded.aspect_rad = 90.0 * (M_PI / 180.0);
    const double r_grid = detection_range_nm(params, gridded);

    // 20 vs 5 m^2 -> fourth root ratio 2^(1/2) = sqrt(2).
    EXPECT_NEAR(r_grid / r_scalar, std::sqrt(2.0), 1e-9);

    // And the placeholder path WOULD have shrunk the beam-on return
    // (lobe factor 0.3): the grid path is strictly different data.
    TargetSignature scalar_beam;
    scalar_beam.rcs_m2 = 5.0;
    scalar_beam.aspect_rad = 90.0 * (M_PI / 180.0);
    EXPECT_GT(r_grid, detection_range_nm(params, scalar_beam));
}

TEST(SimDataSensors, NoGridMeansIdenticalPreSimDataBehavior) {
    const RadarParameters params;
    const auto grid = flatTenM2Grid();

    TargetSignature without;
    without.rcs_m2 = 5.0;
    without.aspect_rad = 0.7;
    without.closure_fps = 250.0;

    TargetSignature with_null;
    with_null = without;
    with_null.rcs_grid = nullptr;   // explicit null: same path

    EXPECT_DOUBLE_EQ(detection_range_nm(params, without),
                     detection_range_nm(params, with_null));
    EXPECT_DOUBLE_EQ(detection_probability(params, without, 30.0),
                     detection_probability(params, with_null, 30.0));
}

TEST(SimDataSensors, GridInterpolatesAspectIntoDetection) {
    const RadarParameters params;
    const auto grid = lobeGrid();

    // Aspect 45 deg interpolates 5 -> 20 = 12.5 m^2.
    TargetSignature mid;
    mid.rcs_grid = &grid;
    mid.aspect_rad = 45.0 * (M_PI / 180.0);

    // Aspect 90 = 20 m^2 exactly; aspect 45 = 12.5 m^2.
    TargetSignature beam;
    beam.rcs_grid = &grid;
    beam.aspect_rad = 90.0 * (M_PI / 180.0);
    const double r_mid = detection_range_nm(params, mid);
    const double r_beam = detection_range_nm(params, beam);
    const double expected =
        r_beam * std::pow(12.5 / 20.0, 0.25);
    EXPECT_NEAR(r_mid, expected, 1e-9);
}

TEST(SimDataSensors, FlatShippedRcsReadsAsTenSquareMeters) {
    const RadarParameters params;
    const auto grid = flatTenM2Grid();
    TargetSignature sig;
    sig.rcs_grid = &grid;
    // Flat 10 m^2: exactly (10/5)^(1/4) times the reference range.
    EXPECT_NEAR(detection_range_nm(params, sig),
                params.reference_range_nm * std::pow(2.0, 0.25),
                1e-9);
}

TEST(SimDataSensors, SignatureComponentLooksUpTheGrid) {
    const auto grid = lobeGrid();
    SignatureComponent sig;
    sig.rcs_m2 = 5.0;

    // Without a grid: the scalar.
    EXPECT_DOUBLE_EQ(sig.effective_rcs_m2(1.2), 5.0);

    // With the grid: aspect-driven (90 deg beam = 20, 45 = 12.5).
    sig.rcs_grid = &grid;
    EXPECT_DOUBLE_EQ(sig.effective_rcs_m2(90.0 * (M_PI / 180.0)), 20.0);
    EXPECT_DOUBLE_EQ(sig.effective_rcs_m2(45.0 * (M_PI / 180.0)), 12.5);
}

// ============================================================================
// RWR parameters (generic.rwr / harm.rwr)
// ============================================================================
TEST(SimDataSensors, RwrConfigDefaultsAreGenericRwr) {
    // The default config IS sim/SENSDATA/RWR/generic.rwr: 180/90 FOV,
    // sensitivity 1.0 — nothing gated, range unchanged. Every
    // pre-SimData caller behaves identically.
    const RwrConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.az_limit_deg, 180.0);
    EXPECT_DOUBLE_EQ(cfg.el_limit_deg, 90.0);
    EXPECT_DOUBLE_EQ(cfg.sensitivity, 1.0);
}

TEST(SimDataSensors, RwrSensitivityExtendsReceiverRange) {
    // harm.rwr: sensitivity 2.0 — an emitter 150 NM out is beyond the
    // 100 NM default but inside 2x.
    RwrConfig cfg;
    cfg.sensitivity = 2.0;
    const RwrModel model{cfg};
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    std::vector<EmitterReading> readings;
    EmitterReading r;
    r.emitter_id = 1;
    r.position = f4::geo::WorldPosition{0.0, 150.0 * kFeetPerNm, 0.0};
    r.is_locked_on_self = true;
    readings.push_back(r);

    ASSERT_EQ(model.evaluate(readings, own, 0.0).size(), 1u);

    // The default (generic) receiver does not hear it.
    const RwrModel generic{};
    EXPECT_TRUE(generic.evaluate(readings, own, 0.0).empty());
}

TEST(SimDataSensors, RwrElevationGateDropsHighEmitters) {
    // A 60/60 receiver (harm.rwr's cone): an emitter 80 degrees up is
    // outside the elevation limit even though it is locked on us.
    RwrConfig cfg;
    cfg.el_limit_deg = 60.0;
    const RwrModel model{cfg};
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    std::vector<EmitterReading> readings;
    EmitterReading r;
    r.emitter_id = 1;
    // 5 NM horizontal, 20 NM up -> ~76 degrees elevation.
    r.position = f4::geo::WorldPosition{0.0, 5.0 * kFeetPerNm,
                                        20.0 * kFeetPerNm};
    r.is_locked_on_self = true;
    readings.push_back(r);

    EXPECT_TRUE(model.evaluate(readings, own, 0.0).empty());

    // The generic receiver (90 deg) still hears it.
    const RwrModel generic{};
    EXPECT_EQ(generic.evaluate(readings, own, 0.0).size(), 1u);
}

TEST(SimDataSensors, RwrAzimuthGateNeedsReceiverHeading) {
    // 60-degree cone (harm.rwr): an emitter 90 deg off the receiver's
    // nose is outside; head-on is inside. Without a heading (NaN) the
    // gate is disabled — generic.rwr's omni contract.
    RwrConfig cfg;
    cfg.az_limit_deg = 60.0;
    const RwrModel model{cfg};
    const auto own = f4::geo::WorldPosition{0.0, 0.0, 0.0};

    EmitterReading beamside;
    beamside.emitter_id = 1;
    beamside.position = f4::geo::WorldPosition{10000.0, 0.0, 0.0};
    beamside.is_locked_on_self = true;
    EmitterReading frontal = beamside;
    frontal.emitter_id = 2;
    frontal.position = f4::geo::WorldPosition{0.0, 10000.0, 0.0};

    // Receiver heading north (+y): emitter east (+x) is 90 deg off.
    const double north = 0.0;
    std::vector<EmitterReading> readings{beamside, frontal};
    const auto warnings =
        model.evaluate(readings, own, 0.0, north);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].emitter_id, 2u);

    // NaN heading: the azimuth gate is off, both come through.
    const auto omni = model.evaluate(readings, own, 0.0);
    EXPECT_EQ(omni.size(), 2u);
}

TEST(SimDataSensors, UpdateRwrUsesVictimHeadingForTheGate) {
    // World-level: a moving victim with a 60-degree RWR only warns on
    // missiles (launch emitters) inside its cone — the missile east of
    // a northbound victim is 90 deg off the nose and gated out; point
    // the victim east and the warning appears.
    entities::EntityWorld world;
    messaging::MessageBus bus;

    auto victim = world.create();
    auto& vt = victim.add<entities::TransformComponent>();
    vt.position = f4::geo::WorldPosition{0.0, 0.0, 0.0};
    vt.vx = 0.0;
    vt.vy = 500.0;   // heading north (+y)
    vt.vz = 0.0;
    victim.add<RwrComponent>();

    auto missile = world.create();
    missile.add<entities::TransformComponent>()
        .position = f4::geo::WorldPosition{10000.0, 0.0, 0.0};  // east
    missile.set_tag(entities::tags::ROLE,
                    entities::TagValue::from(std::string("missile")));

    RwrConfig cfg;
    cfg.az_limit_deg = 60.0;
    update_rwr(world, bus, 0.0, cfg);

    auto* rwr = victim.get<RwrComponent>();
    ASSERT_NE(rwr, nullptr);
    EXPECT_TRUE(rwr->warnings.empty());

    // Turn the victim east: the missile is now dead ahead — inside
    // the cone, the launch warning comes through.
    vt.vx = 500.0;
    vt.vy = 0.0;
    update_rwr(world, bus, 1.0, cfg);
    ASSERT_EQ(rwr->warnings.size(), 1u);
    EXPECT_EQ(rwr->warnings[0].type, RwrWarningType::Launch);
}

// f4-data/tests/test_simdata_loaders.cpp
//
// Loader tests for the SimData JSON fixtures (mnvrdata.json /
// braindata.json / formdat.json + wave 2: vehdef.json / irstdata.json /
// rwrdata.json / visualdata.json / sigdata.json) generated at build
// time by f4-convert's converters from the shipped game files.
// These are the CONTRACT surface between f4-convert and its runtime
// consumers (the f4-ai wiring reads these same files).

#include "f4/data/brain_data.hpp"
#include "f4/data/formation_data.hpp"
#include "f4/data/maneuver_data.hpp"
#include "f4/data/sensor_data.hpp"
#include "f4/data/signature_data.hpp"
#include "f4/data/vehicle_def_data.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace f4::data;

// ============================================================================
// ManeuverData JSON
// ============================================================================

TEST(SimDataLoaders, ManeuverDataLoads) {
    auto r = loadManeuverData(F4_GENERATED_FIXTURES_DIR "/simdata/mnvrdata.json");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "no errors" : r.errors[0]);
    EXPECT_EQ(r.data.populatedCells(), 81u);
}

TEST(SimDataLoaders, ManeuverDataPreservesF16Row) {
    auto r = loadManeuverData(F4_GENERATED_FIXTURES_DIR "/simdata/mnvrdata.json");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data.classFlags[4], 0x737u);
    const auto* c = r.data.choice(4, 0);   // F16 v F4
    ASSERT_NE(c, nullptr);
    // File: 2 intercepts (1 2), 1 merge (3), 1 react (2) — 1-based,
    // converted to 0-based {0,1} / {2} / {1}.
    EXPECT_EQ(c->intercepts, (std::vector<int>{0, 1}));
    EXPECT_EQ(c->merges, (std::vector<int>{2}));
    EXPECT_EQ(c->spikeReacts, (std::vector<int>{1}));
}

TEST(SimDataLoaders, ManeuverDataRejectsWrongTableShape) {
    auto r = loadManeuverDataFromString(
        "{\"kind\":\"f4.mnvrdata\",\"version\":1,\"classFlags\":[1,2],"
        "\"table\":[]}");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
}

// ============================================================================
// BrainData JSON
// ============================================================================

TEST(SimDataLoaders, BrainDataLoadsEightArchetypes) {
    auto r = loadBrainData(F4_GENERATED_FIXTURES_DIR "/simdata/braindata.json");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "no errors" : r.errors[0]);
    ASSERT_EQ(r.data.archetypes.size(), 8u);
    EXPECT_EQ(r.data.archetypes[0].name, "Generic");
}

TEST(SimDataLoaders, BrainDataEngagementRowsSurviveRoundTrip) {
    auto r = loadBrainData(F4_GENERATED_FIXTURES_DIR "/simdata/braindata.json");
    ASSERT_TRUE(r.ok);
    const auto* g = r.data.generic();
    ASSERT_NE(g, nullptr);
    const auto* guns = g->find_mode(BrainModeKey::GunsEngage);
    ASSERT_NE(guns, nullptr);
    EXPECT_DOUBLE_EQ(guns->range_ft, 6000.0);
    EXPECT_DOUBLE_EQ(guns->angle_deg, 45.0);
    const auto* wvr = g->find_mode(BrainModeKey::WVREngage);
    ASSERT_NE(wvr, nullptr);
    EXPECT_DOUBLE_EQ(wvr->range_ft, 50000.0);
    const auto* sead = r.data.find_archetype("SEAD");
    ASSERT_NE(sead, nullptr);
    EXPECT_FALSE(sead->mode_enabled(BrainModeKey::BVREngage));
}

TEST(SimDataLoaders, BrainDataRejectsEmptyArchetypes) {
    auto r = loadBrainDataFromString(
        "{\"kind\":\"f4.braindata\",\"version\":1,\"archetypes\":[]}");
    EXPECT_FALSE(r.ok);
}

// ============================================================================
// FormationLibrary JSON
// ============================================================================

TEST(SimDataLoaders, FormationLibraryLoadsNine) {
    auto r = loadFormationLibrary(F4_GENERATED_FIXTURES_DIR "/simdata/formdat.json");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "no errors" : r.errors[0]);
    ASSERT_EQ(r.data.formations.size(), 9u);
}

TEST(SimDataLoaders, FormationLibraryWedgeStation) {
    auto r = loadFormationLibrary(F4_GENERATED_FIXTURES_DIR "/simdata/formdat.json");
    ASSERT_TRUE(r.ok);
    const auto* w = r.data.find_by_name("wedge");
    ASSERT_NE(w, nullptr);
    EXPECT_FALSE(w->two_ship_explicit);
    EXPECT_DOUBLE_EQ(w->two_ship.rel_az_deg, 135.0);
    EXPECT_DOUBLE_EQ(w->two_ship.range_nm, 0.165);
    // The converted station the wingman flies: 0.165 NM at 135 deg.
    EXPECT_NEAR(w->two_ship.range_ft(), 1002.575, 0.01);
    EXPECT_NEAR(w->two_ship.az_rad(), 2.356194, 1e-6);
}

TEST(SimDataLoaders, FormationLibraryTrailExplicitTwoShip) {
    auto r = loadFormationLibrary(F4_GENERATED_FIXTURES_DIR "/simdata/formdat.json");
    ASSERT_TRUE(r.ok);
    const auto* t = r.data.find_by_name("trail");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->two_ship_explicit);
    EXPECT_DOUBLE_EQ(t->two_ship.rel_az_deg, 180.0);
    EXPECT_DOUBLE_EQ(t->two_ship.range_nm, 2.0);
}

TEST(SimDataLoaders, FormationLibraryMissingTwoShipDefaultsToSlotZero) {
    // JSON without a twoShip block: the loader mirrors formdata.cpp:85-91.
    const std::string json =
        "{\"kind\":\"f4.formdata\",\"version\":1,\"formations\":["
        "{\"name\":\"solo\",\"formNum\":0,"
        "\"slots\":[{\"relAzDeg\":90.0,\"relElDeg\":0.0,\"rangeNm\":0.5}]}]}";
    auto r = loadFormationLibraryFromString(json);
    ASSERT_TRUE(r.ok);
    const auto* s = r.data.find_by_name("solo");
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->two_ship_explicit);
    EXPECT_DOUBLE_EQ(s->two_ship.rel_az_deg, 90.0);
    EXPECT_DOUBLE_EQ(s->two_ship.range_nm, 0.5);
}

// ============================================================================
// VehicleDefinitionLibrary JSON (wave 2 — f4.vehdef)
// ============================================================================
TEST(SimDataLoaders, VehicleLibraryLoadsShippedRows) {
    auto r = loadVehicleDefinitionLibrary(
        F4_GENERATED_FIXTURES_DIR "/simdata/vehdef.json");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(r.library.entries.size(), 86u);
}

TEST(SimDataLoaders, VehicleLibraryF16LoadoutSurvivesRoundTrip) {
    auto r = loadVehicleDefinitionLibrary(
        F4_GENERATED_FIXTURES_DIR "/simdata/vehdef.json");
    ASSERT_TRUE(r.ok);
    const auto* f16 = r.library.find("F16");
    ASSERT_NE(f16, nullptr);
    ASSERT_NE(f16->aircraft(), nullptr);
    EXPECT_EQ(f16->aircraft()->combat_class, 4);
    EXPECT_EQ(f16->aircraft()->airframe_index, 4);
    ASSERT_EQ(f16->aircraft()->player_sensors.size(), 4u);
    ASSERT_EQ(f16->aircraft()->ai_sensors.size(), 3u);
    // AI F-16: Visual + Radar + RWR (no IRST).
    EXPECT_EQ(f16->aircraft()->ai_sensors[0], (SensorSlot{3, 0}));
    EXPECT_EQ(f16->aircraft()->ai_sensors[1], (SensorSlot{1, 0}));
    EXPECT_EQ(f16->aircraft()->ai_sensors[2], (SensorSlot{2, 0}));
}

TEST(SimDataLoaders, VehicleLibraryWeaponCardsSurviveRoundTrip) {
    auto r = loadVehicleDefinitionLibrary(
        F4_GENERATED_FIXTURES_DIR "/simdata/vehdef.json");
    ASSERT_TRUE(r.ok);
    const auto* sa6 = r.library.find("sa6");
    ASSERT_NE(sa6, nullptr);
    ASSERT_NE(sa6->weapon(), nullptr);
    EXPECT_EQ(sa6->weapon()->mnemonic, "SA6");
    EXPECT_DOUBLE_EQ(sa6->weapon()->weight, 225.0);
    EXPECT_EQ(sa6->weapon()->weapon_class, 8);
    EXPECT_EQ(sa6->weapon()->data_idx, 6);
}

TEST(SimDataLoaders, VehicleLibraryRejectsWrongKind) {
    auto r = loadVehicleDefinitionLibraryFromString(
        R"({"kind": "f4.sigdata", "version": 1})");
    EXPECT_FALSE(r.ok);
}

// ============================================================================
// Sensor data JSON (wave 2 — f4.irstdata / f4.rwrdata / f4.visualdata)
// ============================================================================
TEST(SimDataLoaders, IrstDataLoadsEightSeekers) {
    auto r = loadIrstSensorData(
        F4_GENERATED_FIXTURES_DIR "/simdata/irstdata.json");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(r.data.sensors.size(), 8u);
    const auto* aim9l = r.data.find("aim9l");
    ASSERT_NE(aim9l, nullptr);
    EXPECT_DOUBLE_EQ(aim9l->data.az_limit_deg, 60.0);
    EXPECT_DOUBLE_EQ(aim9l->data.nominal_range_nm, 10.0);
    EXPECT_DOUBLE_EQ(aim9l->data.flare_chance, 0.2);
}

TEST(SimDataLoaders, RwrDataLoadsGenericAndHarm) {
    auto r = loadRwrSensorData(
        F4_GENERATED_FIXTURES_DIR "/simdata/rwrdata.json");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data.sensors.size(), 2u);
    const auto* g = r.data.find("generic");
    ASSERT_NE(g, nullptr);
    EXPECT_DOUBLE_EQ(g->data.sensitivity, 1.0);
    const auto* h = r.data.find("harm");
    ASSERT_NE(h, nullptr);
    EXPECT_DOUBLE_EQ(h->data.sensitivity, 2.0);
}

TEST(SimDataLoaders, VisualDataLoadsThreeSensors) {
    auto r = loadVisualSensorData(
        F4_GENERATED_FIXTURES_DIR "/simdata/visualdata.json");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.data.sensors.size(), 3u);
    const auto* g = r.data.find("generic");
    ASSERT_NE(g, nullptr);
    EXPECT_NEAR(g->data.nominal_range_nm(), 10.0, 0.05);
}

TEST(SimDataLoaders, VisualDerivedRangeRecomputesFromGain) {
    // The JSON carries both gain and the derived nominal_range_nm; the
    // loader keeps gain authoritative (nominal_range recomputed on call).
    VisualSensorEntry e;
    e.name = "x";
    e.data.gain = 3.7e11;
    VisualSensorDataLibrary lib;
    lib.sensors.push_back(e);
    auto back = loadVisualSensorDataFromString(writeVisualSensorData(lib));
    ASSERT_TRUE(back.ok);
    EXPECT_NEAR(back.data.find("x")->data.nominal_range_nm(), 100.0, 0.5);
}

// ============================================================================
// Signature data JSON (wave 2 — f4.sigdata)
// ============================================================================
TEST(SimDataLoaders, SignatureDataLoadsGenericSet) {
    auto r = loadSignatureDataLibrary(
        F4_GENERATED_FIXTURES_DIR "/simdata/sigdata.json");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.library.entries.size(), 1u);
    const auto* g = r.library.find("GENERIC");
    ASSERT_NE(g, nullptr);
}

TEST(SimDataLoaders, SignatureGridsSurviveRoundTrip) {
    auto r = loadSignatureDataLibrary(
        F4_GENERATED_FIXTURES_DIR "/simdata/sigdata.json");
    ASSERT_TRUE(r.ok);
    const auto& s = r.library.entries[0];
    // RCS: flat 10 m^2 everywhere.
    EXPECT_DOUBLE_EQ(s.rcs.value_at(0.0, 0.0), 10.0);
    EXPECT_DOUBLE_EQ(s.rcs.value_at(90.0, 45.0), 10.0);
    // IR0: rear-hot (az 180 at el 0 -> 0.1).
    EXPECT_DOUBLE_EQ(s.ir0.value_at(180.0, 0.0), 0.1);
    EXPECT_DOUBLE_EQ(s.ir0.value_at(45.0, 0.0), 0.025);
    // IR2 max: 0.8 nose / 3.5 beam / 4.0 tail at el 0.
    EXPECT_DOUBLE_EQ(s.ir2.value_at(0.0, 0.0), 0.8);
    EXPECT_DOUBLE_EQ(s.ir2.value_at(90.0, 0.0), 3.5);
    // Visual: flat 1.0.
    EXPECT_DOUBLE_EQ(s.visual.value_at(90.0, 0.0), 1.0);
}

TEST(SimDataLoaders, SignatureDataRejectsWrongKind) {
    auto r = loadSignatureDataLibraryFromString(
        R"({"kind": "f4.vehdef", "version": 1})");
    EXPECT_FALSE(r.ok);
}

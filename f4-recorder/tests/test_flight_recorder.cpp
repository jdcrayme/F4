// f4-recorder/tests/test_flight_recorder.cpp
//
// Unit tests for FlightRecorder — recording, queries, and JSON export.

#include <gtest/gtest.h>

#include <f4/recorder/flight_recorder.hpp>

using namespace f4::recorder;

namespace {

FlightSnapshot make_test_snapshot(
    std::uint64_t tick, double time_s,
    std::uint64_t entity_id = 1,
    const std::string& mode = "TakeoffMode",
    const std::string& state = "Taxi")
{
    FlightSnapshot s;
    s.tick = tick;
    s.sim_time_s = time_s;
    s.entity_id = entity_id;
    s.callsign = "Viper1";
    s.position = f4::geo::WorldPosition(100.0, 200.0, 0.0);
    s.heading_rad = 0.0;
    s.vcas_kts = 15.0;
    s.altitude_agl_ft = 0.0;
    s.ai_mode = mode;
    s.ai_state = state;
    s.throttle_cmd = 0.1;
    s.gear_handle_down = true;
    s.on_ground = true;
    return s;
}

} // anonymous namespace

// ============================================================================
// Basic recording
// ============================================================================

TEST(FlightRecorder, EmptyOnConstruction) {
    FlightRecorder rec;
    EXPECT_TRUE(rec.empty());
    EXPECT_EQ(rec.size(), 0u);
}

TEST(FlightRecorder, RecordAndSize) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0));
    rec.record(make_test_snapshot(1, 0.1));
    rec.record(make_test_snapshot(2, 0.2));

    EXPECT_FALSE(rec.empty());
    EXPECT_EQ(rec.size(), 3u);
}

TEST(FlightRecorder, RecordMove) {
    FlightRecorder rec;
    FlightSnapshot s = make_test_snapshot(0, 0.0);
    rec.record(std::move(s));

    EXPECT_EQ(rec.size(), 1u);
}

// ============================================================================
// Queries
// ============================================================================

TEST(FlightRecorder, SnapshotsForEntityId) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0, /*entity_id=*/1));
    rec.record(make_test_snapshot(1, 0.1, /*entity_id=*/2));
    rec.record(make_test_snapshot(2, 0.2, /*entity_id=*/1));
    rec.record(make_test_snapshot(3, 0.3, /*entity_id=*/2));

    auto for_1 = rec.snapshots_for(1);
    auto for_2 = rec.snapshots_for(2);
    auto for_3 = rec.snapshots_for(3);

    EXPECT_EQ(for_1.size(), 2u);
    EXPECT_EQ(for_2.size(), 2u);
    EXPECT_EQ(for_3.size(), 0u);

    EXPECT_EQ(for_1[0].tick, 0u);
    EXPECT_EQ(for_1[1].tick, 2u);
}

TEST(FlightRecorder, SnapshotsInRange) {
    FlightRecorder rec;
    for (int i = 0; i < 10; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1));
    }

    auto in_range = rec.snapshots_in_range(0.3, 0.7);
    EXPECT_EQ(in_range.size(), 5u);  // ticks 3,4,5,6,7
    EXPECT_EQ(in_range[0].tick, 3u);
    EXPECT_EQ(in_range[4].tick, 7u);
}

// ============================================================================
// JSON export (full trace)
// ============================================================================

TEST(FlightRecorder, ToJsonContainsHeader) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0));

    std::string json = rec.to_json("test_takeoff");

    EXPECT_NE(json.find("\"format\":\"f4-flight-recording\""), std::string::npos);
    EXPECT_NE(json.find("\"version\":1"), std::string::npos);
    EXPECT_NE(json.find("\"scenario\":\"test_takeoff\""), std::string::npos);
    EXPECT_NE(json.find("\"snapshot_count\":1"), std::string::npos);
}

TEST(FlightRecorder, ToJsonContainsSnapshotData) {
    FlightRecorder rec;
    auto snap = make_test_snapshot(42, 4.2, 1, "TakeoffMode", "TakeRunway");
    snap.vcas_kts = 120.0;
    snap.throttle_cmd = 1.0;
    rec.record(snap);

    std::string json = rec.to_json();

    EXPECT_NE(json.find("\"tick\":42"), std::string::npos);
    EXPECT_NE(json.find("\"sim_time_s\":"), std::string::npos);
    EXPECT_NE(json.find("\"TakeoffMode\""), std::string::npos);
    EXPECT_NE(json.find("\"TakeRunway\""), std::string::npos);
}

TEST(FlightRecorder, ToJsonMultipleSnapshots) {
    FlightRecorder rec;
    for (int i = 0; i < 5; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1));
    }

    std::string json = rec.to_json();
    EXPECT_NE(json.find("\"snapshot_count\":5"), std::string::npos);
}

// ============================================================================
// JSON export (summary)
// ============================================================================

TEST(FlightRecorder, SummaryJsonContainsPhases) {
    FlightRecorder rec;

    // Phase 1: Taxi (ticks 0-9)
    for (int i = 0; i < 10; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1, 1, "TakeoffMode", "Taxi"));
    }
    // Phase 2: TakeRunway (ticks 10-19)
    for (int i = 10; i < 20; ++i) {
        rec.record(make_test_snapshot(i, i * 0.1, 1, "TakeoffMode", "TakeRunway"));
    }

    std::string json = rec.to_summary_json("test_phases");

    EXPECT_NE(json.find("\"format\":\"f4-flight-summary\""), std::string::npos);
    EXPECT_NE(json.find("\"Takeoff(\\\"|\\\")Mode\""), std::string::npos);  // mode appears
    EXPECT_NE(json.find("\"Taxi\""), std::string::npos);
    EXPECT_NE(json.find("\"TakeRunway\""), std::string::npos);
}

TEST(FlightRecorder, SummaryJsonDetectsAnomalies) {
    FlightRecorder rec;

    // Normal flight for 5 ticks
    for (int i = 0; i < 5; ++i) {
        auto s = make_test_snapshot(i, i * 0.1);
        s.cross_track_error_ft = 10.0;  // within tolerance
        rec.record(s);
    }
    // Anomalous tick
    auto bad = make_test_snapshot(5, 0.5);
    bad.cross_track_error_ft = 200.0;  // exceeds 100ft tolerance
    rec.record(bad);

    std::string json = rec.to_summary_json("test_anomaly", /*cross_track_tol=*/100.0);

    EXPECT_NE(json.find("\"type\":\"path_deviation\""), std::string::npos);
    EXPECT_NE(json.find("\"tick\":5"), std::string::npos);
}

// ============================================================================
// Scenario name
// ============================================================================

TEST(FlightRecorder, ScenarioName) {
    FlightRecorder rec;
    rec.set_scenario_name("takeoff_kunsan_rwy36l");
    EXPECT_EQ(rec.scenario_name(), "takeoff_kunsan_rwy36l");

    // to_json uses scenario_name_ when no override is given
    rec.record(make_test_snapshot(0, 0.0));
    std::string json = rec.to_json();
    EXPECT_NE(json.find("\"scenario\":\"takeoff_kunsan_rwy36l\""), std::string::npos);
}

// ============================================================================
// File I/O (write_json)
// ============================================================================

TEST(FlightRecorder, WriteJsonProducesNonEmptyFile) {
    FlightRecorder rec;
    rec.record(make_test_snapshot(0, 0.0));
    rec.record(make_test_snapshot(1, 0.1));

    // Write to a temp file
    const auto path = std::filesystem::temp_directory_path() / "test_flight_recording.json";
    ASSERT_NO_THROW(rec.write_json(path));

    // Verify the file exists and is non-empty
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    // Cleanup
    std::filesystem::remove(path);
}

// ============================================================================
// Snapshot field coverage
// ============================================================================

TEST(FlightSnapshot, DegreeAccessors) {
    FlightSnapshot s;
    s.heading_rad = 1.5707963267948966;  // 90 degrees
    s.pitch_rad = 0.0;
    s.roll_rad = -0.5235987755982988;  // -30 degrees

    EXPECT_NEAR(s.heading_deg(), 90.0, 0.001);
    EXPECT_NEAR(s.pitch_deg(), 0.0, 0.001);
    EXPECT_NEAR(s.roll_deg(), -30.0, 0.001);
}

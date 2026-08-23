// f4-world-viewer/tests/test_replay_mode.cpp
//
// Unit tests for the ReplayState data model + load_replay free function.
// These tests do NOT touch Raylib or ImGui — they exercise the pure
// data layer (ReplayState struct + load_replay + fit_replay_camera).
// The full render path is exercised by the screenshot smoke test in
// cli/main.cpp's --replay + --screenshot path.
//
// Test fixtures: we synthesize small FlightRecorder traces inline
// (no file I/O needed except for the load-from-file path, which uses
// tmp files).

#include <gtest/gtest.h>

#include <f4/viewer/replay_mode.hpp>
#include <f4/recorder/flight_recorder.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

using namespace f4::viewer;
using f4::recorder::FlightRecorder;
using f4::recorder::FlightSnapshot;

namespace {

FlightSnapshot make_snap(std::uint64_t tick, double t,
                         std::uint64_t eid,
                         double x, double y, double z,
                         const std::string& mode = "TakeoffMode",
                         const std::string& state = "Taxi") {
    FlightSnapshot s;
    s.tick = tick;
    s.sim_time_s = t;
    s.entity_id = eid;
    s.callsign = "Viper" + std::to_string(eid);
    s.position = f4::geo::WorldPosition(x, y, z);
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
// ReplayState — empty state
// ============================================================================

TEST(ReplayState, InactiveWhenEmpty) {
    ReplayState s;
    EXPECT_FALSE(s.active());
    EXPECT_EQ(s.current_snapshot(), nullptr);
    EXPECT_EQ(s.focused_entity_id(), 0u);
    EXPECT_TRUE(s.focused_entity_snapshots().empty());
}

TEST(ReplayState, ActiveWhenRecordingLoaded) {
    ReplayState s;
    s.recording = FlightRecorder{};
    s.recording->record(make_snap(0, 0.0, 1, 0, 0, 0));
    EXPECT_TRUE(s.active());
}

// ============================================================================
// ReplayState — stepping
// ============================================================================

TEST(ReplayState, StepForward) {
    ReplayState s;
    s.recording = FlightRecorder{};
    for (int i = 0; i < 5; ++i) {
        s.recording->record(make_snap(i, i * 0.1, 1, i * 10.0, 0, 0));
    }
    s.entity_ids = {1};

    EXPECT_EQ(s.current_tick, 0u);
    s.step(1);
    EXPECT_EQ(s.current_tick, 1u);
    s.step(2);
    EXPECT_EQ(s.current_tick, 3u);
}

TEST(ReplayState, StepBackward) {
    ReplayState s;
    s.recording = FlightRecorder{};
    for (int i = 0; i < 5; ++i) {
        s.recording->record(make_snap(i, i * 0.1, 1, i * 10.0, 0, 0));
    }
    s.entity_ids = {1};
    s.current_tick = 4;

    s.step(-1);
    EXPECT_EQ(s.current_tick, 3u);
    s.step(-2);
    EXPECT_EQ(s.current_tick, 1u);
}

TEST(ReplayState, StepClampsToRange) {
    ReplayState s;
    s.recording = FlightRecorder{};
    for (int i = 0; i < 5; ++i) {
        s.recording->record(make_snap(i, i * 0.1, 1, i * 10.0, 0, 0));
    }
    s.entity_ids = {1};

    s.step(-100);
    EXPECT_EQ(s.current_tick, 0u);
    s.step(100);
    EXPECT_EQ(s.current_tick, 4u);
}

TEST(ReplayState, JumpToEnd) {
    ReplayState s;
    s.recording = FlightRecorder{};
    for (int i = 0; i < 10; ++i) {
        s.recording->record(make_snap(i, i * 0.1, 1, i * 10.0, 0, 0));
    }
    s.entity_ids = {1};

    s.jump_to_end();
    EXPECT_EQ(s.current_tick, 9u);
    EXPECT_EQ(s.current_snapshot()->tick, 9u);
}

TEST(ReplayState, JumpToStart) {
    ReplayState s;
    s.recording = FlightRecorder{};
    for (int i = 0; i < 10; ++i) {
        s.recording->record(make_snap(i, i * 0.1, 1, i * 10.0, 0, 0));
    }
    s.entity_ids = {1};
    s.current_tick = 5;

    s.jump_to_start();
    EXPECT_EQ(s.current_tick, 0u);
}

// ============================================================================
// ReplayState — multi-entity focus
// ============================================================================

TEST(ReplayState, EntityIdsBuiltInFirstAppearanceOrder) {
    ReplayState s;
    s.recording = FlightRecorder{};
    s.recording->record(make_snap(0, 0.0, 7, 0, 0, 0));
    s.recording->record(make_snap(1, 0.1, 3, 0, 0, 0));
    s.recording->record(make_snap(2, 0.2, 7, 0, 0, 0));
    s.recording->record(make_snap(3, 0.3, 9, 0, 0, 0));

    // Build entity_ids the way load_replay() does
    for (const auto& snap : s.recording->snapshots()) {
        if (std::find(s.entity_ids.begin(), s.entity_ids.end(), snap.entity_id)
            == s.entity_ids.end()) {
            s.entity_ids.push_back(snap.entity_id);
        }
    }

    ASSERT_EQ(s.entity_ids.size(), 3u);
    EXPECT_EQ(s.entity_ids[0], 7u);
    EXPECT_EQ(s.entity_ids[1], 3u);
    EXPECT_EQ(s.entity_ids[2], 9u);
}

TEST(ReplayState, FocusedEntitySnapshotsFiltersById) {
    ReplayState s;
    s.recording = FlightRecorder{};
    s.recording->record(make_snap(0, 0.0, 1, 0, 0, 0));
    s.recording->record(make_snap(1, 0.1, 2, 100, 0, 0));
    s.recording->record(make_snap(2, 0.2, 1, 10, 0, 0));
    s.recording->record(make_snap(3, 0.3, 2, 110, 0, 0));
    s.entity_ids = {1, 2};

    // Focus on entity 1
    s.focused_entity_index = 0;
    auto for_1 = s.focused_entity_snapshots();
    ASSERT_EQ(for_1.size(), 2u);
    EXPECT_EQ(for_1[0].entity_id, 1u);
    EXPECT_EQ(for_1[1].entity_id, 1u);
    EXPECT_EQ(for_1[0].position.x, 0.0);
    EXPECT_EQ(for_1[1].position.x, 10.0);

    // Focus on entity 2
    s.focused_entity_index = 1;
    auto for_2 = s.focused_entity_snapshots();
    ASSERT_EQ(for_2.size(), 2u);
    EXPECT_EQ(for_2[0].entity_id, 2u);
    EXPECT_EQ(for_2[1].entity_id, 2u);
    EXPECT_EQ(for_2[0].position.x, 100.0);
}

// ============================================================================
// ReplayState — sim_dt inference
// ============================================================================

TEST(ReplayState, InferSimDtFromUniformSpacing) {
    ReplayState s;
    s.recording = FlightRecorder{};
    for (int i = 0; i < 10; ++i) {
        s.recording->record(make_snap(i, i * 0.05, 1, i * 10.0, 0, 0));
    }
    s.entity_ids = {1};

    s.infer_sim_dt();
    EXPECT_NEAR(s.sim_dt_s, 0.05, 1e-9);
}

TEST(ReplayState, InferSimDtDefaultWhenSingleSnapshot) {
    ReplayState s;
    s.recording = FlightRecorder{};
    s.recording->record(make_snap(0, 0.0, 1, 0, 0, 0));
    s.entity_ids = {1};

    s.infer_sim_dt();
    EXPECT_NEAR(s.sim_dt_s, 1.0 / 60.0, 1e-9);
}

TEST(ReplayState, InferSimDtRobustToGaps) {
    // Median of spacings — one weird gap shouldn't skew the result
    ReplayState s;
    s.recording = FlightRecorder{};
    double t = 0.0;
    for (int i = 0; i < 9; ++i) {
        s.recording->record(make_snap(i, t, 1, i * 10.0, 0, 0));
        t += 0.1;
    }
    // Add a gap (5s instead of 0.1s)
    s.recording->record(make_snap(9, t + 5.0, 1, 90.0, 0, 0));
    s.entity_ids = {1};

    s.infer_sim_dt();
    // Median should still be 0.1 (5 of 9 spacings are 0.1)
    EXPECT_NEAR(s.sim_dt_s, 0.1, 1e-9);
}

// ============================================================================
// load_replay — file I/O
// ============================================================================

TEST(LoadReplay, FailsOnMissingFile) {
    ReplayState s;
    std::string err;
    EXPECT_FALSE(load_replay(s, "/nonexistent/path/trace.json", &err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(s.active());
}

TEST(LoadReplay, FailsOnEmptyFile) {
    const auto path = std::filesystem::temp_directory_path() / "test_empty_trace.json";
    {
        std::ofstream f(path);
        f << "{}";
    }
    ReplayState s;
    std::string err;
    EXPECT_FALSE(load_replay(s, path, &err));
    EXPECT_FALSE(err.empty());
    std::filesystem::remove(path);
}

TEST(LoadReplay, LoadsValidTrace) {
    // Build a trace with FlightRecorder, write to tmp, load it back
    FlightRecorder rec;
    rec.set_scenario_name("test_replay_load");
    for (int i = 0; i < 5; ++i) {
        auto snap = make_snap(i, i * 0.1, 1, i * 100.0, 0, 0);
        snap.cross_track_error_ft = (i % 2 == 0) ? 10.0 : 50.0;
        rec.record(snap);
    }

    const auto path = std::filesystem::temp_directory_path() / "test_replay_trace.json";
    ASSERT_NO_THROW(rec.write_json(path));

    ReplayState s;
    std::string err;
    ASSERT_TRUE(load_replay(s, path, &err)) << "error: " << err;

    EXPECT_TRUE(s.active());
    ASSERT_TRUE(s.recording.has_value());
    EXPECT_EQ(s.recording->size(), 5u);
    EXPECT_EQ(s.recording->scenario_name(), "test_replay_load");

    // entity_ids should be populated
    ASSERT_EQ(s.entity_ids.size(), 1u);
    EXPECT_EQ(s.entity_ids[0], 1u);

    // current_tick starts at 0
    EXPECT_EQ(s.current_tick, 0u);

    // sim_dt should be inferred
    EXPECT_NEAR(s.sim_dt_s, 0.1, 1e-9);

    // First snapshot's position should match
    const auto* snap = s.current_snapshot();
    ASSERT_NE(snap, nullptr);
    EXPECT_NEAR(snap->position.x, 0.0, 1e-9);

    std::filesystem::remove(path);
}

TEST(LoadReplay, LoadsMultiEntityTrace) {
    FlightRecorder rec;
    for (int i = 0; i < 3; ++i) {
        rec.record(make_snap(i, i * 0.1, 1, i * 10.0, 0, 0));
    }
    for (int i = 0; i < 2; ++i) {
        rec.record(make_snap(i + 10, (i + 10) * 0.1, 2, i * 100.0, 50.0, 0));
    }

    const auto path = std::filesystem::temp_directory_path() / "test_replay_multi.json";
    ASSERT_NO_THROW(rec.write_json(path));

    ReplayState s;
    std::string err;
    ASSERT_TRUE(load_replay(s, path, &err)) << "error: " << err;

    ASSERT_EQ(s.entity_ids.size(), 2u);
    EXPECT_EQ(s.entity_ids[0], 1u);
    EXPECT_EQ(s.entity_ids[1], 2u);

    // Default focus is entity 0
    EXPECT_EQ(s.focused_entity_id(), 1u);
    auto focused = s.focused_entity_snapshots();
    EXPECT_EQ(focused.size(), 3u);

    // Switch focus to entity 2
    s.focused_entity_index = 1;
    EXPECT_EQ(s.focused_entity_id(), 2u);
    auto focused2 = s.focused_entity_snapshots();
    EXPECT_EQ(focused2.size(), 2u);

    std::filesystem::remove(path);
}

// ============================================================================
// fit_replay_camera — bbox fitting
// ============================================================================

TEST(FitReplayCamera, NoOpWhenInactive) {
    ReplayState s;
    float cx = 1.0f, cy = 2.0f, zoom = 3.0f;
    fit_replay_camera(s, 800.0f, 600.0f, cx, cy, zoom);
    // Should be unchanged
    EXPECT_EQ(cx, 1.0f);
    EXPECT_EQ(cy, 2.0f);
    EXPECT_EQ(zoom, 3.0f);
}

TEST(FitReplayCamera, FitsToBoundingBox) {
    ReplayState s;
    s.recording = FlightRecorder{};
    // Snapshots span x=[0, 1000], y=[100, 500]
    s.recording->record(make_snap(0, 0.0, 1, 0.0, 100.0, 0));
    s.recording->record(make_snap(1, 0.1, 1, 500.0, 300.0, 0));
    s.recording->record(make_snap(2, 0.2, 1, 1000.0, 500.0, 0));
    s.entity_ids = {1};

    float cx = 0.0f, cy = 0.0f, zoom = 1.0f;
    fit_replay_camera(s, 800.0f, 600.0f, cx, cy, zoom);

    // Center should be roughly at the bbox center
    // bbox with 10% margin: x=[-100, 1100], y=[60, 540]
    // center = (500, 300)
    EXPECT_NEAR(cx, 500.0f, 1.0f);
    EXPECT_NEAR(cy, 300.0f, 1.0f);
    // Zoom should fit the larger dimension
    // x range: 1200 ft, y range: 480 ft
    // zoom_x = 800/1200 = 0.667, zoom_y = 600/480 = 1.25
    // min = 0.667, * 0.9 = 0.6
    EXPECT_NEAR(zoom, 0.6f, 0.05f);
    EXPECT_GT(zoom, 0.0f);
}

TEST(FitReplayCamera, IncludesTargetPositions) {
    ReplayState s;
    s.recording = FlightRecorder{};
    auto snap = make_snap(0, 0.0, 1, 0.0, 0.0, 0);
    snap.target_position = f4::geo::WorldPosition(2000.0, 1000.0, 0);
    s.recording->record(snap);
    s.entity_ids = {1};

    float cx = 0.0f, cy = 0.0f, zoom = 1.0f;
    fit_replay_camera(s, 800.0f, 600.0f, cx, cy, zoom);

    // bbox should include the target_position, so center should be
    // between (0,0) and (2000,1000)
    EXPECT_GT(cx, 500.0f);
    EXPECT_LT(cx, 1500.0f);
    EXPECT_GT(cy, 250.0f);
    EXPECT_LT(cy, 750.0f);
}

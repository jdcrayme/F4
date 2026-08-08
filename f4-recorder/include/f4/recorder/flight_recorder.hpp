// f4-recorder/include/f4/recorder/flight_recorder.hpp
//
// FlightRecorder — captures per-tick FlightSnapshots and exports them as JSON.
//
// This is the core recording primitive. A headless simulation loop calls
// record() each tick; after the run, to_json() produces a self-contained
// JSON document that the world viewer can load for replay, or an LLM can
// consume for debugging.
//
// The JSON format is designed for three consumers:
//   1. The world viewer replay mode (loads all snapshots, steps through time)
//   2. LLM debugging (phase-level summaries with anomaly flags)
//   3. Regression testing (diff against baseline traces)
//
// Dependencies: f4-geo, f4-json. C++20.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

#include "f4/recorder/snapshot.hpp"

namespace f4::recorder {

// ============================================================================
// FlightRecorder — accumulates snapshots and exports to JSON.
// ============================================================================
class FlightRecorder {
public:
    // --- Recording ---
    void record(const FlightSnapshot& snapshot) {
        snapshots_.push_back(snapshot);
    }

    void record(FlightSnapshot&& snapshot) {
        snapshots_.push_back(std::move(snapshot));
    }

    // --- Access ---
    [[nodiscard]] const std::vector<FlightSnapshot>& snapshots() const noexcept {
        return snapshots_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return snapshots_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return snapshots_.empty();
    }

    // Filter by entity ID (for multi-aircraft recordings).
    [[nodiscard]] std::vector<FlightSnapshot> snapshots_for(
        std::uint64_t entity_id) const;

    // Filter by time range.
    [[nodiscard]] std::vector<FlightSnapshot> snapshots_in_range(
        double t0_s, double t1_s) const;

    // --- JSON export (full trace) ---
    // Produces a JSON document containing every snapshot. This is the format
    // the world viewer loads for replay.
    //
    // Format:
    //   {
    //     "format": "f4-flight-recording",
    //     "version": 1,
    //     "scenario": "...",
    //     "snapshot_count": N,
    //     "snapshots": [ { ... }, ... ]
    //   }
    [[nodiscard]] std::string to_json(
        const std::string& scenario_name = {}) const;

    // --- JSON export (LLM-friendly summary) ---
    // Produces a phase-level summary instead of per-tick data. Each "phase"
    // is a contiguous range of ticks where the AI state is the same.
    // Anomalies are flagged when cross-track error or vertical error
    // exceeds a tolerance.
    //
    // Format:
    //   {
    //     "format": "f4-flight-summary",
    //     "version": 1,
    //     "scenario": "...",
    //     "aircraft": { ... },
    //     "phases": [ { "name", "tick_range", "duration_s", ... }, ... ],
    //     "anomalies": [ { ... }, ... ],
    //     "trace_summary": { "state_sequence", ... }
    //   }
    [[nodiscard]] std::string to_summary_json(
        const std::string& scenario_name = {},
        double cross_track_tolerance_ft = 100.0,
        double vertical_tolerance_ft = 50.0) const;

    // --- JSON import (for replay) ---
    static FlightRecorder from_json(const std::string& json_str);

    // --- File I/O ---
    void write_json(const std::filesystem::path& path,
                     const std::string& scenario_name = {}) const;

    void write_summary_json(const std::filesystem::path& path,
                             const std::string& scenario_name = {},
                             double cross_track_tolerance_ft = 100.0,
                             double vertical_tolerance_ft = 50.0) const;

    static FlightRecorder load_json(const std::filesystem::path& path);

    // --- Metadata ---
    void set_scenario_name(std::string name) { scenario_name_ = std::move(name); }
    [[nodiscard]] const std::string& scenario_name() const noexcept { return scenario_name_; }

private:
    std::vector<FlightSnapshot> snapshots_;
    std::string scenario_name_;
};

} // namespace f4::recorder

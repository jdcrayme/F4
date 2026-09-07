// f4-recorder/tests/test_fcs_trace.cpp
//
// Unit tests for the FcsTraceWriter CSV exporter. Verifies header row,
// column count, basic round-trip, escaping, and file I/O.

#include "f4/recorder/fcs_trace.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <cstdlib>  // std::stod

namespace f4::recorder {
namespace {

// Helper: parse a CSV string into rows of cells (no quoted-comma handling
// — the test inputs deliberately avoid embedded commas).
std::vector<std::vector<std::string>> parse_csv(const std::string& csv) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    for (char c : csv) {
        if (c == ',') {
            row.push_back(cell);
            cell.clear();
        } else if (c == '\n') {
            row.push_back(cell);
            rows.push_back(std::move(row));
            row.clear();
            cell.clear();
        } else {
            cell.push_back(c);
        }
    }
    if (!cell.empty() || !row.empty()) {
        row.push_back(cell);
        rows.push_back(std::move(row));
    }
    return rows;
}

FcsTraceSample make_sample(std::uint64_t tick, double alt) {
    FcsTraceSample s;
    s.tick = tick;
    s.sim_time_s = static_cast<double>(tick) / 60.0;
    s.time_scale = 1.0;
    s.ai_mode = "TakeoffMode";
    s.ai_state = "FlyOut";
    s.pitch_cmd = 0.1;
    s.roll_cmd = 0.0;
    s.yaw_cmd = 0.0;
    s.throttle_cmd = 1.0;
    s.aoacmd_deg = 4.5;
    s.alpha_deg = 4.3;
    s.vcas_kts = 250.0;
    s.alt_msl_ft = alt;
    s.alt_agl_ft = alt;
    s.vs_fpm = 0.0;
    s.heading_deg = 0.0;
    s.pitch_deg = 4.3;
    s.roll_deg = 0.0;
    s.on_ground = false;
    s.gear_pos = 0.0;
    s.engine_rpm = 1.0;
    return s;
}

TEST(FcsTraceTest, EmptyWriterProducesHeaderOnly) {
    FcsTraceWriter w;
    std::ostringstream os;
    w.write_csv(os);
    const std::string csv = os.str();
    ASSERT_FALSE(csv.empty());

    const auto rows = parse_csv(csv);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0], "tick");
    // PHUG-PLAN P0.3: the last column is now the AirSteering energy term.
    EXPECT_EQ(rows[0].back(), "ai_energy_err_ft");
}

TEST(FcsTraceTest, HeaderHasExpectedColumnCount) {
    FcsTraceWriter w;
    std::ostringstream os;
    w.write_csv(os);
    const auto rows = parse_csv(os.str());
    ASSERT_EQ(rows.size(), 1u);
    // FcsTraceSample has 84 columns — keep this in sync with the struct.
    // 53 baseline + 31 PHUG-PLAN P0.3 loop-diagnostic columns (9 plant,
    // 7 FCS internals, 15 AirSteering cascade). Counted by the field list
    // in fcs_trace.hpp and the header in fcs_trace.cpp.
    EXPECT_EQ(rows[0].size(), 84u);
}

TEST(FcsTraceTest, LoopDiagnosticColumnsRoundTrip) {
    // PHUG-PLAN P0.3: the loop-attribution columns must round-trip with
    // values distinguishable from their defaults (0.0/0).
    FcsTraceSample s = make_sample(7, 8000.0);
    s.qsom = 98.7;
    s.qbar = 212.3;
    s.gamma_deg = -3.25;
    s.vt_dot = -1.5;
    s.thrust_accel = 3.4;
    s.stall_state = 2;
    s.tef_pos = 1.0;
    s.lef_pos = 0.6;
    s.dbrake_pos = 0.25;
    s.alpha_bias_deg = 5.75;
    s.q_damper_term = -0.12;
    s.omega_sp = 1.7;
    s.zp01 = 0.9;
    s.tp02 = 0.4;
    s.tp03 = 0.8;
    s.pi_error = 0.05;
    s.ai_alt_err_ft = -320.5;
    s.ai_vs_corr_fpm = 410.0;
    s.ai_vs_target_fpm = -845.0;
    s.ai_vs_ff_fpm = -845.0;
    s.ai_gamma_now_rad = -0.06;
    s.ai_gamma_ff_rad = -0.05;
    s.ai_gamma_corr_rad = 0.02;
    s.ai_alpha_est_rad = 0.20;
    s.ai_theta_target_rad = 0.15;
    s.ai_alt_integral_fpm = 120.0;
    s.ai_hdg_err_rad = 0.01;
    s.ai_bank_target_rad = 0.0;
    s.ai_speed_err_kt = 4.5;
    s.ai_speed_integral = -0.02;
    s.ai_energy_err_ft = -180.0;

    FcsTraceWriter w;
    w.record(s);
    std::ostringstream os;
    w.write_csv(os);
    const auto rows = parse_csv(os.str());
    ASSERT_EQ(rows.size(), 2u);

    // Locate the new columns by header name so this test is robust against
    // future column reordering.
    const auto& header = rows[0];
    auto col = [&header](const std::string& name) -> std::size_t {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == name) return i;
        }
        ADD_FAILURE() << "missing column: " << name;
        return 0;
    };
    const auto& row = rows[1];
    EXPECT_EQ(row[col("stall_state")], "2");
    EXPECT_DOUBLE_EQ(std::stod(row[col("qsom")]), 98.7);
    EXPECT_DOUBLE_EQ(std::stod(row[col("gamma_deg")]), -3.25);
    EXPECT_DOUBLE_EQ(std::stod(row[col("alpha_bias_deg")]), 5.75);
    EXPECT_DOUBLE_EQ(std::stod(row[col("q_damper_term")]), -0.12);
    EXPECT_DOUBLE_EQ(std::stod(row[col("omega_sp")]), 1.7);
    EXPECT_DOUBLE_EQ(std::stod(row[col("pi_error")]), 0.05);
    EXPECT_DOUBLE_EQ(std::stod(row[col("ai_alt_err_ft")]), -320.5);
    EXPECT_DOUBLE_EQ(std::stod(row[col("ai_vs_target_fpm")]), -845.0);
    EXPECT_DOUBLE_EQ(std::stod(row[col("ai_theta_target_rad")]), 0.15);
    EXPECT_DOUBLE_EQ(std::stod(row[col("ai_speed_err_kt")]), 4.5);
}

TEST(FcsTraceTest, OneSampleProducesOneDataRow) {
    FcsTraceWriter w;
    w.record(make_sample(1, 5000.0));
    std::ostringstream os;
    w.write_csv(os);
    const auto rows = parse_csv(os.str());
    ASSERT_EQ(rows.size(), 2u);  // header + 1 data row
    EXPECT_EQ(rows[1].size(), rows[0].size());
    // First column is tick (an integer formatted as such).
    EXPECT_EQ(rows[1][0], "1");
}

TEST(FcsTraceTest, MultipleSamplesPreserveOrder) {
    FcsTraceWriter w;
    w.record(make_sample(1, 5000.0));
    w.record(make_sample(2, 5100.0));
    w.record(make_sample(3, 5200.0));
    std::ostringstream os;
    w.write_csv(os);
    const auto rows = parse_csv(os.str());
    ASSERT_EQ(rows.size(), 4u);  // header + 3 data rows
    EXPECT_EQ(rows[1][0], "1");
    EXPECT_EQ(rows[2][0], "2");
    EXPECT_EQ(rows[3][0], "3");
}

TEST(FcsTraceTest, NumericValuesAreFormattedWithSixDecimals) {
    FcsTraceWriter w;
    auto s = make_sample(1, 5000.0);
    s.aoacmd_deg = 4.5;
    w.record(s);
    std::ostringstream os;
    w.write_csv(os);
    const auto rows = parse_csv(os.str());
    ASSERT_EQ(rows.size(), 2u);
    // Find the aoacmd_deg column (it's the 16th, 0-indexed 15).
    // Verify the value is formatted with exactly 6 decimal places.
    EXPECT_EQ(rows[1][15], "4.500000");
}

TEST(FcsTraceTest, BooleanFieldsEmitZeroOrOne) {
    FcsTraceWriter w;
    auto s = make_sample(1, 5000.0);
    s.gear_down = true;
    s.wheel_brakes = false;
    w.record(s);
    std::ostringstream os;
    w.write_csv(os);
    const auto rows = parse_csv(os.str());
    ASSERT_EQ(rows.size(), 2u);
    // gear_down column = index 12, wheel_brakes = 13
    EXPECT_EQ(rows[1][12], "1");
    EXPECT_EQ(rows[1][13], "0");
}

TEST(FcsTraceTest, AIStateNamesWithCommasAreQuoted) {
    FcsTraceWriter w;
    auto s = make_sample(1, 5000.0);
    s.ai_state = "OnFinal, GearDown";  // contains a comma
    w.record(s);
    std::ostringstream os;
    w.write_csv(os);
    const std::string csv = os.str();
    // The cell must be quoted.
    EXPECT_NE(csv.find("\"OnFinal, GearDown\""), std::string::npos);
}

TEST(FcsTraceTest, FileWriteSucceedsAndContentMatches) {
    FcsTraceWriter w;
    w.record(make_sample(1, 5000.0));
    w.record(make_sample(2, 5100.0));

    const std::string path = "test_fcs_trace_tmp.csv";
    w.write_csv(path);

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.good());
    std::stringstream ss;
    ss << ifs.rdbuf();
    ifs.close();
    std::remove(path.c_str());

    const auto rows = parse_csv(ss.str());
    ASSERT_EQ(rows.size(), 3u);  // header + 2 data rows
    EXPECT_EQ(rows[1][0], "1");
    EXPECT_EQ(rows[2][0], "2");
}

TEST(FcsTraceTest, ClearDropsAllSamples) {
    FcsTraceWriter w;
    w.record(make_sample(1, 5000.0));
    EXPECT_EQ(w.size(), 1u);
    EXPECT_FALSE(w.empty());
    w.clear();
    EXPECT_EQ(w.size(), 0u);
    EXPECT_TRUE(w.empty());
}

TEST(FcsTraceTest, FileWriteThrowsOnBadPath) {
    FcsTraceWriter w;
    w.record(make_sample(1, 5000.0));
    EXPECT_THROW(w.write_csv("/nonexistent/dir/trace.csv"), std::runtime_error);
}

} // namespace
} // namespace f4::recorder

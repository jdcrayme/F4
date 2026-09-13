// f4-simulation/include/f4/simulation/weather_system.hpp
//
// WeatherSystem — the deterministic environment driver (Task 73, Weather
// v1). Owns the theater-uniform WeatherState (f4-world-types) plus the
// campaign clock's time-of-day, evolves both on sim time, and hands the
// detection model its combined visual scale.
//
// FreeFalcon reference (campaign/include/weather.h):
//   - WeatherClass::UpdateCondition(condition) — the 3-state condition
//     model; v1 steers the state toward per-condition profiles.
//   - lockedCondition — the scenario author's "freeze the weather" flag;
//     v1 keeps the exact concept (config.locked).
//   - lastCheck / the periodic re-derive — v1's check cadence is a
//     documented 15 campaign-minutes (FreeFalcon re-derives on a similar
//     cadence with a condition counter).
//
// Determinism contract: the SAME Options + the SAME advance() call
// sequence produce the SAME state sequence — the evolution draws from a
// seeded mt19937 and NOTHING reads wall time, global RNGs, or float
// summation order that could drift between hosts. Two identically-driven
// sessions see identical weather (the C2 discipline, world edition).
//
// Time-of-day: the system advances a seconds-of-day counter (wrapping at
// 86400) and a day-of-year counter (wrapping at 366) from the same sim
// dt; the daylight band derives from the solar model (day_night.hpp).
// Default start 43200 s (solar noon), day 172 — noon daylight, the
// zero-change default.
//
// Consumers (v1): the visual detection scale (combined_visual_scale)
// pushed to every roster brain's SensorFusion by the Simulation; wind /
// temperature / turbulence ride the state for the tranches that consume
// them (FM integration is explicitly deferred, documented in CHANGES.md).
// GCI, radar, and the IR flyout are v1-unaffected by design.

#pragma once

#include <random>
#include <cstdint>

#include <f4/world_types/day_night.hpp>
#include <f4/world_types/weather.hpp>

namespace f4::sim {

class WeatherSystem {
public:
    /// Evolution + clock configuration (scenario "weather"/"time" blocks).
    struct Options {
        /// Initial condition (scenario "weather"."condition"; default
        /// clear — the zero-change state).
        f4::world_types::WeatherCondition initial_condition{
            f4::world_types::WeatherCondition::Clear};
        /// FreeFalcon's lockedCondition: true = the condition never
        /// changes (the per-condition profile still steers in over the
        /// first check so an authored "inclement" start is REAL).
        bool locked{false};
        /// Evolution seed — the deterministic stream id.
        std::uint32_t seed{0x57E47E5};
        /// Condition re-check cadence, seconds of sim time.
        double check_interval_s{900.0};
        /// Clock start: seconds-of-day (solar) and day-of-year.
        double start_seconds_of_day{43200.0};
        int start_day_of_year{172};
        /// Theater latitude for the solar model.
        double latitude_deg{f4::world_types::kDefaultTheaterLatitudeDeg};
        /// Clock advance: false freezes time-of-day (the zero-change
        /// default for scenarios that predate the block entirely —
        /// enforced by the scenario layer, which only builds a clock
        /// when a "time" block exists).
        bool advance_clock{false};
    };

    explicit WeatherSystem(const Options& options);

    /// Advance the environment by dt seconds of sim time (the SAME dt the
    /// Simulation ticks with — one clock). Evolves the condition on the
    /// check cadence, steers the continuous fields, advances the clock.
    void advance(double dt_s);

    /// Authoritative state override (scenario-authored initial states and
    /// tests). The condition fields set here are what the first check
    /// steers FROM; the evolution takes over after.
    void set_state(const f4::world_types::WeatherState& s);

    /// Current theater state (steady between advances).
    [[nodiscard]] const f4::world_types::WeatherState& state() const noexcept {
        return state_;
    }

    /// Current daylight band (recomputed on each advance; frozen while
    /// the clock is).
    [[nodiscard]] f4::world_types::DaylightBand band() const noexcept {
        return band_;
    }

    /// Current seconds-of-day (solar) and day-of-year.
    [[nodiscard]] double seconds_of_day() const noexcept {
        return seconds_of_day_;
    }
    [[nodiscard]] int day_of_year() const noexcept { return day_of_year_; }

    /// The combined visual scale the detection model consumes — the
    /// single number the Simulation pushes to every roster brain.
    [[nodiscard]] double visual_scale() const noexcept {
        return f4::world_types::combined_visual_scale(state_, band_);
    }

    /// True when the clock is running (scenario "time"."advance").
    [[nodiscard]] bool clock_running() const noexcept {
        return options_.advance_clock;
    }

private:
    /// One condition re-check: maybe draw a new condition, refresh the
    /// steered field targets from the (possibly new) profile.
    void check_condition_(double sim_time_s);

    /// Steer the continuous fields toward their targets (exponential
    /// approach — weather never steps discontinuously).
    void steer_fields_(double dt_s);

    /// Refresh the daylight band from the current clock.
    void refresh_band_();

    Options options_;
    f4::world_types::WeatherState state_{};
    f4::world_types::DaylightBand band_{f4::world_types::DaylightBand::Day};

    // Clock (the initializer list runs top-to-bottom in DECLARATION
    // order — keep the constructor's list aligned with this block).
    double seconds_of_day_{43200.0};
    int day_of_year_{172};

    // Determinism: the seeded stream + the check-cadence timer.
    std::mt19937 rng_;
    double check_timer_s_{0.0};
    bool checked_once_{false};

    // Steered-field targets (re-derived per condition check).
    double target_visibility_nm_{40.0};
    double target_cover_{0.0};
    double target_base_ft_{0.0};
    double target_temperature_c_{15.0};
    double target_turb_{0.0};
    double target_wind_low_kts_{0.0};
    double target_wind_med_kts_{0.0};
    double target_wind_high_kts_{0.0};
};

} // namespace f4::sim

// f4-simulation/src/weather_system.cpp
//
// WeatherSystem implementation — see weather_system.hpp for the contract.

#include "f4/simulation/weather_system.hpp"

#include <algorithm>
#include <cmath>

namespace f4::sim {

namespace {

constexpr double kSecondsPerDay = 86400.0;
constexpr int kDaysPerYear = 366;

/// The field-approach time constant (seconds): after a condition change
/// the continuous fields are ~63% of the way there in one tau, ~95% in
/// three. 600 s (10 min) makes a Clear -> Inclement flip a readable
/// front passage over the check cadence, not a light switch.
constexpr double kFieldTauS = 600.0;

/// Jitter fractions applied to the profile targets at each check — the
/// seeded per-check variation that keeps a two-hour war from cycling a
/// single canned value set. Visibility +-20%, cover +-1 tenth, wind
/// +-(1 + fraction), temperature +-1.5 C.
constexpr double kVisJitter = 0.20;
constexpr double kCoverJitter = 1.0;    // absolute tenths
constexpr double kWindJitter = 0.25;
constexpr double kTempJitterC = 1.5;

double approach(double current, double target, double tau_s, double dt) {
    if (tau_s <= 0.0 || dt <= 0.0) return target;
    const double k = 1.0 - std::exp(-dt / tau_s);
    return current + (target - current) * k;
}

double wrap_days(double seconds) noexcept {
    const double w = std::fmod(seconds, kSecondsPerDay);
    return w < 0.0 ? w + kSecondsPerDay : w;
}

} // namespace

WeatherSystem::WeatherSystem(const Options& options)
    : options_(options),
      state_{},
      seconds_of_day_(wrap_days(options.start_seconds_of_day)),
      day_of_year_(std::clamp(options.start_day_of_year, 1, kDaysPerYear)),
      rng_(options.seed) {
    // The initial condition IS the state (the scenario author's or the
    // default clear day); the steered targets start AT the profile so a
    // locked initial state is fully real immediately.
    state_.condition = options_.initial_condition;
    const auto& p = f4::world_types::profile_for(state_.condition);
    target_visibility_nm_ = p.visibility_nm;
    target_cover_ = p.cloud_cover_tenths;
    target_base_ft_ = p.cloud_base_ft;
    target_temperature_c_ = 15.0 + p.temperature_offset_c;
    target_turb_ = p.turb_factor;
    target_wind_low_kts_ = p.wind_speed_kts;
    target_wind_med_kts_ = p.wind_speed_kts * 1.5;
    target_wind_high_kts_ = p.wind_high_kts;
    state_.visibility_nm = target_visibility_nm_;
    state_.cloud_cover_tenths = target_cover_;
    state_.cloud_base_ft = target_base_ft_;
    state_.temperature_c = target_temperature_c_;
    state_.turb_factor = target_turb_;
    state_.wind_low = {270.0, target_wind_low_kts_};
    state_.wind_medium = {270.0, target_wind_med_kts_};
    state_.wind_high = {240.0, target_wind_high_kts_};
    refresh_band_();
}

void WeatherSystem::set_state(const f4::world_types::WeatherState& s) {
    state_ = s;
    const auto& p = f4::world_types::profile_for(state_.condition);
    target_visibility_nm_ = p.visibility_nm;
    target_cover_ = p.cloud_cover_tenths;
    target_base_ft_ = p.cloud_base_ft;
    target_temperature_c_ = 15.0 + p.temperature_offset_c;
    target_turb_ = p.turb_factor;
    target_wind_low_kts_ = p.wind_speed_kts;
    target_wind_med_kts_ = p.wind_speed_kts * 1.5;
    target_wind_high_kts_ = p.wind_high_kts;
    refresh_band_();
}

void WeatherSystem::advance(double dt_s) {
    if (dt_s <= 0.0) return;

    // The clock first (the band tracks it), unless the scenario froze it.
    if (options_.advance_clock) {
        const double pre = seconds_of_day_;
        seconds_of_day_ = wrap_days(pre + dt_s);
        if (seconds_of_day_ < pre) {
            // Wrapped past midnight — the day advances with it (366
            // wraps to 1; the solar model clamps internally anyway).
            day_of_year_ = day_of_year_ >= kDaysPerYear
                               ? 1
                               : day_of_year_ + 1;
        }
        refresh_band_();
    }

    // Condition evolution: on the cadence, unless locked. checked_once_
    // makes the FIRST advance() the first check — an authored initial
    // condition steers in from t=0.
    check_timer_s_ += dt_s;
    if (!options_.locked &&
        (check_timer_s_ >= options_.check_interval_s || !checked_once_)) {
        check_condition_(check_timer_s_);
        check_timer_s_ = 0.0;
        checked_once_ = true;
    }

    steer_fields_(dt_s);
}

void WeatherSystem::check_condition_(double sim_time_s) {
    (void)sim_time_s;
    // The Markov draw — the v1 transition weights (rows sum to 1.0;
    // documented in CHANGES.md Task 73). A uniform draw on [0, 1) walks
    // each row's cumulative intervals.
    constexpr double kTrans[3][3] = {
        //  to: Clear  Hazy   Inclement
        {0.70, 0.25, 0.05}, // from Clear
        {0.40, 0.45, 0.15}, // from Hazy
        {0.15, 0.40, 0.45}, // from Inclement
    };
    const int from = static_cast<int>(state_.condition);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double draw = unit(rng_);
    double cum = 0.0;
    int to = 2;
    for (int k = 0; k < 3; ++k) {
        cum += kTrans[from][k];
        if (draw < cum) {
            to = k;
            break;
        }
    }
    state_.condition = static_cast<f4::world_types::WeatherCondition>(to);

    // Refresh the targets from the (possibly new) condition's profile,
    // with the seeded per-check jitter.
    const auto& p = f4::world_types::profile_for(state_.condition);
    std::uniform_real_distribution<double> jitter(-1.0, 1.0);
    target_visibility_nm_ =
        std::max(1.0, p.visibility_nm *
                          (1.0 + kVisJitter * jitter(rng_)));
    target_cover_ = std::clamp(p.cloud_cover_tenths +
                                   kCoverJitter * jitter(rng_),
                               0.0, 10.0);
    target_base_ft_ = std::max(0.0, p.cloud_base_ft);
    target_temperature_c_ = 15.0 + p.temperature_offset_c +
                            kTempJitterC * jitter(rng_);
    target_turb_ = std::clamp(p.turb_factor, 0.0, 1.0);
    target_wind_low_kts_ =
        std::max(0.0, p.wind_speed_kts * (1.0 + kWindJitter * jitter(rng_)));
    target_wind_med_kts_ =
        std::max(0.0, target_wind_low_kts_ * 1.5);
    target_wind_high_kts_ =
        std::max(0.0, p.wind_high_kts * (1.0 + kWindJitter * jitter(rng_)));

    // Wind direction: a fresh seeded bearing per band per check — the
    // weather's own drift, never a fixed wind rose.
    std::uniform_real_distribution<double> bearing(0.0, 360.0);
    state_.wind_low.dir_deg = bearing(rng_);
    state_.wind_medium.dir_deg = bearing(rng_);
    state_.wind_high.dir_deg = bearing(rng_);
}

void WeatherSystem::steer_fields_(double dt_s) {
    state_.visibility_nm = approach(state_.visibility_nm,
                                    target_visibility_nm_, kFieldTauS, dt_s);
    state_.cloud_cover_tenths =
        approach(state_.cloud_cover_tenths, target_cover_, kFieldTauS, dt_s);
    state_.cloud_base_ft = approach(state_.cloud_base_ft, target_base_ft_,
                                    kFieldTauS, dt_s);
    state_.temperature_c = approach(state_.temperature_c,
                                    target_temperature_c_, kFieldTauS, dt_s);
    state_.turb_factor =
        approach(state_.turb_factor, target_turb_, kFieldTauS, dt_s);
    state_.wind_low.speed_kts =
        approach(state_.wind_low.speed_kts, target_wind_low_kts_,
                 kFieldTauS, dt_s);
    state_.wind_medium.speed_kts =
        approach(state_.wind_medium.speed_kts, target_wind_med_kts_,
                 kFieldTauS, dt_s);
    state_.wind_high.speed_kts =
        approach(state_.wind_high.speed_kts, target_wind_high_kts_,
                 kFieldTauS, dt_s);
}

void WeatherSystem::refresh_band_() {
    band_ = f4::world_types::daylight_band_at(seconds_of_day_, day_of_year_,
                                        options_.latitude_deg);
}

} // namespace f4::sim

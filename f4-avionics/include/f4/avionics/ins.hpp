// f4-avionics/include/f4/avionics/ins.hpp
//
// INS — the inertial navigation unit: the alignment state machine, the
// stored heading/altitude/position chain, and the seeded deterministic
// drift walk.
//
// AVIONICS-1 (Docs/AVIONICS_PLAN.md §4). The plan's charter boundary drawn
// once more: this is pure avionics LOGIC — the unit consumes aircraft truth
// through f4-flight-api's IAircraftState (the plan's seam) and reports the
// chain a cockpit would read. It never draws, never reads devices, and is
// fully deterministic: no wall clock, the alignment clock runs on the sim
// deltas the caller feeds, and the drift walk is a seeded deterministic
// walk keyed on the airframe's nav data.
//
// SCOPE (v1, the error-model decision): the INS maintains the BELIEVED
// chain as truth-plus-error, not a full acceleration-integration inertial
// solver. Rationale: the hosts hand the unit an IAircraftState (position,
// heading, altitude — the same seam the AI consumes), and every avionics
// consumer (HSI, FCR page, HUD boxes) reads the believed chain relative to
// steerpoints. Modeling the ERROR (alignment zeroing + a bounded rate walk)
// gives that read honestly and deterministically; integrating velocities
// open-loop would add an Euler-integration divergence no consumer asked
// for. A full strapdown integrator behind the same interface is a named
// later tranche (AVIONICS_PLAN.md §4 AVIONICS-1 as-built note).
//
// DETERMINISM: the drift walk uses a fully specified splitmix64 sampler —
// NOT std::mt19937 + std:: distributions, whose output sequences are
// implementation-defined across standard libraries. Same seed + same
// update stream => byte-identical believed chain on every platform.
//
// C++20. Header-only.

#pragma once

#include "f4/flight/api/i_aircraft_state.hpp"
#include "f4/geo/f4_geo.hpp"
#include "f4/fsm/f4_fsm.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace f4::avionics {

// ============================================================================
// Alignment states & events
// ============================================================================

enum class InsState {
    Off,        ///< powered down: no believed chain, no drift
    Aligning,   ///< ground align running (the clock accrues only while on_ground)
    Aligned,    ///< navigational quality: the chain is live, drift accrues
};

enum class InsEvent {
    PowerOn,     ///< Off -> Aligning
    AlignTimer,  ///< Aligning -> Aligned (sent by the bridge when the ground
                 ///  clock has accrued align_time_s)
    Shutdown,    ///< Aligning/Aligned -> Off (the chain is discarded)
};

// ============================================================================
// detail — deterministic sampling + angle helpers
// ============================================================================

namespace detail {

inline constexpr std::uint64_t GOLDEN_GAMMA = 0x9E3779B97F4A7C15ULL;

/// splitmix64 — fully specified 64-bit mixer (Steele et al. 2014). Used as
/// the drift walk's sampler so the sequence is byte-identical across
/// compilers and standard libraries (std::shuffle/normal_distribution make
/// no cross-platform sequence guarantees).
[[nodiscard]] inline std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    std::uint64_t z = (state += GOLDEN_GAMMA);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// FNV-1a 64 over a byte string — the nav-data key's hash (same primitive
/// the campaign's vu_hash uses; std::hash is not stable across platforms).
[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view s) noexcept {
    std::uint64_t h = 0xCBF29CE484222325ULL;   // offset basis
    for (const char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ULL;                 // FNV prime
    }
    return h;
}

/// Uniform sample in [-1, +1) from 53 random bits — no libc, no
/// implementation-defined distribution algorithms.
[[nodiscard]] inline double uniform_pm1(std::uint64_t& state) noexcept {
    const std::uint64_t u = splitmix64(state) >> 11;   // top 53 bits
    return static_cast<double>(u) * (1.0 / 4503599627370496.0) - 1.0;  // 2^52
}

[[nodiscard]] inline double clampd(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Wrap to [0, 2*pi).
[[nodiscard]] inline double wrap_two_pi(double a) noexcept {
    double r = std::fmod(a, f4::math::TWO_PI);
    if (r < 0.0) r += f4::math::TWO_PI;
    return r;
}

/// Wrap to [-pi, +pi).
[[nodiscard]] inline double wrap_pi_pi(double a) noexcept {
    double r = std::fmod(a + f4::math::PI, f4::math::TWO_PI);
    if (r < 0.0) r += f4::math::TWO_PI;
    return r - f4::math::PI;
}

}  // namespace detail

// ============================================================================
// InsConfig
// ============================================================================

/// The INS's data card. Defaults model a healthy post-alignment INS; the
/// drift magnitudes are sim-facing tunables, not measured F-16 values —
/// the plan's fidelity bar for v1 is "deterministic, bounded, keyed on the
/// airframe's nav data", with measured constants a data tranche if a host
/// ever pins them.
struct InsConfig {
    /// Ground-align duration (seconds of GROUND clock — the clock only
    /// accrues while the aircraft is on_ground()).
    double align_time_s{300.0};

    /// The airframe's nav-database identity (e.g. "F-16C blk40 Korea
    /// 2003-06"). The drift walk's seed is derived from this when
    /// `seed` is 0 — two airframes with the same nav data drift alike,
    /// different nav data walks differently.
    std::string nav_data_key{};

    /// Nav-database age in days. Scales the walk's step size (older data
    /// drifts harder) and mixes into the seed, so the same key at a
    /// different age is a different walk.
    double nav_data_age_days{0.0};

    /// Per-axis clamp on the walked position-error rate (ft/s, ENU each).
    /// 2.0 ft/s ~= 1.2 nm/h at saturation.
    double max_pos_drift_rate_fps{2.0};

    /// Clamp on the walked heading-error rate (rad/s).
    /// 2.5e-7 rad/s ~= 0.05 deg/h at saturation.
    double max_heading_drift_rate_radps{2.5e-7};

    /// Explicit walk seed. 0 (the default) derives the seed from
    /// nav_data_key; a nonzero value overrides it (scenario reproducibility).
    std::uint64_t seed{0};

    /// The drift-zero twin: when false the walk never runs and the believed
    /// chain compares equal to raw truth member-for-member — the byte
    /// identity the plan's done-when pins. Existing hosts run with drift
    /// off until they opt in.
    bool drift_enabled{true};

    /// Clamp the tunables to their documented domains (mirrors
    /// PilotInput::validate() — clamping, not throwing, at the seam).
    void validate() noexcept {
        if (align_time_s < 0.0) align_time_s = 0.0;
        if (nav_data_age_days < 0.0) nav_data_age_days = 0.0;
        if (max_pos_drift_rate_fps < 0.0) max_pos_drift_rate_fps = 0.0;
        if (max_heading_drift_rate_radps < 0.0) max_heading_drift_rate_radps = 0.0;
    }
};

// ============================================================================
// The alignment machine
// ============================================================================

using InsSm = fsm::StateMachine<InsState, InsEvent>;

/// The alignment machine as a PURE transition table — no captures, no
/// side effects. Side effects (clock accrual, the completion reset/seed)
/// live in InsUnit::update(), the polling->event bridge, the same
/// discipline as the flight model's stall SM (stall_state.hpp). Every
/// transition (and every refusal that matters) is pinned in test_ins.cpp.
[[nodiscard]] inline InsSm make_ins_machine() {
    return InsSm::Builder()
        .initial(InsState::Off)
        .state(InsState::Off, "Off")
        .state(InsState::Aligning, "Aligning")
        .state(InsState::Aligned, "Aligned")
        .event_name(InsEvent::PowerOn, "PowerOn")
        .event_name(InsEvent::AlignTimer, "AlignTimer")
        .event_name(InsEvent::Shutdown, "Shutdown")
        .on(InsState::Off, InsState::Aligning, InsEvent::PowerOn,
            nullptr, nullptr, "align start: the ground clock begins")
        .on(InsState::Aligning, InsState::Aligned, InsEvent::AlignTimer,
            nullptr, nullptr, "align complete: chain zeroed, walk seeded")
        .on(InsState::Aligning, InsState::Off, InsEvent::Shutdown,
            nullptr, nullptr, "shutdown during align")
        .on(InsState::Aligned, InsState::Off, InsEvent::Shutdown,
            nullptr, nullptr, "shutdown: the chain is discarded")
        .build();
}

// ============================================================================
// InsUnit
// ============================================================================

/// The inertial navigation unit.
///
/// Lifecycle: power_on() -> Aligning (the align clock accrues only while
/// the fed truth is on_ground) -> Aligned (the believed chain goes live and
/// the drift walk starts from zero) -> shutdown() discards the chain.
///
/// The believed chain ("the stored heading/altitude/position chain") is the
/// last update()'s truth sample plus the accumulated drift — before
/// alignment completes the drift is identically zero, so the unit reports
/// raw truth. Between update() calls the chain holds (the cockpit reads it
/// at its own cadence); the first update() before any truth arrives reports
/// the zero chain.
class InsUnit final {
public:
    /// What the cockpit reads: the believed ENU position (feet; z is
    /// altitude MSL), the believed true heading ([0, 2*pi), 0 = north,
    /// clockwise positive), and the believed altitude MSL (feet).
    struct Solution {
        geo::WorldPosition position{};
        double heading_rad{0.0};
        double altitude_msl_ft{0.0};

        auto operator<=>(const Solution&) const = default;
    };

    explicit InsUnit(InsConfig cfg = {})
        : cfg_(std::move(cfg)), machine_(make_ins_machine())
    {
        cfg_.validate();
    }

    // The unit is stateful (walk state + alignment clock) — no copies
    // (two units must never share/clone a walk), but moves are fine: the
    // alignment machine is capture-free, so relocation is a plain member
    // move. This is what lets factories return units by value.
    InsUnit(const InsUnit&) = delete;
    InsUnit& operator=(const InsUnit&) = delete;
    InsUnit(InsUnit&&) = default;
    InsUnit& operator=(InsUnit&&) = default;

    /// Drive the unit with one sim delta. dt is clamped at 0 (the INS has
    /// no wall clock and no opinion about backwards time). The same seed
    /// fed the same dt stream produces the byte-identical chain.
    void update(double dt, const flight::IAircraftState& truth) {
        if (dt < 0.0) dt = 0.0;
        cache_truth(truth);

        switch (machine_.current()) {
            case InsState::Off:
                break;   // no believed chain; solution() reports raw truth
            case InsState::Aligning:
                if (truth.on_ground()) {
                    align_elapsed_s_ += dt;
                }
                if (align_elapsed_s_ >= cfg_.align_time_s) {
                    const InsState before = machine_.current();
                    machine_.process(InsEvent::AlignTimer);
                    if (machine_.current() != before) {
                        on_alignment_complete();
                    }
                }
                break;
            case InsState::Aligned:
                walk_drift(dt);
                break;
        }
    }

    /// Off -> Aligning. No-op while not Off (re-power mid-align does not
    /// restart the clock — shutdown first, the real switch's interlock).
    void power_on() {
        if (machine_.current() == InsState::Off) {
            align_elapsed_s_ = 0.0;
            machine_.process(InsEvent::PowerOn);
        }
    }

    /// Discard the chain: Aligning/Aligned -> Off. Drift is zeroed here
    /// (a powered-down INS holds nothing).
    void shutdown() {
        machine_.process(InsEvent::Shutdown);
        drift_pos_ = geo::WorldPosition{};
        drift_rate_ = geo::WorldPosition{};
        heading_drift_rad_ = 0.0;
        heading_drift_rate_radps_ = 0.0;
        align_elapsed_s_ = 0.0;
    }

    // --- queries ---------------------------------------------------------

    [[nodiscard]] InsState state() const noexcept { return machine_.current(); }
    [[nodiscard]] bool aligned() const noexcept { return state() == InsState::Aligned; }

    /// Alignment progress [0, 1] — 0 when Off, 1 when Aligned.
    [[nodiscard]] double align_progress() const noexcept {
        if (machine_.current() == InsState::Off) return 0.0;
        if (machine_.current() == InsState::Aligned) return 1.0;
        if (cfg_.align_time_s <= 0.0) return 1.0;
        return detail::clampd(align_elapsed_s_ / cfg_.align_time_s, 0.0, 1.0);
    }

    /// The believed chain (truth-at-last-update + drift).
    [[nodiscard]] Solution solution() const {
        const geo::WorldPosition pos(last_truth_.x + drift_pos_.x,
                                     last_truth_.y + drift_pos_.y,
                                     last_truth_.z + drift_pos_.z);
        return Solution{pos,
                        detail::wrap_two_pi(last_truth_heading_ + heading_drift_rad_),
                        pos.z};
    }

    /// The config as validated (the unit's own card — hosts/tests read it
    /// back rather than keeping a second copy).
    [[nodiscard]] const InsConfig& config() const noexcept { return cfg_; }

    /// The alignment machine (trace/inspection; the transition table is
    /// data and the tests pin it).
    [[nodiscard]] const InsSm& machine() const noexcept { return machine_; }

    // --- drift introspection (tests + diagnostics; the cockpit never
    //     reads these — the whole point of an INS is that it can't tell) --

    [[nodiscard]] const geo::WorldPosition& position_drift() const noexcept {
        return drift_pos_;
    }
    [[nodiscard]] double heading_drift_rad() const noexcept { return heading_drift_rad_; }

private:
    void cache_truth(const flight::IAircraftState& truth) {
        last_truth_ = geo::WorldPosition(truth.position_east_ft(),
                                         truth.position_north_ft(),
                                         truth.altitude_msl_ft());
        last_truth_heading_ = truth.heading_rad();
    }

    /// Alignment completion: the chain is zeroed against the align truth
    /// (that is what alignment IS) and the walk seeds from the nav data.
    void on_alignment_complete() {
        drift_pos_ = geo::WorldPosition{};
        drift_rate_ = geo::WorldPosition{};
        heading_drift_rad_ = 0.0;
        heading_drift_rate_radps_ = 0.0;
        seed_walk();
    }

    void seed_walk() {
        if (!cfg_.drift_enabled) {
            rng_state_ = 0;
            return;
        }
        const std::uint64_t key = (cfg_.seed != 0)
                                      ? cfg_.seed
                                      : detail::fnv1a64(cfg_.nav_data_key);
        std::uint64_t s = key;
        const auto age_ms = static_cast<std::uint64_t>(
            std::llround(cfg_.nav_data_age_days * 86400.0 * 1000.0));
        std::uint64_t mixed = detail::splitmix64(s);
        std::uint64_t age = age_ms;
        mixed ^= detail::splitmix64(age);
        rng_state_ = detail::splitmix64(mixed);
    }

    /// The drift integral: a bounded rate walk, integrated over dt.
    ///
    /// Per update, each axis's error RATE takes one clamped uniform step
    /// (fixed sample order: east, north, up, heading) and the position
    /// error integrates the rate. sigma scales with the nav data's age;
    /// the clamps do not (old data saturates the limit, it does not
    /// exceed it).
    void walk_drift(double dt) {
        if (!cfg_.drift_enabled || dt <= 0.0) return;
        const double age_scale = 1.0 + cfg_.nav_data_age_days / 365.0;
        const double sqrt_dt = std::sqrt(dt);

        const double pos_sigma = 0.25 * cfg_.max_pos_drift_rate_fps * age_scale;
        drift_rate_.x = detail::clampd(
            drift_rate_.x + detail::uniform_pm1(rng_state_) * pos_sigma * sqrt_dt,
            -cfg_.max_pos_drift_rate_fps, cfg_.max_pos_drift_rate_fps);
        drift_rate_.y = detail::clampd(
            drift_rate_.y + detail::uniform_pm1(rng_state_) * pos_sigma * sqrt_dt,
            -cfg_.max_pos_drift_rate_fps, cfg_.max_pos_drift_rate_fps);
        drift_rate_.z = detail::clampd(
            drift_rate_.z + detail::uniform_pm1(rng_state_) * pos_sigma * sqrt_dt,
            -cfg_.max_pos_drift_rate_fps, cfg_.max_pos_drift_rate_fps);

        const double hdg_sigma = 0.25 * cfg_.max_heading_drift_rate_radps * age_scale;
        heading_drift_rate_radps_ = detail::clampd(
            heading_drift_rate_radps_ + detail::uniform_pm1(rng_state_) * hdg_sigma * sqrt_dt,
            -cfg_.max_heading_drift_rate_radps, cfg_.max_heading_drift_rate_radps);

        drift_pos_.x += drift_rate_.x * dt;
        drift_pos_.y += drift_rate_.y * dt;
        drift_pos_.z += drift_rate_.z * dt;
        heading_drift_rad_ += heading_drift_rate_radps_ * dt;
    }

    // --- state ------------------------------------------------------------

    InsConfig cfg_;
    InsSm machine_;
    double align_elapsed_s_{0.0};     ///< the GROUND clock (accrues on_ground only)
    std::uint64_t rng_state_{0};      ///< the walk's cursor (0 = unseeded/disabled)

    geo::WorldPosition last_truth_{};      ///< truth at the last update()
    double last_truth_heading_{0.0};

    geo::WorldPosition drift_pos_{};       ///< accumulated position error (ft, ENU)
    geo::WorldPosition drift_rate_{};      ///< walked error rate (ft/s, ENU)
    double heading_drift_rad_{0.0};        ///< accumulated heading error (rad)
    double heading_drift_rate_radps_{0.0}; ///< walked heading error rate (rad/s)
};

}  // namespace f4::avionics

// f4-math/integration.hpp
//
// Templated ODE integrators, parameterized on a state concept that supports
// addition and scalar multiplication. The flight model uses these to
// advance the equations of motion (quaternion kinematics + translational
// dynamics), the FCS uses them for filter state, and the campaign uses
// them for path-length accumulation.
//
// Five integrators are provided, in increasing order of accuracy:
//
//   Euler           — 1st order, O(dt) error. Cheap, only for non-stiff
//                     throwaway integration (e.g. coarse campaign ticks).
//   Trapezoidal     — 2nd order, O(dt^2) error. Matches FF's FITust.
//   AdamsBashforth2 — 2nd order, O(dt^2) error. Matches FF's FIAdamsBash.
//                     Slightly cheaper than trapezoidal (one f eval/step
//                     vs one) at the cost of one extra history slot.
//   AdamsBashforth4 — 4th order, O(dt^4) error. Used for high-accuracy
//                     offline simulation, not the real-time loop.
//   RK4             — 4th order, O(dt^4) error. The EOM integrator at
//                     the minor-frame rate; 4 f-evals per step.
//
// The concept IntegrableState below is the same one used in the proposal.
// Concrete state types in f4-flight-model will satisfy this trivially:
//   struct EomState { Vec3 pos; Vec3 vel; Quat q; Vec3 omega; };
//   EomState operator+(EomState, EomState);
//   EomState operator*(EomState, double);
//
// Each integrator owns its own state and exposes step(f, dt). The
// derivative function f is called with const State& and must return
// a State (or anything convertible to State) representing df/dt.
//
// Bootstrapping: AB2 and AB4 require derivative history. The first N-1
// steps fall back to Euler (or, for AB2, to trapezoidal once a prior
// derivative exists). This matches the original FF behaviour where the
// integrator state is seeded at simulation start.

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <utility>

namespace f4::math {

// ============================================================================
// IntegrableState — concept for state types that support the linear algebra
// an integrator needs: state + state and state * scalar.
// ============================================================================
template<typename T>
concept IntegrableState = requires(T a, T b, double s) {
    { a + b } -> std::convertible_to<T>;
    { a - b } -> std::convertible_to<T>;
    { a * s } -> std::convertible_to<T>;
    { s * a } -> std::convertible_to<T>;
};

// ============================================================================
// EulerIntegrator — y_{n+1} = y_n + dt * f(y_n)
// ============================================================================
template<IntegrableState State>
class EulerIntegrator {
    State state_{};
public:
    EulerIntegrator() = default;
    explicit EulerIntegrator(State initial) : state_(std::move(initial)) {}

    template<typename DerivFn>
    const State& step(DerivFn&& f, double dt) {
        state_ = state_ + f(state_) * dt;
        return state_;
    }

    [[nodiscard]] const State& state() const noexcept { return state_; }
    void reset(State s) { state_ = std::move(s); }
};

// ============================================================================
// TrapezoidalIntegrator — explicit trapezoidal rule (Heun's method).
//
//   k1 = f(y_n)
//   k2 = f(y_n + dt * k1)
//   y_{n+1} = y_n + (dt/2) * (k1 + k2)
//
// This is the proper 2nd-order explicit trapezoidal rule for ODE solving.
// It is NOT the same as FF's FITust (which integrates a known input signal
// u(t) using the trapezoidal rule, not solving y' = f(y)). FF's FITust
// behaviour is provided separately by IntegratorTustin in filters.hpp.
//
// One derivative evaluation per step (plus one for the predictor), no
// history needed.
// ============================================================================
template<IntegrableState State>
class TrapezoidalIntegrator {
    State state_{};
public:
    TrapezoidalIntegrator() = default;
    explicit TrapezoidalIntegrator(State initial) : state_(std::move(initial)) {}

    template<typename DerivFn>
    const State& step(DerivFn&& f, double dt) {
        State k1 = f(state_);
        State k2 = f(state_ + k1 * dt);
        state_ = state_ + (k1 + k2) * (dt * 0.5);
        return state_;
    }

    [[nodiscard]] const State& state() const noexcept { return state_; }
    void reset(State s) { state_ = std::move(s); }
};

// ============================================================================
// AdamsBashforth2Integrator — y_{n+1} = y_n + (dt/2) * (3*f_n - f_{n-1})
//
// Direct port of FF's FIAdamsBash. Stores one prior derivative; the first
// step falls back to forward Euler.
// ============================================================================
template<IntegrableState State>
class AdamsBashforth2Integrator {
    State state_{};
    State prev_deriv_{};
    bool  has_prev_ = false;
public:
    AdamsBashforth2Integrator() = default;
    explicit AdamsBashforth2Integrator(State initial) : state_(std::move(initial)) {}

    template<typename DerivFn>
    const State& step(DerivFn&& f, double dt) {
        State deriv = f(state_);
        if (has_prev_) {
            state_ = state_ + (deriv * 3.0 - prev_deriv_) * (dt * 0.5);
        } else {
            state_ = state_ + deriv * dt;
            has_prev_ = true;
        }
        prev_deriv_ = std::move(deriv);
        return state_;
    }

    [[nodiscard]] const State& state() const noexcept { return state_; }
    void reset(State s) {
        state_ = std::move(s);
        prev_deriv_ = State{};
        has_prev_ = false;
    }
};

// ============================================================================
// AdamsBashforth4Integrator — y_{n+1} = y_n + (dt/24) * (55*f_n - 59*f_{n-1}
//                                          + 37*f_{n-2} - 9*f_{n-3})
//
// 4th-order multistep. Bootstraps with RK4 for the first 3 steps so the
// bootstrap error is also O(dt^4) — using Euler/AB2/AB3 for bootstrap would
// leave an O(dt^2) Euler error that dominates the global error and makes
// the method effectively 2nd-order. After 3 RK4 steps, full AB4 kicks in
// (one derivative evaluation per step, vs RK4's four).
// ============================================================================
template<IntegrableState State>
class AdamsBashforth4Integrator {
    State state_{};
    std::array<State, 4> history_{};  // history_[3] = newest, history_[0] = oldest
    std::size_t count_ = 0;
public:
    AdamsBashforth4Integrator() = default;
    explicit AdamsBashforth4Integrator(State initial) : state_(std::move(initial)) {}

    template<typename DerivFn>
    const State& step(DerivFn&& f, double dt) {
        if (count_ < 3) {
            // RK4 bootstrap — keeps bootstrap error at O(dt^4) so it doesn't
            // dominate the AB4 global error.
            // Store the derivative at the START of this step in history
            // position count_+1, so after 3 bootstrap steps the history is:
            //   history_[1] = f(y_0), history_[2] = f(y_1), history_[3] = f(y_2)
            // which is what AB4 expects at step 4.
            State k1 = f(state_);
            State k2 = f(state_ + k1 * (dt * 0.5));
            State k3 = f(state_ + k2 * (dt * 0.5));
            State k4 = f(state_ + k3 * dt);
            state_ = state_ + (k1 + k2 * 2.0 + k3 * 2.0 + k4) * (dt / 6.0);
            history_[count_ + 1] = std::move(k1);
        } else {
            // Full AB4: y_{n+1} = y_n + (dt/24)*(55*f_n - 59*f_{n-1} + 37*f_{n-2} - 9*f_{n-3})
            State deriv = f(state_);
            state_ = state_ + (deriv * 55.0
                             - history_[3] * 59.0
                             + history_[2] * 37.0
                             - history_[1] * 9.0) * (dt / 24.0);
            // Shift history
            history_[0] = history_[1];
            history_[1] = history_[2];
            history_[2] = history_[3];
            history_[3] = std::move(deriv);
        }
        if (count_ < 4) ++count_;
        return state_;
    }

    [[nodiscard]] const State& state() const noexcept { return state_; }
    void reset(State s) {
        state_ = std::move(s);
        history_ = {};
        count_ = 0;
    }
};

// ============================================================================
// RK4Integrator — classical 4th-order Runge-Kutta.
//
//   k1 = f(y_n)
//   k2 = f(y_n + dt/2 * k1)
//   k3 = f(y_n + dt/2 * k2)
//   k4 = f(y_n + dt   * k3)
//   y_{n+1} = y_n + (dt/6) * (k1 + 2*k2 + 2*k3 + k4)
//
// 4 derivative evaluations per step but no history needed. This is the
// best choice when (a) accuracy matters and (b) the derivative function
// is cheap relative to the state size.
// ============================================================================
template<IntegrableState State>
class RK4Integrator {
    State state_{};
public:
    RK4Integrator() = default;
    explicit RK4Integrator(State initial) : state_(std::move(initial)) {}

    template<typename DerivFn>
    const State& step(DerivFn&& f, double dt) {
        State k1 = f(state_);
        State k2 = f(state_ + k1 * (dt * 0.5));
        State k3 = f(state_ + k2 * (dt * 0.5));
        State k4 = f(state_ + k3 * dt);
        state_ = state_ + (k1 + k2 * 2.0 + k3 * 2.0 + k4) * (dt / 6.0);
        return state_;
    }

    [[nodiscard]] const State& state() const noexcept { return state_; }
    void reset(State s) { state_ = std::move(s); }
};

} // namespace f4::math

// f4-data/include/f4/data/engine_rpm_schedule.hpp
//
// Per-engine-type RPM schedule, expressed as DATA (a list of rules) rather
// than CODE (an if/else chain). This replaces the hardcoded schedule that
// lived in f4-flight-model/src/engine.cpp::engineRpmMods().
//
// WHY THIS EXISTS
//   The original engineRpmMods() had a 40-line if/else chain of magic Mach
//   and altitude breakpoints, branchless per-engine-type. Adding a new
//   engine family meant editing engine.cpp. This struct lifts the schedule
//   to data so:
//     - Adding an engine family = adding another builtin (no engine.cpp edit)
//     - The schedule is testable in isolation, without an EngineModel
//     - When f4-data's JSON schema grows to load schedules from config,
//       EngineModel doesn't change at all — the schedule is just data
//
// FORMAT
//   Each rule is a (condition box, value, kind) triple:
//     - Condition box: a rectangular region in (Mach, altitude_ft, vcas_kts)
//       space. NaN bounds mean "no constraint" on that dimension.
//     - Value: either a constant or a linear function of one coordinate.
//     - Kind: Floor (std::max) or Ceiling (std::min).
//   When the rule's condition matches the current flight condition, the
//   commanded RPM is constrained by max (Floor) or min (Ceiling) against
//   the rule's value. Rules apply in vector order.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace f4::data {

// ---------------------------------------------------------------------------
// EngineRpmRule — one rule in an engine RPM schedule.
//
// The commanded RPM is constrained by either:
//   std::max(rpmCmd, value)  when kind == Floor
//   std::min(rpmCmd, value)  when kind == Ceiling
//
// The rule fires only when the current flight condition (mach, alt_ft,
// vcas_kts) falls inside the condition box. A NaN bound means "no
// constraint" on that dimension.
//
// The value is either a constant (valueKind == Constant, value = valueOffset)
// or a linear function of one coordinate (valueKind == Linear,
// value = valueScale * coord + valueOffset).
//
// Examples:
//   Floor at constant 0.99:
//     kind=Floor, valueKind=Constant, valueOffset=0.99
//   Floor at mach / 1.4:
//     kind=Floor, valueKind=Linear, valueScale=1.0/1.4, valueOffset=0.0,
//     valueCoord=Mach
//   Floor at (alt_ft/10000)/30 + 0.7  =  alt_ft/300000 + 0.7:
//     kind=Floor, valueKind=Linear, valueScale=1.0/300000.0, valueOffset=0.7,
//     valueCoord=Altitude_ft
// ---------------------------------------------------------------------------
struct EngineRpmRule {
    enum class Kind      : uint8_t { Floor, Ceiling };
    enum class Coord     : uint8_t { Mach, Altitude_ft, Vcas_kts };
    enum class ValueKind : uint8_t { Constant, Linear };

    Kind        kind{Kind::Floor};

    // --- Condition box bounds. NaN means unconstrained on that dimension. ---
    // Each bound has a paired `*Strict` flag: when true, the comparison is
    // strict (< or >) rather than inclusive (<= or >=). This captures the
    // original if/else chain's mix of `mach >= 0.84` (inclusive) and
    // `mach > 1.4` (strict) without approximation.
    double machLo   = std::numeric_limits<double>::quiet_NaN();
    double machHi   = std::numeric_limits<double>::quiet_NaN();
    bool   machLoStrict = false;
    bool   machHiStrict = false;

    double altLo_ft = std::numeric_limits<double>::quiet_NaN();
    double altHi_ft = std::numeric_limits<double>::quiet_NaN();
    bool   altLoStrict = false;
    bool   altHiStrict = false;

    double vcasLo   = std::numeric_limits<double>::quiet_NaN();
    double vcasHi   = std::numeric_limits<double>::quiet_NaN();
    bool   vcasLoStrict = false;
    bool   vcasHiStrict = false;

    // --- Value definition. ---
    ValueKind valueKind  = ValueKind::Constant;
    double    valueScale  = 0.0;  // linear scale (ignored when valueKind == Constant)
    double    valueOffset = 0.0;  // constant value, OR linear offset
    Coord     valueCoord  = Coord::Mach;  // linear coord (ignored when valueKind == Constant)

    /// Evaluate the rule's value at the given flight condition.
    [[nodiscard]] inline double evaluate(double mach,
                                         double alt_ft,
                                         double vcas_kts) const noexcept {
        if (valueKind == ValueKind::Constant) return valueOffset;
        double c = 0.0;
        switch (valueCoord) {
            case Coord::Mach:        c = mach;     break;
            case Coord::Altitude_ft: c = alt_ft;   break;
            case Coord::Vcas_kts:    c = vcas_kts; break;
        }
        return valueScale * c + valueOffset;
    }

    /// Check if the rule fires at the given flight condition.
    [[nodiscard]] inline bool matches(double mach,
                                      double alt_ft,
                                      double vcas_kts) const noexcept {
        const auto check_lo = [](double v, double lo, bool strict) noexcept -> bool {
            if (std::isnan(lo)) return true;
            return strict ? (v > lo) : (v >= lo);
        };
        const auto check_hi = [](double v, double hi, bool strict) noexcept -> bool {
            if (std::isnan(hi)) return true;
            return strict ? (v < hi) : (v <= hi);
        };
        if (!check_lo(mach,     machLo,   machLoStrict))                return false;
        if (!check_hi(mach,     machHi,   machHiStrict))                return false;
        if (!check_lo(alt_ft,   altLo_ft, altLoStrict))                 return false;
        if (!check_hi(alt_ft,   altHi_ft, altHiStrict))                 return false;
        if (!check_lo(vcas_kts, vcasLo,   vcasLoStrict))                return false;
        if (!check_hi(vcas_kts, vcasHi,   vcasHiStrict))                return false;
        return true;
    }
};

// ---------------------------------------------------------------------------
// EngineRpmSchedule — a list of rules for one engine family.
// ---------------------------------------------------------------------------
struct EngineRpmSchedule {
    std::vector<EngineRpmRule> rules;

    /// Apply all matching rules to the commanded RPM. Floor rules use
    /// std::max, Ceiling rules use std::min. Rules apply in vector order.
    [[nodiscard]] inline double apply(double rpmCmd,
                                      double mach,
                                      double alt_ft,
                                      double vcas_kts) const noexcept {
        for (const auto& r : rules) {
            if (!r.matches(mach, alt_ft, vcas_kts)) continue;
            const double v = r.evaluate(mach, alt_ft, vcas_kts);
            rpmCmd = (r.kind == EngineRpmRule::Kind::Floor)
                       ? std::max(rpmCmd, v)
                       : std::min(rpmCmd, v);
        }
        return rpmCmd;
    }

    /// Look up the built-in schedule for a given engine type code.
    ///   typeEngine 1, 2      → PW-100 / PW-220 family
    ///   typeEngine 3, 4, 5   → PW-229 / GE-110 / GE-129 family
    ///   Any other value     → PW-220 family (the default).
    static const EngineRpmSchedule& builtin(int typeEngine);
};

}  // namespace f4::data

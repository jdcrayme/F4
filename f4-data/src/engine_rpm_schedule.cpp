// f4-data/src/engine_rpm_schedule.cpp
//
// Built-in per-engine-type RPM schedules, ported from the if/else chain that
// previously lived in f4-flight-model/src/engine.cpp::engineRpmMods().
//
// Two families are defined:
//   - PW-100 / PW-220 (typeEngine 1, 2)
//   - PW-229 / GE-110 / GE-129 (typeEngine 3, 4, 5)
//
// Each rule is a direct translation of one `if (condition) rpmCmd = max/min(...)`
// line from the original. The translation is mechanical and reversible — a
// diff against the original if/else should show 1:1 correspondence, including
// strict-vs-inclusive bound semantics.

#include "f4/data/engine_rpm_schedule.hpp"

namespace f4::data {

namespace {

// ---------------------------------------------------------------------------
// PW-100 / PW-220 schedule (typeEngine 1, 2).
// Ported verbatim from engine.cpp lines 72-91.
//
// Rule count: 7 (the original "alt > 55000 OR mach <= 0.4" ceiling is an OR
// condition, split into two rules — one per branch of the OR).
// ---------------------------------------------------------------------------
const EngineRpmSchedule& pw100_pw220() {
    static const EngineRpmSchedule sched = [](){
        EngineRpmSchedule s;
        auto& r = s.rules;
        using K  = EngineRpmRule::Kind;
        using C  = EngineRpmRule::Coord;
        using VK = EngineRpmRule::ValueKind;

        // 1) mach >= 0.84 && mach <= 1.4  →  floor at mach / 1.4
        EngineRpmRule r1;
        r1.kind = K::Floor;
        r1.machLo = 0.84; r1.machHi = 1.4;  // inclusive on both
        r1.valueKind = VK::Linear;
        r1.valueScale = 1.0 / 1.4;
        r1.valueOffset = 0.0;
        r1.valueCoord = C::Mach;
        r.push_back(r1);

        // 2) mach > 1.4  →  floor at 0.99
        EngineRpmRule r2;
        r2.kind = K::Floor;
        r2.machLo = 1.4; r2.machLoStrict = true;  // strict >
        r2.valueKind = VK::Constant;
        r2.valueOffset = 0.99;
        r.push_back(r2);

        // 3) alt > 10000  →  floor at (alt/10000)/30 + 0.7 = alt/300000 + 0.7
        EngineRpmRule r3;
        r3.kind = K::Floor;
        r3.altLo_ft = 10000.0; r3.altLoStrict = true;  // strict >
        r3.valueKind = VK::Linear;
        r3.valueScale = 1.0 / 300000.0;
        r3.valueOffset = 0.7;
        r3.valueCoord = C::Altitude_ft;
        r.push_back(r3);

        // 4) alt >= 35000 && alt <= 45000 && mach >= 0.4 && mach <= 0.8
        //    →  ceiling at 1.025
        EngineRpmRule r4;
        r4.kind = K::Ceiling;
        r4.altLo_ft = 35000.0; r4.altHi_ft = 45000.0;  // inclusive
        r4.machLo = 0.4; r4.machHi = 0.8;              // inclusive
        r4.valueKind = VK::Constant;
        r4.valueOffset = 1.025;
        r.push_back(r4);

        // 5) alt > 45000 && alt <= 55000 && mach >= 0.4 && mach <= 0.95
        //    →  ceiling at 1.01
        EngineRpmRule r5;
        r5.kind = K::Ceiling;
        r5.altLo_ft = 45000.0; r5.altLoStrict = true;  // strict >
        r5.altHi_ft = 55000.0;                         // inclusive
        r5.machLo = 0.4; r5.machHi = 0.95;             // inclusive
        r5.valueKind = VK::Constant;
        r5.valueOffset = 1.01;
        r.push_back(r5);

        // 6a) alt > 55000  →  ceiling at 0.99   (first branch of OR)
        EngineRpmRule r6a;
        r6a.kind = K::Ceiling;
        r6a.altLo_ft = 55000.0; r6a.altLoStrict = true;  // strict >
        r6a.valueKind = VK::Constant;
        r6a.valueOffset = 0.99;
        r.push_back(r6a);

        // 6b) mach <= 0.4  →  ceiling at 0.99   (second branch of OR)
        EngineRpmRule r6b;
        r6b.kind = K::Ceiling;
        r6b.machHi = 0.4;  // inclusive <=
        r6b.valueKind = VK::Constant;
        r6b.valueOffset = 0.99;
        r.push_back(r6b);

        return s;
    }();
    return sched;
}

// ---------------------------------------------------------------------------
// PW-229 / GE-110 / GE-129 schedule (typeEngine 3, 4, 5).
// Ported verbatim from engine.cpp lines 93-103.
// Rule count: 3.
// ---------------------------------------------------------------------------
const EngineRpmSchedule& pw229_ge110_ge129() {
    static const EngineRpmSchedule sched = [](){
        EngineRpmSchedule s;
        auto& r = s.rules;
        using K  = EngineRpmRule::Kind;
        using C  = EngineRpmRule::Coord;
        using VK = EngineRpmRule::ValueKind;

        // 1) mach > 0.55 && mach < 1.1  →  floor at 0.79
        EngineRpmRule r1;
        r1.kind = K::Floor;
        r1.machLo = 0.55; r1.machLoStrict = true;  // strict >
        r1.machHi = 1.1;  r1.machHiStrict = true;  // strict <
        r1.valueKind = VK::Constant;
        r1.valueOffset = 0.79;
        r.push_back(r1);

        // 2) mach >= 1.1 && mach <= 1.4  →  floor at mach / 1.4
        EngineRpmRule r2;
        r2.kind = K::Floor;
        r2.machLo = 1.1; r2.machHi = 1.4;  // inclusive on both
        r2.valueKind = VK::Linear;
        r2.valueScale = 1.0 / 1.4;
        r2.valueOffset = 0.0;
        r2.valueCoord = C::Mach;
        r.push_back(r2);

        // 3) alt > 50000 && vcas < 250  →  ceiling at 0.99 (AB no-light zone)
        EngineRpmRule r3;
        r3.kind = K::Ceiling;
        r3.altLo_ft = 50000.0; r3.altLoStrict = true;  // strict >
        r3.vcasHi = 250.0; r3.vcasHiStrict = true;     // strict <
        r3.valueKind = VK::Constant;
        r3.valueOffset = 0.99;
        r.push_back(r3);

        return s;
    }();
    return sched;
}

}  // namespace

// ---------------------------------------------------------------------------
// EngineRpmSchedule::builtin — family lookup by typeEngine code.
// ---------------------------------------------------------------------------
const EngineRpmSchedule& EngineRpmSchedule::builtin(int typeEngine) {
    if (typeEngine >= 3 && typeEngine <= 5) {
        return pw229_ge110_ge129();
    }
    // typeEngine 1, 2, and any unknown value default to the PW-100/220 family.
    return pw100_pw220();
}

}  // namespace f4::data

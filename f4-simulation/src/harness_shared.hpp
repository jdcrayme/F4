// f4-simulation/src/harness_shared.hpp
//
// Code shared verbatim by the three combat-chain acceptance harnesses
// (bvr_intercept_harness, wvr_merge_harness, ground_strike_harness).
// The full base-class merge (one CRTP CombatChainHarness owning the
// run-pass skeleton, census walk, and certificate machinery) is the
// follow-up; this header starts with the pieces that are already
// byte-identical.

#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#include <f4/json/reader.hpp>

namespace f4::simulation::harness_shared {

/// The M4/M5 pre-flight refusal scan: is this scenario structurally
/// wrong for a combat harness (combat.enabled == false)? Best-effort
/// scan of the raw JSON — any parse problem is IGNORED (run_pass_'s
/// load_scenario() names the real problem), and a non-combat scenario
/// need not carry a valid aircraft list — that is the point of
/// refusing it before any load. Returns true and fills *reason (the
/// QC tools' exit-2 contract prefix: "scenario combat.enabled is
/// false") when the harness must refuse.
inline bool combat_refusal_reason(const std::filesystem::path& scenario_json,
                                  std::string* reason) {
    std::ifstream in(scenario_json);
    if (!in) {
        return false;  // run_pass_'s load_scenario() reports the I/O error
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    try {
        f4::json::Reader r(text);
        r.skip_ws();
        r.expect('{');
        if (r.consume('}')) {
            return false;
        }
        bool combat_enabled = false;  // CombatConfig default
        for (;;) {
            const std::string key = r.read_string();
            r.expect(':');
            if (key == "combat") {
                r.expect('{');
                if (!r.consume('}')) {
                    for (;;) {
                        const std::string ck = r.read_string();
                        r.expect(':');
                        if (ck == "enabled") {
                            combat_enabled = r.read_bool();
                        } else {
                            r.skip_value();
                        }
                        if (r.consume('}')) break;
                        r.expect(',');
                    }
                }
            } else {
                r.skip_value();
            }
            if (r.consume('}')) break;
            r.expect(',');
        }
        if (!combat_enabled) {
            if (reason != nullptr) {
                *reason =
                    "scenario combat.enabled is false — the "
                    "harness refuses to run a non-combat scenario "
                    "(silent success would be the worst failure "
                    "class)";
            }
            return true;
        }
    } catch (const std::exception&) {
        // Malformed or unexpected shape — run_pass_'s
        // load_scenario() names the real problem.
    }
    return false;
}

} // namespace f4::simulation::harness_shared

// f4-data/include/f4/data/aux_aero_record.hpp
//
// The complete AuxAeroData record: all 443 schema keys (see
// auxaero_rosetta.hpp — FreeFalcon's readin.cpp AuxAeroDataDesc[] table)
// with their values after the legacy loader's semantics are applied:
// FreeFalcon's default for every key the .dat file does not override, the
// .dat value for every key it does.
//
// This is the full-fidelity data surface of an aircraft's AuxAeroData —
// the typed AuxAero view (aircraft_config.hpp) stays the flight model's
// convenience projection of the subset it consumes; the record is what
// future engine systems (engine damage, fuel/countermeasures, weapons
// delivery, AAR points, sound/anim charts) read instead of re-deriving
// legacy defaults from code.
//
// C++20.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace f4::data {

/// The value type of one record entry. Mirrors the legacy InputDataDesc
/// kinds (RosettaType) minus the schema-only detail: LookupTable and
/// TwoDTable both carry an opaque token list here.
enum class AuxAeroValueType : int {
    Float = 0,   ///< one double
    Int,         ///< one integer
    Vector,      ///< three doubles (x y z)
    Chart,       ///< token list (count + breakpoint pairs, sound/anim charts)
};

/// One typed value. `type` says which member is live.
struct AuxAeroValue {
    AuxAeroValueType type = AuxAeroValueType::Float;

    double f = 0.0;                 ///< Float
    int64_t i = 0;                  ///< Int
    std::array<double, 3> v{0.0, 0.0, 0.0};  ///< Vector (x y z)
    std::vector<double> t;          ///< Chart tokens
};

/// The complete record, keyed by the legacy .dat key (exact rosetta
/// spelling), in key order (std::map) for deterministic emission.
using AuxAeroRecord = std::map<std::string, AuxAeroValue>;

} // namespace f4::data

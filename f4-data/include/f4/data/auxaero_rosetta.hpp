// f4-data/include/f4/data/auxaero_rosetta.hpp
//
// GENERATED FILE — do not edit by hand.
// Source: f4-convert/rosetta/auxaero_field_map.json (extracted from
// FreeFalcon src/sim/airframe/readin.cpp, AuxAeroDataDesc[]).
// Regenerate with: python3 scripts/gen_auxaero_table.py
//
// The typed schema of the legacy AuxAeroData block: 443 keys, each with a
// type and FreeFalcon's default token string. This is pure schema data —
// no parsing code — so it lives engine-side: f4-data resolves JSON value
// types on load (Vector vs Chart disambiguation), f4-convert completes the
// full AuxAeroRecord from a .dat's verbatim overrides.

#pragma once

#include <cstddef>
#include <string>

namespace f4::data {

/// Legacy InputDataDesc type of one AuxAeroData key.
enum class RosettaType : int {
    Float = 0,        ///< ID_FLOAT        — one double
    Int,              ///< ID_INT          — one integer
    Vector,           ///< ID_VECTOR       — three doubles (x y z)
    LookupTable,      ///< ID_LOOKUPTABLE  — token list (count + breakpoints)
    TwoDTable,        ///< ID_2DTABLE      — token list
};

/// One AuxAeroData schema entry. default_tokens is FreeFalcon's default in
/// legacy .dat units, whitespace-separated (vectors: "x y z"; charts: the
/// breakpoint token list verbatim).
struct RosettaEntry {
    const char* key;
    RosettaType type;
    const char* field;           ///< AuxAeroData struct field (informational)
    const char* default_tokens;
};

/// The full schema, in readin.cpp table order.
extern const RosettaEntry kAuxAeroRosetta[443];
inline constexpr std::size_t kAuxAeroRosettaCount = 443;

/// Exact-key lookup (case-sensitive, matching FreeFalcon's strcmp-based
/// readin.cpp lookup). Returns nullptr when the key is not in the schema.
const RosettaEntry* findAuxAeroEntry(const std::string& key);

} // namespace f4::data

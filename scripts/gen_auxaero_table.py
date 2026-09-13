#!/usr/bin/env python3
"""Generate the C++ rosetta table for the AuxAeroData schema.

Reads the curated rosetta JSON (f4-convert/rosetta/auxaero_field_map.json —
extracted from FreeFalcon's src/sim/airframe/readin.cpp, AuxAeroDataDesc[])
and emits the committed table the f4-data library compiles in:

    f4-data/include/f4/data/auxaero_rosetta.hpp
    f4-data/src/auxaero_rosetta.cpp

The generated files are COMMITTED (no build-time python dependency); re-run
this script only when the rosetta JSON changes:

    python3 scripts/gen_auxaero_table.py

The table is pure schema data (key -> type + default tokens); it contains no
legacy binary parsing, so it legitimately lives engine-side in f4-data —
both f4-data (JSON load-side type resolution) and f4-convert (record
completion from .dat overrides) consume it.
"""
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ROSETTA_JSON = REPO_ROOT / "f4-convert" / "rosetta" / "auxaero_field_map.json"
OUT_HPP = REPO_ROOT / "f4-data" / "include" / "f4" / "data" / "auxaero_rosetta.hpp"
OUT_CPP = REPO_ROOT / "f4-data" / "src" / "auxaero_rosetta.cpp"

TYPE_MAP = {
    "ID_FLOAT": "RosettaType::Float",
    "ID_INT": "RosettaType::Int",
    "ID_VECTOR": "RosettaType::Vector",
    "ID_LOOKUPTABLE": "RosettaType::LookupTable",
    "ID_2DTABLE": "RosettaType::TwoDTable",
}


def cstr(s: str) -> str:
    """C string literal with escaping."""
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    ros = json.loads(ROSETTA_JSON.read_text())
    keys = ros["keys"]  # insertion order == readin.cpp table order
    entries = [(k, v["type"], v.get("field", k), v["default"]) for k, v in keys.items()]
    n = len(entries)

    guard = "F4_DATA_AUXAERO_ROSETTA_HPP"
    hpp = f"""// f4-data/include/f4/data/auxaero_rosetta.hpp
//
// GENERATED FILE — do not edit by hand.
// Source: f4-convert/rosetta/auxaero_field_map.json (extracted from
// FreeFalcon src/sim/airframe/readin.cpp, AuxAeroDataDesc[]).
// Regenerate with: python3 scripts/gen_auxaero_table.py
//
// The typed schema of the legacy AuxAeroData block: {n} keys, each with a
// type and FreeFalcon's default token string. This is pure schema data —
// no parsing code — so it lives engine-side: f4-data resolves JSON value
// types on load (Vector vs Chart disambiguation), f4-convert completes the
// full AuxAeroRecord from a .dat's verbatim overrides.

#pragma once

#include <cstddef>
#include <string>

namespace f4::data {{

/// Legacy InputDataDesc type of one AuxAeroData key.
enum class RosettaType : int {{
    Float = 0,        ///< ID_FLOAT        — one double
    Int,              ///< ID_INT          — one integer
    Vector,           ///< ID_VECTOR       — three doubles (x y z)
    LookupTable,      ///< ID_LOOKUPTABLE  — token list (count + breakpoints)
    TwoDTable,        ///< ID_2DTABLE      — token list
}};

/// One AuxAeroData schema entry. default_tokens is FreeFalcon's default in
/// legacy .dat units, whitespace-separated (vectors: "x y z"; charts: the
/// breakpoint token list verbatim).
struct RosettaEntry {{
    const char* key;
    RosettaType type;
    const char* field;           ///< AuxAeroData struct field (informational)
    const char* default_tokens;
}};

/// The full schema, in readin.cpp table order.
extern const RosettaEntry kAuxAeroRosetta[{n}];
inline constexpr std::size_t kAuxAeroRosettaCount = {n};

/// Exact-key lookup (case-sensitive, matching FreeFalcon's strcmp-based
/// readin.cpp lookup). Returns nullptr when the key is not in the schema.
const RosettaEntry* findAuxAeroEntry(const std::string& key);

}} // namespace f4::data
"""

    rows = []
    for key, typ, field, default in entries:
        rows.append(
            f"    {{{cstr(key)}, {TYPE_MAP[typ]}, {cstr(field)}, {cstr(default)}}},"
        )
    cpp = f"""// f4-data/src/auxaero_rosetta.cpp
//
// GENERATED FILE — do not edit by hand.
// Source: f4-convert/rosetta/auxaero_field_map.json
// Regenerate with: python3 scripts/gen_auxaero_table.py

#include "f4/data/auxaero_rosetta.hpp"

namespace f4::data {{

const RosettaEntry kAuxAeroRosetta[kAuxAeroRosettaCount] = {{
{chr(10).join(rows)}
}};

const RosettaEntry* findAuxAeroEntry(const std::string& key) {{
    for (std::size_t i = 0; i < kAuxAeroRosettaCount; ++i) {{
        if (key == kAuxAeroRosetta[i].key) return &kAuxAeroRosetta[i];
    }}
    return nullptr;
}}

}} // namespace f4::data
"""

    OUT_HPP.write_text(hpp)
    OUT_CPP.write_text(cpp)
    print(f"wrote {OUT_HPP.relative_to(REPO_ROOT)} ({n} entries)")
    print(f"wrote {OUT_CPP.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

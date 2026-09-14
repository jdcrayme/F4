#!/usr/bin/env python3
"""Extract the AuxAeroDataDesc table from FreeFalcon's readin.cpp and emit
it as a JSON Rosetta Stone: a list of {key, type, field, default} entries.

This is the authoritative mapping between .dat file keys and the typed
AuxAeroData struct fields. Both f4-convert (which reads .dat) and f4-data
(which exposes AircraftConfig) consume this map.

Source of truth: src/sim/airframe/readin.cpp, AuxAeroDataDesc[] table.
"""
import json
import re
import sys
from pathlib import Path

FF_READIN_CPP = Path("/home/z/my-project/scripts/freefalcon-central/src/sim/airframe/readin.cpp")
OUTPUT_JSON = Path("/home/z/my-project/scripts/F4/f4-convert/rosetta/auxaero_field_map.json")

# Regex to match one table entry:
#   { "keyName", InputDataDesc::ID_FLOAT, OFFSET(fieldName), "default"},
ENTRY_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*'              # key name (quoted)
    r'InputDataDesc::(ID_\w+)\s*,\s*'      # type (ID_FLOAT, ID_INT, ID_VECTOR, etc.)
    r'OFFSET\(([^)]+)\)\s*,\s*'            # field name (in OFFSET() macro)
    r'"([^"]*)"'                           # default value (quoted)
)


def extract_entries(source: str):
    # Find the AuxAeroDataDesc table block.
    m = re.search(
        r'static const InputDataDesc AuxAeroDataDesc\[\]\s*=\s*\{(.*?)\};',
        source, re.DOTALL
    )
    if not m:
        sys.exit("ERROR: could not locate AuxAeroDataDesc[] table in readin.cpp")

    table_body = m.group(1)
    entries = []
    for em in ENTRY_RE.finditer(table_body):
        key, type_id, field, default = em.groups()
        entries.append({
            "key": key,
            "type": type_id,
            "field": field,
            "default": default,
        })
    return entries


def categorize(entries):
    """Group entries by category for human readability."""
    # Heuristic categories based on key prefixes.
    def category_of(key):
        if any(key.startswith(p) for p in ("normSpool", "abSpool", "jfs", "lightup",
                                            "flameout", "mainGen", "stbyGen", "epu",
                                            "engineDamage", "DeepStall")):
            return "engine"
        if any(key.startswith(p) for p in ("fuel", "nChaff", "nFlare")):
            return "fuel_consumables"
        if any(key.startswith(p) for p in ("roll", "pitch", "yaw", "gear")):
            return "fcs_inertia"
        if any(key.startswith(p) for p in ("tef", "lef", "flap", "CLtef", "CDtef",
                                            "CDlef", "CDSPDB", "CDLDG")):
            return "surfaces_flaps"
        if any(key.startswith(p) for p in ("rudder", "aileron", "elevon", "airbrake",
                                            "canopy", "swing", "isComplex")):
            return "surfaces_other"
        if any(key.startswith(p) for p in ("area2Span", "dragChute", "landingAOA",
                                            "sinkRate", "vortex", "wingTip",
                                            "refuel", "hardpoint", "engineSmoke",
                                            "gunLocation", "gunTrail")):
            return "geometry_visuals"
        if any(key.startswith(p) for p in ("A2G", "A2A", "Bullet", "rocket")):
            return "weapons_delivery"
        if key.startswith("hsi") or key.startswith("ecmSy") or key.startswith("radio"):
            return "avionics"
        if key in ("nEngines", "typeEngine") or key.startswith("engine") and "Location" in key:
            return "engine_geometry"
        if "Alt" in key or "Speed" in key or "Mach" in key or "Range" in key:
            return "weapons_delivery"
        return "misc"
    # Return list of (category, entries) preserving first-appearance order.
    seen = []
    by_cat = {}
    for e in entries:
        c = category_of(e["key"])
        if c not in by_cat:
            by_cat[c] = []
            seen.append(c)
        by_cat[c].append(e)
    return [(c, by_cat[c]) for c in seen]


def main():
    source = FF_READIN_CPP.read_text()
    entries = extract_entries(source)
    print(f"Extracted {len(entries)} entries from AuxAeroDataDesc", file=sys.stderr)

    # Build the output: a top-level object with metadata + the entries
    # grouped by category, plus a flat key->entry index for quick lookup.
    categorized = categorize(entries)
    output = {
        "_meta": {
            "description": "Rosetta Stone mapping .dat AuxAeroData keys to typed struct fields.",
            "source_of_truth": "FreeFalcon src/sim/airframe/readin.cpp, AuxAeroDataDesc[] table",
            "source_file": str(FF_READIN_CPP),
            "entry_count": len(entries),
            "type_meanings": {
                "ID_FLOAT": "single double-precision float",
                "ID_INT": "single integer (also used for booleans; see field default)",
                "ID_VECTOR": "three doubles (x y z), typically a body-frame position",
            },
            "usage": (
                "f4-convert uses this map to populate the typed AuxAero struct from "
                "the rawAuxAeroData verbatim map. f4-data uses it to validate that "
                "every key in a JSON has a known field mapping. If a key appears in "
                "a .dat file but not in this map, it is captured verbatim into "
                "rawAuxAeroData but does not get a typed field — that is by design, "
                "not a bug."
            ),
        },
        "categories": [
            {"name": cat, "entries": ents} for cat, ents in categorized
        ],
        "keys": {e["key"]: {"type": e["type"], "field": e["field"], "default": e["default"]}
                 for e in entries},
    }
    OUTPUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_JSON.write_text(json.dumps(output, indent=2) + "\n")
    print(f"Wrote {OUTPUT_JSON}", file=sys.stderr)


if __name__ == "__main__":
    main()

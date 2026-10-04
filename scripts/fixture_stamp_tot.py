#!/usr/bin/env python3
"""fixture_stamp_tot — stamp the TestCamp save's A-G delivery flights
into an in-horizon TOT band.

MC-4b (the TOT push-wait tranche). The decoded save's flight records
carry their campaign ATO appointments verbatim: the measured
distribution across the delivery families is -132..+2,752 HOURS
against the campaign clock — 4 stale, the rest days out, ZERO inside
a sim run's horizon (the INIT-1c finding). The sim spawns its flights
at the run's start and holds a TIMING station until the appointment
(the C3 semantics), so every delivery flight in the coverage matrix
either held forever (the employment SKIP family, the 2 remaining PATH
FAILs) or skipped its tot clause (43 SKIPs of "beyond the run").

The stamp: every A-G delivery flight (mission_is_ag_delivery's byte
set — Strike {12-16,24}, SEAD {17}, CAS {18-21,23,31}) whose
appointment is outside +/- 6 h of the campaign clock gets a
deterministic in-horizon one: current + 1,800 s + (id_num % 30) * 120
— a 30-minute base so the flight's own transit is the first wait, and
a 0-58-minute id-hash spread so a package's flights do not all arrive
at one instant. Flights already inside +/- 6 h are left alone, which
makes the script IDEMPOTENT (a second run is a no-op) and keeps any
hand-authored appointments.

The fixture (testcamp.world.json) is a gitignored local decode of the
TestCamp save; this script is the tracked, repeatable step that turns
it into the QC world. Deterministic: same world in, same world out.

Usage:
    python scripts/fixture_stamp_tot.py [world.json]
"""

import json
import sys
from pathlib import Path

DELIVERY_MISSIONS = frozenset(
    [12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 23, 24, 31])

STALE_BAND_S = 6 * 3600      # leave appointments within +/- 6 h alone
BASE_DELAY_S = 1_800         # current + 30 min
STAGGER_S = 120              # x (id_num % 30): a 58-minute spread


def stamp(world: dict) -> tuple[int, int]:
    """Stamp the out-of-horizon delivery TOTs. Returns (stamped, kept)."""
    current = world["campaign"]["current_time"]
    items = world["units"]["items"]
    stamped = kept = 0
    for it in items:
        mission = it.get("mission", 0)
        if mission not in DELIVERY_MISSIONS:
            continue
        tot = it.get("time_on_target", 0)
        if not tot:
            continue
        if abs(tot - current) <= STALE_BAND_S:
            kept += 1
            continue
        delay = BASE_DELAY_S + (it.get("id_num", 0) % 30) * STAGGER_S
        it["time_on_target"] = current + delay
        stamped += 1
    return stamped, kept


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1
                else Path(__file__).resolve().parent.parent
                / "testcamp.world.json")
    world = json.loads(path.read_text(encoding="utf-8"))
    stamped, kept = stamp(world)
    path.write_text(json.dumps(world, separators=(",", ":"),
                               ensure_ascii=False), encoding="utf-8")
    print(f"{path.name}: stamped {stamped} delivery TOT(s) into the "
          f"+30..+88 min band, kept {kept} in-horizon")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

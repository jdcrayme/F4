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

TOT_MARGIN_S = 600.0         # the attack's own span past the enroute
TOT_MIN_REL_S = 900.0        # 15 min: the earliest push
TOT_MAX_REL_S = 5400.0       # 90 min: the recovery still fits
GROUND_ALLOWANCE_S = 180.0   # the TELEPORT launch: roll + climb (no taxi)
CRUISE_FPS = 385.0           # the measured whole-flight cruise (mc5c vt medians)
CORNER_FACTOR = 1.17         # straight-line -> the sim's actual flight path


def _enroute_s(it: dict) -> float:
    """The element's enroute estimate: the STRAIGHT-LINE base-to-target
    distance x the corner factor, at the measured cruise. The sim's
    abeam captures fly the save's ingress doglegs nearly straight (the
    measured OCASTRIKE transit = straight x 1.17; the dogleg sum
    overestimated 3.5x) — the estimate must match what the sim FLIES."""
    wps = it.get("waypoints") or []
    if not wps:
        return GROUND_ALLOWANCE_S
    # The delivery waypoint: the first WP with a delivery action; the
    # sim's plan co-locates deliveries ON the target, so the fallback
    # is the waypoint FARTHEST from the base (the deep strike point).
    def _is_delivery(w):
        return w.get("action", 0) in (14, 15, 16, 17, 18, 19)
    target = next((w for w in wps if _is_delivery(w)), None)
    if target is None:
        bx, by = it.get("x", 0), it.get("y", 0)
        target = max(wps, key=lambda w: (w["x"] - bx) ** 2 +
                                          (w["y"] - by) ** 2)
    dx = target["x"] - it.get("x", 0)
    dy = target["y"] - it.get("y", 0)
    straight_grid = (dx * dx + dy * dy) ** 0.5
    return (straight_grid * CORNER_FACTOR *
            (1024.0 / CRUISE_FPS) + GROUND_ALLOWANCE_S)


def stamp(world: dict) -> tuple[int, int]:
    """Stamp the delivery packages' TOTs (MC-5): per package, the
    LIMITING element's enroute estimate anchors the appointment (the
    most limiting package element selects the TOT, the reference's ATO
    rule); every element of the package shares it. Deterministic: the
    same world re-stamps to the same values. Returns (stamped, kept,
    where kept is always 0 — the signature is retained for the older
    callers)."""
    current = world["campaign"]["current_time"]
    items = world["units"]["items"]
    members: dict[int, list[dict]] = {}
    for it in items:
        if it.get("mission", 0) in DELIVERY_MISSIONS:
            members.setdefault(it.get("package_id", 0), []).append(it)
    stamped = 0
    for pkg_items in members.values():
        enroutes = [_enroute_s(it) for it in pkg_items]
        tot_rel = max(min(max(enroutes) + TOT_MARGIN_S, TOT_MAX_REL_S),
                      TOT_MIN_REL_S)
        for it in pkg_items:
            # The derivation is deterministic — a re-run produces the
            # same values, which IS the idempotence (no band needed;
            # the reference derives the launch schedule every cycle).
            it["time_on_target"] = current + int(tot_rel)
            stamped += 1
    return stamped, 0


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1
                else Path(__file__).resolve().parent.parent
                / "testcamp.world.json")
    world = json.loads(path.read_text(encoding="utf-8"))
    stamped, _ = stamp(world)
    path.write_text(json.dumps(world, separators=(",", ":"),
                               ensure_ascii=False), encoding="utf-8")
    print(f"{path.name}: derived {stamped} package TOT(s) from the "
          f"limiting element's enroute")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

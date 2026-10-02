#!/usr/bin/env python3
# qc_missions — the per-mission-type QC matrix runner (the sanity-check
# layer the world viewer can't give you).
#
# The world viewer answers "what is happening?" — 449 flights, 8 teams,
# a map full of links. It cannot answer "is the CAP mission behaving
# like a CAP mission?", because behavior lives in the headless run and
# its artifacts, not on the map. This script runs THAT question:
#
#   one campaign_qc invocation per mission type (the --mission filter
#   is byte-exact in C++), then a per-CATEGORY expectation pass over
#   each run's campaign_qc_summary.json — the knowledge layer the
#   generic exit-code gates don't have:
#
#     * routes_attached == aircraft_spawned   (every spawn flew its plan)
#     * airborne_at_end >= 1                  (ground ops did not stall)
#     * Strike/SEAD/CAS: armed > 0 and released == 0  → EMPLOYMENT FAIL
#       (the flight was armed but never received/pressed a ground
#       target; autopsy: trace.json ai_state + target_description)
#     * armed == 0 and released == 0 → NOTE (loadout/weapon-table
#       concern, not tasking). The counter is measured POST-run (a
#       release debits the store), so released > 0 means the flight
#       carried and delivered: that row gets the release note instead.
#
# The C++ gates stay the authority (exit 2/3/4/5/6/7/8 — see the
# campaign_qc header); this tool renders them PER TYPE alongside its
# own expectations and writes a matrix artifact:
#
#   <out-root>/qc_matrix.json   machine form (CI diffable)
#   <out-root>/qc_matrix.md     the human verdict table
#   <out-root>/<mission>/       each run's campaign_qc_summary.json,
#                               campaign_result.json (+ trace.json
#                               when --record)
#
# The trace is the viewer's replay format — a failing strike row is
# investigated by opening <out-root>/<mission>/trace.json in the world
# viewer's replay mode (scrubber + ai_state), not by staring at the map.
#
# Usage (from the repo root, after a build):
#
#   python3 scripts/qc_missions.py build/testcamp.world.json
#   python3 scripts/qc_missions.py world.json --missions INTSTRIKE,CAP
#   python3 scripts/qc_missions.py world.json --tasking 35 --tasking-cycle 300
#   python3 scripts/qc_missions.py world.json --jobs 4 --record --minutes 30
#
# Horizons: --minutes is the FLOOR (and the flat fallback with
# --flat-minutes). By default each run's horizon comes from the mission's
# own MissionProfiles.json max_time (Falcon's planned sortie window),
# clamped to [--minutes, --max-horizon]: a 30-min horizon was cutting
# missions mid-flight (38 recovery FAILs were the horizon, not behavior)
# and could never hold a 45-min station contract. The per-run horizon is
# recorded on each row ("minutes") and in every verdict string.
#
# Exit codes: 0 all rows PASS / SKIPPED-absent; 1 any FAIL.
#
# Stdlib only. Python 3.10+.

from __future__ import annotations

import argparse
import concurrent.futures
import json
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

# ---------------------------------------------------------------------------
# The mission vocabulary — mirrored from
# f4-campaign/include/f4/campaign/mission_type.hpp (single source of
# truth in C++; keep this table in sync, the tool is a reader).
# ---------------------------------------------------------------------------
MISSION_NAMES: dict[int, str] = {
    0: "AMIS_NONE", 1: "AMIS_BARCAP", 2: "AMIS_BARCAP2", 3: "AMIS_HAVCAP",
    4: "AMIS_TARCAP", 5: "AMIS_RESCAP", 6: "AMIS_AMBUSHCAP", 7: "AMIS_SWEEP",
    8: "AMIS_ALERT", 9: "AMIS_INTERCEPT", 10: "AMIS_ESCORT",
    11: "AMIS_SEADESCORT", 12: "AMIS_OCASTRIKE", 13: "AMIS_INTSTRIKE",
    14: "AMIS_STRIKE", 15: "AMIS_DEEPSTRIKE", 16: "AMIS_STSTRIKE",
    17: "AMIS_SEADSTRIKE", 18: "AMIS_ONCALLCAS", 19: "AMIS_PRPLANCAS",
    20: "AMIS_CAS", 21: "AMIS_SAD", 22: "AMIS_INT", 23: "AMIS_BAI",
    24: "AMIS_STRATBOMB", 25: "AMIS_AWACS", 26: "AMIS_JSTAR",
    27: "AMIS_TANKER", 28: "AMIS_ECM", 29: "AMIS_RECON", 30: "AMIS_BDA",
    31: "AMIS_FAC", 32: "AMIS_SAR", 33: "AMIS_AIRLIFT", 34: "AMIS_ASW",
    35: "AMIS_ASHIP", 36: "AMIS_PATROL", 37: "AMIS_TRAINING",
    38: "AMIS_OTHER", 39: "AMIS_TANK", 40: "AMIS_SEARCH",
}
NAME_TO_BYTE = {v: k for k, v in MISSION_NAMES.items()}

# mission_category() — the behavioral many-to-one (same mapping).
CATEGORIES: dict[str, list[int]] = {
    "CAP": [1, 2, 3, 4, 5, 6, 36],
    "Sweep": [7],
    "Intercept": [8, 9],
    "Escort": [10, 11],
    "Strike": [12, 13, 14, 15, 16, 24],
    "SEAD": [17],
    "CAS": [18, 19, 20, 21, 23, 31],
    "Recon": [22, 29, 30],
    "Support": [25, 26, 27, 28, 32, 33, 34, 35, 39, 40],
    "Other": [37, 38],
}
BYTE_TO_CATEGORY = {
    b: cat for cat, bytes_ in CATEGORIES.items() for b in bytes_
}
# The A-G employment families: these categories carry ground ordnance,
# so "armed but never released" is an employment-chain verdict.
ORDNANCE_CATEGORIES = {"Strike", "SEAD", "CAS"}

# campaign_qc exit codes (the B.3/C2 path; --war adds 9-16, --strategy 17).
EXIT_MEANING = {
    1: "input error (bad world/config/class table)",
    2: "filter matched no flights — nothing to run",
    3: "ground ops stalled — spawned, 0 airborne",
    4: "A-G employment broke — armed strike released nothing",
    5: "silent result loss — outcomes happened, ledger recorded nothing",
    6: "tasking ladder drew nothing (generation broke)",
    7: "tasking drew but no route built / nothing materialized",
    8: "ATM pipeline built no packages",
}

# One representative byte per category for --missions auto: the point of
# "auto" is a one-command sweep, not exhaustive coverage (the matrix is
# per BYTE below it; name the bytes when you want the full family).
AUTO_REPRESENTATIVE = {
    "CAP": 2,        # AMIS_BARCAP2 — the stock war's dominant CAP
    "Sweep": 7,
    "Intercept": 9,
    "Escort": 10,
    "Strike": 13,    # AMIS_INTSTRIKE — the deepest A-G slice
    "SEAD": 17,
    "CAS": 18,
    "Recon": 30,
    "Support": 39,   # AMIS_TANK — the store-and-forward supply shuttle
    "Other": 38,
}


# ---------------------------------------------------------------------------
def world_mission_histogram(world_json: Path) -> tuple[Counter, int]:
    """Flights by mission byte, read straight from the WorldState JSON —
    the same histogram campaign_qc prints (world.flights / tasked)."""
    with world_json.open("r", encoding="utf-8") as f:
        w = json.load(f)
    units = w.get("units") or {}
    items = units.get("items") or []
    hist: Counter = Counter()
    tasked = 0
    for u in items:
        if u.get("unit_class") != "flight":
            continue
        mission = u.get("mission", 0)
        if mission:
            hist[mission] += 1
            tasked += 1
    return hist, tasked


def mission_horizons(profiles_json: Path, floor_min: int,
                     cap_min: int) -> dict[str, int]:
    """Per-mission sim horizons from Falcon's own sortie windows: the
    profile's max_time (minutes — the planner's planned duration) is the
    honest prior for "how long until this mission's arc can complete",
    clamped to [floor_min, cap_min]. A 30-min horizon was cutting missions
    mid-flight (the recovery clause was measuring the horizon, not the
    behavior) and could never hold the bridge's 45-min station contracts.
    Missing/zero max_time falls back to the floor."""
    try:
        with profiles_json.open("r", encoding="utf-8") as f:
            profs = json.load(f).get("profiles") or []
    except (json.JSONDecodeError, OSError) as e:
        print(f"note: no per-profile horizons ({e}); flat {floor_min} min")
        return {}
    out: dict[str, int] = {}
    for p in profs:
        name = p.get("name")
        if not name:
            continue
        out[name] = max(floor_min, min(int(p.get("max_time") or 0), cap_min))
    return out


def resolve_mission_spec(spec: str, hist: Counter) -> list[tuple[int, str]]:
    """'AMIS_INTSTRIKE' | 'Strike' | 'auto' | comma list -> [(byte, name)].
    Category names expand to the family bytes PRESENT in the world;
    absent bytes are reported once, not run."""
    spec = spec.strip()
    if not spec:
        return []
    out: list[tuple[int, str]] = []
    for token in (t.strip() for t in spec.split(",") if t.strip()):
        if token.lower() == "auto":
            for cat in CATEGORIES:
                rep = AUTO_REPRESENTATIVE[cat]
                if hist.get(rep, 0) > 0:
                    out.append((rep, MISSION_NAMES[rep]))
            continue
        if token in CATEGORIES:  # category: expand to present family bytes
            present = [b for b in CATEGORIES[token] if hist.get(b, 0) > 0]
            if not present:
                print(f"note: {token} family has no flights in this world")
            out.extend((b, MISSION_NAMES[b]) for b in present)
            continue
        if token in NAME_TO_BYTE:
            out.append((NAME_TO_BYTE[token], token))
            continue
        raise SystemExit(f"unknown mission/category '{token}'")
    # de-dup, keep order
    seen: set[int] = set()
    uniq: list[tuple[int, str]] = []
    for b, n in out:
        if b not in seen:
            seen.add(b)
            uniq.append((b, n))
    return uniq


# ---------------------------------------------------------------------------
def _absorb_summary(row: dict, summary_path: Path) -> None:
    """Parse a run's campaign_qc_summary.json into the row's summary
    (the numbers the verdict pass reads) + tasking block when present."""
    try:
        s = json.loads(summary_path.read_text(encoding="utf-8"))
        row["summary"] = {
            "spawned": s.get("b3_loop", {}).get("aircraft_spawned", 0),
            "routes": s.get("b3_loop", {}).get("routes_attached", 0),
            "airborne": s.get("sim_run", {}).get("airborne_at_end", 0),
            "aircraft": s.get("sim_run", {}).get("aircraft", 0),
            # Complete-phase aircraft at run end — the recovery counter
            # the airborne gate needs once horizons outlive the sorties
            # (everything landed is SUCCESS, not ground ops stalled; the
            # C++ exit-3 gate carries the same two conditions).
            "complete": sum(1 for e in (s.get("sim_run", {})
                                        .get("aircraft_end") or [])
                            if e.get("phase") == "Complete"),
            "armed": s.get("ordnance", {}).get("strike_flights_armed", 0),
            "released": s.get("ordnance", {}).get("bombs_released", 0),
            "impacts": s.get("ordnance", {}).get("bombs_impacted", 0),
        }
        if "tasking" in s:
            t = s["tasking"]
            row["tasking"] = {
                "cycles": t.get("cycles_fired", 0),
                "intents": t.get("intents", 0),
                "drawn": t.get("drawn_aircraft", 0),
                "routes_built": t.get("routes_built", 0),
                "synthetic_spawned": t.get("synthetic_spawned", 0),
                "reinforced": t.get("reinforced_aircraft", 0),
            }
    except (json.JSONDecodeError, OSError) as e:
        row["summary_error"] = str(e)


def run_one(tool: Path, world_json: Path, byte: int, name: str, args,
            out_dir: Path) -> dict:
    """One campaign_qc invocation for one mission byte + verdict parsing."""
    out_dir.mkdir(parents=True, exist_ok=True)
    # ROUTE-HOLD follow-up: the per-mission horizon (profile max_time,
    # clamped) — recorded on the row and embedded in the run itself.
    minutes = (args._horizons.get(name, args.minutes)
               if not args.flat_minutes else args.minutes)

    # --reuse: an existing summary + exit sidecar means this run already
    # finished (resume an interrupted matrix without re-paying runs).
    if getattr(args, "reuse", False):
        prior = out_dir / "campaign_qc_summary.json"
        prior_exit = out_dir / "exit.txt"
        if prior.exists() and prior_exit.exists():
            row = {
                "mission": name,
                "byte": byte,
                "category": BYTE_TO_CATEGORY.get(byte, "Other"),
                "flights_in_world": args._hist.get(byte, 0),
                "minutes": minutes,
                "exit": int(prior_exit.read_text(encoding="ascii").strip()),
                "wall_sec": 0.0,
                "out_dir": str(out_dir),
                "reused": True,
                "cmd": "",
            }
            _absorb_summary(row, prior)
            row["verdict"] = verdict_for(row, args)
            card_column(row, out_dir, args)
            return row

    cmd = [
        str(tool), str(world_json),
        "--mission", name,
        "--minutes", str(minutes),
        "--max-flights", str(args.max_flights),
        "--class-table", str(args.class_table),
        "--config", str(args.config),
        "--profiles", str(args.profiles),
        "--out-dir", str(out_dir),
    ]
    if args.record:
        cmd += ["--record-every", "10"]
    else:
        cmd.append("--no-record")
    if args.tasking > 0:
        cmd += ["--tasking", str(args.tasking),
                "--tasking-cycle", str(args.tasking_cycle)]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True)
    wall = time.time() - t0

    row: dict = {
        "mission": name,
        "byte": byte,
        "category": BYTE_TO_CATEGORY.get(byte, "Other"),
        "flights_in_world": args._hist.get(byte, 0),
        "minutes": minutes,
        "exit": proc.returncode,
        "wall_sec": round(wall, 1),
        "out_dir": str(out_dir),
        "cmd": " ".join(cmd),
    }

    summary_path = out_dir / "campaign_qc_summary.json"
    if summary_path.exists():
        _absorb_summary(row, summary_path)
    (out_dir / "exit.txt").write_text(f"{proc.returncode}\n",
                                      encoding="ascii")

    row["verdict"] = verdict_for(row, args)
    card_column(row, out_dir, args)
    return row


def card_column(row: dict, out_dir: Path, args) -> None:
    """MC-4 - the contract cards: run mission_verify on the run's
    trace (when --record kept one) and absorb the per-flight card
    verdicts. Card FAILs SURFACE in the matrix (the coverage map
    reads them) but do not gate the matrix exit - the owned findings
    (the splice transient, the transfer stub) are card data until
    their tranches land; gating them would hide the coverage behind
    the known reds."""
    if not getattr(args, "verify", False):
        return
    trace = out_dir / "trace.json"
    if not trace.exists():
        row["card"] = {"status": "SKIP", "detail": "no trace (--record? )"}
        return
    verifier = Path(__file__).resolve().parent / "mission_verify.py"
    proc = subprocess.run(
        [sys.executable, str(verifier), str(trace),
         "--out", str(out_dir / "cards")],
        capture_output=True, text=True)
    report = out_dir / "cards" / "mission_report.json"
    if not report.exists():
        row["card"] = {"status": "ERROR",
                       "detail": (proc.stderr or proc.stdout)[-200:]}
        return
    rep = json.loads(report.read_text(encoding="utf-8"))
    flights = rep.get("flights", [])
    passed = sum(1 for c in flights if c.get("overall") == "PASS")
    failed = [c for c in flights if c.get("overall") != "PASS"]
    row["card"] = {
        "status": "PASS" if not failed else "FAIL",
        "flights": len(flights),
        "passed": passed,
        "failed": len(failed),
        "failed_detail": [
            {"callsign": c.get("callsign"), "mission": c.get("mission"),
             "clauses": [cl for cl in c.get("clauses", [])
                         if cl.get("verdict") == "FAIL"]}
            for c in failed],
    }


def verdict_for(row: dict, args) -> tuple[str, str]:
    """(status, detail) — the C++ gate first, then the category
    expectations the C++ side doesn't know about."""
    code = row["exit"]
    if code == 0:
        pass  # fall through to the expectation pass
    elif code == 2:
        return ("SKIPPED", "no flights of this type in this world")
    elif code in EXIT_MEANING:
        return ("FAIL", f"campaign_qc gate: exit {code} — {EXIT_MEANING[code]}")
    else:
        return ("FAIL", f"campaign_qc exit {code} (war/accel gate — "
                        "see campaign_qc header)")

    s = row.get("summary")
    if not s:
        return ("FAIL", "no campaign_qc_summary.json parsed")

    cat = row["category"]
    minutes = row.get("minutes", args.minutes)
    notes: list[str] = []

    if s["routes"] != s["spawned"]:
        return ("FAIL",
                f"routes {s['routes']} != spawned {s['spawned']} "
                "(saved plan did not attach)")
    # 0 airborne at end is a stall only when NOTHING recovered — the
    # same two conditions the C++ exit-3 gate carries. A horizon past
    # the sorties' length lands every flight Complete on the deck, and
    # that is the system working.
    if s["airborne"] < 1 and s.get("complete", 0) == 0:
        return ("FAIL",
                f"0 airborne at end and none recovered "
                f"({s['aircraft']} spawned, {minutes} min horizon) — "
                "ground ops stalled")
    if s["airborne"] < s["aircraft"]:
        notes.append(f"{s['aircraft'] - s['airborne']} not airborne at end "
                     f"({s.get('complete', 0)} recovered)")
    elif s.get("complete", 0) == s["aircraft"]:
        notes.append(f"all {s['aircraft']} recovered by {minutes} min")

    if cat in ORDNANCE_CATEGORIES:
        if s["armed"] > 0 and s["released"] == 0:
            return ("FAIL",
                    "EMPLOYMENT: armed %d, released 0 after %d min — no "
                    "ground target ever set? autopsy: trace.json ai_state "
                    "+ target_description" % (s["armed"], minutes))
        if s["armed"] == 0 and s["released"] == 0:
            # strike_flights_armed counts live Bomb stations POST-run, so
            # a flight that released its whole stick ends at 0 — that is
            # delivery, not a loadout concern.
            notes.append("unarmed (loadout/weapon-table concern)")
        elif s["released"] > 0:
            notes.append("released %d, impacts %d"
                         % (s["released"], s["impacts"]))
    elif s.get("released", 0) > 0:
        notes.append("released %d (non-ordnance category?)"
                     % s["released"])

    return ("PASS", "; ".join(notes) if notes else
            f"spawned {s['spawned']}, airborne {s['airborne']}/"
            f"{s['aircraft']} at {minutes} min")


# ---------------------------------------------------------------------------
def write_markdown(rows: list[dict], args, path: Path) -> None:
    hz = ("per-profile max_time, floor "
          f"{args.minutes} / cap {args.max_horizon}"
          if getattr(args, "_horizons", None) else f"flat {args.minutes}")
    lines = [
        "# qc_missions — the per-mission-type QC matrix",
        "",
        f"world: `{args.world_json}`  ",
        f"horizon: {hz} · cap: {args.max_flights} "
        f"flights/type · record: {'on' if args.record else 'off'} · "
        f"tasking: {args.tasking or 'off'} min "
        f"(cycle {args.tasking_cycle} s)",
        "",
        "| mission | cat | in world | horizon | spawned | routes | "
        "airborne | armed | released | exit | verdict | cards |",
        "|---|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        s = r.get("summary", {})
        status, detail = r["verdict"]
        c = r.get("card", {})
        cards = (f"{c.get('passed', '-')}/{c.get('flights', '-')}"
                 if isinstance(c, dict) and 'status' in c else "-")
        lines.append(
            f"| {r['mission']} | {r['category']} | {r['flights_in_world']} "
            f"| {r.get('minutes', args.minutes)} min "
            f"| {s.get('spawned', '-')} | {s.get('routes', '-')} "
            f"| {s.get('airborne', '-')}/{s.get('aircraft', '-')} "
            f"| {s.get('armed', '-')} | {s.get('released', '-')} "
            f"| {r['exit']} | **{status}** — {detail} | {cards} |")
    lines += [
        "",
        "Row dirs carry each run's artifacts; `trace.json` (with "
        "`--record`) opens in the world viewer's replay mode.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    p = argparse.ArgumentParser(
        description="Per-mission-type QC matrix over campaign_qc")
    p.add_argument("world_json", type=Path)
    p.add_argument("--tool", type=Path,
                   default=repo / "build/f4-simulation/campaign_qc")
    p.add_argument("--class-table", type=Path,
                   default=repo / "Data/Classes/falcon4.ct.json")
    p.add_argument("--config", type=Path,
                   default=repo / "build/generated_fixtures/f16.json")
    p.add_argument("--profiles", type=Path,
                   default=repo / "build/generated_campaign/"
                                  "MissionProfiles.json")
    p.add_argument("--missions", default="auto",
                   help="comma list of AMIS names and/or categories, "
                        "or 'auto' (one representative per category)")
    p.add_argument("--minutes", type=int, default=30,
                   help="sim horizon FLOOR per run (default 30); the "
                        "per-mission horizon comes from the profile's "
                        "max_time clamped to [--minutes, --max-horizon]")
    p.add_argument("--max-horizon", type=int, default=90,
                   help="per-mission horizon cap, minutes (default 90 — "
                        "covers the bridge's 45-min station contracts "
                        "plus transit and recovery)")
    p.add_argument("--flat-minutes", action="store_true",
                   help="run every type at --minutes (the pre-profile "
                        "flat-horizon behavior; for A/B comparisons)")
    p.add_argument("--max-flights", type=int, default=4,
                   help="spawn cap per run (default 4)")
    p.add_argument("--record", action="store_true",
                   help="keep trace.json (viewer replay / autopsy)")
    p.add_argument("--tasking", type=int, default=0,
                   help="synthetic ladder window, minutes (0 = off)")
    p.add_argument("--tasking-cycle", type=int, default=1800)
    p.add_argument("--jobs", type=int, default=1)
    p.add_argument("--reuse", action="store_true",
                   help="skip a run whose campaign_qc_summary.json "
                        "already exists (resume an interrupted matrix)")
    p.add_argument("--no-verify", action="store_true",
                   help="skip the MC-2 contract cards (mission_verify.py) "
                        "on each run's trace")
    p.add_argument("--out-root", type=Path, default=None)
    args = p.parse_args()

    args.world_json = args.world_json.resolve()
    for req in (args.tool, args.class_table, args.config, args.profiles):
        if not req.exists():
            raise SystemExit(f"missing: {req}")
    # campaign_qc resolves relative data paths against --out-dir — always
    # hand it absolute ones (the same trap the C++ usage text hints at).
    for a in ("tool", "class_table", "config", "profiles"):
        setattr(args, a, getattr(args, a).resolve())

    hist, tasked = world_mission_histogram(args.world_json)
    args._hist = hist
    args._horizons = ({} if args.flat_minutes else
                      mission_horizons(args.profiles, args.minutes,
                                       args.max_horizon))
    hz = (f"per-profile (floor {args.minutes}, cap {args.max_horizon})"
          if args._horizons else f"flat {args.minutes}")
    print(f"world: {args.world_json.name}  flights(tasked)={tasked}  "
          f"distinct mission bytes={len(hist)}  horizon: {hz}")

    targets = resolve_mission_spec(args.missions, hist)
    if not targets:
        raise SystemExit("no missions selected (spec + world histogram "
                         "intersect to nothing)")
    names = ", ".join(n for _, n in targets)
    print(f"matrix: {len(targets)} runs — {names}")

    if args.out_root is None:
        args.out_root = (repo / "qc-missions" /
                         time.strftime("%Y%m%d-%H%M%S"))
    args.out_root = args.out_root.resolve()
    args.out_root.mkdir(parents=True, exist_ok=True)
    args.verify = not args.no_verify

    def work(item: tuple[int, str]) -> dict:
        b, n = item
        return run_one(args.tool, args.world_json, b, n, args,
                       args.out_root / n.removeprefix("AMIS_").lower())

    t0 = time.time()
    if args.jobs > 1 and len(targets) > 1:
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=args.jobs) as ex:
            rows = list(ex.map(work, targets))
    else:
        rows = [work(t) for t in targets]
    print(f"runs finished in {time.time() - t0:.0f}s")

    # ---- the matrix -----------------------------------------------------
    print()
    hdr = (f"{'mission':<17}{'cat':<10}{'n':>4}{'min':>5}{'sp':>4}{'rt':>4}"
           f"{'air':>5}{'arm':>5}{'rel':>5}{'exit':>6}  verdict")
    print(hdr)
    print("-" * len(hdr))
    any_fail = False
    for r in rows:
        s = r.get("summary", {})
        status, detail = r["verdict"]
        if status == "FAIL":
            any_fail = True
        c = r.get("card", {})
        cmark = (f"{c['status']}/{c.get('flights', 0)}"
                 if isinstance(c, dict) and 'status' in c else "-")
        print(f"{r['mission']:<17}{r['category']:<10}"
              f"{r['flights_in_world']:>4}{r.get('minutes', args.minutes):>5}"
              f"{s.get('spawned', 0):>4}"
              f"{s.get('routes', 0):>4}{s.get('airborne', 0):>5}"
              f"{s.get('armed', 0):>5}{s.get('released', 0):>5}"
              f"{r['exit']:>6}  {status}: {detail}  [cards {cmark}]")

    (args.out_root / "qc_matrix.json").write_text(
        json.dumps({
            "world": str(args.world_json),
            "minutes": args.minutes,
            "max_horizon": args.max_horizon,
            "horizons": "flat" if args.flat_minutes else
                        "per-profile max_time (floor/cap clamped)",
            "max_flights": args.max_flights,
            "tasking": args.tasking,
            "tasking_cycle": args.tasking_cycle,
            "rows": rows,
        }, indent=1) + "\n",
        encoding="utf-8")
    write_markdown(rows, args, args.out_root / "qc_matrix.md")
    print(f"\nwrote: {args.out_root}/qc_matrix.json")
    print(f"wrote: {args.out_root}/qc_matrix.md")
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# mission_verify — the contract verifier + report card (MC-2,
# MISSION_CONTRACT_PLAN.md §3/§5).
#
# Reads a recorded run (the f4-flight-recording JSON the sim writes —
# with the MC-1 joins: per-snapshot mission identity + the mission
# event stream), evaluates the flight's ARCHETYPE CONTRACT (a typed
# clause list), and emits the report card:
#
#   mission_report.md    the human card (verdicts + measured numbers +
#                        tick references into the trace)
#   mission_report.json  the schema-versioned machine card (diffable —
#                        the review artifact, the ledger-MD5 discipline)
#
# The verifier is GENERIC: contracts are data (archetype -> clause
# list); the evaluator walks clauses. Archetype selection is from the
# flight's own AMIS name; flights without campaign identity (scenario
# fixtures) get the generic contract.
#
# Verdicts: PASS / FAIL (band missed — the measured number is on the
# card) / SKIP (clause not applicable, with the reason). A red clause
# is fixed or added to the known-reds-style exception list (MC-4 owns
# the list; the band lives in the contract data below until then).
#
# Usage:
#   python3 scripts/mission_verify.py qc/diag_x/trace.json
#   python3 scripts/mission_verify.py trace.json --out qc/reports/x
#   python3 scripts/mission_verify.py --selftest
#
# Exit codes: 0 all flights pass (or only SKIP); 1 any FAIL; 2 usage.
#
# Stdlib only. Python 3.10+.

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Contract data — archetype family -> clause list. Bands are data: a
# re-pin is a diff here (and shows in the card history).
# ---------------------------------------------------------------------------

DELIVERY_ACTIONS = {14, 15, 17, 18, 19}   # GNDSTRIKE/NAVSTRIKE/STRIKE/BOMB/SEAD
CAP_MISSIONS = ("BARCAP", "BARCAP2", "HAVCAP", "TARCAP", "RESCAP",
                "AMBUSHCAP", "SWEEP", "PATROL", "INTERCEPT", "ALERT")
SUPPORT_MISSIONS = ("TANKER", "AWACS", "JSTAR", "ECM", "RECON", "SAR",
                    "BDA", "FAC")
STRIKE_MISSIONS = ("STRIKE", "SEADSTRIKE", "OCASTRIKE", "INTSTRIKE",
                   "DEEPSTRIKE", "STSTRIKE", "CAS", "SAD", "BAI",
                   "ONCALLCAS", "PRPLANCAS", "STRATBOMB")

TOT_BAND_S = 300.0            # |capture - appointment| tolerance
PATH_MISS_BAND_FT = 2000.0    # per-waypoint capture miss distance
STATION_MIN_DWELL_S = 600.0   # a station hold shorter than this is a drive-by
FLARE_PHASES = ("Approach", "Complete")


def archetype_for(mission: str) -> str:
    """Mission wire name -> contract family (MISSION_CONTRACT_PLAN §3)."""
    m = (mission or "").upper()
    if any(k in m for k in CAP_MISSIONS):
        return "cap"
    if any(k in m for k in STRIKE_MISSIONS):
        return "strike"
    if any(k in m for k in SUPPORT_MISSIONS):
        return "support"
    if "ESCORT" in m:
        return "escort"
    return "generic"


def clauses_for(archetype: str) -> list[dict]:
    """The archetype's clause list. Each clause: kind + band data."""
    clauses = [
        {"kind": "timeline", "require_enroute": True},
        {"kind": "path", "max_miss_ft": PATH_MISS_BAND_FT},
    ]
    if archetype in ("cap", "support"):
        clauses.append({"kind": "station", "min_dwell_s": STATION_MIN_DWELL_S})
    if archetype == "strike":
        clauses.append({"kind": "tot", "band_s": TOT_BAND_S})
        clauses.append({"kind": "employment", "min_releases": 1})
    if archetype == "cap":
        clauses.append({"kind": "tot", "band_s": TOT_BAND_S})
    clauses.append({"kind": "first_attempt", "allow_goaround": False})
    clauses.append({"kind": "duration", "max_sortie_min": 120.0})
    clauses.append({"kind": "recovery"})
    return clauses


# ---------------------------------------------------------------------------
# Flight extraction — the trace -> one record per flight.
# ---------------------------------------------------------------------------

def flights_from_trace(trace: dict) -> dict:
    flights: dict[int, dict] = {}

    def slot(eid: int) -> dict:
        f = flights.get(eid)
        if f is None:
            f = {
                "entity_id": eid,
                "callsign": "",
                "mission": "",
                "home_airbase_vu": 0,
                "tot_s": 0.0,
                "station_contract_s": 0.0,
                "snapshots": [],
                "events": [],
            }
            flights[eid] = f
        return f

    for s in trace.get("snapshots", []):
        f = slot(s["entity_id"])
        if not f["callsign"] and s.get("callsign"):
            f["callsign"] = s["callsign"]
        if not f["mission"] and s.get("mission"):
            f["mission"] = s["mission"]
            f["home_airbase_vu"] = s.get("home_airbase_vu", 0)
            f["tot_s"] = s.get("tot_s", 0.0)
            f["station_contract_s"] = s.get("station_contract_s", 0.0)
        f["snapshots"].append(s)

    for e in trace.get("mission_events", []):
        slot(e["entity_id"])["events"].append(e)

    # INIT-1 follow-up: released ordnance are simulated entities too —
    # the moment flights actually deliver, each bomb's snapshots enter
    # the trace and get carded as a "flight" that never phases, never
    # recovers (16 MK-82 FAIL rows on the first releasing matrix). The
    # recorder already stamps its weapon tracks (snap.missile — the
    # replay discriminator), so an entity carrying that stamp anywhere
    # is ordnance, not a flight.
    flights = {eid: f for eid, f in flights.items()
               if not any(s.get("missile") for s in f["snapshots"])}

    for f in flights.values():
        f["archetype"] = archetype_for(f["mission"])
        f["clauses"] = clauses_for(f["archetype"])
        f["phases"] = phase_sequence(f)
        f["t_end"] = f["snapshots"][-1]["sim_time_s"] if f["snapshots"] else 0.0
    return flights


def phase_sequence(f: dict) -> list[str]:
    """The ordered mission-phase arc, from the PhaseChanged events."""
    seq: list[str] = []
    for e in f["events"]:
        if e["kind"] != "phase_changed":
            continue
        frm = e.get("from_phase", "")
        if not seq and frm:
            seq.append(frm)
        seq.append(e.get("to_phase", ""))
    return seq


# ---------------------------------------------------------------------------
# The generic clause evaluator. Every clause returns
# (verdict, detail) where verdict ∈ {PASS, FAIL, SKIP}.
# ---------------------------------------------------------------------------

def eval_timeline(f: dict, clause: dict) -> tuple[str, str]:
    if not f["phases"]:
        return "SKIP", "no phase events recorded (pre-MC-1 trace?)"
    # An arc that STARTS at Approach is the legal approach-start contract
    # (StartPhase::Approach — the landing_only fixtures): Enroute is only
    # required when the flight actually launched from the ground.
    ground_launch = bool(f["phases"]) and f["phases"][0] == "Ground"
    if (clause.get("require_enroute") and ground_launch
            and "Enroute" not in f["phases"]):
        return "FAIL", "never went Enroute (phases: " + " -> ".join(f["phases"]) + ")"
    if f["phases"][0] == "Ground" and "Ground" in f["phases"][1:]:
        return "FAIL", "phase arc returns to Ground"
    return "PASS", " -> ".join(f["phases"])


def enroute_window(f: dict) -> tuple[float, float]:
    """The [t0, t1) the nav owns the jet: Enroute entry -> Approach entry
    (or run end). The Ground roll and the Approach are other clauses'/phases'
    jurisdiction."""
    t0 = t1 = None
    for e in f["events"]:
        if e["kind"] != "phase_changed":
            continue
        if e.get("to_phase") == "Enroute" and t0 is None:
            t0 = e["sim_time_s"]
        if e.get("to_phase") == "Approach" and t0 is not None:
            t1 = e["sim_time_s"]
            break
    if t0 is None:
        return (None, None)
    return (t0, t1 if t1 is not None else f["t_end"] + 1.0)


def eval_path(f: dict, clause: dict) -> tuple[str, str]:
    """PATH = STEADY-STATE leg tracking, phase-scoped to Enroute. The
    departure transient is reported, not banded: a splice-resumed flight
    legitimately starts its first leg off-line (the departure geometry vs
    the T3 route-path projection — measured: 21,308 ft converging over
    ~4 min on the stock BARCAP, then 110-390 ft held for the rest of the
    flight). The band governs the convergence, i.e. the p90 of the final
    quarter of the Enroute window. The per-waypoint capture distance is
    NOT a metric at all: the captures are turn-anticipated (NAV-B cuts
    the corner up to a turn radius early BY DESIGN).
    """
    band = clause["max_miss_ft"]
    t0, t1 = enroute_window(f)
    if t0 is None:
        return "SKIP", "no Enroute phase (airborne-start approach or unrouted)"
    xs = [(s["sim_time_s"], abs(s.get("cross_track_error_ft", 0.0)))
          for s in f["snapshots"]
          if t0 <= s["sim_time_s"] < t1
          and s.get("cross_track_error_ft", 0.0) > 0.0]
    if not xs:
        return "SKIP", ("snapshots carry no leg cross-track (pre-MC-2 "
                        "documents)")
    xs.sort()
    quarter = xs[int(len(xs) * 0.75):]
    steady_p90 = sorted(x for _, x in quarter)[
        int(len(quarter) * 0.9)] if quarter else 0.0
    peak = max(x for _, x in xs)
    if steady_p90 > band:
        return "FAIL", (f"steady-state leg cross-track p90 {steady_p90:.0f} ft "
                        f"(tol {band:.0f} ft) — the flight is not holding "
                        f"its route")
    transient = (f"; departure transient peaked {peak:.0f} ft"
                 if peak > band else "")
    return "PASS", (f"steady-state cross-track p90 {steady_p90:.0f} ft "
                    f"(tol {band:.0f}){transient}")


def eval_station(f: dict, clause: dict) -> tuple[str, str]:
    if f.get("station_contract_s", 0.0) <= 0.0:
        # The route promised no station (the stock saves' own BARCAPs fly
        # the ATO planner's waypoints; the racetrack rides the strategy
        # layer's own filings). Skipping is the honest verdict — a FAIL
        # here would condemn every save-driven CAP.
        return "SKIP", "no station contract on this route"
    entered = [e for e in f["events"] if e["kind"] == "station_entered"]
    exited = [e for e in f["events"] if e["kind"] == "station_exited"]
    if not entered:
        return "FAIL", "never held station"
    # Dwell: each entry pairs the next exit; an unpaired entry dwells to
    # the end of the recording (the run ended while holding).
    dwells = []
    for e in entered:
        ex = next((x for x in exited if x["sim_time_s"] >= e["sim_time_s"]), None)
        end = ex["sim_time_s"] if ex else f["t_end"]
        dwells.append(end - e["sim_time_s"])
    best = max(dwells)
    band = clause["min_dwell_s"]
    if best < band:
        return "FAIL", f"best station dwell {best:.0f} s (min {band:.0f})"
    return "PASS", f"station dwell {best:.0f} s (min {band:.0f})"


def eval_tot(f: dict, clause: dict) -> tuple[str, str]:
    if f["tot_s"] <= 0.0:
        return "SKIP", "no TOT appointed"
    if f["tot_s"] > f["t_end"]:
        # INIT-1c: the saved ATO's appointment is campaign-RELATIVE and
        # can be days out (the measured TestCamp delivery families: 4
        # stale rows, 109 beyond, 0 in-horizon). The sim does not model
        # the ATO push wait — flights spawn at their bases and fly at
        # the run's start — so an appointment beyond the run is scoped
        # out, not judged against a push that never happened.
        return "SKIP", (f"appointment beyond the run (+{f['tot_s'] / 3600:.0f} h "
                        "against a push wait the spawn does not model)")
    deliveries = [e for e in f["events"]
                  if e["kind"] == "waypoint_captured"
                  and e.get("wp_action") in DELIVERY_ACTIONS]
    if not deliveries:
        return "FAIL", (f"no delivery waypoint captured (TOT appointed "
                        f"{f['tot_s']:.0f}s)")
    d = deliveries[0]
    delta = d["sim_time_s"] - f["tot_s"]
    band = clause["band_s"]
    sign = "+" if delta >= 0 else "-"
    dd = f"{sign}{abs(delta) / 60:.0f}:{abs(delta) % 60:02.0f}"
    if abs(delta) > band:
        return "FAIL", (f"delivery {dd} vs TOT (tol ±{band:.0f}s) — "
                        f"trace tick {d.get('tick', 0)}")
    return "PASS", f"delivery {dd} vs TOT (tol ±{band:.0f}s)"


def eval_employment(f: dict, clause: dict) -> tuple[str, str]:
    """EMPLOYMENT = opportunity vs execution. The route's delivery-action
    waypoints (wp_action 14-19, carried on the capture events) are the
    OPPORTUNITY; the BombReleased combat events are the EXECUTION. A
    flight with no delivery waypoint on its route never had the chance —
    SKIP (the loadout/tasking concern qc_missions' own taxonomy names) —
    that split is what separates "the chain broke" from "there was
    nothing to employ". Measured on the isolation runs: the saved strike
    routes carry their delivery waypoints with action 0 and no target
    stamp (the A-G route arming's saved-flight gap — the EMPL tranche's
    finding), which reads SKIP until the arming lands.
    """
    releases = 0
    for e in f.get("combat_events", []):
        if e.get("kind") == "bomb_released" and e.get("subject_id") == f["entity_id"]:
            releases += 1
    opportunities = [e for e in f["events"]
                     if e["kind"] == "waypoint_captured"
                     and e.get("wp_action") in DELIVERY_ACTIONS]
    died = any(e.get("kind") == "entity_killed" and e.get("subject_id") == f["entity_id"]
               for e in f.get("combat_events", []))
    if releases >= clause["min_releases"]:
        return "PASS", f"{releases} release(s)"
    if died:
        return "SKIP", "shot down before employment (0 releases)"
    if not opportunities:
        # A receiver route (WP_REFUEL waypoints — the AAR stack rides the
        # plan) legitimately never delivers: the flight is the package's
        # refueling client, not a shooter. Measured: the single-mission
        # STRIKE isolation drew exactly such a flight (a save package's
        # receiver — its route: ingress, the refuel anchor, the STK1-3
        # racetrack, the recovery tail), held the stack, and recovered
        # when no tanker answered. Correct degraded AAR, not a defect.
        refuel_legs = sum(1 for e in f["events"]
                          if e["kind"] == "waypoint_captured"
                          and e.get("wp_action") == 4)
        if refuel_legs:
            return "SKIP", (f"receiver route (no delivery opportunity; "
                            f"{refuel_legs} refuel leg(s) flown — AAR "
                            f"package client)")
        return "SKIP", ("no delivery waypoint on the route (no employment "
                        "opportunity — the route arming/tasking concern)")
    return "FAIL", (f"{releases} releases across {len(opportunities)} "
                    f"delivery waypoint(s) (min {clause['min_releases']}) — "
                    f"the flight survived without employing")


def eval_recovery(f: dict, clause: dict) -> tuple[str, str]:
    if f["phases"] and f["phases"][-1] == "Complete":
        return "PASS", "mission Complete (recovered)"
    if f["phases"] and f["phases"][-1] in FLARE_PHASES:
        # still recovering at run end — honest, but not complete
        return "FAIL", f"run ended mid-recovery ({f['phases'][-1]})"
    snaps = f["snapshots"]
    if snaps and snaps[-1].get("on_ground") and snaps[-1].get("vcas_kts", 999) < 30:
        return "PASS", "on the deck, stopped"
    return "FAIL", ("last phase " + (f["phases"][-1] if f["phases"] else "?")
                    + " — no recovery, no booked death")


def eval_first_attempt(f: dict, clause: dict) -> tuple[str, str]:
    """FIRST_ATTEMPT: a go-around is a deviation from the plan — FAIL,
    unless the contract is deliberately testing go-arounds
    (allow_goaround). Counted from the snapshot ai_state transitions
    (the landing module's GoAround state persists minutes; the 6-s
    decimation sampling cannot miss it)."""
    if clause.get("allow_goaround"):
        return "SKIP", "the contract tests go-arounds"
    states = [s.get("ai_state", "") for s in f["snapshots"]]
    n = sum(1 for i, st in enumerate(states)
            if st == "GoAround" and (i == 0 or states[i - 1] != "GoAround"))
    if n == 0:
        return "PASS", "first attempt"
    return "FAIL", f"{n} go-around(s) before recovery"


def eval_duration(f: dict, clause: dict) -> tuple[str, str]:
    """DURATION: wheels-up -> recovery inside the band. The saved wires'
    own waypoint times are the ATO planner's multi-day horizon (the
    aggregate-clock finding), so the honest expectation is a SORTIE
    duration, not an absolute clock time."""
    if not f["phases"] or f["phases"][-1] not in FLARE_PHASES:
        return "SKIP", "no recovery to time"
    # Wheels-up: the Enroute entry, or the first airborne snapshot.
    t_up = None
    for e in f["events"]:
        if e["kind"] == "phase_changed" and e.get("to_phase") == "Enroute":
            t_up = e["sim_time_s"]
            break
    if t_up is None:
        airborne = next((s["sim_time_s"] for s in f["snapshots"]
                         if not s.get("on_ground")), None)
        t_up = airborne
    if t_up is None:
        return "SKIP", "never airborne"
    duration_s = f["t_end"] - t_up
    band = clause["max_sortie_min"] * 60.0
    if duration_s > band:
        return "FAIL", (f"sortie {duration_s / 60:.0f} min "
                        f"(max {clause['max_sortie_min']:.0f})")
    return "PASS", f"sortie {duration_s / 60:.0f} min (max {clause['max_sortie_min']:.0f})"


EVALUATORS = {
    "timeline": eval_timeline,
    "path": eval_path,
    "station": eval_station,
    "tot": eval_tot,
    "employment": eval_employment,
    "first_attempt": eval_first_attempt,
    "duration": eval_duration,
    "recovery": eval_recovery,
}


def attach_combat_events(flights: dict, trace: dict) -> None:
    for f in flights.values():
        f["combat_events"] = trace.get("combat_events", [])


# ---------------------------------------------------------------------------
# Cards.
# ---------------------------------------------------------------------------

def flight_card(f: dict) -> dict:
    results = []
    for clause in f["clauses"]:
        verdict, detail = EVALUATORS[clause["kind"]](f, clause)
        results.append({"clause": clause["kind"], "verdict": verdict,
                        "detail": detail})
    overall = "PASS" if all(r["verdict"] in ("PASS", "SKIP") for r in results) \
        else "FAIL"
    snaps = f["snapshots"]
    return {
        "schema": "f4-mission-card/1",
        "entity_id": f["entity_id"],
        "callsign": f["callsign"],
        "mission": f["mission"] or "(non-campaign)",
        "archetype": f["archetype"],
        "flight_vu": f.get("home_airbase_vu", 0) and None,  # placeholder-free below
        "home_airbase_vu": f["home_airbase_vu"],
        "tot_s": f["tot_s"],
        "overall": overall,
        "clauses": results,
        "fuel_lbs_at_end": snaps[-1].get("fuel_lbs") if snaps else None,
    }


def card_md(cards: list[dict], scenario: str) -> str:
    lines = [f"# Mission report — {scenario}", ""]
    for c in cards:
        mark = "PASS" if c["overall"] == "PASS" else "**FAIL**"
        lines.append(f"## {c['callsign'] or c['entity_id']} — {c['mission']} "
                     f"({c['archetype']}) — {mark}")
        lines.append("")
        for r in c["clauses"]:
            lines.append(f"- {r['clause']:<10} {r['verdict']:<4} {r['detail']}")
        if c.get("fuel_lbs_at_end") is not None:
            lines.append(f"- fuel at end: {c['fuel_lbs_at_end']:.0f} lbs")
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Selftest — crafted traces pinning the evaluator's verdicts.
# ---------------------------------------------------------------------------

def selftest() -> int:
    def ev(kind, **kw):
        e = {"kind": kind, "sim_time_s": kw.pop("t", 0.0),
             "tick": kw.pop("tick", 0), "entity_id": 1}
        e.update(kw)
        return e

    def snap(t, **kw):
        s = {"entity_id": 1, "sim_time_s": t, "tick": int(t * 60),
             "callsign": "T1", "mission": kw.pop("mission", ""),
             "ai_mode": "NavigationMode",
             "on_ground": kw.pop("on_ground", False), "vcas_kts": 300,
             "fuel_lbs": 5000, "cross_track_error_ft": 120.0}
        s.update(kw)
        return s

    def base_events(tot=0.0):
        evs = [
            ev("phase_changed", t=10, from_phase="Ground", to_phase="Enroute"),
            ev("waypoint_captured", t=600, wp_index=0, wp_action=0,
               cross_track_ft=100),
            ev("waypoint_captured", t=1200, wp_index=1, wp_action=0,
               cross_track_ft=150),
            ev("station_entered", t=700, wp_index=1),
        ]
        if tot > 0:
            evs.append(ev("waypoint_captured", t=tot - 60, wp_index=2,
                          wp_action=17, cross_track_ft=200))
        evs.append(ev("phase_changed", t=2000, from_phase="Enroute",
                      to_phase="Approach"))
        evs.append(ev("phase_changed", t=2100, from_phase="Approach",
                      to_phase="Complete"))
        return evs

    ok = True

    def check(name, got, want):
        nonlocal ok
        if got != want:
            print(f"SELFTEST FAIL: {name}: got {got!r}, want {want!r}")
            ok = False
        else:
            print(f"selftest: {name}: {got}")

    # 1. A good BARCAP: station + full arc -> PASS overall.
    trace = {
        "snapshots": [snap(t) for t in range(0, 2400, 60)],
        "mission_events": base_events(),
        "combat_events": [],
    }
    for s in trace["snapshots"]:
        s["mission"] = "AMIS_BARCAP"
        s["station_contract_s"] = 1800.0   # the strategy layer's racetrack
    flights = flights_from_trace(trace)
    attach_combat_events(flights, trace)
    card = flight_card(list(flights.values())[0])
    check("good barcap overall", card["overall"], "PASS")
    verdicts = {r["clause"]: r["verdict"] for r in card["clauses"]}
    check("good barcap station", verdicts["station"], "PASS")
    check("good barcap recovery", verdicts["recovery"], "PASS")

    # 2. A strike with a delivery waypoint and no release -> the real
    # EMPLOYMENT FAIL (opportunity existed, execution didn't happen).
    evs2 = base_events()
    evs2.append(ev("waypoint_captured", t=1500, wp_index=2, wp_action=17,
                   cross_track_ft=180))
    trace2 = {
        "snapshots": [snap(t, mission="AMIS_STRIKE") for t in range(0, 2400, 60)],
        "mission_events": evs2,
        "combat_events": [],
    }
    flights = flights_from_trace(trace2)
    attach_combat_events(flights, trace2)
    card = flight_card(list(flights.values())[0])
    verdicts = {r["clause"]: r["verdict"] for r in card["clauses"]}
    check("no-release strike employment", verdicts["employment"], "FAIL")
    check("no-release strike overall", card["overall"], "FAIL")

    # 3. A strike with a TOT appointment missed by 400 s -> TOT FAIL.
    trace3 = {
        "snapshots": [snap(t, mission="AMIS_STRIKE") for t in range(0, 2400, 60)],
        "mission_events": base_events(tot=600.0),   # delivery at 540 vs TOT 600? no:
        "combat_events": [],
    }
    # base_events(tot) puts the delivery at tot-60 = 540; appoint 100 → miss -460
    trace3["snapshots"][0]["tot_s"] = 100.0
    flights = flights_from_trace(trace3)
    attach_combat_events(flights, trace3)
    card = flight_card(list(flights.values())[0])
    verdicts = {r["clause"]: r["verdict"] for r in card["clauses"]}
    check("tot-miss strike tot", verdicts["tot"], "FAIL")

    # 4. Scenario flight (no identity) -> generic contract; a landing-only
    # arc (Approach -> Complete) whose PATH is owned by the landing clauses.
    trace4 = {
        "snapshots": [snap(t) for t in range(0, 600, 60)],
        "mission_events": [
            ev("phase_changed", t=60, from_phase="Spawn",
               to_phase="Approach"),
            ev("phase_changed", t=500, from_phase="Approach",
               to_phase="Complete"),
        ],
        "combat_events": [],
    }
    flights = flights_from_trace(trace4)
    attach_combat_events(flights, trace4)
    card = flight_card(list(flights.values())[0])
    check("scenario archetype", card["archetype"], "generic")
    verdicts = {r["clause"]: r["verdict"] for r in card["clauses"]}
    check("scenario arc pass", verdicts["timeline"], "PASS")
    check("scenario path skip", verdicts["path"], "SKIP")

    print("selftest", "GREEN" if ok else "RED")
    return 0 if ok else 1


# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="MC-2 — the mission contract verifier + report card")
    ap.add_argument("trace", nargs="?", help="the recording (trace.json)")
    ap.add_argument("--out", default="",
                    help="output dir for the cards (default: beside the trace)")
    ap.add_argument("--selftest", action="store_true",
                    help="run the evaluator's verdict pins and exit")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.trace:
        ap.error("a trace path is required (or --selftest)")

    trace = json.loads(Path(args.trace).read_text(encoding="utf-8"))
    scenario = trace.get("scenario", Path(args.trace).stem)
    flights = flights_from_trace(trace)
    attach_combat_events(flights, trace)
    cards = [flight_card(f) for _, f in sorted(flights.items())]

    out_dir = Path(args.out) if args.out else Path(args.trace).parent
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "mission_report.json").write_text(
        json.dumps({"scenario": scenario, "flights": cards}, indent=2) + "\n",
        encoding="utf-8")
    (out_dir / "mission_report.md").write_text(
        card_md(cards, scenario), encoding="utf-8")

    failed = [c for c in cards if c["overall"] != "PASS"]
    for c in cards:
        mark = "PASS" if c["overall"] == "PASS" else "FAIL"
        print(f"{mark}  {c['callsign'] or c['entity_id']}  {c['mission']} "
              f"({c['archetype']})")
        for r in c["clauses"]:
            print(f"    {r['clause']:<10} {r['verdict']:<4} {r['detail']}")
    print(f"\n{len(cards) - len(failed)}/{len(cards)} flights PASS — "
          f"cards: {out_dir / 'mission_report.md'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

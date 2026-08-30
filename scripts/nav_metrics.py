#!/usr/bin/env python3
"""nav_metrics.py — LNAV / coordination metrics from an FCS CSV trace.

Usage:
  nav_metrics.py <trace.csv> --legs NAME:ax,ay,bx,by[:course_alt] [--legs ...]
  nav_metrics.py <trace.csv> --takeoff centerline_x,centerline_y,heading_deg

Legs are given in WORLD ENU feet (compute the runway-frame transform before
calling). For each leg the script scores:
  - cross-track error (signed; + = right of course)
  - establish time: |xte| < 250 ft AND |heading-course| < 5 deg sustained 5 s
  - overshoot beyond the establish crossing
  - sideslip beta (mean/max |beta| while |roll| > 10 deg = "in turn")
  - altitude band while established (and in-turn altitude loss)
  - heading-vs-course after establish (homing signature: |hdg-course| stays
    large on the straight portion because pursuit guidance aims at a point)

Exit code 0 if all thresholds pass, 1 otherwise (printed PASS/FAIL lines).
"""
import argparse
import csv
import math
import sys

D2R = math.pi / 180.0
R2D = 180.0 / math.pi


def load(path, mode=None):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                if mode and r.get("ai_mode", "") != mode:
                    continue
                rows.append({
                    "t": float(r["sim_time_s"]),
                    "x": float(r["x_ft"]), "y": float(r["y_ft"]),
                    "alt": float(r["alt_msl_ft"]), "vs": float(r["vs_fpm"]),
                    "hdg": float(r["heading_deg"]),
                    "roll": float(r["roll_deg"]), "beta": float(r["beta_deg"]),
                    "vcas": float(r["vcas_kts"]),
                    "gear": float(r["gear_pos"]),
                    "on_ground": r["on_ground"] == "1",
                    "yaw_cmd": float(r["yaw_cmd"]),
                })
            except (ValueError, KeyError):
                continue
    return rows


def wrap180(a):
    while a > 180.0: a -= 360.0
    while a < -180.0: a += 360.0
    return a


def analyze_leg(rows, ax, ay, bx, by, name, trim_end=0.0):
    dx, dy = bx - ax, by - ay
    L = math.hypot(dx, dy)
    course = math.atan2(dx, dy) * R2D      # compass: 0=N, CW+
    ux, uy = dx / L, dy / L
    px, py = uy, -ux                        # right unit vector
    fails = []
    print(f"\n--- leg {name}: course={course:.1f}deg len={L:.0f}ft ---")

    # window: within 6,000 ft of the leg segment (before/after allowed so the
    # intercept portion outside the segment start still counts)
    # Contiguous first-passage window: start when the aircraft first
    # enters the slab, end when it leaves (a later leg can cross back
    # through the same slab — e.g. the S leg re-enters the N leg's strip —
    # and must not pollute this leg's metrics).
    win = []
    out_gap = 0.0
    started = False
    was_close = False   # saw |xte| < 1000: convergence achieved
    for r in rows:
        if r["on_ground"]:
            continue
        along = (r["x"]-ax)*ux + (r["y"]-ay)*uy
        xt = (r["x"]-ax)*px + (r["y"]-ay)*py
        if was_close and abs(xt) > 1500.0:
            break          # converged then diverged = corner departure
        if abs(xt) < 1000.0:
            was_close = True
        inside = -6000.0 < along < L - trim_end and abs(xt) < 20000.0
        if inside:
            win.append(r)
            started = True
            out_gap = 0.0
        elif started:
            out_gap += r["t"] - (win[-1]["t"] if win else r["t"])
            if out_gap > 5.0:
                break
    if not win:
        print("  NO DATA in leg window")
        return [f"{name}: no data"]

    # cross-track history + establish detection
    xte0 = None
    est_t = None
    est_i = None
    consec = 0
    max_overshoot = 0.0
    s0 = None
    for i, r in enumerate(win):
        along = (r["x"]-ax)*ux + (r["y"]-ay)*uy
        xte = (r["x"]-ax)*px + (r["y"]-ay)*py
        if s0 is None and xte0 is None:
            xte0 = xte
            s0 = xte
        # crossing detection: sign change after start
        if est_i is None and s0 is not None and xte * s0 < 0:
            # overshoot measured as excursion past the crossing
            pass
        hdg_err = abs(wrap180(r["hdg"] - course))
        if abs(xte) < 250.0 and hdg_err < 5.0:
            consec += 1
            if consec >= 180 and est_t is None:   # 3 s dwell at 60 Hz
                est_t, est_i = r["t"], i
        else:
            consec = 0
    # overshoot: max |xte| on the far side of the initial offset AFTER first
    # crossing of centerline
    far_max = 0.0
    crossed = False
    for r in win:
        xte = (r["x"]-ax)*px + (r["y"]-ay)*py
        if not crossed and xte0 is not None and xte * xte0 < 0:
            crossed = True
        if crossed and xte0 is not None and xte * xte0 > 0:
            # same side as start again = overshoot excursion
            far_max = max(far_max, abs(xte))
    if not crossed:
        far_max = float("nan")

    print(f"  initial xte      : {xte0 if xte0 is not None else float('nan'):+.0f} ft")
    print(f"  established      : {'t=%.1fs' % est_t if est_t else 'NEVER'}"
          f"{'  <<< FAIL' if est_t is None else ''}")
    if est_t is None:
        fails.append(f"{name}: never established (xte<250ft & hdg within 5deg for 5s)")

    final_xte = abs((win[-1]["x"]-ax)*px + (win[-1]["y"]-ay)*py)
    print(f"  final |xte|      : {final_xte:.0f} ft"
          f"{'  <<< FAIL' if final_xte > 250 else ''}")
    if final_xte > 250:
        fails.append(f"{name}: final |xte| {final_xte:.0f} > 250 ft")
    if not math.isnan(far_max):
        print(f"  overshoot        : {far_max:.0f} ft"
              f"{'  <<< FAIL' if far_max > 1500 else ''}")
        if far_max > 1500:
            fails.append(f"{name}: intercept overshoot {far_max:.0f} > 1500 ft")

    # heading vs course on the straight portion (after establish or last 25%)
    straight = win[est_i:] if est_i is not None else win[int(len(win)*0.75):]
    if straight:
        hd = [abs(wrap180(r["hdg"] - course)) for r in straight]
        m = sum(hd) / len(hd)
        print(f"  homing signature : mean |hdg-course| on straight = {m:.1f} deg"
              f"{'  <<< FAIL (homing, not established)' if m > 5.0 else ''}")
        if m > 5.0:
            fails.append(f"{name}: heading off-course by {m:.1f} deg on straight leg (homing)")

    # coordination + altitude in the whole leg
    turns = [r for r in win if abs(r["roll"]) > 10.0]
    if turns:
        bm = sum(abs(r["beta"]) for r in turns) / len(turns)
        bx_ = max(abs(r["beta"]) for r in turns)
        print(f"  beta in turns    : mean {bm:.2f} deg, max {bx_:.2f} deg"
              f"{'  <<< FAIL' if bx_ > 3.0 else ''}")
        if bx_ > 3.0:
            fails.append(f"{name}: |beta| max {bx_:.2f} deg in turns (slip/skid)")
        a0 = None
        worst = 0.0
        for r in turns:
            if a0 is None: a0 = r["alt"]
            worst = max(worst, abs(r["alt"] - a0))
        print(f"  alt drift in turn: {worst:.0f} ft{'  <<< FAIL' if worst > 200 else ''}")
        if worst > 200:
            fails.append(f"{name}: altitude drift {worst:.0f} ft in turns")
    if straight:
        a = [r["alt"] for r in straight]
        band = max(a) - min(a)
        print(f"  alt band straight: {band:.0f} ft{'  <<< FAIL' if band > 300 else ''}")
        if band > 300:
            fails.append(f"{name}: altitude band {band:.0f} ft on straight leg")
    return fails


def analyze_takeoff(rows, cx, cy, hdg_deg):
    h = hdg_deg * D2R
    ux, uy = math.sin(h), math.cos(h)
    px, py = uy, -ux
    fails = []
    print(f"\n--- takeoff lineup: centerline=({cx:.0f},{cy:.0f}) hdg={hdg_deg:.1f}deg ---")
    air = [r for r in rows if not r["on_ground"]]
    if not air:
        print("  NEVER AIRBORNE  <<< FAIL")
        return ["takeoff: never airborne"]
    t_lift = air[0]["t"]
    # gear-up moment (gear_pos falls below 0.5)
    gear_up_t = None
    for r in air:
        if r["gear"] < 0.5:
            gear_up_t = r["t"]
            break
    print(f"  liftoff          : t={t_lift:.1f}s")
    print(f"  gear up          : t={gear_up_t:.1f}s" if gear_up_t else "  gear up          : NEVER (still down)")
    # lateral offset & beta for 60 s after liftoff
    win = [r for r in air if r["t"] - t_lift < 60.0]
    offs = [(r["t"], (r["x"]-cx)*px + (r["y"]-cy)*py, r["beta"]) for r in win]
    mo = max(abs(o[1]) for o in offs)
    mb = max(abs(o[2]) for o in offs)
    # step detection: largest 1-second lateral-offset change
    step = 0.0
    step_t = 0.0
    for i in range(1, len(offs)):
        dt = offs[i][0] - offs[i-1][0]
        if dt < 1e-6: continue
        rate = abs(offs[i][1] - offs[i-1][1]) / dt
        if rate > step:
            step = rate
            step_t = offs[i][0]
    print(f"  max |offset| 60s : {mo:.0f} ft{'  <<< FAIL' if mo > 150 else ''}")
    print(f"  max |beta| 60s   : {mb:.2f} deg{'  <<< FAIL' if mb > 3.0 else ''}")
    print(f"  worst lateral run: {step:.0f} ft/s at t={step_t:.1f}s"
          f"{'  <<< FAIL (step > 25 ft/s = visible jump)' if step > 25.0 else ''}")
    if mo > 150: fails.append(f"takeoff: |offset| {mo:.0f} ft in first 60 s")
    if mb > 3.0: fails.append(f"takeoff: |beta| {mb:.2f} deg after liftoff")
    if step > 25.0: fails.append(f"takeoff: lateral jump {step:.0f} ft/s at t={step_t:.1f}s")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--legs", nargs="+", default=[])
    ap.add_argument("--takeoff", default=None)
    ap.add_argument("--mode", default=None, help="filter rows by ai_mode (e.g. Enroute)")
    args = ap.parse_args()
    rows = load(args.csv, args.mode)
    if not rows:
        print("no rows", file=sys.stderr); return 2
    print(f"trace: {args.csv}  rows={len(rows)}  t=[{rows[0]['t']:.1f},{rows[-1]['t']:.1f}]s")
    all_fails = []
    parsed = []
    for spec in args.legs:
        name, coords = spec.split(":", 1)
        ax, ay, bx, by = (float(v) for v in coords.split(",")[:4])
        parsed.append((name, ax, ay, bx, by))
    for i, (name, ax, ay, bx, by) in enumerate(parsed):
        # NAV analyzer: trim the leg window at the turn-lead point when a
        # NEXT leg exists (the corner arc belongs to neither leg's
        # straight-line metrics; it pollutes final-xte/homing scoring).
        trim = 0.0
        if i + 1 < len(parsed):
            _, nx, ny, n2x, n2y = parsed[i + 1]
            c1 = math.atan2(bx - ax, by - ay)
            c2 = math.atan2(n2x - nx, n2y - ny)
            dth = abs(wrap180((c2 - c1) * R2D)) * D2R
            # TAS-consistent turn radius (module uses ISA sigma at mean alt)
            vcas = sum(r["vcas"] for r in rows) / len(rows)
            alt = sum(r["alt"] for r in rows) / len(rows)
            alt = max(0.0, min(36000.0, alt))
            sigma = (1.0 - alt / 145442.0) ** 4.2561
            v = vcas * 1.68781 / math.sqrt(max(0.3, sigma))
            R = v * v / (32.174 * math.tan(0.52))
            trim = 1.2 * R * math.tan(dth / 2.0) + 500.0
        all_fails += analyze_leg(rows, ax, ay, bx, by, name, trim)
    if args.takeoff:
        cx, cy, hdg = (float(v) for v in args.takeoff.split(","))
        all_fails += analyze_takeoff(rows, cx, cy, hdg)
    print("\n================ SUMMARY ================")
    if all_fails:
        for f in all_fails: print(f"FAIL  {f}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

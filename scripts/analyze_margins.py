#!/usr/bin/env python3
# scripts/analyze_margins.py
#
# PHUG-PLAN Phase 2 — loop-margin analysis from fm_sysid `margin` CSVs.
#
# Input: one or more margin-mode summary CSVs (rows: run_id,case,inj,gear,
# alt_ft,vt_fps,freq_used,channel,Xc,Xs,R_re,R_im,R_db,R_phase_deg).
#
# For each (config, case) this script:
#   1. Reconstructs the complex closed-loop response R(jw) per channel
#      against the injected reference.
#   2. Forms the direct loop transfer:
#        g/g0 : Rd = R(nzcgs)/R(ptcmd)      -> L0 = Rd/(1-Rd)
#               (using the measured ptcmd as reference cancels the
#                square-law stick shaping exactly)
#        C    : Rd = R(alt_ft)              -> L3 = Rd/(1-Rd)
#        E    : 2x2 matrix from the alt-target and spd-target injections:
#               Rmat = [[R(alt|alt),  R(alt|spd)],
#                       [R(vcas|alt), R(vcas|spd)]]
#               L = Rmat (I - Rmat)^-1  -> L3_diag, L4_diag (other loop
#               closed) + the off-diagonal couplings.
#   3. Reports gain crossover (|L|=1) -> phase margin, and the -180 deg
#      crossing -> gain margin. Gate (plan section 6): PM >= 45, GM >= 6 dB.
#
# Usage:
#   python3 scripts/analyze_margins.py /tmp/sysid/margin_*.csv [--json out.json]

import argparse
import cmath
import csv
import json
import math
import sys
from collections import defaultdict


def load(paths):
    """-> data[(vt_fps, gear, case)][inj][freq] = {channel: complex R}"""
    data = defaultdict(lambda: defaultdict(dict))
    for path in paths:
        with open(path) as f:
            for row in csv.DictReader(f):
                if row["channel"] == "REF":
                    continue
                key = (float(row["vt_fps"]), int(row["gear"]), row["case"])
                freq = float(row["freq_used"])
                r = complex(float(row["R_re"]), float(row["R_im"]))
                data[key][row["inj"]].setdefault(freq, {})[row["channel"]] = r
    return data


def loop_from_R(rd):
    """L = R/(1-R); None where 1-R ~ 0 (ill-conditioned, e.g. f -> 0)."""
    denom = 1.0 - rd
    if abs(denom) < 1e-9:
        return None
    return rd / denom


def crossing(pts, metric, target):
    """pts: [(f, L)]. Find f where metric(L) crosses target
    (log-f interpolation). Returns (f_cross, L_cross) or None."""
    for i in range(len(pts) - 1):
        (f1, L1), (f2, L2) = pts[i], pts[i + 1]
        m1, m2 = metric(L1), metric(L2)
        if m1 == m2:
            continue
        # bracket the target between the two samples
        if (m1 - target) * (m2 - target) <= 0:
            t = (target - m1) / (m2 - m1)
            lf = math.log(f1) + t * (math.log(f2) - math.log(f1))
            Lc = L1 + t * (L2 - L1)  # linear complex interp (adequate)
            return math.exp(lf), Lc
    return None


def margins_from_L(table):
    """table: {freq: L|None}. Returns (wc, PM, w180, GM_dB)."""
    pts = [(f, L) for f, L in sorted(table.items()) if L is not None]
    if len(pts) < 2:
        return None, None, None, None

    wc = pm = w180 = gm = None
    gc = crossing(pts, lambda L: abs(L), 1.0)
    if gc:
        wc, Lc = gc
        pm = 180.0 + math.degrees(cmath.phase(Lc))
    pc = crossing(pts, lambda L: math.degrees(cmath.phase(L)), -180.0)
    if pc:
        w180, Lc = pc
        gm = -20.0 * math.log10(abs(Lc))
    return wc, pm, w180, gm


def analyze(table, name, Rd):
    """table: {f: L}, Rd: {f: R-direct}. Emits one markdown row."""
    Lt = {f: loop_from_R(r) for f, r in table.items()}
    wc, pm, w180, gm = margins_from_L(Lt)
    # closed-loop resonance metrics from the RAW R (robust even when the
    # L-inversion degenerates at |R| >> 1)
    peak_f, peak_R = None, 0.0
    for f, r in sorted(Rd.items()):
        if abs(r) > peak_R:
            peak_R, peak_f = abs(r), f
    zeta = 1.0 / (2.0 * peak_R) if peak_R > 1.2 else None
    def f(x, w=7, p=3):
        return f"{x:{w}.{p}f}" if x is not None else " " * w + "--"
    pm_s = f"{pm:6.1f}" if pm is not None else "    --"
    gm_s = f"{gm:6.1f}" if gm is not None else "    --"
    pk_s = f"{peak_R:6.2f}@{peak_f:5.2f}" if peak_f else "      --"
    z_s = f"{zeta:6.3f}" if zeta is not None else "   --"
    ok = ""
    if pm is not None and gm is not None:
        ok = "PASS" if (pm >= 45.0 and gm >= 6.0) else "**FAIL**"
    elif pm is not None:
        ok = "PASS*" if pm >= 45.0 else "**FAIL**"
    if zeta is not None and zeta < 0.08:
        ok = "**FAIL**(res)"
    print(f"| {name} | {f(wc)} | {pm_s} | {f(w180)} | {gm_s} | {pk_s} | {z_s} | {ok} |")
    return {str(fq): [v.real, v.imag] for fq, v in Lt.items() if v is not None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    data = load(args.csvs)

    out_json = {}
    print("# Loop margin report — PHUG-PLAN Phase 2 (measured)\n")
    print("Gate (plan §6): PM ≥ 45°, GM ≥ 6 dB. L = R/(1−R) from the "
          "injected-reference closed-loop identification; the E-case "
          "diagonals use the 2×2 inversion (other loop closed). "
          "PM/GM undefined where no crossover lies inside the measured "
          "0.02–3.2 rad/s band.\n")
    print(r"| config | loop | ωc (rad/s) | PM (°) | ω180 (rad/s) | GM (dB) | \|R\|peak@f | ζ_est | gate |")
    print("|---|---|---|---|---|---|---|---|---|")

    for key in sorted(data):
        vt, gear, case = key
        cfg = f"{vt * 0.592484:.0f} kts{' gear' if gear else ''}"
        cfg_id = f"{vt:.0f}fps_g{gear}_{case}"
        runs = data[key]
        if case in ("g", "g0"):
            freqs = sorted(runs["pstick"])
            Rd = {}
            for f in freqs:
                ch = runs["pstick"].get(f, {})
                if "nzcgs" in ch and "ptcmd" in ch and abs(ch["ptcmd"]) > 1e-14:
                    Rd[f] = ch["nzcgs"] / ch["ptcmd"]
            if len(Rd) < 2:
                print(f"| {cfg} | {case} | incomplete | | | | |")
                continue
            out_json[cfg_id] = analyze({f: loop_from_R(r) for f, r in Rd.items()},
                                      f"{cfg} | L0 (case {case})", Rd)
        elif case == "C":
            freqs = sorted(runs["alt_target"])
            Rd = {f: runs["alt_target"][f]["alt_ft"] for f in freqs}
            out_json[cfg_id] = analyze({f: loop_from_R(r) for f, r in Rd.items()},
                                      f"{cfg} | L3 isolated (thr frozen)", Rd)
        elif case == "E":
            Ld = {}
            for f in sorted(runs["alt_target"]):
                if f not in runs["spd_target"]:
                    continue
                a, s = runs["alt_target"][f], runs["spd_target"][f]
                r11, r12 = a["alt_ft"], s["alt_ft"]
                r21, r22 = a["vcas_kts"], s["vcas_kts"]
                det = (1 - r11) * (1 - r22) - r12 * r21
                if abs(det) < 1e-12:
                    continue
                b11, b12 = (1 - r22), -r12
                b21, b22 = -r21, (1 - r11)
                l11 = (r11 * b11 + r12 * b21) / det   # alt loop, spd closed
                l22 = (r21 * b12 + r22 * b22) / det   # spd loop, alt closed
                Ld[f] = (l11, l22)
            if len(Ld) < 2:
                print(f"| {cfg} | E | incomplete | | | | |")
                continue
            L3m = {f: v[0] for f, v in Ld.items()}
            L4m = {f: v[1] for f, v in Ld.items()}
            Ra = {f: runs["alt_target"][f]["alt_ft"] for f in runs["alt_target"]}
            Rs = {f: runs["spd_target"][f]["vcas_kts"] for f in runs["spd_target"]}
            j3 = analyze({f: v[0] for f, v in Ld.items()},
                         f"{cfg} | L3 diag (2×2, L4 closed)", Ra)
            j4 = analyze({f: v[1] for f, v in Ld.items()},
                         f"{cfg} | L4 diag (2×2, L3 closed)", Rs)
            out_json[cfg_id] = {"L3": j3, "L4": j4}
    print()
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out_json, f, indent=1)
        print(f"wrote {args.json}")


if __name__ == "__main__":
    main()

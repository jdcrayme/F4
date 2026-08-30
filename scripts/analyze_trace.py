#!/usr/bin/env python3
"""Analyze FCS CSV traces against FLIGHT_CONTROL_NEXT_STEPS.md §5 criteria,
plus segment-level diagnostics for the approach/landing phases."""
import csv, sys, math
from collections import defaultdict

def load(path):
    with open(path, newline='') as f:
        return list(csv.DictReader(f))

def f(row, k, default=0.0):
    try: return float(row[k])
    except (ValueError, KeyError, TypeError): return default

def analyze(path):
    rows = load(path)
    print(f"\n=== {path.split('/')[-1]} — {len(rows)} ticks, t=[{f(rows[0],'sim_time_s'):.0f}..{f(rows[-1],'sim_time_s'):.0f}]s ===")
    # Phase occupancy
    phases = defaultdict(int)
    for r in rows: phases[f"{r['ai_mode']}/{r['ai_state']}"] += 1
    print("-- state occupancy (s) --")
    for k, v in sorted(phases.items(), key=lambda kv: -kv[1]):
        print(f"   {k:<28} {v/60:8.1f}")

    # Go-around detection: state leaving OnFinal->GoAround etc. shown by occupancy already
    def window(state_name, t0=None, t1=None):
        return [r for r in rows if state_name in r['ai_state'] and
                (t0 is None or f(r,'sim_time_s') >= t0) and (t1 is None or f(r,'sim_time_s') <= t1)]

    fin = window('OnFinal')
    if fin:
        print("-- OnFinal tracking (§5 metrics) --")
        # beam deviation: we don't have beam alt in CSV; use target_alt as proxy for commanded alt
        # report alt error vs commanded target
        errs = [f(r,'alt_msl_ft') - f(r,'target_alt_ft') for r in fin]
        print(f"   alt vs commanded: min={min(errs):7.0f} max={max(errs):7.0f} final={errs[-1]:7.0f} ft")
        vs_err = [f(r,'vs_fpm') for r in fin]
        print(f"   vs_fpm: min={min(vs_err):6.0f} max={max(vs_err):6.0f} (target: |vs|<=300 of beam cmd)")
        lat = [abs(f(r,'course_lateral_ft')) for r in fin]
        print(f"   |course_lateral|: max={max(lat):6.0f} final={lat[-1]:6.0f} ft (§5: <50 at threshold)")
        spd_err = [f(r,'vcas_kts') - f(r,'target_speed_kts') for r in fin]
        print(f"   speed err: min={min(spd_err):6.0f} max={max(spd_err):6.0f} kts")
        roll = [abs(f(r,'roll_deg')) for r in fin]
        print(f"   |roll|: max={max(roll):5.1f} mean={sum(roll)/len(roll):5.1f} deg (§5: ±3)")
        print(f"   throttle: min={min(f(r,'throttle_cmd') for r in fin):.2f} max={max(f(r,'throttle_cmd') for r in fin):.2f}")
        print(f"   tef_cmd set: {any(f(r,'tef_cmd')>0 for r in fin)}  lef_cmd set: {any(f(r,'lef_cmd')>0 for r in fin)}")
        # sample every ~10 s
        print("   t      alt_agl  target_alt  alt_msl  vs_fpm  vcas  thr   along     lat    pitch alpha  nz")
        for r in fin[::600]:
            print(f"   {f(r,'sim_time_s'):6.0f} {f(r,'alt_agl_ft'):8.0f} {f(r,'target_alt_ft'):9.0f} {f(r,'alt_msl_ft'):8.0f} {f(r,'vs_fpm'):7.0f} {f(r,'vcas_kts'):5.0f} {f(r,'throttle_cmd'):5.2f} {f(r,'course_along_ft'):8.0f} {f(r,'course_lateral_ft'):6.0f} {f(r,'pitch_deg'):5.1f} {f(r,'alpha_deg'):5.1f} {f(r,'nz'):5.2f}")

    itc = window('InterceptFinal')
    if itc:
        print("-- InterceptFinal --")
        print("   t      alt_agl  target_alt  alt_msl  vs_fpm  vcas  thr   along     lat   pitch  nz")
        for r in itc[::600]:
            print(f"   {f(r,'sim_time_s'):6.0f} {f(r,'alt_agl_ft'):8.0f} {f(r,'target_alt_ft'):9.0f} {f(r,'alt_msl_ft'):8.0f} {f(r,'vs_fpm'):7.0f} {f(r,'vcas_kts'):5.0f} {f(r,'throttle_cmd'):5.2f} {f(r,'course_along_ft'):8.0f} {f(r,'course_lateral_ft'):6.0f} {f(r,'pitch_deg'):5.1f} {f(r,'nz'):5.2f}")

    fl = window('Flare')
    if fl:
        print("-- Flare --")
        print("   t      alt_agl  vs_fpm  vcas  thr  pitch  alpha   nz  along")
        for r in fl[::20]:
            print(f"   {f(r,'sim_time_s'):8.1f} {f(r,'alt_agl_ft'):7.0f} {f(r,'vs_fpm'):6.0f} {f(r,'vcas_kts'):5.0f} {f(r,'throttle_cmd'):4.2f} {f(r,'pitch_deg'):6.1f} {f(r,'alpha_deg'):5.1f} {f(r,'nz'):5.2f} {f(r,'course_along_ft'):7.0f}")

    ro = window('Rollout')
    if ro:
        r0 = ro[0]
        print(f"-- Rollout entry (touchdown) --")
        print(f"   t={f(r0,'sim_time_s'):.1f}s along={f(r0,'course_along_ft'):.0f} ft cross={f(r0,'course_lateral_ft'):.0f} ft "
              f"vs={f(r0,'vs_fpm'):.0f} fpm vcas={f(r0,'vcas_kts'):.0f} kts pitch={f(r0,'pitch_deg'):.1f}")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)

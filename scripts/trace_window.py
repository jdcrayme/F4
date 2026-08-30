#!/usr/bin/env python3
"""Dump a tick window of the FCS CSV trace with the columns that matter
for phugoid/stall/loop diagnosis."""
import csv
import sys

path = sys.argv[1]
t0 = float(sys.argv[2])
t1 = float(sys.argv[3])
every = int(sys.argv[4]) if len(sys.argv) > 4 else 30

cols = ["sim_time_s", "ai_state", "pitch_cmd", "throttle_cmd", "tef_cmd",
        "ptcmd", "nzcgs", "alpha_deg", "nz", "vcas_kts", "alt_agl_ft",
        "vs_fpm", "heading_deg", "pitch_deg", "roll_deg",
        "target_alt_ft", "target_speed_kts", "target_heading_deg",
        "course_lateral_ft", "course_along_ft", "on_ground"]

with open(path) as f:
    r = csv.DictReader(f)
    print("  t      state            pcmd  thr  tef  ptcmd  nzcgs  alpha   nz  vcas   agl     vs    hdg  pitch  roll  talt tspd  thdg   lat    alng  gnd")
    for row in r:
        t = float(row["sim_time_s"])
        if t < t0 or t > t1:
            continue
        if int(row["tick"]) % every:
            continue
        print(" ".join([
            f"{t:7.1f}",
            f"{row['ai_state'][:16]:16s}",
            f"{float(row['pitch_cmd']):5.2f}",
            f"{float(row['throttle_cmd']):4.2f}",
            f"{float(row['tef_cmd']):4.2f}",
            f"{float(row['ptcmd']):6.2f}",
            f"{float(row['nzcgs']):6.2f}",
            f"{float(row['alpha_deg']):6.1f}",
            f"{float(row['nz']):5.2f}",
            f"{float(row['vcas_kts']):6.1f}",
            f"{float(row['alt_agl_ft']):7.1f}",
            f"{float(row['vs_fpm']):6.0f}",
            f"{float(row['heading_deg']):6.1f}",
            f"{float(row['pitch_deg']):5.1f}",
            f"{float(row['roll_deg']):5.1f}",
            f"{float(row['target_alt_ft']):6.0f}",
            f"{float(row['target_speed_kts']):5.0f}",
            f"{float(row['target_heading_deg']):6.1f}",
            f"{float(row['course_lateral_ft']):7.0f}",
            f"{float(row['course_along_ft']):7.0f}",
            row["on_ground"],
        ]))

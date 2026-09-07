#!/usr/bin/env python3
# scripts/trace_metrics.py
#
# PHUG-PLAN Phase 3/5 — oscillation metrics from time-series CSVs.
#
# For a given signal column (default: alt_msl_ft) over a warmed-up window:
#   - dominant period (DFT peak over 0.005-0.5 Hz)
#   - damping ratio zeta (log-decrement over successive positive peaks)
#   - amplitude envelope (half range)
#   - settling (time after which the envelope stays below 50% of its early
#     value; None if still oscillating)
#
# Usage:
#   python3 scripts/trace_metrics.py CSV [CSV...] --column alt_msl_ft \
#       [--t0 20] [--group-by param,factor]
#
# For fm_sysid `rootlocus` CSVs (param,factor prefix columns) the script
# automatically reports one metrics line per (param, factor) run.

import argparse
import csv
import math
import sys
from collections import defaultdict


def read_series(path, column, t0):
    t, x = [], []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                ti = float(row["t"])
                xi = float(row[column])
            except (KeyError, ValueError, TypeError):
                continue
            if ti >= t0:
                t.append(ti)
                x.append(xi)
    return t, x


def metrics(t, x, fs=None):
    n = len(x)
    if n < 16:
        return None
    if fs is None:
        fs = (n - 1) / (t[-1] - t[0])
        if fs <= 0:
            return None
    mean = sum(x) / n
    xs = [v - mean for v in x]
    # first-order trend removal (means, not sums)
    half = n // 2
    m_early = sum(xs[:half]) / max(1, half)
    m_late = sum(xs[half:]) / max(1, n - half)
    dt_mid = (t[half] - t[0])
    slope = (m_late - m_early) / max(1e-9, (t[-1] - t[half]))
    xs = [xs[i] - m_early - slope * (t[i] - t[0]) for i in range(n)]

    amp_est = (max(xs) - min(xs)) / 2.0
    if amp_est <= 0:
        return None

    # dominant frequency: Goertzel-style DFT over the band (Hann window)
    f_lo, f_hi, steps = 0.005, 0.5, 400
    best_f, best_p = None, -1.0
    for i in range(steps + 1):
        f = f_lo + (f_hi - f_lo) * i / steps
        w = 2.0 * math.pi * f / fs
        c = s = 0.0
        for j, v in enumerate(xs):
            hann = 0.5 - 0.5 * math.cos(2.0 * math.pi * j / (n - 1))
            ang = w * j
            c += v * hann * math.cos(ang)
            s += v * hann * math.sin(ang)
        p = c * c + s * s
        if p > best_p:
            best_p, best_f = p, f

    # positive-peak picking (threshold relative to the envelope)
    thr = 0.2 * amp_est
    peaks = [j for j in range(1, n - 1)
             if xs[j] >= xs[j - 1] and xs[j] > xs[j + 1] and xs[j] > thr]
    period_spacing = None
    if len(peaks) >= 3:
        gaps = [t[peaks[i + 1]] - t[peaks[i]] for i in range(len(peaks) - 1)]
        gaps.sort()
        period_spacing = gaps[len(gaps) // 2]  # median gap

    # log-decrement damping over successive positive peaks
    zeta = None
    if len(peaks) >= 4:
        vals = [xs[j] for j in peaks]
        ratios = [math.log(vals[i] / vals[i + 1])
                  for i in range(len(vals) - 1)
                  if vals[i] > 0 and vals[i + 1] > 0]
        if ratios:
            ratios.sort()
            d = ratios[len(ratios) // 2]
            zeta = d / math.sqrt(4.0 * math.pi * math.pi + d * d)

    return {
        "period_dft_s": 1.0 / best_f if best_f else None,
        "period_s": period_spacing,
        "freq_hz": best_f,
        "zeta": zeta,
        "amplitude": amp_est,
        "n_peaks": len(peaks),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+")
    ap.add_argument("--column", default="alt_msl_ft")
    ap.add_argument("--t0", type=float, default=20.0)
    ap.add_argument("--group-by", default=None,
                    help="comma-separated columns to group runs (e.g. param,factor)")
    args = ap.parse_args()

    groups = args.group_by.split(",") if args.group_by else None
    buckets = defaultdict(lambda: ([], []))
    meta = {}
    for path in args.csvs:
        if groups:
            # per-(group-value) series inside one file (rootlocus layout)
            rows = []
            with open(path) as f:
                for row in csv.DictReader(f):
                    rows.append(row)
            key_ids = defaultdict(int)
            for row in rows:
                key = tuple(row[g] for g in groups)
                try:
                    ti = float(row["t"])
                    xi = float(row[args.column])
                except (KeyError, ValueError, TypeError):
                    continue
                buckets[(path, key)][0].append(ti)
                buckets[(path, key)][1].append(xi)
        else:
            t, x = read_series(path, args.column, args.t0)
            buckets[(path, None)] = (t, x)

    for (path, key), (t, x) in sorted(buckets.items(), key=lambda kv: str(kv[0])):
        m = metrics(t, x)
        label = f"{path}" + (f"[{','.join(key)}]" if key else "")
        if m is None:
            print(f"{label}: insufficient data ({len(t)} samples)")
            continue
        z = f"{m['zeta']:.3f}" if m["zeta"] is not None else "  -- "
        tp = f"{m['period_s']:.2f}" if m["period_s"] is not None else "--"
        print(f"{label}: T_peak={tp:>8s}s  T_dft={m['period_dft_s'] or 0:7.2f}s  "
              f"zeta={z}  amp={m['amplitude']:.2f} peaks={m['n_peaks']}")


if __name__ == "__main__":
    main()

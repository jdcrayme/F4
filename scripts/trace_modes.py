#!/usr/bin/env python3
"""trace_modes.py — Phase D mode discrimination from flight-test CSV traces.

Companion to FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md §5. Given a CSV trace
(f4-recorder or diag_poles --verify format), this tool:

  1. Welch-estimates the power spectral density of each signal column,
  2. finds the dominant oscillation frequency / period and its prominence,
  3. estimates the pairwise cross-spectrum PHASE at that frequency
     (the mode's fingerprint: what leads what, by how much),
  4. for aperiodic (drift-dominated) segments, estimates the exponential
     doubling / halving time from the log-envelope slope,
  5. prints a discrimination verdict against the plan's signature table.

Usage:
  python3 scripts/trace_modes.py trace.csv [--t0 0] [--t1 180]
         [--fs 60] [--cols vs_fpm,vcas_kts,alpha_deg,nz] [--json out.json]

CSV requirements: a "t" column (seconds); the remaining columns are treated
as signals. Typical recorder/verify columns:
  vs_fpm, vcas_kts, alt_ft, nz, alpha_deg, pstick, throttle

Discrimination table (plan §5):
  | Candidate mode                          | Signature                          |
  |-----------------------------------------|------------------------------------|
  | Bare-plant speed mode (phugoid-like)    | T ~ 0.138*V_tas; alpha/nz ~ const; |
  |                                         | V and h anti-phase                 |
  | FCS-integrator/leak mode (QIL "20 s")   | T = 10-30 s; alpha & nz in phase;  |
  |                                         | eintg winding                      |
  | AI altitude-hold mode                   | T = 5-15 s; pstick leads vs ~90deg |
  | Back-side divergence (aperiodic)        | no clean period; V monotonic drift |
  |                                         | until a clamp catches (sawtooth)   |
  | Stall-latch relay cycle                 | irregular; alpha ~ criticalAOA;    |
  |                                         | nz square-ish steps                |
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys

import numpy as np
from scipy.signal import csd, detrend, hilbert, welch

# ---------------------------------------------------------------------------
# CSV loading
# ---------------------------------------------------------------------------


def load_csv(path: str, t0: float, t1: float) -> tuple[np.ndarray, dict[str, np.ndarray], float]:
    """Load the CSV, clip to [t0, t1], return (t, {col: vals}, mean sample dt)."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = [r for r in reader if r]
    cols = [h.strip() for h in header]
    if "t" not in cols:
        sys.exit("FATAL: CSV has no 't' column")
    data = np.array([[float(x) for x in r[: len(cols)]] for r in rows if len(r) >= len(cols)])
    t = data[:, cols.index("t")]
    keep = (t >= t0) & (t <= t1)
    t = t[keep]
    signals = {}
    for i, c in enumerate(cols):
        if c == "t":
            continue
        signals[c] = data[keep, i]
    if len(t) < 32:
        sys.exit(f"FATAL: only {len(t)} samples in window")
    dts = np.diff(t)
    fs = 1.0 / np.median(dts)
    if np.max(dts) - np.min(dts) > 1e-3 * np.median(dts) * 10:
        print(f"WARN: non-uniform sampling (dt {np.min(dts):.4g}..{np.max(dts):.4g} s); "
              f"using median fs={fs:.4g} Hz", file=sys.stderr)
    return t, signals, fs


# ---------------------------------------------------------------------------
# Spectral estimates
# ---------------------------------------------------------------------------


def dominant_peak(x: np.ndarray, fs: float) -> dict:
    """Welch PSD of the detrended signal; dominant peak + prominence + drift."""
    n = len(x)
    x_dt = detrend(x)
    trend = x - x_dt
    drift = trend[-1] - trend[0]
    drift_rate = drift / (n / fs) if n > 1 else 0.0
    # Drift significance: how much of the variance is the linear trend?
    var_total = np.var(x) or 1e-30
    var_trend = np.var(trend)
    drift_frac = var_trend / var_total

    nper = min(n, max(256, n // 4))
    f, pxx = welch(x_dt, fs=fs, nperseg=nper, detrend="constant")
    var_resid = np.trapezoid(pxx, f) or 1e-30
    if len(f) < 3:
        return dict(period_s=None, freq_hz=None, prominence=0.0,
                    drift_rate=drift_rate, drift_frac=drift_frac,
                    resid_var=var_resid, verdict="too_short")
    imax = int(np.argmax(pxx[1:]) + 1)  # skip DC bin
    f_pk, p_pk = f[imax], pxx[imax]
    # Prominence: peak level vs the median floor of the spectrum
    floor = np.median(pxx[1:])
    prominence = 10.0 * math.log10(p_pk / floor) if floor > 0 else 0.0
    period = 1.0 / f_pk if f_pk > 0 else None
    # A peak is "clean" if it stands out of the floor; otherwise the segment
    # is aperiodic (drift / broadband).
    clean = prominence >= 6.0
    return dict(period_s=period if clean else None,
                freq_hz=f_pk if clean else None,
                prominence_db=prominence,
                drift_rate_per_s=drift_rate,
                drift_frac=drift_frac,
                verdict="oscillatory" if clean else "aperiodic")


def cross_phase(x: np.ndarray, y: np.ndarray, fs: float, f0: float) -> float:
    """Phase of the cross-spectrum y/x at frequency f0, in degrees."""
    nper = min(len(x), max(256, len(x) // 4))
    f, pxy = csd(detrend(y), detrend(x), fs=fs, nperseg=nper)
    i = int(np.argmin(np.abs(f - f0)))
    # Weight by a small window of bins around f0 for robustness
    lo, hi = max(0, i - 1), min(len(f), i + 2)
    seg = pxy[lo:hi]
    w = np.abs(seg)
    if w.sum() <= 0:
        return float("nan")
    ph = np.angle(np.sum(seg * (w / w.sum())))
    return float(math.degrees(ph))


def envelope_t2x(x: np.ndarray, fs: float) -> float | None:
    """Doubling(+)/halving(-) time from the log of the Hilbert envelope of
    the detrended signal, fitted by least squares. Positive = growing."""
    x_dt = detrend(x)
    env = np.abs(hilbert(x_dt))
    # Smooth the envelope with a moving average (~2 periods worth is unknown;
    # use ~2 s)
    w = max(1, int(2.0 * fs))
    ker = np.ones(w) / w
    env_s = np.convolve(env, ker, mode="same")
    t = np.arange(len(x)) / fs
    # Trim convolution edge artifacts
    m = (t > t[len(t) // 10]) & (t < t[-1] - t[-1] / 10)
    if m.sum() < 16:
        return None
    e = env_s[m]
    if np.any(e <= 0):
        return None
    slope, _ = np.polyfit(t[m], np.log(e), 1)
    if abs(slope) < 1e-5:
        return 0.0
    return float(math.log(2.0) / slope) if slope > 0 else float(-math.log(2.0) / slope) * -1.0


# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------


def classify(per: dict, phases: dict, t2x: float | None) -> str:
    """Apply the plan §5 discrimination table to one segment."""
    if per["verdict"] == "too_short":
        return "too_short"
    if per["verdict"] == "aperiodic":
        return ("aperiodic divergence (speed-mode / back-side signature): "
                "no clean spectral period; monotonic drift "
                f"{per['drift_rate_per_s']:+.3g}/s")
    T = per["period_s"]
    ph_vs_alpha = phases.get("vs_fpm~alpha_deg", phases.get("vcas_kts~alpha_deg"))
    ph_vs_nz = phases.get("vs_fpm~nz", phases.get("vcas_kts~nz"))
    ph_pstick_vs = phases.get("pstick~vs_fpm")
    notes = []
    if ph_vs_alpha is not None and abs(ph_vs_alpha) < 30.0:
        notes.append("alpha/nz ~ in phase with speed")
    if ph_pstick_vs is not None and 60.0 <= abs(ph_pstick_vs) <= 120.0:
        notes.append("pstick ~90deg vs vs (AI altitude-hold signature)")
    if T and 10.0 <= T <= 30.0 and ph_vs_alpha is not None and abs(ph_vs_alpha) < 45.0:
        return f"FCS-integrator/leak mode candidate (T={T:.1f}s, alpha~nz in phase)"
    if T and 5.0 <= T <= 15.0 and ph_pstick_vs is not None and 60.0 <= abs(ph_pstick_vs) <= 120.0:
        return f"AI altitude-hold mode candidate (T={T:.1f}s, pstick leads vs ~90deg)"
    if T and T > 40.0 and ph_vs_nz is not None and abs(ph_vs_nz) < 30.0:
        return (f"bare-plant speed mode (phugoid-like) candidate (T={T:.1f}s, "
                "alpha/nz ~ const relative to V/h exchange)")
    if notes:
        return f"T={T:.1f}s; " + "; ".join(notes)
    return f"T={T:.1f}s (unclassified — compare against eigenvalue predictions)"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description="Phase D trace mode discrimination")
    ap.add_argument("csv", help="trace CSV (needs 't' column)")
    ap.add_argument("--t0", type=float, default=None, help="window start [s]")
    ap.add_argument("--t1", type=float, default=None, help="window end [s]")
    ap.add_argument("--fs", type=float, default=None, help="sample rate override [Hz]")
    ap.add_argument("--cols", type=str, default=None,
                    help="comma list of signal columns (default: all numeric)")
    ap.add_argument("--ref", type=str, default=None,
                    help="reference column for cross-phase (default: vs_fpm or vcas_kts)")
    ap.add_argument("--json", type=str, default=None, help="JSON output path")
    args = ap.parse_args()

    t, signals, fs = load_csv(args.csv, args.t0 if args.t0 is not None else -1e30,
                              args.t1 if args.t1 is not None else 1e30)
    if args.fs:
        fs = args.fs
    if args.cols:
        want = [c.strip() for c in args.cols.split(",")]
        signals = {c: signals[c] for c in want if c in signals}
    # Drop constant columns
    signals = {c: v for c, v in signals.items() if float(np.ptp(v)) > 1e-9}
    if not signals:
        sys.exit("FATAL: no non-constant signal columns in window")

    ref = args.ref
    if ref is None:
        ref = "vs_fpm" if "vs_fpm" in signals else (
            "vcas_kts" if "vcas_kts" in signals else sorted(signals)[0])
    if ref not in signals:
        sys.exit(f"FATAL: reference column {ref} not present")

    print(f"== trace_modes: {args.csv}")
    print(f"   window {t[0]:.2f}..{t[-1]:.2f} s, {len(t)} samples, fs={fs:.4g} Hz, "
          f"ref='{ref}'")
    print()

    results: dict = {"file": args.csv, "t0": float(t[0]), "t1": float(t[-1]),
                     "fs": fs, "ref": ref, "signals": {}, "phases": {},
                     "verdict": None}

    per_cols = {}
    for name, x in sorted(signals.items()):
        per_cols[name] = dominant_peak(x, fs)
        r = per_cols[name]
        t2x = envelope_t2x(x, fs) if r["verdict"] == "aperiodic" else None
        results["signals"][name] = {**{k: (None if (isinstance(v, float) and math.isnan(v))
                                                or v is None else
                                           (float(v) if isinstance(v, (int, float, np.floating))
                                            else v))
                                          for k, v in r.items()},
                                    "t2x_s": t2x}
        if r["verdict"] == "oscillatory":
            print(f"  {name:<12} T={r['period_s']:7.1f} s  (f={r['freq_hz']:.4g} Hz, "
                  f"prominence {r['prominence_db']:.1f} dB)")
        else:
            t2s = f", t2x={t2x:+.1f} s" if t2x and abs(t2x) > 0.1 else ""
            print(f"  {name:<12} APERIODIC  drift={r['drift_rate_per_s']:+.3g}/s "
                  f"(trend var {100 * r['drift_frac']:.0f}% of total{t2s})")
    print()

    # Cross-phase of every signal against the reference at the reference's
    # dominant frequency (or the shared dominant band).
    f0 = per_cols[ref]["freq_hz"]
    phases = {}
    if f0:
        print(f"  cross-phase at f={f0:.4g} Hz (T={1 / f0:.1f} s), relative to '{ref}':")
        for name, x in sorted(signals.items()):
            if name == ref:
                continue
            ph = cross_phase(signals[ref], x, fs, f0)
            phases[f"{ref}~{name}"] = ph
            results["phases"][f"{ref}~{name}"] = ph
            print(f"    {name:<12} phase {ph:+7.1f} deg")
        print()

    # Segment verdict: use the reference signal's classification
    t2x_ref = (envelope_t2x(signals[ref], fs)
               if per_cols[ref]["verdict"] == "aperiodic" else None)
    verdict = classify(per_cols[ref], phases, t2x_ref)
    results["verdict"] = verdict
    print(f"  VERDICT: {verdict}")
    if t2x_ref:
        print(f"  reference '{ref}' envelope doubling time: {t2x_ref:+.1f} s "
              f"(negative = halving; compare to pole prediction t2x = ln2/Re)")
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)
        print(f"\n  JSON written to {args.json}")


if __name__ == "__main__":
    main()

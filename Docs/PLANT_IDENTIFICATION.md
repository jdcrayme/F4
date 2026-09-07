# Plant Identification — PHUG-PLAN Phase 1 Results

> **Status**: Complete. First measured deliverable of
> `Docs/LONGITUDINAL_STABILITY_PLAN.md` (Phase 1 + the Phase 3 bisection
> matrix, executed early because the instrument was ready).
> **Tool**: `tools/fm_sysid/` (built on the `diag_trajectory` pattern).
> **Config**: F-16 (`f16.json` fixture), clean (gear up, no flaps),
> 10,000 ft, flat ground, 60 Hz major frames, FM internal 6× substep.
> All raw CSVs reproducible via the commands in §6.

---

## 1. Headline Findings

| # | Finding | Evidence |
|---|---|---|
| **F1** | **The FCS-only loop is stable at every cruise trim point.** Hands-off flight (pstick=0, throttle frozen) is aperiodic — monotone drift to a new equilibrium, zero oscillation — at 250, 300, and 450 kts. | §3.1 |
| **F2** | **The persistent "phugoid" is owned by the AirSteering altitude cascade (loop L3).** It appears the moment the altitude loop closes and is a **non-decaying ~16.7 s limit cycle**. | §3.2 |
| **F3** | **The cascade's damping term is saturated — it relays instead of damping.** `gamma_corr` rails at its ±0.15 limit every half-cycle throughout the limit cycle; `vs_target` swings to ±2500 fpm (slew-limiter state saturating). | §3.2 |
| **F4** | **The speed channel (L4) does not create the mode but doubles its amplitude** (413 ft vs 231 ft range) via throttle slamming 0.08–1.00 (TECS energy term + anti-balloon guard). | §3.2 |
| **F5** | **Engine thrust has a two-timeconstant response**: an instantaneous jump to the throttle step (one frame) followed by a very slow creep (~30 s to settle). The speed loop therefore contains a large, unmodeled lag — a pumping hazard. | §3.3 |
| **F6** | **Trim solver leaves a residual flight-path angle**: after `trim()`, θ retains its pre-trim value so γ = θ − α ≈ 3.1° at 250 kts — every run/mission starts with an unintended climb transient. | §3.4 |

**Bottom line**: the mode that thirty heuristic fixes chased is a **cascaded-loop
limit cycle in the AI altitude channel**, exactly the failure class predicted
by plan §P2.4/P2.5 (outer-loop bandwidth comparable to inner-loop lag +
saturated damper + slew-limiter phase). The FCS core, q-damper, and
alpha-bias coupling are **exonerated** for cruise: config A does not
oscillate. Phase 4 therefore redesigns/retunes the **L3/L4 outer loops**
(TECS per plan §P4.2), not the FCS.

---

## 2. Static Maps (loop gains)

From `alpha-sweep` / `speed-sweep` at 10,000 ft, clean:

| vt (fps) | vcas (kt) | Mach | qsom (ft/s²) | K_nz = clalph0·qsom/g (per deg) | trim drag (ft/s²) |
|---|---|---|---|---|---|
| 300 | 178 | 0.28 | 30.0 | 0.0729 | 7.92 |
| 425 | 251 | 0.39 | 60.2 | 0.1472 | 7.63 |

- `qsom ∝ V²` confirmed: K_nz doubles from 178→251 kts. **The L0 loop gain
  quadruples per doubling of airspeed** — any fixed-gain outer loop sees a
  plant gain that varies 4× across the subsonic band (speed scheduling of
  the outer loops is not optional).
- The FCS's own K_nz assumption (`clalph0·qsom/g`, fcs.cpp:228) matches the
  measured static slope — **the computeGains plant assumption is consistent
  with the static plant** (its remaining dynamics audit moves to Phase 2,
  but the static gain is right).
- Trim (`FlightModel::trim()`) **fails below ≈ vt 250 fps / 178 kts** at
  10,000 ft — the 1-G stall boundary. The sweep treats that as out-of-envelope
  (skip, not abort).

---

## 3. Time-Domain Measurements

### 3.1 Config A — FCS only (trim-hold, hands-off, 120 s)

| Trim point | alt range (t>30 s) | VS crossings | Character |
|---|---|---|---|
| 250 kts | 856 ft | 1 | monotone drift — **no oscillation** |
| 300 kts | 1,069 ft | 1 | monotone drift — **no oscillation** |
| 450 kts | 3,963 ft | 1 | monotone drift — **no oscillation** |

nzcgs held at 0.985–0.993 throughout; alpha follows the bias formula down as
V builds (6.9°→4.6° at 250 kts) — the 1-G hold and its `1/qsom` feedforward
behave as designed. The altitude drift is the **aperiodic** response to the
frozen-throttle energy offset (F6's residual γ plus trim-throttle error),
not a mode. Over-damped.

**Conclusion: L0 (G-error PI), L1 (q-damper), L2 (bias/1-V² coupling) do not
produce the persistent oscillation.** (Their damping-adequacy at the L3
crossover still matters for Phase 2 margins, but they own no mode here.)

### 3.2 Configs C/D/E — AirSteering loops (ai-hold, 120 s)

| Config | Loops | alt range (t>20 s) | dominant period | verdict |
|---|---|---|---|---|
| C | altitude only (throttle frozen 0.214) | 231 ft | **~16.7 s** | **non-decaying limit cycle** |
| D | speed only (pstick=0) | 2,963 ft | — | not meaningful in isolation: no pitch authority; aircraft climbs at the frozen trim throttle while the speed PI chases (throttle floors at 0.25). Expected for this architecture — speed control requires the pitch loop. |
| E | full stack | 413 ft | **~16.7 s** | same mode as C, ~1.8× amplitude; throttle slams 0.08↔1.00 |

Config C trace signature (the smoking gun):
- `gamma_corr` (VS-error damping, `path_gain × (vs_target − vs)`, clamp
  ±0.15 rad) is **railing at ±0.15 every half-cycle** for the entire run —
  the damper is saturated and behaves as a relay.
- `vs_target` sweeps −1,700 → +330 fpm against a steady-altitude target:
  the cascade demands ±1,700 fpm corrections for ~200 ft of altitude error
  (`vs_gain = 6.0 fpm/ft` + the alt integral), which the airframe cannot
  follow through the ~1.4 s FCS lag (§3.3) + 400 fpm/s slew limiter.
- VS amplitude does not decay between cycles 3 and 7 — steady-state limit
  cycle, not a transient.

Config E adds the L4 coupling: the TECS energy term (V2) and the anti-balloon
guard fire on the L3-induced VS excursions, slamming the throttle rail-to-rail
— which feeds the next cycle through the §3.3 spool lag. **Same owner (L3),
worse amplitude (L4 interaction)** — consistent with plan §P2.5's MIMO beat
prediction.

### 3.3 Plant dynamic elements

- **G response (stick-step +0.3 at 250 kts)**: nzcgs 0.99 → 1.06 G commanded
  path, t₆₃ ≈ **1.4 s**, overshoot ≈ **53%** — the L0 closed loop is
  moderately underdamped at this amplitude; its effective lag (~1.4 s) sets
  the ceiling for any outer-loop bandwidth.
- **Throttle response (+0.2)**: thrust jumps **within one frame** (5.54 →
  8.94 ft/s²) then creeps to 10.20 ft/s² over **~30 s**. A single rpmLag
  cannot produce a step-plus-30 s-creep; there is likely a direct
  throttle→thrust feed term plus the spool lag. **Phase 2 item**: read
  `engine.cpp` and characterize both paths; the 30 s lag inside the L4
  speed loop is a classic pumping ingredient (worklog ALT-4/E44 context).

### 3.4 Trim-solver gap (F6)

`FlightModel::trim()` iterates alpha to nzcgs = 1 but does not re-set
`theta = alpha` after convergence, leaving γ = θ − α ≈ 3.1° at the 250-kt
trim. Every scenario run therefore starts with an unintended ~1,400 fpm
climb transient (visible at t=0 in every CSV). One-line fix candidate —
**deferred to Phase 4** under the patch moratorium (it changes all baselines;
it is recorded so the warm-up window in analysis covers it).

---

## 4. Consequences for the Plan

1. **Phase 2 narrows**: compute margins for **L3 (altitude cascade) and L4
   (speed channel) only**; L0/L1/L2 margins are checked once at the
   envelope corners for completeness, but no mode-ownership question remains
   for them at cruise.
2. **Phase 4 target confirmed**: the redesign surface is the outer loops
   (plan §P4.2 TECS / cascade retune). The measured design constraints are:
   - outer-loop VS bandwidth must sit well below the ~1.4 s FCS lag
     (≥ 5–10× separation rule → VS loop crossover ≤ ~0.1–0.2 rad/s);
   - `gamma_corr_limit` must exceed the maximum *needed* correction by
     margin, or the damper relays (current ±0.15 rails);
   - the speed loop needs the engine's true lag characterized first (§3.3).
3. **The landing-config point** (160 kts, gear+flaps) is still unmeasured —
   the harness needs a gear/flaps config switch (small tool addition, no FM
   changes). Scheduled with Phase 2.

---

## 5. Data & Reproduction

All CSVs are regenerated with (from `build-gl`):

```bash
./tools/fm_sysid/fm_sysid alpha-sweep   10000 422  /tmp/sysid/alpha_sweep_422.csv
./tools/fm_sysid/fm_sysid speed-sweep   10000 250 800 22 /tmp/sysid/speed_sweep_10000.csv
./tools/fm_sysid/fm_sysid trim-hold     10000 422 120 /tmp/sysid/trimhold_422.csv
./tools/fm_sysid/fm_sysid trim-hold     10000 506 120 /tmp/sysid/trimhold_506.csv
./tools/fm_sysid/fm_sysid trim-hold     10000 758 120 /tmp/sysid/trimhold_758.csv
./tools/fm_sysid/fm_sysid stick-step    10000 422 0.3 30 /tmp/sysid/stickstep.csv
./tools/fm_sysid/fm_sysid throttle-step 10000 422 0.2 40 /tmp/sysid/throttlestep.csv
./tools/fm_sysid/fm_sysid ai-hold 10000 422 120 1 0 /tmp/sysid/aihold_C_alt_only.csv
./tools/fm_sysid/fm_sysid ai-hold 10000 422 120 0 1 /tmp/sysid/aihold_D_spd_only.csv
./tools/fm_sysid/fm_sysid ai-hold 10000 422 120 1 1 /tmp/sysid/aihold_E_full.csv
```

Trace columns (time-series modes): state (vt, vcas, alt, VS, γ, α, θ, φ, q),
FCS loop signals (ptcmd, aoacmd, alpha_bias, q_damper_term, pi_error,
pitch_integral, ω_sp, zp01, tp02/03), plant (thrust_accel, vt_dot, qsom,
qbar, stall_state), and the AirSteering cascade intermediates (vs_target,
vs_corr, alt_err, gamma_corr, alpha_est, theta_target, speed_err,
speed_integral, energy_err).

---

## 6. Phase 1 Exit Checklist

- [x] Sysid harness built and reproducible (`tools/fm_sysid/`, CMake target).
- [x] Static maps at 2+ trim points; K_nz(V) measured; FCS gain assumption verified.
- [x] Config A stability at 3 trim points — **stable**.
- [x] Bisection configs C/D/E — **mode ownership assigned to L3**.
- [x] Engine lag characterized (two-timeconstant anomaly flagged).
- [x] Trace diagnostic columns wired into the scenario FCS trace (P0.3).
- [ ] Approach-config point (gear+flaps) — deferred to Phase 2 (tool switch).
- [ ] Frequency-domain margins (sinusoid injection) — Phase 2 proper.

---

## 7. Correction note (Phase 2)

Two harness defects were found during Phase 2 that affect this document's
config C/D/E runs (§3.2, §3.3) — see `LOOP_MARGIN_REPORT.md` §8 for the
full description:

1. The speed target was passed as **TAS** while `steer()` regulates **CAS**
   (phantom +34 kt underspeed at 10,000 ft; `energy_err` +700 ft at trim).
   Config D's behavior and config E's throttle slamming 0.08–1.00 were
   artifacts of that phantom, not of the control laws.
2. The frozen throttle came from an 8-s AI settle (0.214) rather than the
   true trim value (0.157).

**Superseded**: F4's "L4 doubles the amplitude" (config E) and the config
D description. **Unchanged and re-confirmed at the corrected operating
points**: F1 (config A stable), F2 (mode owned by L3), F3 (`gamma_corr`
rails), F5 (reinterpreted — see `LOOP_MARGIN_REPORT.md` M5: the ~30 s
thrust creep is the airframe speed integration through the thrust tables'
dThrust/dMach slope, not a second engine lag; below MIL the thrust is
algebraic in throttle), F6 (residual-γ trim gap). Corrected numbers:
`BISECTION_RESULTS.md`.

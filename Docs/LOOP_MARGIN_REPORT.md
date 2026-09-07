# Loop Margin Report — PHUG-PLAN Phase 2 (Measured)

> **Status**: Complete. Deliverable of `LONGITUDINAL_STABILITY_PLAN.md` §6
> (Phase 2), executed with the `tools/fm_sysid margin` sinusoid-injection
> mode built for this phase. Phase 3's gain-sweep root locus is included in
> §7 (it uses the same harness). Companion to `PLANT_IDENTIFICATION.md`
> (Phase 1) and `BISECTION_RESULTS.md` (Phase 3).
> **Config**: F-16 (`f16.json`), 10,000 ft clean at 250/300/450 kts TAS and
> 5,000 ft gear+TEF+LEF at 160 kts CAS (the approach point). 60 Hz major
> frames, FM-internal 6× substep. All CSVs reproducible via §9.
> **Moratorium**: no control-law code was changed. All changes are
> `tools/fm_sysid`, two analysis scripts, and documents.

---

## 1. Method

Sinusoidal-reference injection at the summing junction of each loop, with
lock-in (synchronous demodulation) extraction over an integer number of
periods — zero spectral leakage by construction. Self-test: the recorded
`R(pstick)` equals `1 + j0` to 15 digits at every frequency.

| Case | Injection | Loops live | Direct response |
|---|---|---|---|
| `g` | pstick ±0.03 | FCS core only, throttle at exact trim | `Rd = R(nzcgs)/R(ptcmd)` — the raw closed-loop G response; `L0 = Rd/(1−Rd)` |
| `g0` | pstick ±0.03 | same, Tranche-42 q-damper zeroed | isolates L1's contribution |
| `C` | alt target ±30 ft | altitude cascade, throttle frozen at trim | `Rd = R(alt)`; `L3 = Rd/(1−Rd)` |
| `E` | alt ±30 ft and speed ±4 kt (two sub-sweeps) | full stack | 2×2 matrix → `L = R(I−R)⁻¹` diagonals (other loop closed) |

Frequencies: 0.02–1.6 rad/s (outer loops), 0.1–3.2 rad/s (inner loop).
Frequencies below ~0.2 rad/s for the `g` case invert ill-conditioned
(`R→1` makes `L = R/(1−R)` amplify numeric noise); the **closed-loop peak
|R| and its implied damping ζ ≈ 1/(2|R|ₚₑₐₖ)** are used as the primary
inner-loop metrics — they are raw measurements and never degenerate.

Two harness defects were found and fixed during this phase (they change
Phase 1's D/E baselines; see §8): the speed target was passed as TAS while
`steer()` regulates CAS (a phantom +34 kt underspeed at 10,000 ft), and the
8-s throttle discovery was replaced by the exact inverse of the linear
MIL-branch thrust map.

---

## 2. Headline Findings

| # | Finding | Evidence |
|---|---|---|
| **M1** | **The FCS G-loop is healthy at cruise but only because of its feedbacks.** \|R\|ₚₑₐₖ = 0.91–1.05 (flat, no resonance) at 250–450 kts with the q-damper live. With the damper zeroed the 450-kt point develops a 2.76× peak (ζ ≈ 0.18) at 0.2 rad/s — L1 is doing real damping work at high speed. | §3.1, Table A |
| **M2** | **The approach point (160 kts, gear+TEF+LEF) has a resonant G-loop: 6.9× peak (ζ ≈ 0.073) at 0.2 rad/s (T ≈ 31 s).** The q-damper is *gated off* exactly there (Tranche 44: gear; Tranche 46: <200 ft AGL) and `pitchGearGain` = 0.8 scales kp05 down. This is the inner-loop signature of the historically reported 8–12 s approach porpoising. Fails the ζ ≥ 0.08 gate. | §3.1, Table A |
| **M3** | **The L3 cascade's cruise limit cycle is owned by SATURATION, not by small-signal margins.** Linearized about a nulled state, L3 looks acceptable (|R|ₚₑₐₖ 1.2–4.0, ζ 0.13–0.41). But every operational run at 250 kts shows `gamma_corr` **railing at ±0.15 rad every half-cycle** — the damper acts as a relay, and a relay in the loop sustains the ~16.7 s cycle regardless of the linear margins. The plan's §P2.4 prediction ("clamped damping still subtracts damping") is confirmed mechanistically. | §3.2, Table A |
| **M4** | **The speed loop has no authority at the 250-kt/10-kft trim: `throttle_min` = 0.25 sits ABOVE the trim throttle 0.157.** Config E's throttle is pinned at the floor for the entire run while the true trim needs 0.157 — the L4 loop is one-sided there. (F7 in the corrected bisection document.) | §3.3 |
| **M5** | **The engine has no 30-s lag — the "two-timeconstant" step response of Phase 1 is an airframe feedback path.** Below MIL the thrust is *algebraic* in throttle (`engine.cpp` MIL branch); the rpm lag (τ ≈ 1.5–2 s) gates only the AB branch. The ~30 s thrust creep in the throttle-step test tracks **mach through the thrust tables' slope** (dThrust/dMach ≈ 4.4 (ft/s²)/mach at 0.4 G throttle, rising convexly to ≈ 17 by 0.6). **∂Thrust/∂V > 0 is a destabilizing slope for the speed mode** — the opposite sign from a real turbojet. | §3.4 |
| **M6** | **The pitch "integrator" is not an integrator.** The QIL leak writes back through `reset()`, which on the `AdamsBash2Filter` clobbers `u_prev` with the *output*. Effective law: `ė = 1.5·e_in − 0.508·e` — a first-order lag with DC gain 2.95·kp03 = 5.9 and τ ≈ 2 s. Measured: the integrator state freezes at exactly `5.9 × error` (predicted 0.02754, observed 0.0276) while a persistent error stands. The G-loop still tracks because the alpha-bias feedforward holds nzcgs = baseline by construction (type-0 behavior, not type-1). | §3.5 |
| **M7** | **The alpha-bias path (L2) is a second, comparably-gained loop at low frequency.** Δbias = 2·bias·ΔV/V propagates the speed/energy response into alpha with the same order of gain as the G-loop at 0.1 rad/s — the measured "L0" is honestly L0+L1+L2 combined. Any Phase-4 inner-loop redesign must treat the bias as an in-loop element, not a feedforward. | §3.6 |

---

## 3. Measured Data

### 3.1 Table A — inner loop L0 (closed-loop G response to pstick)

Primary metric: closed-loop resonant peak |R|ₚₑₐₖ and ζ ≈ 1/(2|R|ₚₑₐₖ).
Gate: no peak above 1.6 (ζ ≥ 0.31) at cruise; ζ ≥ 0.08 everywhere.

| config | q-damper | \|R\|ₚₑₐₖ @ f | ζ est | verdict |
|---|---|---|---|---|
| 250 kts | on | 0.91 @ 0.2 | — | **PASS** (flat) |
| 250 kts | off | 1.18 @ 0.2 | — | PASS |
| 300 kts | on | 0.99 @ 0.2 | — | **PASS** |
| 300 kts | off | 1.33 @ 0.2 | 0.38 | PASS |
| 450 kts | on | 1.05 @ 0.2 | — | **PASS** |
| 450 kts | off | 2.76 @ 0.2 | 0.18 | marginal — damper earns its keep |
| 160 kts gear | (gated off) | **6.88 @ 0.2** | **0.073** | **FAIL** |

The PM columns produced by the `L = R/(1−R)` inversion inside the
`R ≈ 1` band are numerically meaningless and are deliberately omitted;
the raw |R| peak is the evidence.

### 3.2 Table B — L3 altitude cascade (config C, isolated)

Closed-loop alt response to an injected altitude-target sinusoid:

| config | \|R\|ₚₑₐₖ @ f | ζ est | operational behavior (120–240 s runs) |
|---|---|---|---|
| 250 kts | 3.68 @ 0.2 | 0.14 | sustained ~16.7 s limit cycle, `gamma_corr` rails ±0.15 every half-cycle, amp ≈ 142 ft |
| 300 kts | 1.74 @ 0.2 | 0.29 | sustained ~12.6 s cycle, amp ≈ 26 ft |
| 450 kts | 1.23 @ 0.1 | 0.41 | no significant cycle |
| 160 kts gear | 6.51 @ 0.2 | 0.077 | slow large cycle (T ≈ 61 s, amp ≈ 330 ft) + inner-loop resonance (M2) |

The contradiction between acceptable linearized margins and the sustained
cycle is the finding: the cycle lives in the **saturated** region
(`gamma_corr` rails, `vs_target` slew-limits), where the describing
function of the clamped damper — not the linear gain — sets the loop.

### 3.3 Table C — L4 speed loop (config E 2×2 diagonal, L3 closed)

| config | \|R(vcas)\|ₚₑₐₖ | note |
|---|---|---|
| 250 kts | 0.99 | throttle pinned at floor 0.25 (M4) — loop inert, response = open-loop plant |
| 300 kts | 0.92 | sluggish; speed loop integrates through the ~30 s energy path |
| 450 kts | 1.07 | borderline |
| 160 kts gear | 1.63 @ 0.1 | ζ 0.31 — acceptable |

### 3.4 Engine (M5)

- Static map (`thrust-map`): below MIL, `T(thr, mach) = ((Tmil−Tidle)·thr + Tidle)/mass`
  — algebraic in throttle. Confirmed against the Phase-1 step: jump to
  T(0.414) = 8.74 predicted, 8.94 measured (1-frame).
- rpm lag: measured τ ≈ 1.5–2 s (0.748 → 0.8243 settled by t+5 s); affects
  only the AB branch engagement.
- dThrust/dMach at 10,000 ft: 0 below M 0.2, then +4.37 (ft/s²)/mach at
  thr 0.2–0.4 (thr 0.4–0.6 slope ≈ +17). The Phase-1 "30 s creep"
  (+1.26 ft/s²) reproduces as Δmach(t) × dT/dmach through the airframe's
  speed integration. **A thrust-boosts-speed slope is anti-damping for the
  speed mode** — quantified for Phase 4's TECS design as
  ∂(T−D)/∂V = +0.004 (ft/s²)/(ft/s) at 250 kts.

### 3.5 The poisoned pitch integrator (M6)

`fcs.cpp` (QIL leak) ends with `pitchIntegral.reset(eintg)` every call.
For `AdamsBash2Filter`, `reset(y)` sets **both** `y_prev` and `u_prev` to
`y` — the previous *input* becomes the previous *output*. The recurrence

```
e[n] = e[n−1] + (dt/2)(3·u − u_prev)  →  ė = 1.5·u − 0.5·e (per second at 360 Hz)
```

becomes a first-order lag: DC gain `2.95·kp03 = 5.9`, τ ≈ 2 s (plus the
120 s leak, negligible). Measured freeze value vs prediction at the
stick-step operating point: predicted `5.9 × 0.00467 = 0.02755`, observed
`0.0276`. Consequences: the G-loop has finite DC gain (type 0); steady-state
G errors persist at `(1 + 5.9·kp05·plsdamp·K_nz)⁻¹` of the command; the
pole-placement algebra's type-1 assumption is void. **Phase 4 (P4.1)
replaces the whole mechanism** — the fix belongs there, not in a hot patch.

### 3.6 L2 bias coupling (M7)

At 250 kts the bias is α_bias ≈ 6.9°; a 1-fps speed perturbation moves it
by `2·α_bias/V ≈ 0.033°` → 0.0048 G through K_nz — the same order as the
G-loop's own response to a 0.15 G command at 0.1 rad/s. The measured
`g`-case transfer therefore *includes* the bias loop; separating them
further is deferred to the Phase-4 redesign where the bias is restructured
anyway.

---

## 4. Phase 2 Gate — Verdict per Loop per Trim Point

Gate: PM ≥ 45°, GM ≥ 6 dB (or, for resonance-metric loops, ζ ≥ 0.08).

| Loop | 250 kts | 300 kts | 450 kts | 160 kts gear |
|---|---|---|---|---|
| L0 G-loop | PASS | PASS | PASS | **FAIL (ζ 0.073)** |
| L1 q-damper | helps | helps | **essential** (removing it → ζ 0.18) | gated OFF (Tranche 44/46) |
| L2 bias | in-loop element — folded into L0 | | | |
| L3 alt cascade | linearly OK; **saturation-owned 16.7 s cycle** | smaller cycle (12.6 s) | OK | slow cycle + L0 resonance |
| L4 speed | **no authority (floor > trim)** | sluggish | borderline | OK-ish |

**Phase 2 conclusion**: the persistent cruise "phugoid" is not a
small-signal margin failure anywhere; it is the saturated-damper relay
cycle in L3 (with L4 pinned/inert at 250 kts), and the approach regime
adds a genuine inner-loop resonance (M2). Phase 4's redesign must
therefore (a) bound the damper's authority *above* the demand (kill the
relay), (b) restore speed-loop authority (floor below trim), (c) re-derive
the approach-config inner loop (M2), and (d) implement the P4.1
anti-windup replacement (M6).

---

## 5. Root Locus (Phase 3 remainder) — period tracking

One-at-a-time ×0.5/×2 sweeps at config E, 240 s runs (full tables in
`BISECTION_RESULTS.md`). The mode's period tracks:

| parameter | ×0.5 → T | base T | ×2 → T | reading |
|---|---|---|---|---|
| `path_gain` (gamma_corr) | **31.8 s** | 17.9 s | 18.9 s | strongest single-owner signature |
| `alt_integral_gain` | 16.7 s | 17.9 s | **30.7 s** | co-owner (integrator) |
| `attitude_gain` | 89 s | 17.9 s | 26 s | strong effect, non-monotone |
| `vs_gain` | (cycle collapses) | 17.9 s | 36.8 s | co-owner |
| `speed_damp` | 18.0 s | 17.9 s | 28.1 s, **ζ = +0.04, 4 peaks** | the only knob that ADDS damping |
| `pitch_rate_damp`, `max_vs_fpm` | ~no effect | | | exonerated |

At 300 kts the same mode runs at 12.6 s; at 160 kts gear it slows to 61 s.

---

## 6. Consequences for Phase 4

1. **TECS (P4.2)** is confirmed as the outer-loop replacement, with the
   measured design constraints: outer-loop crossover ≤ 0.1–0.2 rad/s
   (5–10× below the ~1.4 s G-loop lag), damper authority ≥ max needed
   correction (kill the relay), speed-loop throttle floor BELOW trim
   (M4), and the ∂T/∂V > 0 engine slope (M5) folded into the energy-rate
   loop's plant model.
2. **P4.1** must replace the AB2-reset "integrator" (M6) with a real
   integrator + back-calculation anti-windup, and decide L1's fate with
   the M1/M2 data: keep the damper at cruise (it carries 450 kts), restore
   an equivalent damper for the approach config where it is currently
   gated off (M2).
3. **P4.3 (approach)**: the gear-config inner loop needs its own margin
   check against the M2 resonance before any outer-loop work lands there.

---

## 7. Data & Reproduction

```bash
# from build-gl (after the Phase-2 harness):
./tools/fm_sysid/fm_sysid margin 10000 422 g  0 /tmp/sysid/margin_g_422.csv
./tools/fm_sysid/fm_sysid margin 10000 422 g0 0 /tmp/sysid/margin_g0_422.csv
./tools/fm_sysid/fm_sysid margin 10000 422 C  0 /tmp/sysid/margin_C_422.csv
./tools/fm_sysid/fm_sysid margin 10000 422 E  0 /tmp/sysid/margin_E_422.csv
# (repeat for 506, 758; approach point: margin 5000 270 <case> 1)
./tools/fm_sysid/fm_sysid thrust-map 10000 /tmp/sysid/thrustmap_10000.csv
./tools/fm_sysid/fm_sysid rootlocus 10000 422 0 240 /tmp/sysid/rootlocus_422.csv
python3 scripts/analyze_margins.py /tmp/sysid/margin_*.csv
python3 scripts/trace_metrics.py /tmp/sysid/rootlocus_422.csv \
    --column alt_msl_ft --t0 20 --group-by param,factor
```

`fm_sysid` modes added in this phase: `margin` (lock-in frequency
response), `rootlocus` (gain sweep), `thrust-map` (static engine map);
`alpha-sweep`/`speed-sweep` gained a gear/flaps config argument; time-series
CSVs gained `speed_brake, tef_pos, lef_pos, gear_pos, rpm, ai_pitch_cmd`
columns. New analysis scripts: `scripts/analyze_margins.py`,
`scripts/trace_metrics.py` (the Phase-5 metrics tool, delivered early
because Phase 3's root locus needs it).

## 8. Harness Defects Found and Fixed (affects Phase 1's D/E baselines)

1. **TAS-vs-CAS speed target**: `ai-hold`/`margin`/`rootlocus` passed
   `vt·KT_PER_FPS` (TAS) as the speed target while `steer()` regulates
   CAS — at 10,000 ft that is a phantom +34 kt underspeed
   (`energy_err = +700 ft` at trim), slamming the throttle. All
   config-D/E-style runs measured a skewed operating point. Fix: targets
   are the trim CAS (`state().vcas`).
2. **Throttle discovery**: the 8-s AirSteering settle returned
   `throttle_mid`-biased values (0.59 with a correct target, 0.214 with
   the phantom). Replaced with the exact inverse of the linear MIL-branch
   thrust map against the trim solver's converged thrust → 0.1566 at the
   250-kt point (true trim; the old 0.214 was ~0.06 hot — part of config
   A's slow drift in Phase 1).
3. Lock-in window sizing now degrades gracefully under the run-time cap
   (the f = 0.02 rad/s window previously overran its buffer).

Corrected config-matrix numbers supersede `PLANT_IDENTIFICATION.md` §3.2
and are recorded in `BISECTION_RESULTS.md`; that document carries a
correction note rather than being rewritten (its F1/F2/F3/F6 conclusions
are unchanged; F4's amplitude numbers and the D-config description are
superseded).

## 9. Phase 2 Exit Checklist

- [x] Sinusoid-injection margin harness with lock-in extraction + self-test
- [x] Per-loop PM/GM/|R|-peak per trim point incl. 2×2 MIMO (L3/L4)
- [x] `computeGains` audit: pole-placement algebra reproduces the traced
      tp02/tp03 exactly; the plant-assumption mismatch is localized to
      the integrator mechanism (M6) + AOA-command-mode kp05 (0.244, not
      the G-command formula — gsAvail ≤ maxGs at all four trim points)
- [x] Engine two-timeconstant anomaly resolved (M5)
- [x] Root locus for mode ownership (§5, Phase 3 remainder)
- [x] Approach-config point measured (M2)
- [x] `LOOP_MARGIN_REPORT.md` (this document) + `BISECTION_RESULTS.md`

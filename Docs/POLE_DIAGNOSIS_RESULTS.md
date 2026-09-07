# Pole Diagnosis — Phase A–C Results

> **Status**: Phase A–C executed (Task 63). Phase D (trace mode-ID) and the
> first Phase E ablation + F-3 retune executed (Task 64) — see §6.
> **The leading hypotheses were tested and four were refuted; the mechanism
> is identified.**
> **Tool**: `f4-flight-model/tests/diag_poles.cpp` (build target `diag_poles`)
> **Plan**: [FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md](FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md)
> **Evidence**: [diagnostics/phaseB_plant_poles.csv](diagnostics/phaseB_plant_poles.csv),
> [diagnostics/verify_approach_200kts_gearflaps.csv](diagnostics/verify_approach_200kts_gearflaps.csv)

---

## 1. What was built (Phase A)

- `f4-math/include/f4/math/eigen_real.hpp` — dense real nonsymmetric
  eigensolver (balance → Hessenberg → shifted complex QR → inverse-iteration
  eigenvectors) with a residual-checked test suite (`test_eigen.cpp`, 7 tests).
- `f4-flight-model/tests/diag_poles.cpp` — the pole tool:
  - **Trim**: quasi-steady α/throttle solve with the FCS in the loop, then a
    nested fixed-point Newton — outer secant on the throttle, inner square
    Newton on the state vector. The trim is a genuine fixed point of the
    major-frame map (vt pinned to the requested speed; throttle converges;
    residual floor is the physical integrator-leak creep).
  - **Linearization**: central-difference Jacobian of the major-frame map
    (dt = 1/60 s) over 24 carried dynamic states: FCS filter/integrator
    histories, actuator positions, aero coefficients the FCS reads stale,
    load factors, engine spool, (+position and AI integrators with `--ai`).
    Dead states (no feedback path at a given trim — e.g. `cnalpha` in
    AOA-command mode) are detected and pruned automatically.
  - **Speed stability** (Phase B): `S_u = d(T−D)/dV` from the aero + engine
    models directly, with the frozen-α prediction ζ = −S_u·V/(2√2·g).
  - **Loop knobs** (Phase C): `--freeze-bias` (L3), `--qdamp-scale` (L4),
    `--ai` + `--ai-gain-scale` (L1/L2), `--verify` (time domain).

## 2. Findings

### F1 — The back-side-of-the-drag-curve hypothesis is REFUTED

The plan's P-B2 predicted the approach-config speed mode sits in the RHP
because S_u > 0 on the back side. Measured S_u is **negative everywhere
tested** (18 conditions, 160–450 KCAS × clean/gear/gear+flaps, SL–15k ft):

| Condition | S_u [1/s] | ζ frozen-α (predicted) |
|---|---|---|
| 250 kt clean 15k ft | −0.0139 | +0.081 |
| 200 kt gear+flaps 5k ft | −0.0422 | +0.168 |
| 160 kt gear+flaps 5k ft | −0.0418 | +0.158 |

The frozen-α plant is **stably damped at every condition**. The drag-curve is
not the problem.

### F2 — The plant+FCS closed map is unstable at EVERY trim

The measured spectrum has an aperiodic, vt-dominated unstable mode at all 18
conditions (Re +0.004 … +0.29; doubling time 3–144 s; eigenvector 92–99% vt).
Examples (speed-band modes only):

| Condition | unstable modes Re(λ_c) | t₂ₓ |
|---|---|---|
| 250 kt clean 15k | +0.0100, +0.0211 | 69 s, 33 s |
| 200 kt gear+flaps 5k | +0.0236, +0.0487 | 29 s, 14 s |
| 450 kt gear 15k | +0.2743 | 2.5 s |

Time-domain cross-check (`--verify`, +25 ft/s perturbation, plant only):
at the approach condition the response diverges into a mean +1,736 fpm
balloon with ±2,224 fpm swings in 180 s — aperiodic, consistent with the two
real unstable poles. **This is the mechanism behind the ±1,500–3,000 fpm
final-approach oscillation**: in flight the AI throttle loop fights these
modes from outside; the residual swings are the closed-loop residue.

### F3 — The α-bias feedback (L3) is NOT the destabilizer

`--freeze-bias` (bias' sensed cosγ/cosμ/qsom feedback frozen at trim values,
trim value unchanged) does **not** stabilize the speed modes: they persist
(and re-appear as an anti-damped oscillation ζ = −0.29 at T = 57.6 s — the
textbook phugoid period). P-C1 as the sole cause is refuted.

### F4 — The G-hold law itself is the mechanism

With `pstick = 0` the FCS holds `nzcgs = cosγ/cosμ` — **lift pinned to the
weight component** — through a chain of PI integrator + F7Tust lead-lag +
pitch-rate lag. This cancels the natural phugoid restoring term (Δγ̇ =
2g/V²·ΔV) and replaces it with a lagged α response. The destabilizing path
is V↑ → (G-loop, lagged) α↓ → induced drag↓ → V↑: the α-path converts the
airframe's positive drag damping into positive feedback. The bias formula
merely duplicates what the G-loop already does; that is why freezing it
changes nothing. The remaining damping measured (the −0.056 stable vt mode)
is the drag term acting through the fraction of lift response that is
in-phase.

### F5 — The q-damper cannot fix it (explains Tranche 42's failure)

`--qdamp-scale` 0 → 4: the unstable vt modes move from +0.0099/+0.0236 to
+0.0104/+0.0164 — nearly inert. The mode's eigenvector is 92–99% vt with
negligible q participation: **q-feedback is orthogonal to this mode.** The
damping must come from a term that enters **V̇** directly: thrust, or drag
via α-in-phase-with-V.

### F6 — AI-closed leaves a slow instability driven by the altitude integral

With `--ai` (AirSteering cruise law closed), the fast unstable modes are
stabilized by the throttle loop, but a slow unstable mode remains
(+0.0048, t₂ₓ ≈ 144 s, dominated by `ai_altI` — the leaky altitude integral
winding). This matches the field observation that motivated the QIL
integrator leak and the various AI-side dampers. (Caveat: the AI map's
slew-limiter/clamp structure produces some linearization artifacts at other
frequencies; the AI-side spectrum needs the describing-function treatment
from the plan's Phase C before its numbers are quoted as margins.)

## 3. What fixes it (revised Phase F direction)

> **Task 64 update (§6): prediction 1 below was REFUTED by measurement —
> the pitch-channel G-command damper destabilizes the plant-only aperiodic
> modes and only marginally helps the AI-closed approach proxy. The F-3
> bandwidth-separation retune (§6, STAB-P1) is the first fix that landed,
> and the throttle/TECS channel (2) remains the architectural fix for the
> aperiodic mode.**

The mode is stabilized only by feedback that adds damping to V̇:

1. **Pitch-channel speed damper**: `ptcmd −= k_V·(V − V_trim)` inside the
   FCS, gain-scheduled, ALWAYS ON (no gear/AGL gates). This is the classical
   phugoid damper and is architecturally the same thing the AI's
   `speed_damp_rad_per_kt` does from outside — but inside the FCS it has no
   sample lag and no AI-loop interaction. Predicted: closes both unstable
   vt modes at k_V ≈ (2·ζ_des·ω_n·√2·g)/V.
2. **Throttle energy loop** (TECS): the AI throttle PI already helps (F6);
   formalizing it as the TECS energy-rate law gives the approach
   configuration a stabilizing channel that does not exist today.
3. The integrator leak / shedding / speed_damp band-aids can then be
   removed (they are all compensating F4's loop); re-verify with the
   ablation matrix (Phase E).

## 4. Tool usage

```
diag_poles --alt 15000 --kcas 250 --config clean          # single condition
diag_poles --sweep out.csv                                # Phase B grid
diag_poles ... --freeze-bias                              # L3 break
diag_poles ... --qdamp-scale 2                            # L4 locus
diag_poles ... --ai --ai-gain-scale 1.5                   # L1/L2 closed
diag_poles ... --verify out.csv                           # time domain
```

Known limitations (all documented in the tool header): stall relay frozen at
None; fuel mass excluded (hours-scale); plant-mode position excluded (the
integrator leak means no exact equilibrium exists with z stationary — the
trim is taken at the pinned speed); AI-side spectrum carries clamp artifacts.

## 5. Next steps

1. ~~Phase D: run `trace_modes.py` (to be written) over the existing
   `digi_full_mission` CSVs~~ — DONE for the committed verify traces (§6,
   F7); the user's mission CSVs should be run through
   `scripts/trace_modes.py` the same way.
2. ~~Implement the FCS speed damper (§3.1) behind a gain constant~~ — DONE
   and REFUTED at plant level (§6, F8); the AI-side L1 retune (STAB-P1)
   landed instead with the CI golden-pole gate.
3. ~~Phase E ablation: with the damper on, remove leak/shedding/speed_damp
   one at a time~~ — DONE without the damper (§6, F10): leak/q-damper earn
   their place at cruise, are harmful at the approach proxy; shedding is a
   cruise no-op; the T≈12.7 s limit cycle is sustained by none of them.
4. Remaining: the slew-limiter limit cycle needs the describing-function
   treatment (plan §4 output 2); the F-2 TECS energy loop on the throttle
   remains the fix for the aperiodic plant mode; the approach law
   (`steer_approach`) must be wired into `diag_poles --ai` before
   approach-condition poles are quoted as margins (the cruise-law proxy
   trims poorly there — trim residual 1e3–1e4).

## 6. Task 64 — Phase D mode-ID, the pitch-damper experiment, STAB-P1, and the first Phase E ablation

Evidence:
[diagnostics/phaseB2_plant_default_poles.csv](diagnostics/phaseB2_plant_default_poles.csv),
[diagnostics/phaseB2_ai_default_poles.csv](diagnostics/phaseB2_ai_default_poles.csv),
[diagnostics/phaseE_ablation.csv](diagnostics/phaseE_ablation.csv).

### F7 — Phase D: the verify trace's oscillation is a closed-loop mode, not a bare-plant phugoid

`scripts/trace_modes.py` (Welch PSD + cross-spectrum phase + envelope
doubling time) on the committed 180 s approach verify trace:

- Full window: dominant T = 45.1 s (prominence >100 dB), α leads VS by
  +138° — α is NOT constant relative to the V/h exchange, so the
  bare-plant phugoid signature (α ≈ const) is absent: this is the
  closed-loop G-hold mode.
- Early window (0–70 s, before clamp contact): T = 17.5 s with α ~
  anti-phase to VS (−167°) — again FCS-α-involved.
- Both windows match the plan §5 discrimination table's
  "aperiodic-divergence + clamp sawtooth" and "AI altitude-hold" bands
  rather than the bare-plant row.

### F8 — The FCS pitch-channel speed damper (§3.1) is REFUTED at plant level

Implemented exactly as §3.1 sketched (washout-referenced G-command damper,
`FcsState::speedDampGain` in G per ft/s, washout τ default 60 s, always-on
in air, contribution clamp ±0.4 G; knobs `--sd-gain/--sd-tau`, default OFF):

- k > 0 (fast → pull) at cruise 15000/250 clean: the two aperiodic modes
  WORSEN from +0.010/+0.021 to +0.016/+0.062 (k=0.005); at approach
  5000/160/gf it additionally creates an unstable oscillatory pair
  +0.031 ± 0.028. Robust across τ ∈ {20, 60, 120, 240} s.
- k < 0 is inert by construction (gate `speedDampGain > 0`).
- Mechanism: the G-loop PI enforces the damper's extra G, and the
  induced-drag benefit is overwhelmed by the lagged-α path the damper
  drives — the same anti-damping channel as F4, now with more gain.
  Consistent with §9.2's phase-budget argument: pitch cannot damp this
  mode; only thrust enters V̇ with the right sign structure.

The knob remains in the code (default off) as the documented negative
result; the washout state is reusable for a future alpha-channel or
reference-speed variant if the TECS redesign wants it.

### F9 — STAB-P1 (F-3 bandwidth separation, L1): alt_integral_gain 1.2 → 0.6

AI-closed (cruise law) at the canonical cruise trim, the remaining
instability (F6) is the altitude-integral loop: crossover ≈ 0.125 rad/s
with the 0.1/s integral-leak pole inside its bandwidth. Measured:

| altI gain scale | worst sub-1-rad/s Re (1/s) | t₂ₓ |
|---|---|---|
| 1.0 (stock 1.2) | **+0.1035** (+0.0048) | 6.7 s (144 s) |
| **0.5 (= 0.6, shipped)** | **+0.0015** | ~460 s (neutral) |
| 0.25 | +0.377 | 1.9 s |
| 0.0 | +0.0069 | 101 s |

Non-monotonic below 0.5 (other loops take over) — the eigenvalue scan, not
hand-tuning, located the optimum. thrI ×{0.5, 2} and vs_gain ×0.6 both made
it worse; their stock values stand. Time domain (+5 ft/s perturbation):
mean VS error improves −37.6 → −5.6 fpm. **Shipped as STAB-P1**
(`air_steering.hpp`), with the pole evidence in the header comment.

### F10 — Phase E ablation matrix (first pass)

At cruise (250/clean AI-closed, base worst +0.0015): removing the QIL
integrator leak costs +0.080 of damping (the leak earns its place);
removing the q-damper costs +0.061 (it DOES participate with the AI closed
— contrasting the plant-only F5); freezing the bias feedback costs +0.048;
shedding is a no-op (+0.000) at this trim — candidate for removal, pending
the approach numbers.

At the approach proxy (160/gf, cruise law — trim residual 1e3–1e4, LOW
CONFIDENCE): the leak is HARMFUL (removing it: +0.244 → +0.021), shedding
is harmful (+0.244 → +0.100), and the FCS speed damper helps (+0.244 →
+0.066 at k=0.002). Condition-dependent patches — exactly what the matrix
exists to expose; re-measure after `steer_approach` is wired into the tool.

### F11 — The residual limit cycle is a nonlinear AI-pitch mode, not any single band-aid

Time-domain verify at +5 ft/s perturbation still sustains an
amplitude-independent T ≈ 12.7 s cycle (±770 fpm VS; α and nz in phase at
−126°; pstick anti-phase to VS at −167°, grazing the −0.35 pitch clamp).
Ablations that do NOT change it: QIL leak, shedding, STAB-E10 window
(already inactive), U2 alpha-rate damp (already inactive), q-damper scale.
Ablation that does: the STAB-E29 VS slew limiter — ×4 slew rate cuts the
amplitude 43%. Conclusion: the cycle is set by the slew-limiter describing
function interacting with the AI pitch path; needs the plan §4 output-2
treatment (empirical Bode / describing functions), not another damper.

### Regression and CI

- The plant-only 18-condition sweep reproduces the Task-63 CSV to 0.0%
  with every Task-64 code change default-off — the additions are provably
  behavior-neutral when disabled.
- `test_poles_envelope` (3 tests, drives `diag_poles` end-to-end): plant
  golden +0.02106 ±5%; AI-cruise slow-mode gate ≤ +0.005 and golden
  +0.00151. A PR that re-grows the L1 instability now fails CI.
- Known open items (not yet at §8 acceptance): high-speed conditions
  (350–450 kt) and the approach-proxy retain slow unstable modes; the
  AI-closed envelope lives in phaseB2_ai_default_poles.csv as the living
  baseline for the next tasks (F-2 TECS, slew describing functions,
  approach-law wiring).

---

## 7. Task 66 — merged-tree re-validation (PHUG-PLAN merge, `--tune` selector, re-derived CI gates)

The Task 63/64 program was rebased onto the upstream PHUG-PLAN merge
(origin/main `d46807a`: `tools/fm_sysid`, the P4.1 FCS inner-loop
corrections, the P5 time-domain CI gates). The two programs were run
independently and **converged on the same diagnosis** — the persistent
oscillation is a closed-loop artifact of the L3 AI altitude cascade, and
the FCS G-hold law (not the bare airframe) owns the aperiodic speed
instability. Everything below is re-measured on the merged tree with
both toolchains.

### 7.1 The P4.1 corrections largely fix the Task-63 plant finding

| metric (15k ft / 250 KCAS clean) | Task 63 (pre-P4.1) | merged tree |
|---|---|---|
| plant+FCS worst aperiodic slow mode | **+0.02106 /s** | **+0.00115 /s** (26× better) |

`kp05 = 1/K_nz` (the realized loop gain was 0.036) + the real integrator
fix the type-0 G-loop whose speed-mode anti-damping F1–F4 measured. The
pole gate is converted from a golden pin to the stability acceptance
(≤ +0.005 /s). **The two programs' headline findings fixed each other's
targets.**

### 7.2 STAB-P1 re-validated and load-bearing against the P4.1 loop

AI-closed worst sub-1-rad/s mode, nav cruise tune, 15k/250:
`alt_integral_gain` **1.2 → +0.2196 /s**; **0.6 (STAB-P1) → +0.0166 /s**.
The bandwidth-separation fix transfers to the corrected inner loop.

### 7.3 The default-tune cascade remains marginally unstable (parked TECS work)

AI-closed worst slow mode across the cruise envelope: default tune
+0.23..+0.58 /s (15k/250: +0.2306 ± 0.1023j, T 61 s); nav tune
+0.02..+0.53 /s. `fm_sysid margin C` agrees independently (|R|peak 48.3
@ 0.2 rad/s, ζ 0.010, class defaults). Wingman/BVR/WVR/missile/
collision-avoid/ground-avoid fly the class defaults. Measured verdict:
**gain re-hunting is refused** — the landscape is razor-non-monotonic
(vs_gain ×0.25 helps at 15k, hurts at 5k; halving the alt integral again
triples the mode; the slew rate inverts sign twice), which is the
30-patch-loop signature. The structural fix is the F-2/P4.2 TECS margin
campaign, exactly as upstream's own gate requires. New CI gates bound
the regression meanwhile (§7.4).

### 7.4 Re-derived CI contract (`test_poles_envelope.cpp`, 5 gates)

1. `PlantCruiseAperiodicModeGolden` — plant+FCS ≤ +0.005 /s (measured +0.00115).
2. `AiCruiseNavTuneSlowModeGolden` — nav-tune golden +0.01656 (±max(5%, 5e-3)).
3. `AiCruiseNavTuneSlowModeGate` — nav-tune ≤ +0.05 (3× headroom).
4. `AiCruiseNavTuneBoundedness` — **time-domain**: +25 ft/s kick at trim,
   nav tune, max |dVS| < 1500 fpm (measured 731). This is the robust
   acceptance: the AI-closed linear scans are polluted by clamp-kink
   Jacobian artifacts (spurious +8..+173 /s modes at the slew-limiter
   kinks); the nonlinear response is ground truth.
5. `AiCruiseDefaultTuneBound` — default tune ≤ +0.70 until the TECS work lands.

New tool knob: `diag_poles --tune default|nav|nav-linband|approach`
(named cruise tunes; `nav` mirrors NavigationModule, `approach` mirrors
the P5.2 gate tune — both composable with the per-knob scales). Note:
`nav-linband` (the M3 linear-band rescale the nav comment claims but the
code never applied) measures **worse** at 15k/250 (+0.419 vs +0.017) —
at cruise the gamma_corr damper is load-bearing; do not rescale it
without the margin campaign.

### 7.5 Cross-verification with the upstream instruments (this tree)

- `test_p5_stability`: PhugoidDampingCruise + AltitudeCapture GREEN (2
  disabled tests re-measured, baselines pinned in the test comments:
  SpeedHoldStep overshoot 46.23 kt / no settle = upstream M4; the
  approach catch-down cannot hold 160 kts at idle throttle).
- `fm_sysid margin g` at 160 kts gear: |R|peak 5.59 @ 0.40 rad/s at the
  default amp 0.03 — upstream's §6.3 open question reproduces exactly;
  the amplitude sweep (53.5× @ 0.01 → 5.59× @ 0.03 → 1.05× @ 0.1 →
  0.48× @ 1.0) resolves it as amplitude conditioning of the approach
  inner loop, with the shipped q-damper **destabilizing** small-signal
  (53.5× live vs 6.2× zeroed at amp 0.01). Recorded as upstream work.

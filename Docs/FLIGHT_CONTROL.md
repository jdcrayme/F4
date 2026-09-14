# Flight Control — As-Built

> **Status**: Reference (as-built). This doc absorbs the surviving truth of
> nine superseded plan/result documents (archive map in §8). It states what
> the FCS *is*, what was measured, what is gated in CI, and what remains open.

---

## 1. Plant architecture (why this FCS looks the way it does)

The FDM is the FreeFalcon **khill pseudo-model**, not a moment-based 6-DOF.
Four facts drive everything else:

1. **Body rates are kinematically commanded** (`eom.cpp`): q from
   `atan(nzcgs·g/V)` minus gravity/gear terms through a first-order lag;
   r from side force + bank kinematics; p is the FCS roll command verbatim.
   No pitching moments, no Iyy, no Cmq, no natural short-period physics.
2. **Alpha is set directly by the FCS** (`fcs.cpp`): `aero.alpha` is written
   every minor step; `alpha_dot` is a finite difference of the command. The
   FCS *is* the alpha actuator.
3. **γ is derived, not integrated**: `gmma = theta − alpha·cosφ`.
4. **nzcgs is algebraic**: a static function of (α, β, Mach, qbar) via the
   CL table.

Consequence: the longitudinal plant Δα→Δnzcgs is a static gain
`K_nz(V) = clalph0·qsom/g` plus one minor-step delay — **a static gain cannot
oscillate**. The only true longitudinal dynamic states are vt, the attitude
integrator, and controller states. All oscillation is closed-loop.

## 2. The measured diagnosis (supersedes 30+ heuristic fixes)

The pre-pole-program pitch channel carried 5 integrators, 5 competing
dampers, 3 self-referential feedforwards, 8+ nonlinear clamps, 4 dynamic
lags. Thirty-plus symptom patches (Exp L/QIL/C/G/Q3/V2/W/U2, STAB-E1…E51,
Tranches 42/45/46) each moved the oscillation to a new frequency or loop and
never closed the class — the signature of patching without a plant model.

The `diag_poles` program (Task 63–66) replaced this with measurement:

- **The closed map is unstable at every trim**: an aperiodic,
  airspeed-dominated mode (eigenvector 92–99% vt, Re +0.004…+0.29 /s) across
  18 envelope conditions. The frozen-alpha airframe itself is positively
  damped everywhere (S_u < 0, ζ +0.07…0.17) — including approach config.
- **Mechanism**: the G-hold law (lift pinned to the weight component through
  an integrator + lag chain) converts drag damping into positive feedback via
  the alpha/induced-drag path. This explains why the Tranche-42 q-damper is
  inert (no q participation) and why the leak/shedding/speed_damp band-aids
  kept being needed. The back-side-of-the-drag-curve and alpha-bias-feedback
  hypotheses were tested and **refuted**.
- **The proposed FCS pitch speed damper was refuted by measurement**
  (Task 64): k > 0 worsens the aperiodic modes at every τ and creates an
  unstable oscillatory pair at approach. Kept default-off as documented
  negative evidence.

## 3. What landed

| Change | Effect |
|---|---|
| **P4.1** inner-loop correction: `kp05 = 1/K_nz`, real integrator | Task-63 G-hold aperiodic mode reduced **26×** (+0.02106 → +0.00115 /s @ 15k/250); closed map passes the original stability acceptance |
| **STAB-P1**: `alt_integral_gain` 1.2 → 0.6 | Last measured cruise instability (AI-closed alt integral, Re +0.1035) → +0.0015 /s (neutral); mean VS error after a speed perturbation improves 8× |
| **STAB-E series (~55 fixes)** incl. E47 damped localizer, E48 past-the-fix capture, E49/E57–E62 flare arrest budget | Full `digi_full_mission` passes end to end; suite 2,435+ |
| **`test_poles_envelope`** (5 CI gates) | Plant golden +0.02106 ±5%; nav-tune golden +0.00151 + regression gate; AI-cruise slow-mode ≤ +0.005; time-domain boundedness (+25 ft/s kick < 1500 fpm, measured 731); default-tune regression bound |

**Instrumentation** (all committed): `diag_poles` (trim → finite-difference
Jacobian → eigenvalues, per-knob loop scales, `--tune` presets),
`f4-math/eigen_real.hpp` (real nonsymmetric eigensolver),
`scripts/trace_modes.py` (Welch PSD + cross-spectrum + doubling time),
`scripts/trace_window.py`, `f4-recorder` CSV trace, FCS HUD overlay.
Evidence CSVs: `Docs/diagnostics/phaseB*.csv`, `phaseE_ablation.csv`,
`verify_approach_200kts_gearflaps.csv`.

## 4. Known-open items

1. **Residual T ≈ 12.7 s limit cycle** (α/nz in phase, amplitude-independent,
   pstick grazing the pitch clamp). No band-aid sustains it (all ablate to no
   change); the VS slew limiter modulates amplitude (×4 slew → −43%).
   Next step: **describing-function analysis** — not another damper.
2. **Default-tune cascade remains marginally unstable** (envelope
   +0.23…+0.58 /s; wingman/BVR/WVR/missile/collision/ground-avoid fly it).
   Gain re-hunting is refused by measurement (razor-non-monotonic landscape);
   the structural fix is the **parked P4.2 TECS margin campaign**. The
   default-tune CI gate bounds regression until then.
3. **Landing precision residual**: cross-track 93–162 ft measured vs the
   <50 ft gate (Tranche A1). Tuning target: wings-level through flare +
   centerline-hold in rollout, iterated against the CSV trace. Do not relax
   the gate silently.
4. **Tranche B (taxi-back)**: not started (PLT_PARK data investigation +
   Kunsan `taxi_in_route`/`parking_spots` wiring).
5. **Approach-condition margin campaign** (upstream cross-validation):
   `fm_sysid` reproduces the upstream §6.3 open question (5.59 @ 0.40 rad/s);
   amplitude sweep resolves it as amplitude conditioning with the shipped
   q-damper destabilizing small-signal at approach config.

## 5. Cross-validated against upstream (PHUG tree)

The pole program and the upstream PHUG-PLAN program merged into one tree
(Task 66) and cross-validated: `test_p5_stability` green
(PhugoidDampingCruise, AltitudeCapture); two disabled P5 gates re-measured
with baselines pinned in test comments (SpeedHoldStep 46.23 kt overshoot /
no settle = upstream M4; approach catch-down cannot hold 160 kt at idle).
Disabled-gate baselines live in the test sources, not in docs.

## 6. Ablation matrix (what earns its place)

At **cruise**: QIL leak (+0.080 Re), q-damper (+0.061), bias feedback
(+0.048) earn their place; shedding is a no-op. At **approach** the signs
flip: leak and shedding are harmful; the (otherwise-refuted) speed damper
helps (+0.244 → +0.066). Condition-dependent patches are now measurable per
condition — `phaseE_ablation.csv` is the baseline.

## 7. Rules for future FCS work

1. No new damping term without a pole measurement before/after
   (`diag_poles` + the §3 CI gates). The 30-patch loop is the failure mode
   this program exists to prevent.
2. Tune changes ride with their measured pole delta in the header comment
   (see `air_steering.hpp` STAB-P1 for the pattern).
3. Negative results (refuted dampers) stay in the tree default-off with the
   measurement noted — they are load-bearing evidence.

## 8. Archive map

| Historical doc (now `Docs/archive/`) | What it contributed |
|---|---|
| `FLIGHT_CONTROL_STABILITY_PLAN.md` | Symptom→subsystem catalogue (§4 remains valid input) |
| `FLIGHT_CONTROL_NEXT_STEPS.md` | Experiment ledger (Exp L…U2); Phase 0b/0d observability items (folded into §1 methodology) |
| `FLIGHT_CONTROL_POLE_DIAGNOSIS_PLAN.md` | The pole methodology (trim → linearize → eigenvalues; §8.4 golden-move protocol) |
| `POLE_DIAGNOSIS_RESULTS.md` | Findings F1–F11; §7 gate-move log |
| `PLANT_IDENTIFICATION.md` | First measured plant model; configs A/B |
| `LOOP_MARGIN_REPORT.md` | §6 deliverable: loop margins |
| `BISECTION_RESULTS.md` | Bisection matrix results (supersedes configs C/D/E) |
| `LONGITUDINAL_STABILITY_PLAN.md` | **Still active** — source of §2 method and §4 items 1–2 |
| `LANDING_PRECISION_FORMATION_AAR_PLAN.md` | **Still active** — source of §4 items 3–5 |

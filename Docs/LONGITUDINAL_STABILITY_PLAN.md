# Longitudinal Stability — Control-Theoretic Diagnosis Plan

> **Status**: Active. Supersedes the *fix-sequencing* approach of
> `FLIGHT_CONTROL_NEXT_STEPS.md` §4. That document's Phase 0b/0c/0d
> observability items remain valid and are folded in here (§4, Phase 0).
> **Source of Truth**: `https://github.com/jdcrayme/F4`
> **Predecessors**: `FLIGHT_CONTROL_STABILITY_PLAN.md` (root-cause catalogue),
> `FLIGHT_CONTROL_NEXT_STEPS.md` (Exp L/QIL/C/G/Q3/V2/W/S/U2 history),
> worklog tasks DIGI-1/2, ALT-2…5, STAB-E1…E51, Tranches 31–46, NAV-A…E.
> **Scope**: longitudinal (phugoid-class oscillations, pitch/altitude/speed
> loop instabilities). The same methodology transfers to roll/yaw afterwards.

---

## 1. The Case for Control Theory (why 30+ heuristic fixes haven't closed this)

The worklog records more than thirty distinct anti-oscillation interventions:
Exp L (bias `1/cos μ`), Exp QIL (120 s integrator leak), Exp C (zero cruise
rudder), Exp G (bank-rate taper), Exp Q3 (adaptive γ-corr limit), Exp V2
(TECS energy term), Exp W (predictive speed brake), Exp U2 (alpha-rate damper),
STAB-E1…E51, Tranche 42/45/46 (speed-scheduled q-damper), NAV-E speed-brake
proportionalization, E29 VS slew limiter, E44 speed-lead damping. Each fix
moved the oscillation to a different frequency, operating point, or loop.
None eliminated the *class* of behavior.

**That is the signature of patching symptoms without a plant model.** The
current longitudinal channel contains, at time of writing:

| Category | Count | Elements |
|---|---|---|
| Integrators | 5 | FCS pitch I (w/ conditional anti-windup + 120 s leak + shedding = 3 interacting nonlinearities on one state), AirSteering alt integral (600 s leak), AirSteering speed integral (leaky), FCS yaw integral, `vs_target_` slew-limiter state |
| Damping mechanisms targeting the same mode | 5 | FCS q-damper (Tranche 42/45/46), AirSteering `pitch_rate_damp`, `alpha_rate_damp` (U2), `speed_damp_rad_per_kt` (E44), `gamma_corr` adaptive limit (Q3) |
| Feedforwards derived from the oscillating state itself | 3 | `alpha_bias` ∝ 1/qsom ∝ 1/V², `alpha_est = pitch − γ(VS)`, `bank_g_ff` |
| Nonlinear clamps/schedules in the pitch path | 8+ | aoamin/aoamax, gsAvail, ptcmd clamp, alpha clamp, `cosmu_safe`, `gamma_corr_lim_eff`, `vs_corr` window, slew limiter |
| Dynamic lags | 4 | F7Tust lead-lag (τ1/2/3 × pitchMomentum), `pitchRateLag` (tp01 × pitchElasticity), `rollRateLag`, engine rpmLag |

A loop with 5 integrators, 5 competing dampers, and 8 nonlinearities cannot be
diagnosed by adding term #16. It has to be *linearized and measured*.

## 2. The Architectural Fact That Makes This Solvable

This FDM is the FreeFalcon **khill pseudo-model**, not a moment-based 6-DOF:

1. **Body rates are kinematically commanded** — `eom.cpp:147–214`: q comes
   from `atan(nzcgs·g/V)` minus gravity/gear terms through a first-order lag;
   r comes from side force + bank kinematics; p is the FCS roll command
   verbatim. There are no pitching moments, no Iyy, no Cmq, no natural
   short-period physics.
2. **Alpha is set directly by the FCS** — `fcs.cpp:609–625` writes
   `aero.alpha` every minor step; `aero.alpha_dot` is a finite difference of
   the command. The FCS *is* the alpha actuator.
3. **γ is derived, not integrated** — `eom.cpp:312`:
   `gmma = theta − alpha·cosφ` (small-angle approximation).
4. **nzcgs is algebraic** — `flight_model.cpp:326`: `nzcgs = −zsaero/g`,
   i.e. a static function of (α, β, Mach, qbar) through the CL table.

Consequence: the longitudinal "plant" from Δα → Δnzcgs is a **static gain**
`K_nz(V) = clalph0·qsom/g` with a one-minor-step sample delay. **A static
gain cannot oscillate.** The only true dynamic states in the longitudinal
channel are vt, the attitude integrator, and the controller states
(FCS filters/integrators, AI cascade states, engine spool, actuator rates).

Therefore **every observed oscillation is a closed-loop artifact** — a property
of the FCS + AirSteering + engine loop structure, with finite, linearizable,
bounded dynamic elements. This is why it is solvable from a control-theory
standpoint: the mode set is finite, and each mode has an owner we can name.

A second consequence: **classical phugoid is not available as an excuse.** The
ideal-airplane phugoid (period `T ≈ 4.44·V/g`, damping `ζ ≈ 1/(√2·L/D)`)
arises from constant-alpha speed–altitude exchange. This model *commands
alpha to hold 1-G*, which destroys the classical mechanism. Reference numbers
for comparison (not prediction):

| Speed | Classical T = 4.44·V/g | Observed in traces |
|---|---|---|
| 160 kts (approach) | ≈ 37 s | 8–12 s (FLIGHT_CONTROL_STABILITY_PLAN §3.5) |
| 250 kts | ≈ 58 s | ~10 s (altitude hold) |
| 300 kts | ≈ 70 s | ~20 s (sustained turn, QIL) |

**None of the observed periods match the classical phugoid.** They match loop
crossover / beat frequencies of the AI cascade and FCS filters (inner-loop
ω_sp ≈ 1.7 rad/s ≈ 0.27 Hz at 250 kts; observed modes 0.05–0.125 Hz — an
order below the inner crossover, i.e. they live in the outer loops and the
speed-coupling paths). Phase 2 measures this properly; Phase 3 uses it as the
ownership discriminator.

## 3. Method Overview

```
Phase 0  Freeze + inventory          (0.5 d)  no behavior changes
Phase 1  Open-loop plant ID          (2 d)    measure what the plant actually is
Phase 2  Loop linearization + margins(3 d)    L(s), PM/GM per loop per trim point
Phase 3  Closed-loop bisection       (2 d)    which loop owns which mode
Phase 4  Redesign                    (3-5 d)  one owner per mode, margins by design
Phase 5  Regression guardrails       (1 d)    automated damping metrics in CI
```

Gate between every phase: a written report in `Docs/` with the measured
numbers. No fix merges without its before/after CSV + metrics (extends
NEXT_STEPS §8).

---

## 4. Phase 0 — Freeze & Inventory (0.5 day)

**P0.1 — Patch moratorium.** No behavior-changing merges to
`fcs.cpp`, `eom.cpp`, `air_steering.cpp`, `landing_module.cpp`,
`navigation_module.cpp` until the Phase 2 margin report lands. The next
"fix" added without a model is patch #17 and makes Phase 2's linearization
harder, not easier.

**P0.2 — Loop inventory.** §10 below is the pre-populated inventory (every
dynamic element, its loop, its file:line). Review and complete it — it is the
checklist Phase 2 linearizes.

**P0.3 — Trace completeness check.** `f4-recorder/src/fcs_trace.cpp` exists
(NEXT_STEPS Phase 0b ✅). Verify it exports ALL of: `qsom, qbar, pitchIntegral
(eintg), alpha_bias_deg, gamma_corr, vs_target_, alt_integral_, speed_integral,
throttle_cmd, rpm, thrust_lbf, tefPos, lefPos, dbrake, stallState, vtDot,
nzcgs, ptcmd, aoacmd, alpha, gamma_now, gamma_ff, theta_target, pitch_cmd,
pstab, q, omega_sp, tp01/02/03, zp01`. Any FCS/AI intermediate not in the
trace is invisible to Phase 3 bisection — add missing columns now.

**P0.4 — Spawn trim (NEXT_STEPS Phase 0d, still open).** `simulation.cpp`
spawns vt=0; the first seconds of every trace are a startup transient that
contaminates period/damping measurement. Spawn at trim (vt, alpha, theta =
alpha, engine spooled) via the existing `FlightModel::trim()` machinery
(`flight_model.cpp:480–561`), or discard a fixed 10 s warm-up window in the
metrics tool. Must be done before Phase 3 baselines.

---

## 5. Phase 1 — Open-Loop Plant Identification (2 days)

Build a standalone sysid harness (new `tools/fm_sysid/`, links
`f4-flight-model` only — no AI, no sim): run `FlightModel` with held inputs,
emit CSV, and extract parameters at four trim points:
**{250 kts clean, 300 kts clean, 450 kts clean, 160 kts gear+flaps+TEF}**.

Identify, in order of importance:

**P1.1 — Static maps (these set every loop gain).**
- `K_nz(V) = ∂nzcgs/∂alpha` — sweep alpha at fixed V; compare against
  `clalph0·qsom/g` (the FCS's own assumption, `fcs.cpp:228`). Any mismatch
  here invalidates computeGains' pole placement (see P2.1).
- `∂nzcgs/∂V` at fixed alpha — the speed→G coupling through qbar ∝ V².
  This is the *actual* phugoid-coupling gain in this architecture.
- Drag polar `D(V, alpha, config)`; locate min-drag speed per config. The
  backside region (below min-drag) has `∂D/∂V < 0` and destabilizes the
  speed mode; quantify where 160 kts gear+flaps sits.

**P1.2 — Dynamic elements.**
- Throttle → thrust: step and PRBS; fit rpmLag time constant + thrust table
  slope `∂T/∂V` (a stabilizing phugoid-damping term in real aircraft —
  measure ours, including the idle −900 lbf anomaly, worklog ALT-4).
- Commanded-G → q: verify `pitchRateLag` τ = tp01·pitchElasticity behavior
  and the `atan()` command shaping at moderate amplitudes.
- Actuator rates: TEF/LEF rate limits, speed-brake rate — these bound the
  achievable loop bandwidths in Phase 2.

**P1.3 — Discrete structure.** Document the minor-step pipeline
(`flight_model.cpp:339–430`): FCS consumes nzcgs/q from the *previous* minor
step (one-sample delay ≈ 2.8 ms at 6×60 Hz — negligible), AI runs at major
rate with internal states assuming exactly 1/60 s (`air_steering.cpp` leaky
integrals, slew limiter). Confirm the actual major rate everywhere the stack
runs (time_scale clamping, recorder, replay) so Phase 2's discrete analysis
uses true sample times.

**Deliverable**: `Docs/PLANT_IDENTIFICATION.md` — parameter table at 4 trim
points. This document is the yardstick every later margin claim is checked
against.

---

## 6. Phase 2 — Loop Linearization & Margin Analysis (3 days)

For each loop below: derive L(s) around trim from the Phase 1 parameters,
then verify numerically by injecting sinusoids (0.01–10 rad/s) at the loop's
summing junction in the sysid harness and measuring gain/phase. Report
**PM / GM / crossover per loop per trim point**. Gate: PM ≥ 45°, GM ≥ 6 dB.

**P2.1 — L0: G-error loop (the FCS core).**
Path: `error = ptcmd − (nzcgs − cosγ/cosμ − gearT)` ×kp05 → PI(kp02=1, kp03=2)
→ ×plsdamp → clamp → F7Tust lead-lag(τ1,2,3·pitchMomentum) → α → K_nz(V) → nzcgs.
Because the plant is static, this reduces to a clean SISO design problem —
the lead-lag and PI alone determine the poles. **Audit `computeGains`
(`fcs.cpp:205–360`)**: it solves for tp02/tp03 from a pole-placement algebra
built on `nzalpha`/`ttheta2` — a *plant assumption*. Check whether the assumed
plant matches the measured static gain + lag chain. If it assumed a rate-type
plant (1/s²-ish), the real closed-loop poles are elsewhere and every downstream
tune inherited the error. This is the single most likely *original sin* in the
inner loop.

**P2.2 — L1: q-damper loop (Tranche 42/45/46).**
`ptcmd −= kq·min(2, √(18/qbar))·q`, where q is itself the *lag-filtered,
command-derived* pitch rate (`eom.cpp:190`) — i.e. this routes a filtered
function of the command back into the command, *inside* L0, with base gain
up to 60 (`flight_model.cpp:210`). No margin analysis exists for this loop.
Measure its loop gain and phase at L0's crossover. A damper fed with
command-derived rate (not sensed rate from a physical mode) can only reshape
L0 — determine whether it helps or costs phase margin at the frequencies that
actually oscillate. Also note it is gated OFF gear-down / <200 ft AGL
(Tranche 46) — precisely the regime with the remaining approach instability,
so L0/L3 must be stable without it there.

**P2.3 — L2: alpha-bias speed coupling.**
`alpha_bias ∝ 1/qsom ∝ 1/V²` (`fcs.cpp:411–432`). Combined with
`V̇ = (T−D)/m − g·sinγ` and D ∝ V², derive the net speed-mode damping
contribution of the bias path. Hypothesis to check: the bias coupling
(α↓ as V↑ → L↓) partially *cancels* the natural −g·sinγ exchange and shifts
the speed mode's frequency into the 10–20 s band seen in traces. Compute
`∂alpha_bias/∂V` at each trim point from the tables; fold into the L4 MIMO
analysis.

**P2.4 — L3: gamma-hold outer loop (AirSteering).**
`alt_err → vs_corr (P + 600s-leaky I) → ±window clamp → +vs_ff → ±max_vs →
slew limiter (400 fpm/s state) → γ_ff + γ_corr (adaptive limit) →
theta_target = clamp(α_est·lift_comp + γ_ff + γ_corr − speed_damp·ΔV) →
pitch_cmd = attitude_gain·err − pitch_rate_damp·q + α_rate_damp + bank_g_ff
→ FCS (L0 inside)`.
Structural risks to quantify:
- **Two integrators in series** (FCS pitch I + alt integral) with only 5:1
  leak-ratio separation (120 s vs 600 s) — rule of thumb wants ≥10:1
  dominant-time separation or an explicit cascade design.
- **α_est positive feedback**: `α_est = pitch − γ(VS)` uses the *instantaneous*
  VS — the phugoid's own oscillating variable — inside the command; the clamp
  bounds it but clamped positive feedback still subtracts damping. Measure the
  net damping added by this path at the observed 0.1 Hz.
- γ_corr adaptive limit (Q3) is a state-dependent loop gain — verify the
  gain variation (×0.6 in climb) doesn't cross a stability boundary between
  its level and climb settings.

**P2.5 — L4: speed channel (2×2 MIMO with L3).**
Actuators: throttle (P + leaky I + TECS-energy term V2), pitch (via
`speed_damp_rad_per_kt` inside theta_target), speed brake (proportional,
E44/W). Three actuators and two controlled variables (V, γ) with cross-paths:
speed_damp puts the speed error into the pitch loop; the TECS term puts both
into throttle; the bias coupling (P2.3) puts speed into alpha. Build the 2×2
transfer matrix at each trim point and check the *relative* gain/array
stability (e.g. compute the characteristic loci or at minimum the two SISO
loops' margins with the other loop closed). The classic failure this predicts:
V-loop and γ-loop each stable alone, marginally stable together at the beat
frequency ≈ 10–20 s. This is the leading candidate owner of the "persistent
phugoid".

**P2.6 — Anti-windup nonlinearities.** The pitch integrator carries three
mechanisms (conditional integration, 120 s leak, 1.5/s shedding E51) plus
clamping. Describe-function-style check: verify the combination cannot
produce a slow limit cycle (the "20 s phugoid" QIL attributed to windup/
unwind). Deliverable: replace all three with ONE documented scheme
(back-calculation) in Phase 4 — but first *measure* which mechanism, if any,
owns a mode.

**Deliverable**: `Docs/LOOP_MARGIN_REPORT.md` — table of PM/GM/crossover per
loop × trim point, with the failing loops highlighted and the predicted mode
ownership for Phase 3.

---

## 7. Phase 3 — Closed-Loop Bisection Matrix (2 days)

Run the 60–120 s scripted scenarios at the 4 trim points with loop
configurations switched on cumulatively. All inputs held at trim where a loop
is "off". Metrics from every run via the same tool (P5.1): dominant period
(FFT/autocorrelation), damping ratio (log-decrement), amplitude envelope.

| Config | L0 | L1 (q-dmp) | L3 (alt/γ) | L4 (speed) | Question answered |
|---|---|---|---|---|---|
| A | ✔ | ✘ | ✘ | ✘ | Is the FCS core alone stable? |
| B | ✔ | ✔ | ✘ | ✘ | Does the q-damper help or hurt? |
| C | ✔ | ✔ | ✔ | ✘ | Altitude loop stability w/o speed loop |
| D | ✔ | ✔ | ✘ | ✔ | Speed loop stability w/o altitude loop |
| E | ✔ | ✔ | ✔ | ✔ | Full stack (current behavior) |

Discrimination rules:
- Oscillation first appears in **A/B** → inner loop (L0/L1): frequency should
  track L0 crossover; if it also moves with kq, the damper loop is the owner.
- Appears in **C** only → L3 (cascade integrators / α_est path).
- Appears in **D** only → L4 (throttle lag + TECS term).
- Appears only in **E** → the L3/L4 MIMO interaction (P2.5's beat-frequency
  prediction) — the expected outcome given the trace history.
- Period scales with V like 4.44·V/g → something reintroduced plant-like
  dynamics (audit any α path that is *not* command-derived — e.g. stall SM
  lift modification, ground effect entering/leaving); this should NOT happen
  and would be a significant find.

Gain-sweep root-locus by brute force: for each of
{kp03, kq, attitude_gain, vs_gain, speed_damp, bias on/off}, ±50% steps,
one-at-a-time. A mode whose frequency tracks a given gain is owned by that
gain's loop. Twelve runs per trim point; cheap at 60–120 s sim time.

**Deliverable**: `Docs/BISECTION_RESULTS.md` — mode → owner table with
period/ζ evidence per config. This is the document the redesign implements
against.

---

## 8. Phase 4 — Redesign (3–5 days)

Design principles: **one owner per mode; one integrator per controlled
variable; margins by design, verified by the P2 harness; every deleted term
replaced by a named mechanism, never by nothing.**

**P4.1 — Inner loop (L0).** Recompute tp02/tp03 (and kp05) from the *measured*
static plant so closed-loop poles land where computeGains intended
(ω from ttheta2 as today, ζ = zp01). Verify PM ≥ 45° across the envelope by
the P2 harness. Keep exactly one pitch-damping mechanism — either the
lead-lag shaping **or** the q-damper — whichever the Phase 2 margins justify;
delete the other. Delete the QIL leak + shedding + conditional-integration
trio; implement back-calculation anti-windup (one mechanism, documented).

**P4.2 — Outer longitudinal axes: converge on TECS done properly.**
The V2 "TECS-inspired energy term" is a hint of the right answer bolted into
the wrong structure. Replace the gamma-hold cascade + speed_damp + TECS-hack
+ predictive speed brake with the standard two-loop TECS:
- **Energy-rate loop**: throttle ← PI on (V̇/g + γ̇) error → total energy.
- **Energy-distribution loop**: pitch ← PI on (V − V_cmd) − (γ–γ_cmd)·V/g
  error → energy split between speed and flight path.
This is the proven architecture for exactly our failure regime — back side of
the drag curve on approach (the open defect in NEXT_STEPS §1) — because it
commands *energy* where the backside problem lives, and it decouples the V/γ
pair by construction (no beat frequency, P2.5's failure mode disappears
structurally rather than by tuning).
Delete with it: α_est positive-feedback path (P2.4), alt leaky integral
(replaced by distribution-loop integral), vs slew limiter (TECS rates are
bounded by design), speed_damp (energy split owns the speed-pitch coupling).
Keep the speed brake as a drag device with its proportional law, not as a
damping loop.

**P4.2-alt** (only if P2 shows inner-loop margins are the sole defect): keep
the cascade, retune against measured margins, and still collapse the 5 dampers
to 1 and the integrators per the principles above. Decide at the Phase 3 gate.

**P4.3 — Landing configuration.** With TECS in place, re-derive the approach
law: energy-rate holds γ_track on the glideslope, distribution loop holds
160 kts. This replaces `steer_approach()` (dead code, 57× gain bug) rather
than resurrecting it.

**P4.4 — Worked example of "delete a patch" accounting.** Every term removed
from §1's table must appear in the PR with: the mode it used to (attempt to)
own, the mechanism that now owns it, and the before/after trace pair. The §1
table shrinks from ~15 entries to a target of **≤ 6**: lead-lag (L0), one
pitch damper, throttle PI (energy rate), pitch PI (energy distribution), yaw
damper, actuator rate limits.

---

## 9. Phase 5 — Regression Guardrails (1 day)

**P5.1 — Metrics tool** (`scripts/trace_metrics.py` or a C++ tool in
`tools/`): CSV → dominant period, ζ (log-decrement), amplitude envelope,
settling time. Used by Phase 3 and by CI.

**P5.2 — CI stability tests** (gtest integration sims, fixed seed, fixed
timestep; thresholds from the Phase 3 baselines, tightened after Phase 4):
- `test_phugoid_damping_cruise` — 120 s hands-off at 250/300/450 kts:
  no growing oscillation, ζ ≥ 0.08 for any mode with T < 120 s.
- `test_speed_hold_step` — ±25 kt throttle step: settle < 30 s, overshoot
  < 15%.
- `test_altitude_capture` — 1,000 ft capture: overshoot < 150 ft, no
  re-excited speed oscillation > 10 kt.
- `test_approach_vs_tracking` — glideslope track at 160 kts gear+flaps:
  VS within ±300 fpm of beam command, speed within ±10 kt.
- All at 1× and 4× time_scale (NEXT_STEPS §5 rule).

---

## 10. Pre-Populated Loop Inventory (the linearization map)

| # | Element | Loop | Type | Location |
|---|---|---|---|---|
| 1 | G-error PI (kp02=1, kp03=2) | L0 | I | `fcs.cpp:524–588` |
| 2 | Conditional anti-windup | L0 | NL | `fcs.cpp:534–553` |
| 3 | 120 s integrator leak (QIL) | L0 | I mod | `fcs.cpp:555–567` |
| 4 | Integrator shedding (E51) | L0 | NL | `fcs.cpp:569–588` |
| 5 | F7Tust lead-lag (τ·pitchMomentum) | L0 | lag | `fcs.cpp:597–603` |
| 6 | alpha_bias feedforward ∝ 1/qsom | L2 | ff/V² | `fcs.cpp:380–432` |
| 7 | q-damper, speed-scheduled (T42/45/46) | L1 | fb | `fcs.cpp:501–520`, gain at `flight_model.cpp:210` |
| 8 | pitchRateLag (tp01·elasticity) | plant | lag | `eom.cpp:187–190` |
| 9 | γ = θ − α·cosφ | plant | kinematic | `eom.cpp:308–314` |
| 10 | −g·sinγ in V̇ | plant | coupling | `eom.cpp:343` |
| 11 | engine rpmLag + thrust tables | L4 | lag | `engine.cpp` |
| 12 | alt_err → vs_corr P + 600 s I | L3 | I | `air_steering.cpp:135–146` |
| 13 | vs_corr window clamp (E10) | L3 | NL | `air_steering.cpp:147–153` |
| 14 | vs slew limiter state (E29) | L3 | state | `air_steering.cpp:163–173` |
| 15 | γ_corr adaptive limit (Q3) | L3 | sched | `air_steering.cpp:186–189` |
| 16 | α_est = pitch − γ(VS) | L3 | ff (pos. fb risk) | `air_steering.cpp:175–184` |
| 17 | speed_damp into theta_target (E44) | L3↔L4 | x-couple | `air_steering.cpp:197` |
| 18 | α_rate_damp (U2) | L3 | fb | `air_steering.cpp:223–231` |
| 19 | pitch_rate_damp + bank_g_ff | L3 | fb/ff | `air_steering.cpp:233–238` |
| 20 | speed P + leaky I | L4 | I | `air_steering.cpp` (speed block) |
| 21 | TECS energy term (V2) | L4 | fb | `air_steering.cpp` (throttle block) |
| 22 | speed brake proportional law | L4 | fb | landing/nav modules |
| 23 | minor-step one-sample FCS delay | all | delay | `flight_model.cpp:339–430` |

(Verify line numbers when working — they will drift; the element *names* are
the durable key.)

## 11. Risks & mitigations

| Risk | Mitigation |
|---|---|
| computeGains audit finds the pole-placement algebra was correct and margins are fine | Then Phase 3's bisection output *is* the answer; proceed to P4.2-alt with measured numbers |
| Static-map ID exposes .dat conversion defects (clalph0 conventions, thrustIdle −900 lbf, clFactor history) | File as separate data-pipeline fixes; they change K_nz, so re-run Phase 1 after they land |
| TECS rewrite regresses takeoff/ground regimes | Keep the existing ground guards (ground_guard, nose-wheel authority) untouched; TECS activates on `inAir` only; run the full isolated-scenario suite (NEXT_STEPS Phase 0c) before merge |
| Trace exporter gaps discovered mid-Phase-3 | P0.3 audit front-loads this; exporter columns are additive and safe |
| Moratorium pressure ("just one more gain tweak") | The moratorium has a hard exit: the Phase 2 report. Anything merged before it restarts §1's accretion problem |

## 12. Schedule

| Phase | Effort | Exit gate |
|---|---|---|
| 0 Freeze & inventory | 0.5 d | inventory reviewed, trace columns complete, spawn trim or warm-up rule |
| 1 Plant ID | 2 d | `PLANT_IDENTIFICATION.md` (4 trim points) |
| 2 Margins | 3 d | `LOOP_MARGIN_REPORT.md` (PM/GM per loop × trim point) |
| 3 Bisection | 2 d | `BISECTION_RESULTS.md` (mode → owner) |
| 4 Redesign | 3–5 d | patch table ≤ 6 entries, margins verified |
| 5 Guardrails | 1 d | CI stability tests green at 1× and 4× |

**Total: ~11–13 days, first decisive evidence (mode ownership) by day 5–6.**

The likely end state, predicted but to be confirmed by measurement: the
persistent oscillation is the L3/L4 beat mode (two coupled loops, two
integrator pairs, comparable time constants), compounded by an inner loop
whose pole-placement algebra assumes a plant different from the static gain
it actually drives. Both are exactly the kind of defect that survives thirty
heuristic fixes and falls to one honest Bode plot.

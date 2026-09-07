# Flight Control Instability — Pole-Based Diagnosis Plan

> **Status**: Proposed. Supersedes the *methodology* of
> `FLIGHT_CONTROL_STABILITY_PLAN.md` and `FLIGHT_CONTROL_NEXT_STEPS.md`
> (their symptom catalogues remain valid input); replaces patch-accumulation
> with model-based control design.
> **Source of Truth**: `https://github.com/jdcrayme/F4`
> **Predecessors**: `FLIGHT_CONTROL_STABILITY_PLAN.md` (symptom→subsystem map),
> `FLIGHT_CONTROL_NEXT_STEPS.md` (experiment ledger), worklog tasks DIGI-1/2,
> ALT-2..5, Tranches 42–46, STAB-E1..E51, Experiments A–Z.
> **Tooling already in place**: `f4-flight-model/tests/diag_trajectory.cpp`
> (trimmed-F16 harness), `f4-recorder` CSV trace, `scripts/trace_window.py`,
> FCS HUD overlay.

---

## Table of Contents

- [0. Thesis — why this is solvable, and what the oscillation actually is](#0-thesis)
- [1. The system under test, stated as a control problem](#1-the-system-under-test)
- [2. Phase A — Analysis harness: trim, linearize, eigenvalue](#2-phase-a)
- [3. Phase B — Trim database and the speed-stability map](#3-phase-b)
- [4. Phase C — Loop-at-a-time closure (numerical root locus) + margins](#4-phase-c)
- [5. Phase D — Mode discrimination from existing traces](#5-phase-d)
- [6. Phase E — Patch ablation matrix](#6-phase-e)
- [7. Phase F — Synthesis directions (post-diagnosis)](#7-phase-f)
- [8. Acceptance criteria and CI regression](#8-acceptance)
- [9. Math appendix — derivations and predicted numbers](#9-appendix)

---

<a name="0-thesis"></a>
## 0. Thesis — why this is solvable, and what the oscillation actually is

Three facts about this codebase change the whole diagnosis:

**Fact 1 — There is no bare-airframe phugoid to diagnose.** This is the
Falcon-style kinematic model, not a moment-driven 6-DOF:

- `alpha` is not an aerodynamic state — the FCS **writes it directly**
  (`fcs.cpp:609–625`: `aero.alpha = clamp(alpha_bias + filtered_PI)`).
- `q` is a lag-filtered function of *commanded* G, not of pitching moments
  (`eom.cpp:147–214`: `qptchc = atan(nz·g/V) − atan(...) − atan(cosμ·cosγ·g/V)`,
  then `pitchRateLag`).
- `p` is the FCS roll-rate command; `r` is algebraic; `γ = θ − α·cosφ` is an
  approximation (`eom.cpp:308–314`); the only "force" integrations are
  `vṫ = xwaero − g·sinγ` (`eom.cpp:335–360`) and position.

A conventional F-16 has a short period (≈2–4 rad/s) and a lightly damped
phugoid (ω ≈ g√2/V ≈ 0.1–0.2 rad/s) as its bare-airframe longitudinal modes.
**This plant has neither.** Any sustained oscillation we see — including
everything we have been calling "phugoid" — is a **closed-loop mode** created
by the cascade of: FCS G-command PI → kinematic (V, γ) dynamics → AI outer
loops → back. That is good news: closed-loop modes are exactly what control
theory diagnoses (eigenvalues of the linearized cascade), and exactly what we
can move once located.

**Fact 2 — The FCS's own trim architecture deletes the natural damping, then
the patches re-add it by hand.** The 1-G alpha bias
(`fcs.cpp:400–432`) commands `α_bias ∝ g·cosγ/(cosμ·qsom) ∝ 1/V²`, i.e. it
**pins lift to weight at every speed**. The classical phugoid restoring /
damping mechanism (speed↑ → lift↑ → γ̇↑, and drag↑ → V̇↓ — see §9.1) is
thereby removed from the plant and replaced by a lagged feedback path
(bias → α → L → γ, through the PI integrator, the F7Tust lead-lag, and the
pitch-rate lag). What is left is a near-marginal energy oscillator
(`V̇ ≈ −g·γ`, `γ̇ ≈ small, lagged`) whose damping is decided by **the phase of
the filter chain at the oscillation frequency** — not by aerodynamics. Every
lag or integrator we add (Tranche 42 q-damper, QIL 120 s leak, STAB-E51
shedding, AI leaky integrals, slew limiters) inserts another pole into exactly
this band.

**Fact 3 — The approach configuration is special: back side of the drag
curve.** `FLIGHT_CONTROL_NEXT_STEPS.md` already states it: at approach speed
the F-16 is on the back side of the drag curve (`dD/dV < 0`). §9.2 shows that
for a G/lift-held aircraft this makes the speed–flightpath mode **divergent**
(ζ flips sign with `S_u = d(T−D)/dV`). The persistent ±1500–3000 fpm final-
approach oscillation is the signature of an unstabilized speed mode being
fought by a P-dominant throttle and by pitch (the wrong actuator for that
mode).

**Consequence:** the "persistent phugoid and other control instabilities" is
a finite set of identifiable closed-loop poles in a deterministic, fixed-step,
piecewise-smooth plant. We can compute them. The plan below does exactly
that: trim → linearize → eigenvalues → loop-at-a-time root locus → margins →
ablation → compensate. No more gain-tuning by trace-eyeballing.

---

<a name="1-the-system-under-test"></a>
## 1. The system under test, stated as a control problem

### 1.1 Plant definition

For analysis purposes the **plant P** is one `FlightModel::update(dt)` major
frame (6 minor frames of 1/360 s each, `flight_model.hpp:242`,
`flight_model.cpp:470–473`) — FCS, aero, engine, and EOM included. Inputs and
outputs:

- **Inputs** u = {pstick, rstick, ypedal, throttle, tefCmd, lefCmd,
  speedBrake, gearHandle} (the `PilotInput` boundary — this is where the AI
  connects).
- **States** x (the ones that matter dynamically — every one is a potential
  pole):

  | Group | States | Where |
  |---|---|---|
  | Kinematic | vt, θ, φ, ψ, (γ, σ derived), x, y, z | `eom.cpp` |
  | Command shaping | `pitchIntegral` (eintg), `pitchAlphaLag` (y,u hist), `pitchRateLag`, roll lag | `aircraft_state.hpp:220–224` |
  | FCS extras | integrator leak state (implicit in eintg), stall latch `stallState` | `fcs.cpp:545–588`, `stall_state.cpp` |
  | Engine | rpm lag (spool), throttle dynamics | `engine.cpp` |
  | Discrete config | gearPos, tefPos, lefPos, dbrake (rate-limited) | `flight_model.cpp:420–433` |

- **Outputs** y = {nzcgs, alpha, beta, vt, vcas, q, γ, h, vs, …} — everything
  the AI reads via `AirSteering::Input`.

### 1.2 Controller definition (the loops around P)

1. **AI outer — altitude/γ**: alt_err → (P + leaky I, 1/600/tick ≈ 10 s) →
   VS cmd (slew-limited 400 fpm/s) → γ_cmd → θ_cmd = γ_cmd + α_est·(1/cosφ)
   → pstick (`air_steering.cpp:118–210`).
2. **AI outer — speed**: (P + leaky I + TECS-style energy term + speed_damp)
   → throttle (`air_steering.cpp`, Experiments V2/W).
3. **FCS inner — G/α**: G error (with gravity baseline cosγ/cosμ and gear
   term) → PI (kp02=1, kp03=2) → clamp → F7Tust lead-lag (tp01/02/03 from
   scheduled pole placement) → α = α_bias + filtered PI (`fcs.cpp:435–626`).
   Plus the parallel path: **α_bias feedback of sensed γ and qsom**
   (`fcs.cpp:426`), which is a *second controller fighting the first for the
   same actuator*.
4. **FCS dampers, gated**: q-damper `ptcmd -= k_q·q`, **off when gear down or
   AGL < 200 ft** (`fcs.cpp:511–520`) — i.e. off precisely on approach, where
   Fact 3 says the plant is worst.

Bandwidth stacking (why the modes mix): ω_phugoid-band ≈ 0.1–0.2 rad/s;
AI altitude loop with 400 fpm/s slew and ±2000 fpm authority responds in
5–7 s ≈ 0.15–0.2 rad/s; FCS integrator-leak pole ≈ 1/120 s ≈ 0.008 rad/s;
AI alt-integral leak ≈ 0.1 rad/s. **The AI outer loop, the phugoid band, and
two leak poles all sit on top of each other.** The classic separation rule
(outer ≤ inner/3) is violated by construction; that is the architectural
smell the eigenvalues will confirm.

### 1.3 Known hard nonlinearities that sustain limit cycles

These cannot be removed by linear analysis but must be catalogued because
they convert a linearly-unstable/marginal mode into a *persistent* limit
cycle:

- Integrator clamp + conditional integration + leak + shedding
  (`fcs.cpp:534–588`) — four interacting mechanisms on one state.
- Stall hysteresis latch (`aerodynamics.cpp:209–228`) — deliberate, but it is
  a relay element.
- Ground clamp step release (`eom.cpp:383–396`, `eom.cpp:80–125`).
- `reset()` calls on `pitchIntegral`/`pitchAlphaLag` at mode transitions
  (`flight_model.cpp:494–495, 553–554`) — step injections.
- Gear/AGL gating of the q-damper — a *switched* damper (linear-gain jump
  every approach).

---

<a name="2-phase-a"></a>
## 2. Phase A — Analysis harness: trim, linearize, eigenvalue (~2 days)

Goal: a CLI tool, `tools/pole_tool/` (or `tests/diag_poles.cpp` following the
`diag_trajectory.cpp` pattern), that for any flight condition prints the
eigenvalues of the linearized plant and of each loop closure.

**A1. State injection.** `FlightModel::state()` already returns a mutable
`AircraftState&` (`flight_model.hpp:94`) and `trim()` exists
(`flight_model.hpp:119`). Extend `trim()` (or add `trimTo(vt, alt, γ, φ,
config)`) to solve for (α, throttle, θ) that null the *major-frame* residuals
{vṫ, q, γ̇, θ̇-mean, ḣ}: Newton/Gauss–Newton on the residual vector, using
the existing 1-G scan as the initial guess (`flight_model.cpp:150–175`).
Report the residual norm — **a trim that doesn't null residuals guarantees
drift and pollutes every later measurement.**

**A2. Linearization.** Central finite differences of the major-frame map
x[k+1] = F(x[k], u[k]) about trim: perturb each state by ±δ (relative:
1e-4 for continuous states; 1e-3 rad for angles; integrator/filter history
states included — they are members of `FcsState` and reachable). Produce
A_d (n×n), B_d. With n ≈ 15–20 this is ~40 sim-runs per condition — cheap.

**A3. Eigenvalues.** Add Eigen (header-only) to `third_party/`, or a 40-line
Hessenberg-QR for real nonsymmetric matrices. Output per condition:
eigenvalue, damping ζ, frequency, and a participation label (which state
dominates each eigenvector — this is what turns "a pole" into "the
alpha-bias pole" / "the alt-integral pole").

**A4. Golden test.** `test_pole_tool_smoke`: linearize, verify the
dominant-eigenvalue set is invariant under dt-halving (proves the FD
step size is small enough), and verify F(x_trim) ≈ x_trim to 1e-6.

Deliverable: `pole_tool --cond "alt=15000,kcas=250,cfg=clean" --json` and a
human-readable table.

---

<a name="3-phase-b"></a>
## 3. Phase B — Trim database and the speed-stability map (~2 days)

Sweep and **map the speed mode before touching any gain**:

- Conditions: alt {SL, 5k, 15k, 30k} × KCAS {145, 180, 200, 250, 300, 350,
  450} × config {clean, gear, gear+TEF/LEF} × weight {0.5, 1.0 fuel} ×
  γ {0, ±3°}.
- Per point, report:
  1. **Trim residual norm** (reject points that can't trim — that itself is
     a finding: e.g. approach config may have no steady-state solution at
     idle-like throttle, which would explain porpoising around a
     non-existent equilibrium).
  2. **Speed stability** S_u = ∂(T−D)/∂V (ft/s per ft/s, evaluated as
     thrust sensitivity at trim throttle + finite-difference of drag).
  3. The two "speed-mode" eigenvalues of A_d.

**Pre-registered predictions** (falsifiable by the sweep):

- P-B1: clean/cruise → S_u < 0, speed-mode pair stable but lightly damped
  (ζ ≈ |S_u|·V/(2√2·W), likely 0.02–0.1 — see §9.2).
- P-B2: gear+flaps at 145 KCAS → S_u > 0 → **the speed pair is in the RHP**.
  This single result confirms the back-side hypothesis and explains why the
  approach is uncontrollable by pitch alone.
- P-B3: with the α-bias active, the *unforced* natural phugoid damping is
  near zero in ALL conditions (the bias pins L=W), so every condition's
  damping comes from the loop path — confirming Fact 2. (Compare A_d with
  α-bias vs. α-bias frozen at trim: the frozen variant should show the
  textbook front-side damping; the live variant should not.)

Deliverable: heat map `real(speed-mode eigenvalue)` over (KCAS × config);
the RHP region boundary is the "no-go envelope" for the current architecture.

---

<a name="4-phase-c"></a>
## 4. Phase C — Loop-at-a-time closure (numerical root locus) + margins (~3 days)

The plant is already nested; we don't add loops, we **break them one at a
time** and re-linearize with the remaining loop's gain scaled by k ∈ [0, 2].
The loop whose gain sweep drags the slow pair into the RHP is the culprit;
the k at which it crosses is its stability margin. Break points:

| Loop | Break at | Sweep |
|---|---|---|
| L1 AI pitch (γ/altitude hold) | `pstick` | scale vs_gain, alt-integral gain |
| L2 AI throttle (speed/energy) | `throttle` | scale P, then add I (see F-2) |
| L3 FCS α-bias feedback of sensed γ,qsom | `fcs.cpp:426` | replace cosγ, qsom by their trim constants (freeze) — the difference between live and frozen A_d is the bias loop's contribution |
| L4 FCS q-damper | `fcs.cpp:519` | scale gain; sweep gate (on/off) |
| L5 AI integrators/leaks | `alt_integral_`, speed I, QIL leak, shedding | toggle each; log pole movement |

Per condition and per loop configuration, record: all eigenvalues, the
worst damping ratio below 1 rad/s, and gain margin k*. Two outputs matter:

1. **Root-locus plots** (pole trajectories vs k) for L1, L2 at the three
   canonical conditions (cruise-clean, approach-gear, climb).
2. **Empirical Bode cross-check**: at each break point, drive the nonlinear
   sim with a logarithmic sine sweep (0.005–2 Hz, amplitude small enough to
   stay linear, e.g. ±0.02 pstick), record the broken-loop return signal,
   and estimate the loop transfer function by FFT correlation. Compute gain/
   phase margins and compare with the pole-derived k*. Agreement validates
   the linearization; disagreement flags a dominant nonlinearity (the
   integrator clamps are the prime suspect).

**Pre-registered predictions:**

- P-C1 (L3): freezing the α-bias's γ/qsom feedback moves the slow pair
  substantially — the bias feedback is a parallel controller with
  integrator+lag phase lag, and at 0.1–0.2 rad/s it is the phase-lag
  largest contributor. If confirmed: the bias must be fed from *commanded*
  (not sensed) γ, or folded into the G-loop, see F-1.
- P-C2 (L2): P-only throttle (with rpm lag ≈ 1–3 s) has phase margin < 30°
  at approach condition because the plant's speed pole is already RHP
  (P-B2); adding integral action and raising bandwidth is what stabilizes
  it — the TECS energy term was a step in this direction but is un-verified
  by any margin measurement.
- P-C3 (L4): enabling the q-damper in the gated regimes (gear down, AGL<200)
  improves approach damping; the gates exist for flare/takeoff reasons and
  should become gain-scheduled instead of binary.

---

<a name="5-phase-d"></a>
## 5. Phase D — Mode discrimination from existing traces (~1 day)

Before/while Phases A–C run, classify what the *existing* CSVs actually
contain, so the model work targets the observed frequency. Extend
`scripts/trace_window.py` with `trace_modes.py`: per mission phase, Welch
PSD of {vs_fpm, vcas, alt, alpha, nzcgs, pstick} + pairwise phase
(cross-spectrum) at the dominant frequency.

Discrimination table (frequency and phase identify the mode uniquely):

| Candidate mode | Period @cond | Signatures in trace |
|---|---|---|
| Bare-plant speed mode (phugoid-like) | 0.138·V_tas s → ~34 s @145 kt, ~58 s @250 kt clean | α ≈ const, nz ≈ const, V and h anti-phase |
| FCS-integrator/leak mode (the QIL "20 s" mode) | 10–30 s | α and nz oscillate in phase; eintg visibly winding/unwinding |
| AI altitude-hold mode | set by vs_gain/slew ≈ 5–15 s | pstick leads vs by ~90°; alt_integral visible |
| Back-side divergence (aperiodic) | no clean period | V monotonic drift + γ drift until a clamp catches → sawtooth in vs |
| Stall-latch relay cycle | irregular | α near criticalAOA; nz square-ish steps |

Cross-reference the measured dominant frequency against the eigenvalue
predictions from Phase C. **A matched frequency + phase signature closes the
diagnosis**: the observed instability is then *assigned* to a pole, and the
fix targets that pole.

---

<a name="6-phase-e"></a>
## 6. Phase E — Patch ablation matrix (~2 days)

Every damper/leak/shedding patch added since ALT-2 is a state in the loop.
Now that poles are measurable, re-admit each patch only if it buys pole
margin:

| Patch | Ablation test | Keep if |
|---|---|---|
| QIL integrator leak (120 s) | off vs on, all conditions | worst ζ improves ≥ 0.02 with no trim-drift regression |
| STAB-E51 shedding | off vs on | improves worst ζ or removes a limit cycle |
| Tranche 42–46 q-damper + gates | gates removed (always-on, gain-scheduled) vs current | approach-condition ζ improves |
| STAB-E7/E10/E29 AI leak, window, slew | each toggled | each earns its pole |
| Exp V2 TECS energy term / Exp W speed brake | toggled | margin at L2 improves |
| α-bias after filter (ALT-5) | bias before vs after (already measured once) | confirm with eigenvalues, not just trace range |
| Stall hysteresis margins (E2) | ±margins varied | latch chatter gone without re-adding ballistic mode |

Method per row: re-trim → linearize → eigenvalues → 60 s time-domain run on
`diag_trajectory` scenarios → log all four. One patch per commit, worklog
entry each.

**Expected outcome:** several patches become no-ops once the *architectural*
fixes below land (a damper that compensates a self-inflicted phase lag).
Deleting them reduces the state count and widens margins — this is how we
stop the whack-a-mole.

---

<a name="7-phase-f"></a>
## 7. Phase F — Synthesis directions (post-diagnosis, sketch only)

Do **not** start these until Phases B–D name the guilty loops. The likely
architecture fixes, in dependency order:

- **F-1. Single trim authority.** The 1-G α-bias and the G-error PI are two
  controllers on one actuator. Either (a) drop the sensed-state bias and let
  the PI (which has integral action) own trim, keeping a *feedforward* from
  commanded nz only; or (b) keep the bias but compute it from commanded γ and
  scheduled qsom (no sensed feedback, no new loop). Option (a) is the
  textbook FLCS structure (G-command + integrator trim); the bias exists
  today to make pstick=0 exactly trim, which the PI integral achieves anyway
  once windup is handled properly.
- **F-2. Stabilize the speed mode with the correct actuator: throttle.** The
  speed–γ mode's damping term lives in V̇ (§9.2); only thrust (or drag) enters
  V̇. Add a proper PI autothrottle on speed (or full TECS: throttle → total
  energy rate, pitch → energy *distribution*, with the classic cross-feeds).
  This is the fix for P-B2; pitch-for-speed (`steer_approach`) fights the
  mode with 90° of phase lag and cannot add damping.
- **F-3. Bandwidth separation.** After F-2, re-tune the AI altitude loop to
  ω ≤ ω_speed-loop/3 and the FCS q-damper to sit above both. Replace binary
  gates (gear, AGL 200 ft) with gain schedules (function of qbar, config).
- **F-4. Sample-rate hygiene.** The AI integrators/leaks/slews hardcode 1/60
  s (`air_steering.cpp` comments). Pass actual dt; verify eigenvalues at
  30/60/120 Hz major rates. The FF-faithful `LeadFilter` has DC gain 0 by
  construction (`filters.hpp:90–104`) — audit no active path relies on lead
  DC gain.
- **F-5. Anti-windup hygiene.** Replace leak+shedding+clamp with one standard
  scheme (back-calculation or conditional integration with proper tracking
  mode), and remove `reset()` calls from mode transitions (use tracking-mode
  bumpless transfer instead) — each `reset()` is a step input into the loop.

---

<a name="8-acceptance"></a>
## 8. Acceptance criteria and CI regression

1. **Envelope pole map**: at every Phase-B trim point, closed loop
   (AI + FCS + plant) has **no eigenvalue with Re > 0**, and every mode
   below 1 rad/s has ζ ≥ 0.15 (cruise) / ζ ≥ 0.10 (approach config,
   gear+flaps, idle-ish throttle). The RHP approach-config region from P-B2
   must be gone with the throttle loop closed.
2. **Margins**: L1 and L2 gain margin ≥ 6 dB, phase margin ≥ 45° at the
   three canonical conditions.
3. **Time domain** (inherits FLIGHT_CONTROL_STABILITY_PLAN §6): vs within
   ±300 fpm of beam on final; alt within ±100 ft enroute; no `rstick` sign
   reversals in steady state; touchdown bounds — unchanged.
4. **CI golden-pole test**: `test_poles_envelope` runs `pole_tool` over the
   trim grid and asserts the acceptance-1 conditions; a PR that moves any
   pole more than 5% must update the golden file *and* the worklog entry
   explains why. This is the machine-checkable version of "don't destabilize
   what you didn't mean to touch."

---

<a name="9-appendix"></a>
## 9. Math appendix — derivations and predicted numbers

### 9.1 Why this architecture's "phugoid" is a loop property

Minimal longitudinal model of the kinematic plant with α as an input
(states ΔV, Δγ; trim L=W, small γ):

    ΔV̇ = (S_u/m)·ΔV − g·Δγ          S_u = ∂(T−D)/∂V
    Δγ̇ = (L_V/(mV))·ΔV = (2g/V²)·ΔV   (α frozen)

    A = [[S_u/m, −g], [2g/V², 0]]
    λ² − (S_u/m)·λ + 2g²/V² = 0
    ω_n = g·√2/V          (the textbook phugoid frequency)
    ζ   = −S_u·V/(2·√2·W)  (W = mg)

Two consequences:

1. With α frozen, the restoring term `2g/V²` is *always* positive, so
   stability is decided **entirely by the sign of S_u** — the speed
   stability. Jet at fixed throttle: S_u ≈ −dD/dV. Front side (dD/dV > 0):
   stable, ζ ≈ (D/L)/(2√2)·(L/D ratio terms) ≈ 0.03–0.1 — the classic
   lightly damped phugoid. **Back side (dD/dV < 0, i.e. approach with gear +
   flaps below min-drag speed): S_u > 0 → the pair crosses into the RHP.**
   Predicted numbers, F-16 at 145 KCAS / SL, gear+TEF: drag ≈ 0.10–0.13
   around V_md≈180–200 KCAS-equivalent; a back-side slope dD/dV ≈ −0.3·(D/V)
   with W/S ≈ 43 lb/ft² gives ζ ≈ −0.05…−0.15 → time-to-double ≈ 8–25 s.
   That matches the final-approach sawtooth period and growth in the traces.
2. But α is not frozen here — `α_bias ∝ g·cosγ/(cosμ·qsom)` makes
   CL·qsom ≈ W at every V, so L_V·ΔV is (nominally) **cancelled** by the
   bias's Δα, deleting the `2g/V²` restoring term and the D_V·ΔV damping
   term alike. What remains is `ΔV̇ ≈ −g·Δγ` with Δγ̇ supplied only through
   the lag chain (PI integrator → F7Tust → pitch-rate lag ≈ 0.2–0.5 s each)
   — a marginally oscillatory loop whose damping is set by filter phase,
   i.e. by whichever patch happens to add phase lead. **This is why damping
   keeps having to be re-added by hand (q-damper, speed_damp, slew limits)
   and why the phugoid keeps returning: the architecture removed the
   physical damper.**

### 9.2 The bandwidth-stack check (why the AI loop can't fix it)

Phugoid band at approach: ω ≈ 0.19 rad/s. Engine spool lag τ≈1–3 s adds
phase lag atan(ω·τ) ≈ 11–29° at that frequency; the FCS PI + F7Tust + rate
lag add ~30–60° more; the AI's own integrator and slew add the rest. A
P-dominant throttle or pitch path closing a loop around an already-RHP
speed mode with 60–90° of lag **cannot add net damping** — it can only
rescale the oscillation. This is a theorem-shaped statement (negative
stability margin), which is exactly what the Phase-C Bode run will quantify.

### 9.3 Numbers to sanity-check the tool

| Quantity | Formula | Check value |
|---|---|---|
| Phugoid ω (clean, α frozen) | g√2/V | @250 kt TAS (422 ft/s): 0.108 rad/s, T≈58 s |
| Phugoid ζ (clean) | −S_u·V/(2√2·W) | ≈ 0.03–0.10 |
| Approach speed-mode | same, S_u>0 | Re ≈ +0.02…+0.08 s⁻¹ (divergent) |
| FCS G-loop band | ωsp = 1/(ttheta2·0.65), ttheta2 = V/(g·nzα) | ≈ 2–4 rad/s |
| Integrator-leak pole | 1/120 s | 0.0083 rad/s |
| AI alt-integral leak | (1/600)/tick @60 Hz | 0.1 rad/s |

---

## Execution order and effort

| Phase | Days | Blocks |
|---|---|---|
| A — harness | 2 | — |
| B — speed-stability map | 2 | A |
| C — loop closure + margins | 3 | A, B |
| D — trace mode ID | 1 | (parallel with B) |
| E — patch ablation | 2 | C |
| F — synthesis | 5–8 | B–D results |

Total diagnosis (A–E): ~8 working days, of which the first *decision point*
is the end of Phase B (day 4): if P-B2 confirms (approach-config speed mode
in the RHP), the architecture direction is already decided (F-2 autothrottle/
TECS) and Phases C–E refine rather than explore.

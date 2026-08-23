# Flight Control Stability — Diagnostic & Fix Plan

> **Status**: Active diagnostic plan, picks up where DIGI-1/DIGI-2 left off.
> **Source of Truth**: `https://github.com/jdcrayme/F4`
> **Companions**: [AI Implementation Plan](AI_IMPLEMENTATION_PLAN.md), [DIGI AI Phase 2 Plan](DIGI_AI_PHASE2_PLAN.md) §347–348, [Architecture Proposal](ARCHITECTURE%20PROPOSAL.md) §12
> **Predecessor Lessons**: `worklog.md` Task DIGI-1 (airspeed-rotated gamma-hold law, att-cmd rotation), DIGI-2 (NED→ENU quaternion fix), DIGI_AI_PHASE2_PLAN §10 (trace summary/anomaly workflow)

---

## Table of Contents

- [1. Problem Statement](#1-problem-statement)
- [2. Symptom → Subsystem Map](#2-symptom--subsystem-map)
- [3. Diagnostic Methodology — Observe Before You Fix](#3-diagnostic-methodology--observe-before-you-fix)
- [4. Root-Cause Catalogue](#4-root-cause-catalogue)
  - [4.1 Roll flutter during turns](#41-roll-flutter-during-turns)
  - [4.2 Altitude oscillation (phugoid)](#42-altitude-oscillation-phugoid)
  - [4.3 Ground-track error (poor runway alignment)](#43-ground-track-error-poor-runway-alignment)
  - [4.4 Landing outside the runway](#44-landing-outside-the-runway)
  - [4.5 Cross-cutting observability gaps](#45-cross-cutting-observability-gaps)
- [5. Phased Fix Plan](#5-phased-fix-plan)
- [6. Acceptance Criteria](#6-acceptance-criteria)
- [7. Risks & Mitigations](#7-risks--mitigations)

---

## 1. Problem Statement

The F-16 in the `digi_full_mission` takeoff-and-landing scenario exhibits four
distinct instability symptoms. Each is severe enough on its own to fail the
mission; together they make the scenario unwatchable.

1. **Roll flutter during turns** — bank oscillates ±10–15° at ~0.3–0.5 Hz
   during the FlyOut climb, the navigation turns, and the pattern turns.
   The oscillation does not decay.
2. **Altitude oscillation** — vertical speed swings ±1500–3000 fpm on final
   approach and during climb-out; altitude never settles within ±200 ft
   of target.
3. **Ground-track error** — aircraft drifts off the runway centerline during
   the takeoff roll, fails to establish on the localizer during the
   intercept, and lands well off-centerline.
4. **Landing outside the runway** — touchdown point is 1000–4000 ft short
   or long of the threshold and/or 100–500 ft off the centerline, depending
   on which instability dominates the final approach.

The full mission test `DigiMission.FullLoopTaxiTakeoffNavigateApproachLandParks`
currently passes, but its tolerances are loose (lateral within `establish_lateral_ft = 500 ft`,
touchdown anywhere on the runway), so it does not catch these symptoms.

---

## 2. Symptom → Subsystem Map

Each symptom is unlikely to have a single cause; the integration is a
feedback loop (AI → PilotInput → FCS → EOM → AircraftState → AI). The map
below lists the subsystems whose dynamics plausibly contribute to each
symptom, with the specific code locations investigated in §4.

| Symptom | AI layer | FCS layer | EOM layer | Integration |
|---------|----------|-----------|-----------|-------------|
| Roll flutter | `air_steering.cpp:44` (bank cascade, no beta damping); `takeoff_module.cpp:477` (FlyOut heading hold, no damping) | `fcs.cpp:514–556` (yaw stubbed, beta forced to 0); `fcs.cpp:478–481` (roll rate fades below 220 kts) | `eom.cpp:80–125` (ground clamp step-releases k.p and k.r) | `brain_component.hpp:224–237` (AI → PilotInput mapping) |
| Altitude oscillation | `air_steering.cpp:53–68` (gamma-hold, `speed_damp_rad_per_kt` term); `landing_module.cpp:45` (speed_damp reduced to 0.0008 on final) | `fcs.cpp:421–427` (pitch integrator reset on alpha saturation) | `eom.cpp:80–125` (ground clamp fires on terrain step) | `simulation.cpp:474` vs `simulation.cpp:484–497` (brain reads AGL 1 tick stale vs FM ground clamp); `player_app.cpp:251–254` (time_scale up to 16×) |
| Ground-track error | `landing_module.cpp:364–368` (localizer saturates at 333 ft xtrack); `takeoff_module.cpp:445–447` (takeoff yaw via pedal, fades above 150 ft/s) | `fcs.cpp:514–556` (yaw stubbed; rudder pedals have no airborne effect) | `eom.cpp:92–100` (nose-wheel steer rate fades to 5°/s above 150 ft/s) | — |
| Landing outside runway | `landing_module.cpp:649–659` (flare law, no energy management); `landing_module.hpp:222` (missed-approach window 4000 ft) | — | — | `brain_component.hpp:224–237` (TEF/LEF never commanded, flaps always retracted) |

---

## 3. Diagnostic Methodology — Observe Before You Fix

The four symptoms are coupled through the same control loop. Changing
gains to fix one symptom can amplify another (the worklog at DIGI-1
line 2326 documents exactly this: the previous VS-feedback law was
"anti-damping at the phugoid frequency"). The methodology is therefore
**instrument first, hypothesize second, fix third, verify fourth.**

### 3.1 Instrumentation — Phase 0 (no behaviour change)

The HUD currently shows only `KCAS / AGL / Hdg / Pitch / Roll / Gear / AI-phase`
(`renderer.cpp:332–425`). The FCS internals — `pstick, rstick, ypedal,
throttle, speedBrake, aoacmd, pscmd, pstab, pitchIntegral, betcmd,
alpha, beta, nzcgs, qptchc, k.p, k.q, k.r` — are invisible at runtime.
This makes live debugging guesswork.

**Phase 0 deliverables:**

1. **FCS-state HUD overlay** — extend `PlayerApp::Impl::draw_hud()`
   (`renderer.cpp:332`) with a second column showing:
   - AI outputs: `pstick, rstick, ypedal, throttle, speedBrake, gearHandle`
   - FCS intermediates: `aoacmd (deg), pscmd (deg/s), pstab (deg/s),
     pitchIntegral, ptcmd (G), nzcgs (G)`
   - Body rates: `k.p, k.q, k.r (deg/s)`
   - Sideslip + alpha: `alpha (deg), beta (deg) — currently always 0`
   - A toggle key (`F2`) to show/hide this column.

2. **CSV trace exporter** — add `f4-recorder` support for a per-tick
   CSV with the same fields. The trace JSON already exists
   (`flight_recorder.cpp`); the CSV is for offline plotting. One
   row per tick, one column per field, comma-separated, with a header
   row. This lets us plot `pstick` vs `phi` vs `k.p` over time and
   see the loop phase relationships directly.

3. **Isolated test scenario** — add `scenarios/takeoff_only.json` (no
   landing, no navigation) and `scenarios/landing_only.json` (spawn
   already on final at 5 nm, 2000 ft, gear down). These isolate the
   takeoff and landing control loops from each other so we can
   reproduce each symptom independently.

4. **Aircraft trim initialization** — spawn the aircraft at a small
   non-zero `vt` (e.g. 5 ft/s) or pre-trim alpha for `qsom > QSOM_FLOOR`
   so the first FCS tick is not a transient (`flight_model.cpp:171–189`
   currently skips trim when `qsom <= QSOM_FLOOR`, and the aircraft
   spawns at `vt=0` per `simulation.cpp:166–172`). This removes a
   confounding startup transient.

### 3.2 Hypothesis confirmation — Phase 1

For each symptom, run the isolated scenario with instrumentation on,
capture a 60-second CSV trace, and verify the predicted signature:

| Hypothesis | Predicted CSV signature | If confirmed → fix in §4 |
|------------|------------------------|--------------------------|
| Roll flutter is sideslip-induced (no yaw damper) | `beta` non-zero (currently always 0 — needs `runYaw` un-stubbed first to observe), `k.p` oscillates 90° out of phase with `phi` | §4.1 RC-1 |
| Roll flutter is FlyOut heading hold over-banking | `rstick` flips sign every ~2 s, `phi` overshoots ±15° around 0 | §4.1 RC-2 |
| Roll flutter is ground-clamp step release at liftoff | `k.p` jumps from 0 to FCS `pstab` in 1 tick at the moment `gear.inAir` flips true | §4.1 RC-3 |
| Altitude oscillation is FCS pitch integrator reset | `pitchIntegral` repeatedly saturates at `aoamax` and clears; `aoacmd` shows step changes | §4.2 RC-1 |
| Altitude oscillation is terrain phugoid (stale AGL) | oscillation correlates with terrain elevation changes along route; absent on flat terrain | §4.2 RC-2 |
| Altitude oscillation is time_scale driving FM too fast | oscillation amplitude scales with `time_scale`; absent at 1× | §4.2 RC-3 |
| Ground-track error is localizer saturation | `localizer_heading_rad` clamped at ±0.5 rad from 333 ft out; `course_lateral_ft` shrinks slowly | §4.3 RC-1 |
| Ground-track error is no yaw authority above 150 ft/s | heading error grows monotonically during the takeoff roll above 150 ft/s | §4.3 RC-2 |
| Landing outside is approach high/fast (no flaps) | `vcas > approach_speed + 15` on short final; flare floats long | §4.4 RC-1 |
| Landing outside is flare law no energy management | touchdown point shifts 1000+ ft when `vcas` at flare entry varies ±10 kts | §4.4 RC-2 |

### 3.3 Fix verification — Phase 2

For each fix applied, re-run the isolated scenario and the full mission
and verify the acceptance criteria in §6. **Do not batch fixes** — apply
one fix per commit, re-test, then apply the next. This preserves the
cause-effect chain in the worklog.

---

## 4. Root-Cause Catalogue

Each entry below has: **Symptom**, **Mechanism** (with file:line citation),
**Evidence chain** (what to observe in the CSV/HUD), **Proposed fix**, and
**Verification test**.

### 4.1 Roll flutter during turns

#### RC-1: Yaw channel stubbed — no coordinated-turn damping

**Mechanism**: `FlightControlSystem::runYaw()` computes a `betcmd` via
a PI controller (`fcs.cpp:544–549`), then **forces `aero.beta = 0` and
`aero.beta_dot = 0`** at `fcs.cpp:554–555`. The comment at `fcs.cpp:517–520`
states this is intentional: "the EOM has no rudder-to-yaw dynamics, so
driving beta directly would create a positive feedback loop."

The consequence is that **the aircraft cannot coordinate turns**. In a
banked turn, the fuselage sideslips (beta builds up through the kinematics
of banked flight), and the side force `cy` couples into the rolling moment.
The bank cascade in `AirSteering::steer()` (`air_steering.cpp:44`) tracks
bank angle only — it has no beta-damping term. The closed loop is:
beta → rolling moment → phi drifts → bank cascade corrects with stick →
phi overshoots → roll reverses → repeat.

The worklog at DIGI-1 line 2325 documents that the heading cascade was
designed assuming "bank-to-turn" with no beta feedback, which is correct
**only if the underlying aircraft dynamics coordinate the turn
automatically**. They don't, because the yaw channel is stubbed.

**Evidence chain**:
- HUD will show `beta = 0` always (because it's forced to 0).
- To confirm: temporarily un-stub the yaw channel (force `aero.beta` to
  be the integrated beta command instead of 0), run the isolated takeoff
  scenario, and observe whether `beta` actually builds up during banked
  flight. If it does, the sideslip-coupling-into-roll hypothesis is
  confirmed.
- The roll flutter should disappear or attenuate significantly when beta
  is allowed to develop and is fed back through `fcs.ky05` (already
  computed at `fcs.cpp:333–336`).

**Proposed fix** (two phases):

1. **Phase A — un-stub the yaw channel** by removing `aero.beta = 0`
   at `fcs.cpp:554` and instead assigning `aero.beta = fcs.betcmd`.
   This makes the PI controller's output actually drive the state.
   The "positive feedback" concern at `fcs.cpp:517–520` is unfounded
   as long as `fcs.ky05` has the correct sign — and the explicit sign
   preservation at `fcs.cpp:330–336` was added precisely to fix an
   earlier sign-inversion bug.

2. **Phase B — add a coordinated-turn feedforward** to `AirSteering::steer()`
   that commands `yaw_cmd = tan(bank_target) * vcas_kts * G / 32.2`
   (standard "rudder for bank" coordination, equivalent to FreeFalcon's
   `PedalInputs` law in `sim/auto.cpp`). This eliminates steady-state
   sideslip in turns and reduces the load on the PI yaw damper.

**Verification test**: `test_fcs_yaw_unstubbed` — drive the FCS with a
fixed `pstick = 0.3` for 5 s, observe `beta` converges to near-zero
within 2 s (Phase A) and within 0.5 s (Phase B). Then in the isolated
takeoff scenario, observe roll flutter amplitude reduced by ≥50%.

#### RC-2: FlyOut heading hold is an un-damped proportional law

**Mechanism**: `TakeoffModule::controls_for_flyout()` at
`takeoff_module.cpp:462–483` commands:

```cpp
output.roll_cmd = std::clamp(flyout_heading_gain * hdg_err, -1.0, 1.0);
```

with `flyout_heading_gain` defaulted high (see header). This is a pure
proportional law with **no bank-angle feedback** and **no roll-rate
damping**. Any heading error > ~6° commands full stick, the FCS
saturates the roll rate at `psmax` (360°/s by default, scaled by
`vcas/220` below 220 kts — see RC-3), the aircraft over-banks, then
the heading error reverses sign, then the stick reverses, then the
aircraft over-banks the other way. This is a textbook limit cycle.

The standard fix is to wrap the heading hold in a bank-angle cascade,
exactly as `AirSteering::steer()` does at `air_steering.cpp:42–45`:
command a target bank from the heading error, then command roll rate
from the bank error.

**Evidence chain**:
- CSV trace shows `rstick` flipping sign every ~2 s, with `phi`
  overshooting ±15° around 0.
- The frequency matches the roll-mode time constant `tr01 = 0.25 s`
  (`fcs.cpp:296–300`) × the closed-loop phase delay.

**Proposed fix**: replace `controls_for_flyout()`'s heading hold with
a call to `air_steering.steer(runway_heading_rad_, departure_alt_ft,
climb_speed_kts, air_input())` — the same call `NavigationModule` uses.
This gives the FlyOut phase the same bank-cascade heading hold that
works in enroute navigation. The flyout-specific climb attitude
command can be preserved by overriding `air_steering.attitude_gain`
or adding a pitch-feedforward term.

**Verification test**: `test_takeoff_flyout_no_flutter` — run the
isolated takeoff scenario, observe `phi` stays within ±5° during the
first 30 s of FlyOut (was ±15° before fix).

#### RC-3: FCS roll-rate slow-speed fade + ground clamp step release

**Mechanism**: At `fcs.cpp:478–481`:

```cpp
if (vcas_kts < 220.0) {
    pscmd *= std::max(0.0, vcas_kts / 220.0);
}
```

At `rotate_speed_kts = 140`, the fade is `140/220 = 0.64` — the FCS has
only 64% roll authority during rotation. This combines with the EOM
ground clamp at `eom.cpp:80–125`: when on the ground and close to the
terrain, `k.p` and `k.r` are forced to 0 and `k.phi` is clamped to 0
every tick. The instant the aircraft lifts off (`gear.inAir` flips
true), `k.p` is suddenly fed by the FCS `pstab` — which can be at the
saturated `psmax` if the heading hold commanded full stick during the
roll. This is a step input to the roll axis at the moment of liftoff,
when airspeed is lowest and roll authority is most limited.

**Evidence chain**:
- HUD/CSV will show `k.p` jumping from 0 to ~5 rad/s in a single tick
  at the moment `gear.inAir` flips true.
- This is the most-likely cause of the roll flutter that appears
  specifically at liftoff and persists through FlyOut.

**Proposed fix**:
1. In `TakeoffModule::controls_for_takeoff()` (`takeoff_module.cpp:435`),
   zero `roll_cmd` during the takeoff roll (the runway-heading hold is
   already via `yaw_cmd` → nose-wheel). The FCS will not command roll,
   and `pstab` will be 0 at liftoff — no step input.
2. Add a 1–2-second rate-limited transition from "ground roll" to
   "airborne" gains in `AirSteering`, so the bank cascade ramps in
   smoothly rather than activating instantly at liftoff.
3. Re-evaluate the `vcas_kts / 220` fade — it was inherited from
   FreeFalcon but may be too aggressive for the F-16 in this codebase's
   aero model. Consider raising the fade floor to 0.5 (so authority is
   never below 50%) once RC-1 and RC-2 are fixed.

**Verification test**: `test_takeoff_liftoff_no_step` — observe `k.p`
stays within ±1 rad/s for the first 2 s after liftoff.

### 4.2 Altitude oscillation (phugoid)

#### RC-1: FCS pitch integrator reset on alpha saturation

**Mechanism**: `FlightControlSystem::runPitch()` at `fcs.cpp:421–427`:

```cpp
if (eintg > aoamax) {
    eintg = aoamax;
    fcs.pitchIntegral.reset(eintg);  // clears y_prev, u_prev, u_now
} else if (eintg < aoamin) {
    eintg = aoamin;
    fcs.pitchIntegral.reset(eintg);
}
```

This is correct anti-windup. The problem is that **during sustained
high-alpha climb-out** (FlyOut at `climb_pitch_deg = 10°`, see
`takeoff_module.hpp`), `aoacmd` can saturate against `aoamax` repeatedly.
Each saturation clears the integrator; each clear produces a step in
`aoacmd` → step in `aero.alpha` (via the lead-lag at `fcs.cpp:439`) →
step in `nzcgs` → step in `qptchc` (`eom.cpp:176–185`) → step in `k.q`
→ step in `k.theta`. The resulting pitch transient looks like a
low-frequency oscillation in the kinematic theta.

The same dynamic fires during the flare (`landing_module.cpp:649–659`,
flare pitch attitude 8° at low airspeed = high alpha).

**Evidence chain**:
- CSV trace shows `pitchIntegral` repeatedly saturating at `aoamax`
  and clearing within 1–2 s; `aoacmd` shows step changes; `k.theta`
  follows the steps with a 1–2 s lag.
- HUD shows `Pitch` oscillating ±5° around the target during climb-out
  and flare.

**Proposed fix**:
1. Replace the "clear integrator" anti-windup with conditional
   integration: stop integrating when `eintg` is at the limit and the
   error is in the saturating direction (sign-matched). This preserves
   the integrator's value across the saturation and lets it unwind
   smoothly when the error reverses. Standard practice for PI loops.
2. Add an `alpha_rate` term to the FCS pitch loop (currently absent —
   `runPitch` has no derivative term) so the closed loop has explicit
   damping on alpha, not just on the integrator.

**Verification test**: `test_fcs_pitch_no_step_on_saturation` — drive
the FCS with a sustained `pstick` that commands `aoacmd = aoamax`,
hold for 10 s, release. Observe `aoacmd` rises to `aoamax`, stays
there without step changes, and unwinds smoothly when released.

#### RC-2: Brain reads AGL one tick stale vs FM ground clamp

**Mechanism**: In `Simulation::tick()` at `simulation.cpp:474`, the
brain's `BrainComponent::update(dt)` runs first (priority 100), reading
`IAircraftState::altitude_agl_ft()` — which is computed from the FM's
current `gear.groundZ_ft`. The terrain query that updates
`gear.groundZ_ft` happens **after** `world_.update_all()` at
`simulation.cpp:484–497`. So the brain is making altitude commands
against the previous tick's ground elevation.

On flat terrain this is invisible. On real Korea terrain at the
scenario player's `time_scale = 16×` (slider at `player_app.cpp:251–254`),
the brain can be ~0.27 s stale relative to the FM's ground clamp. When
the aircraft crosses a terrain feature that rises 200 ft in one tick,
the brain commands "target_alt = 6000" against `current_alt_msl = 5800`
(delta = +200, commands climb), but the FM is actually at
`5800 - 200 (rising ground) = 5600 AGL equivalent` and the ground
clamp fires. The aircraft oscillates between "climb commanded" and
"ground clamp hit" — a terrain-following phugoid.

**Evidence chain**:
- Run the isolated takeoff scenario at `time_scale = 1×` and observe
  whether altitude oscillation amplitude is reduced. If it is, this
  hypothesis is confirmed.
- Plot `current_alt_msl_ft` (from the AI's view) vs `-kin.z` (from the
  FM's view) vs `gear.groundZ_ft`. They should be identical at 1×;
  they diverge at higher time scales.

**Proposed fix**:
1. **Move the terrain query before `world_.update_all()`** in
   `Simulation::tick()`, so the brain reads the current tick's ground
   elevation. This is a 5-line change.
2. **Clamp `time_scale` to 4× maximum** in the scenario player. The
   FCS PI and lead-lag filters were tuned for a 1/360 s minor step
   (`flight_model.cpp:228` sets `minorPerMajor_ = 6`, so the minor
   step is 1/360 s). At 16× the minor step is effectively 1/22.5 s,
   which is past the stability margin of the FCS's discrete filters.
   The slider should preserve the 0.1×–4× range (slow-motion to
   4× fast-forward) but cap above that.

**Verification test**: `test_terrain_agl_not_stale` — query
`IAircraftState::altitude_agl_ft()` from the brain and the FM's
internal AGL on the same tick; assert they differ by < 1 ft.

#### RC-3: `speed_damp` term reduced on final approach

**Mechanism**: `LandingModule` constructor at
`landing_module.cpp:45` sets:

```cpp
air_steering.speed_damp_rad_per_kt = 0.0008;
```

down from the default `0.002` (`air_steering.hpp:68`). The comment at
`landing_module.cpp:32–36` explains the rationale: a large value
"acts as a constant nose-down bias whenever the aircraft is faster
than target — on final that cancelled the climb-back-to-beam command
and the aircraft settled BELOW the beam all the way to a short
touchdown."

This is a **band-aid for an airspeed-control problem**: the speed
damper is doing two jobs (phugoid damping AND nose-down trim for
overspeed), and tuning it for one breaks the other. The right
architecture is to (a) keep the phugoid damper at full strength, and
(b) fix the overspeed problem at the source — the throttle channel,
which currently has no integral term (`air_steering.hpp:71–73` is pure
proportional with a mid-setting).

**Evidence chain**:
- Run the isolated landing scenario with `speed_damp = 0.002` (default)
  and observe whether the aircraft settles below the beam. If yes, the
  overspeed problem is real and the speed_damp reduction is masking it.
- Plot `vcas` vs `target_speed` vs `throttle_cmd`. If `throttle_cmd` is
  oscillating to compensate for the P-only speed loop, the speed loop
  is the root cause.

**Proposed fix**:
1. Add an integral term to the speed channel in `AirSteering::steer()`
   (`air_steering.cpp:71–73`) with anti-windup. This eliminates
   steady-state speed error and removes the "nose-down bias when fast"
   symptom at its source.
2. Restore `speed_damp_rad_per_kt = 0.002` in the LandingModule
   constructor once the speed loop is fixed.
3. Consider adding a `gamma_rate` feedback term (the DIGI_AI_PHASE2_PLAN
   §348 mentions this) — `gamma_rate` is the natural phugoid state
   variable and is more directly damping than `speed_damp`.

**Verification test**: `test_landing_no_phugoid_on_final` — run the
isolated landing scenario, observe `vs_fpm` stays within ±300 fpm of
the beam-commanded VS for the entire final approach (was ±1500 fpm
before fix).

### 4.3 Ground-track error (poor runway alignment)

#### RC-1: Localizer gain saturates at 333 ft cross-track

**Mechanism**: `LandingModule::localizer_heading_rad()` at
`landing_module.cpp:364–368`:

```cpp
const double corr = std::clamp(localizer_gain * course_lateral_ft(),
                               -max_localizer_corr_rad, max_localizer_corr_rad);
return runway_heading_rad_ - corr;
```

With `localizer_gain = 0.0015` (`landing_module.hpp:213`) and
`max_localizer_corr_rad = 0.5` (~30°), the correction saturates at
`0.5 / 0.0015 = 333 ft` of cross-track. Beyond 333 ft off the
centerline, the localizer commands full 30° correction and the
aircraft turns toward the centerline at the maximum bank — but the
turn radius at 200 kts and 25° bank is ~7000 ft, so it takes a long
time to close the offset.

The 8% proportional beam undershoot at `landing_module.cpp:299–304`
only shrinks the offset geometrically — at 1000 ft initial offset,
the aircraft will only converge to ~80 ft off centerline at touchdown.

**Evidence chain**:
- CSV trace shows `localizer_heading_rad` clamped at ±0.5 rad for
  extended periods during the intercept; `course_lateral_ft` shrinks
  slowly.

**Proposed fix**:
1. **Raise `max_localizer_corr_rad`** to 0.87 (~50°) — large enough
   that the correction only saturates when the cross-track is more
   than ~580 ft, allowing the aircraft to point more aggressively at
   the centerline during intercept.
2. **Add a beam-intercept lead angle**: when far from the centerline
   (say > 1000 ft), command heading directly toward a point 1500 ft
   ahead on the centerline, not the localizer correction. This is
   the standard ILS intercept geometry.
3. **Tighten the 8% undershoot to a smaller value** (or eliminate it
   entirely once the localizer loop is fast enough) — it was a
   workaround for slow convergence, and the right fix is faster
   convergence, not biased convergence.

**Verification test**: `test_landing_localizer_capture` — start the
aircraft 1000 ft off the centerline at 5 nm; observe
`course_lateral_ft` converges to < 50 ft within 3 nm of the threshold.

#### RC-2: No yaw authority above 150 ft/s during takeoff roll

**Mechanism**: The EOM nose-wheel steering at `eom.cpp:92–100`:

```cpp
if (k.vt < STEER_RATE_LOW_VT) {            // 50 ft/s = 30 kts
    steerRate = TAXI_STEER_RATE;            // 30 deg/s
} else if (k.vt < STEER_RATE_HIGH_VT) {    // 150 ft/s = 89 kts
    // Linear fade from TAXI_STEER_RATE to 5 deg/s
    steerRate = TAXI_STEER_RATE * (STEER_RATE_HIGH_VT - k.vt) /
                (STEER_RATE_HIGH_VT - STEER_RATE_LOW_VT) + 5.0;
} else {
    steerRate = 5.0;                       // deg/s at high speed (rudder authority)
}
```

Above 150 ft/s (89 kts), the nose-wheel steering rate drops to 5°/s.
The comment says "rudder authority" takes over, but the FCS yaw channel
is stubbed (`fcs.cpp:554–555`, see RC-1 above) so the rudder pedals have
no airborne effect. The takeoff roll at `TakeoffModule::controls_for_takeoff()`
(`takeoff_module.cpp:435–460`) commands `yaw_cmd` based on heading error,
which routes through `ypedal` to the nose-wheel steering — but above
89 kts the steering rate is 5°/s, and the aircraft cannot correct a
heading error fast enough. By the time the aircraft reaches Vr (140 kts),
any heading error accumulated during the high-speed roll is open-loop
and the aircraft drifts downwind.

**Evidence chain**:
- HUD shows heading error growing above 89 kts during the takeoff
  roll, with no corresponding correction in the aircraft's track.
- After liftoff, the FlyOut heading hold (RC-2 above) has to correct
  this accumulated error, which triggers the roll flutter.

**Proposed fix**:
1. **Un-stub the yaw channel** (RC-1 Phase A) — this gives the rudder
   real authority above 89 kts and lets the takeoff roll correct
   heading throughout the acceleration.
2. **Tighten the lineup tolerance** in `TakeoffModule::check_runway_alignment()`
   (`takeoff_module.cpp:316–341`) — currently `centerline_align_tolerance_ft`
   and `heading_align_tolerance_rad` allow the aircraft to enter the
   takeoff roll with a small but non-zero heading error, which then
   compounds during the high-speed roll.

**Verification test**: `test_takeoff_roll_heading_hold` — observe
heading error stays within ±1° throughout the takeoff roll from
0 to 140 kts (was ±5° before fix).

### 4.4 Landing outside the runway

#### RC-1: AI never commands TEF/LEF (flaps always retracted)

**Mechanism**: `BrainComponent::map_to_pilot_input()` at
`brain_component.hpp:224–237` maps AI outputs to `PilotInput`:

```cpp
pi.pstick = ai_out.pitch_cmd;
pi.rstick = ai_out.roll_cmd;
pi.ypedal = ai_out.yaw_cmd;
pi.throttle = ai_out.throttle_cmd;
pi.speedBrake = ai_out.speed_brake_cmd;
pi.gearHandle = ai_out.gear_handle_down ? 1.0 : -1.0;
// ... (no tefCmd, no lefCmd assignment)
```

The `PilotInput` struct has `tefCmd` and `lefCmd` fields (per the
worklog at DIGI-1 line 2324, they're used by the FCS via the gear-down
landing-gains branch). But the AI never sets them — they default to 0
(retracted). So the approach is "clean" at high AOA, which means higher
approach speed, longer landing roll, and less pitch authority in the
flare.

**Evidence chain**:
- HUD shows `Gear: DOWN` during final but `Flaps: UP` (once we add
  flaps to the HUD per §3.1).
- CSV trace shows `vcas` at flare entry is ~210 kts (the
  `approach_speed_kts` default); with flaps, a real F-16 lands at
  ~130–150 kts. The 60+ kt speed difference translates to ~2× the
  landing roll distance.

**Proposed fix**:
1. **Add `flaps_extended` and `flaps_setting` to `AIControlOutput`**
   (`ai_output.hpp`).
2. **Set flaps in `LandingModule::on_enter(OnFinal)`** — when gear
   goes down, flaps go to landing setting.
3. **Lower `approach_speed_kts`** in `LandingModule` from 210 to ~160
   once flaps are wired. This is the single biggest win for landing
   distance.

**Verification test**: `test_landing_flaps_extended_on_final` —
observe `tefCmd > 0` after OnFinal entry; observe `vcas` at flare
entry within ±10 kts of the new lower `approach_speed_kts`.

#### RC-2: Flare law has no energy management

**Mechanism**: `LandingModule::controls_for_flare()` at
`landing_module.cpp:649–659`:

```cpp
out.throttle_cmd = 0.0;
const double target = flare_pitch_deg * D2R;
out.pitch_cmd = std::clamp(flare_pitch_gain * (target - current_pitch_rad_),
                           -0.1, 0.5);
out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
```

The flare fires at `flare_agl_ft = 60` (`landing_module.hpp:215`) and
holds an 8° pitch attitude with idle throttle. There is **no energy
management**: if the approach was high/fast (which it often is, see
RC-1 above), the aircraft carries extra kinetic energy into the flare
and floats long — landing 1000–3000 ft past the threshold, possibly
past the runway end. If low/slow, the flare is late and the aircraft
touches down short of the threshold (the `beam_aim_offset_ft = 1500`
past threshold compensates for the second case but not the first).

The DIGI_AI_PHASE2_PLAN §347 explicitly flags this as future work:
"Flare law refinement (energy-managed touchdown point control)".

**Evidence chain**:
- Run the isolated landing scenario with `vcas` at flare entry varied
  ±10 kts around the target. Observe touchdown point shifts 1000+ ft.
- This is the dominant cause of "lands well outside the runway bound".

**Proposed fix**:
1. **Add a touchdown-point predictor** in `LandingModule::controls_for_flare()`:
   compute the predicted touchdown point from current `vcas`, `vs_fpm`,
   `pitch_rad`, and the flare dynamics. If the predicted touchdown is
   past the runway end (or past `missed_along_ft`), command a steeper
   flare (more pitch, less energy). If short, command a shallower
   flare.
2. **Add a go-around trigger** based on predicted touchdown point,
   not just current AGL: if at flare entry the predicted touchdown
   is outside the runway bounds, go around instead of flaring.
3. **Tighten the missed-approach window** from 4000 ft to ~2500 ft
   past threshold — 4000 ft is too generous given the typical runway
   length of 5000–8500 ft.

**Verification test**: `test_landing_touchdown_within_bounds` —
observe touchdown point is within ±500 ft of the aim point and within
±50 ft of the centerline across a range of approach speeds.

### 4.5 Cross-cutting observability gaps

These are not root causes of any single symptom, but they make
diagnosis of every symptom harder. Fix them first (Phase 0).

#### RC-OBS-1: HUD does not show FCS internals

Already covered in §3.1. The HUD at `renderer.cpp:332–425` shows
KCAS/AGL/Hdg/Pitch/Roll/Gear/AI-phase but not the FCS intermediate
state. Without FCS visibility, you cannot tell whether roll flutter
is caused by the AI commanding the wrong stick input or the FCS
mis-shaping a correct input.

#### RC-OBS-2: `PilotInput{}` default is unsafe in flight

**Mechanism**: `FlightModelComponent::update()` at
`flight_model_component.hpp:94` clears `pending_input_ = PilotInput{}`
after each tick. The default `PilotInput{}` is
`pstick=0, rstick=0, ypedal=0, throttle=0, gearHandle=1.0 (down),
wheelBrakes=false, parkingBrake=false` (per `pilot_input.hpp:26–41`).
This is safe on the ground but **not safe in flight** — if a brain
module returns an empty `AIControlOutput` (e.g. `NavigationModule`
at end-of-route, or any module transition that produces an empty
output for one tick), the FM flies idle for that tick. At low altitude
this is catastrophic.

**Evidence chain**:
- Hard to observe without instrumentation — the symptom is a 1-tick
  transient. Add `pstick, rstick, throttle` to the HUD/CSV (RC-OBS-1)
  and watch for zero values during phase transitions.

**Proposed fix**:
1. Change `BrainComponent::map_to_pilot_input()` to preserve the
   previous tick's `PilotInput` if the AI output is empty (i.e.,
   hold last known good controls). This is the standard "watchdog"
   pattern for safety-critical control loops.
2. Alternatively, change `PilotInput{}` defaults to
   `throttle = previous_throttle, gearHandle = previous_gearHandle`
   when in air — but this requires state, so the watchdog pattern
   is simpler.

**Verification test**: `test_brain_empty_output_holds_last` — drive
the brain with a non-zero output for 10 ticks, then an empty output
for 1 tick, then a non-zero output again. Observe `PilotInput` is
held during the empty tick (no zero transient).

---

## 5. Phased Fix Plan

The fixes are ordered by severity, dependency, and risk. Each phase
is independently shippable; do not skip phases.

### Phase 0 — Observability (1–2 days)

| Step | Deliverable | Files touched | Risk |
|------|------------|---------------|------|
| 0a | FCS-state HUD overlay with F2 toggle | `f4-scenario-player/src/renderer.cpp` | None — additive |
| 0b | CSV trace exporter | `f4-recorder/src/flight_recorder.cpp`, `f4-recorder/include/f4/recorder/flight_recorder.hpp` | None — additive |
| 0c | Isolated test scenarios | `f4-simulation/tests/fixtures/takeoff_only.json`, `landing_only.json` | None — additive |
| 0d | Aircraft trim initialization at spawn | `f4-simulation/src/simulation.cpp`, `f4-flight-model/src/flight_model.cpp` | Low — only affects first tick |

### Phase 1 — Roll flutter (3–5 days)

| Step | Deliverable | Files touched | Depends on | Risk |
|------|------------|---------------|------------|------|
| 1a | Un-stub yaw channel (Phase A of RC-1) | `f4-flight-model/src/fcs.cpp:554–555` | 0a | Medium — touches FCS sign convention; needs careful testing |
| 1b | Replace FlyOut heading hold with `air_steering.steer()` (RC-2) | `f4-ai/src/takeoff_module.cpp:462–483` | 0a | Low — uses existing, tested cascade |
| 1c | Zero `roll_cmd` during takeoff roll + rate-limited liftoff transition (RC-3) | `f4-ai/src/takeoff_module.cpp:435–460` | 0a, 1b | Low |
| 1d | Coordinated-turn feedforward in AirSteering (Phase B of RC-1) | `f4-ai/src/air_steering.cpp`, `f4-ai/include/f4/ai/air_steering.hpp` | 1a | Low — additive feedforward |

### Phase 2 — Altitude oscillation (3–5 days)

| Step | Deliverable | Files touched | Depends on | Risk |
|------|------------|---------------|------------|------|
| 2a | Move terrain query before `world_.update_all()` in `tick()` (RC-2) | `f4-simulation/src/simulation.cpp:474–497` | 0a | Low — 5-line reorder |
| 2b | Clamp `time_scale` to 4× max (RC-2) | `f4-scenario-player/src/player_app.cpp:251–254` | 0a | None |
| 2c | Conditional-integration anti-windup in FCS pitch (RC-1) | `f4-flight-model/src/fcs.cpp:421–427` | 0a | Medium — touches FCS stability |
| 2d | Add integral term to AirSteering speed channel (RC-3) | `f4-ai/src/air_steering.cpp:71–73` | 0a | Low |
| 2e | Restore `speed_damp_rad_per_kt = 0.002` in LandingModule (RC-3) | `f4-ai/src/landing_module.cpp:45` | 2d | None — once 2d works |

### Phase 3 — Ground-track (2–3 days)

| Step | Deliverable | Files touched | Depends on | Risk |
|------|------------|---------------|------------|------|
| 3a | Raise `max_localizer_corr_rad` + add beam-intercept lead (RC-1) | `f4-ai/include/f4/ai/modules/landing_module.hpp:213–214`, `f4-ai/src/landing_module.cpp:364–368` | 0a | Low |
| 3b | Tighten lineup tolerance in `check_runway_alignment` (RC-2) | `f4-ai/src/takeoff_module.cpp:316–341`, `f4-ai/include/f4/ai/modules/takeoff_module.hpp` | 1a | Low |

### Phase 4 — Landing precision (3–5 days)

| Step | Deliverable | Files touched | Depends on | Risk |
|------|------------|---------------|------------|------|
| 4a | Add `flaps_extended` / `flaps_setting` to `AIControlOutput` (RC-1) | `f4-ai/include/f4/ai/ai_output.hpp`, `f4-ai/include/f4/ai/brain_component.hpp:224–237` | 0a | Low |
| 4b | Set flaps on `on_enter(OnFinal)` + lower `approach_speed_kts` to 160 (RC-1) | `f4-ai/src/landing_module.cpp:158–167` | 4a | Low |
| 4c | Touchdown-point predictor + go-around trigger (RC-2) | `f4-ai/src/landing_module.cpp:649–659`, `f4-ai/src/landing_module.cpp:522–536` | 4a | Medium — new logic |
| 4d | Tighten `missed_along_ft` to 2500 ft (RC-2) | `f4-ai/include/f4/ai/modules/landing_module.hpp:222` | 4c | Low |

### Phase 5 — Safety hardening (1–2 days)

| Step | Deliverable | Files touched | Depends on | Risk |
|------|------------|---------------|------------|------|
| 5a | Watchdog: hold last good `PilotInput` on empty AI output (RC-OBS-2) | `f4-ai/include/f4/ai/brain_component.hpp:224–237` | 0a | Low |
| 5b | Re-tighten `DigiMission` test tolerances to match new acceptance criteria | `f4-simulation/tests/test_digi_mission.cpp` | All above | None |

---

## 6. Acceptance Criteria

The full mission test `DigiMission.FullLoopTaxiTakeoffNavigateApproachLandParks`
must pass with the following tightened tolerances:

### 6.1 Roll stability

- `phi` stays within ±5° during FlyOut climb-out (was ±15°)
- `phi` stays within ±3° during OnFinal (was ±10°)
- `k.p` stays within ±1 rad/s during steady-state flight (was unbounded)
- No sign reversals of `rstick` within any 3-second window during
  steady-state flight (i.e. no limit-cycle)

### 6.2 Altitude stability

- `vs_fpm` stays within ±300 fpm of the beam-commanded VS during OnFinal
  (was ±1500 fpm)
- `altitude_msl_ft` stays within ±100 ft of target during waypoint
  navigation (was ±500 ft)
- `pitchIntegral` does not saturate-and-clear more than once per 10
  seconds during climb-out

### 6.3 Ground-track

- `course_lateral_ft` converges to < 50 ft within 3 nm of the threshold
  (was < 500 ft at touchdown)
- Heading error during takeoff roll stays within ±1° from 0 to 140 kts
  (was ±5°)
- Touchdown cross-track error < 50 ft from centerline (was unbounded)

### 6.4 Landing precision

- Touchdown along-track position is within ±500 ft of the aim point
  (beam_aim_offset_ft = 1500 ft past threshold) — i.e. between 1000
  and 2000 ft past the threshold
- Touchdown `vcas` is within ±10 kts of `approach_speed_kts` (was
  unbounded — the approach speed itself was wrong, see RC-1 §4.4)
- Flaps (`tefCmd > 0`) observed from OnFinal entry through touchdown
- No go-around on a nominal approach (the scenario does not include
  any go-around triggers)

### 6.5 Time-scale stability

- All above criteria pass at `time_scale = 1×` and `time_scale = 4×`
- The HUD shows identical FCS state at both time scales (within
  expected noise)

---

## 7. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Un-stubbing the yaw channel exposes a sign error in `fcs.ky05` | Medium | High — yaw damper becomes positive feedback | The sign preservation at `fcs.cpp:330–336` was added to fix exactly this; verify with `test_fcs_yaw_unstubbed` (Phase 1 step 1a) before merging |
| Replacing FlyOut heading hold breaks the climb-out altitude capture | Low | Medium | The `air_steering.steer()` call already handles altitude; only the pitch feedforward needs preserving |
| FCS pitch anti-windup change destabilizes the FCS at high alpha | Medium | High — could make pitch oscillation worse | Phase 0 instrumentation must be in place first; test on the isolated takeoff scenario before merging |
| Lowering `approach_speed_kts` to 160 without flaps makes the aircraft stall on final | High | High — stall at low altitude | Phase 4a (flaps wiring) MUST land before Phase 4b (speed reduction). The flaps change the FCS landing gains (`fcs.cpp:310–312`, `fcs.cpp:339–342`) and the aero lift curve, both of which lower stall speed |
| Tightening `missed_along_ft` to 2500 ft causes spurious go-arounds on nominal approaches | Medium | Medium | Test with the isolated landing scenario first; if go-arounds fire, the issue is likely the flare timing, not the missed-approach window |
| The `time_scale` clamp to 4× breaks user workflows that depend on 16× | Low | Low | The 16× setting was always past the FCS stability margin; the clamp is a feature, not a regression. Document in CHANGES.md |
| The watchdog (RC-OBS-2) masks a real bug where the brain produces empty outputs | Medium | Medium | The watchdog is a safety net, not a fix. Log every instance of "empty output, holding last" so the underlying brain bug can be diagnosed separately |

---

*This document is the active diagnostic plan for the flight control
instability. Update it as fixes land and symptoms evolve. Each fix
should reference this document in its commit message.*

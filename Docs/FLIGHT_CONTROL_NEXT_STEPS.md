# F4 Flight Control — Next-Step Diagnostic & Fix Plan

> **Status**: Picks up where `FLIGHT_CONTROL_STABILITY_PLAN.md` left off, after ALT-2…ALT-5 and Phases 0a/1b/1c/2a–2e landed.
> **Source of Truth**: `https://github.com/jdcrayme/F4`
> **Predecessors**: `FLIGHT_CONTROL_STABILITY_PLAN.md`, worklog tasks DIGI-1, DIGI-2, ALT-2, ALT-3, ALT-4, ALT-5
> **Symptoms being addressed**: altitude oscillation, ground-track drift, runway overflight offset, landing outside runway bounds.

---

## 1. Why a New Plan

The existing `FLIGHT_CONTROL_STABILITY_PLAN.md` was a comprehensive catalogue of root causes. Several of its fixes have landed since (ALT-5 alpha-bias feedforward, Phase 0a FCS HUD, Phase 1b/c takeoff roll hardening, Phase 2a–e altitude-loop cleanup). The aircraft is **still unstable** in the full takeoff-and-landing mission.

The most likely reason: the altitude-loop fix (ALT-5) was validated **in isolation** — a steady-state altitude hold at 10 000 ft, no turns, no localizer capture, no flare. The user is observing the **full mission**, which exercises subsystems that the existing plan identified but that are still unaddressed: the stubbed yaw channel, the 333-ft localizer saturation, the missing flaps wiring, and the energy-blind flare law. Those subsystems excite the phugoid that ALT-5 damps in isolation.

This document audits the gap between plan and code, sequences the remaining work, and tightens the diagnostic methodology so we stop fixing things that "work in isolation, fail in mission."

---

## 2. Code-State Audit

Verified by reading current source, not by trusting the plan:

| Plan item | Status | Evidence (file:line) |
|---|---|---|
| Phase 0a — FCS-state HUD overlay (F3) | ✅ Done | `renderer.cpp:426` — FCS column with aoacmd/pscmd/pstab/nzcgs/k.p/k.q/k.r |
| Phase 0b — CSV trace exporter | ❌ Not done | `f4-recorder/` has no CSV support |
| Phase 0c — Isolated `takeoff_only` / `landing_only` scenarios | ❌ Not done | Only `takeoff_kunsan.json` exists |
| Phase 0d — Aircraft trim init at spawn | ❌ Not done | `simulation.cpp` still spawns vt=0 |
| Phase 1a — Un-stub yaw channel | ❌ Not done | `fcs.cpp:618` still `aero.beta = zero_angle()` |
| Phase 1b — FlyOut uses air_steering cascade | ✅ Done | `takeoff_module.cpp:498` calls `air_steering.steer()` |
| Phase 1c — Zero roll_cmd during takeoff roll | ✅ Done | `takeoff_module.cpp:460` explicit zero |
| Phase 1d — Coordinated-turn feedforward | ❌ Not done | `air_steering.cpp` has no `tan(bank)·v·g` term |
| Phase 2a — Terrain query before `update_all` | ✅ Done | `simulation.cpp:482–492` moved before `world_.update_all` |
| Phase 2b — time_scale clamp to 4× | ✅ Superseded | Replaced by the fixed-timestep accumulator (`player_app.cpp`): dt is always `sim_dt`, the slider scales wall-clock time (now up to 10×), so filter stability no longer depends on the slider. See CHANGES.md. |
| Phase 2c — Conditional-integration anti-windup | ✅ Done | `fcs.cpp:475–483` sign-matched halt |
| Phase 2d — Speed integral in AirSteering | ✅ Done | `air_steering.cpp:92` leaky integral |
| Phase 2e — `speed_damp_rad_per_kt = 0.002` restored | ✅ Done | `landing_module.cpp:47` comment confirms |
| Phase 3a — Raise localizer corr + intercept lead | ❌ Not done | `landing_module.hpp:213–214` still 0.0015 / 0.5 rad |
| Phase 3b — Tighten takeoff lineup tolerance | ❌ Not done | unchanged |
| Phase 4a — `flaps_extended` in `AIControlOutput` | ❌ Not done | `ai_output.hpp` has no flap fields |
| Phase 4b — Set flaps on `on_enter(OnFinal)` | ❌ Not done | depends on 4a |
| Phase 4c — Touchdown-point predictor | ❌ Not done | `landing_module.cpp:653–662` flare unchanged |
| Phase 4d — Tighten `missed_along_ft` | ❌ Not done | still 4000 ft |
| Phase 5a — Watchdog hold-last | ❌ Not done | `brain_component.hpp:251` no hold-last |
| Phase 5b — Tighten DigiMission tolerances | ❌ Not done | still 500 ft / anywhere on runway |
| ALT-5 — alpha_bias after lead-lag | ✅ Done | `fcs.cpp:504` adds bias post-filter |

**The single biggest unaddressed defect is the stubbed yaw channel.** It contributes to every user-visible symptom: roll flutter (no coordinated-turn damping), ground-track drift (no rudder authority above 89 kts), localizer capture distortion (beta build-up in intercept turns), and altitude oscillation (lift vector tilts off-vertical in uncoordinated turns, exciting the phugoid that ALT-5 otherwise damps).

---

## 3. Diagnostic Methodology — Instrument Before You Touch

The existing plan's §3.1 specified observability deliverables that are still pending. **Before applying any more behaviour-changing fixes**, complete them. The repository has been gaining fixes based on isolated-test results; that approach has demonstrably failed for the full mission. Capture full-mission traces first.

### 3.1 CSV trace exporter (Phase 0b — 1 day)

Add a CSV writer to `f4-recorder`. One row per tick, columns grouped by source:

- **Mission state**: tick, sim_time_s, time_scale, AI phase, AI sub-state
- **AI commands**: pitch_cmd, roll_cmd, yaw_cmd, throttle_cmd, speed_brake_cmd, gear_handle_down, wheel_brakes, (add `tef_cmd` / `lef_cmd` once Phase C1 lands)
- **FCS intermediates**: aoacmd_deg, pscmd, pstab, ptcmd, nzcgs, pitchIntegral, betcmd, alpha_deg, beta_deg, ypedal
- **Body rates**: k.p, k.q, k.r (deg/s)
- **Kinematics**: vcas_kts, vt_fps, alt_msl_ft, alt_agl_ft, vs_fpm, heading_rad, pitch_rad, roll_rad, x_ft, y_ft
- **Navigation state**: target_alt_ft, target_speed_kts, target_heading_rad, course_lateral_ft, course_along_ft, localizer_heading_rad

Wire into `simulation.cpp` after the per-aircraft sync loop. Output path: `download/traces/<scenario>_<timestamp>.csv`. Toggle with the existing recorder enable flag.

### 3.2 Isolated test scenarios (Phase 0c — 1 day)

Add two scenarios:

- `scenarios/takeoff_only.json.in` — taxi → lineup → takeoff roll → rotate → FlyOut for 60 s → terminate. No navigation, no landing. Isolates the takeoff + climb-out control loop.
- `scenarios/landing_only.json.in` — spawn 5 nm out on final at 2000 ft AGL, gear down, flaps up (initially), fly the OnFinal → Flare → Rollout → stop sequence. Isolates the approach + flare loop.

These let us reproduce each symptom independently and avoid the confounding effect of having six state-machine phases in one trace.

### 3.3 Trim-init at spawn (Phase 0d — 0.5 day)

`flight_model.cpp:171–189` skips trim when `qsom ≤ QSOM_FLOOR`. The aircraft spawns at vt=0, so the first FCS tick is a transient. Spawn at vt=5 ft/s (or pre-seed the FCS lead-lag and integrator to trim alpha) so the first tick is not a confounding startup transient.

### 3.4 Capture baseline traces

Run each isolated scenario at time_scale = 1× and 4× with the CSV exporter on. Capture 60-second traces. These baselines are the verification reference for every subsequent fix. **No fix is merged unless its before/after CSV traces are attached to the PR.**

### 3.5 Predicted signatures to confirm before fixing

| Symptom | Predicted CSV signature | Likely live root cause |
|---|---|---|
| Altitude oscillates on final | `vs_fpm` swings ±1500–3000 fpm with 8–12 s period, correlated with `roll_rad` oscillation | Beta build-up in uncoordinated turns (RC-1 yaw stub) perturbs the lift vector → ALT-5 damping cannot hold |
| Drifts off runway during takeoff roll | `heading_rad` drifts >2° above 89 kts (150 ft/s) | EOM fades nose-wheel to 5°/s at 89 kts AND FCS yaw stubbed → open-loop above 89 kts |
| Flies overhead offset on approach | `course_lateral_ft` > 333 ft clamps `localizer_heading_rad` at ±0.5 rad; banked turns cannot close the offset | Localizer saturation at 333 ft (§4.3 RC-1) + no intercept lead angle |
| Lands outside runway | `vcas` at flare entry > 220 kts; touchdown along-track > 3000 ft past threshold | No flaps wired (§4.4 RC-1) + flare law has no energy management (§4.4 RC-2) |
| Phase-transition transients | `pitch_cmd = roll_cmd = throttle_cmd = 0` for 1 tick at phase boundaries | `PilotInput{}` default unsafe in air (§4.5 RC-OBS-2) |

---

## 4. Hypothesis-Driven Fix Sequence

Each fix is one commit. Each commit message references the CSV signature it addresses. The order is by **dependency + symptom severity**, not by the original plan's phase numbering.

### Phase A — Yaw channel (the linchpin)

**Rationale.** The stubbed yaw channel is the single most damaging remaining defect. It affects every user-visible symptom:

- **Roll flutter**: no coordinated-turn damping → sideslip builds in banked turns → side force couples into rolling moment → bank cascade over-corrects → limit cycle.
- **Ground-track drift above 89 kts**: nose-wheel steering fades to 5°/s at 89 kts AND the FCS yaw channel is stubbed → rudder pedals have no airborne effect → heading is open-loop during the high-speed takeoff roll.
- **Approach centerline distortion**: in the localizer correction turns, beta builds because there's no yaw damper → the lift vector tilts off-vertical → altitude hold fights an asymmetric load → phugoid excited.
- **Altitude oscillation in turns**: same lift-vector-tilt mechanism as above. ALT-5 damps the phugoid in straight-and-level flight but cannot hold it in uncoordinated turns.

**A1 — Un-stub the yaw channel.** At `fcs.cpp:618`, remove `aero.beta = zero_angle()` and `aero.beta_dot = zero_angular_rate()`. Replace with `aero.beta = fcs.betcmd` (the PI output already computed at `fcs.cpp:614`). The comment at `fcs.cpp:583–585` warns of positive feedback, but the sign-preservation at `fcs.cpp:330–336` was added precisely to prevent that — verify with the test below.

Verification: new unit test `test_fcs_yaw_unstubbed` drives the FCS with `pstick=0, rstick=0.3` for 5 s. Expect `beta` to converge to <1° within 2 s. If beta diverges, the sign of `fcs.ky05` is wrong — flip it and re-run.

**A2 — Coordinated-turn feedforward in AirSteering.** In `air_steering.cpp`, after computing `bank_target` at line 47–48, add a feedforward pedal command:

```cpp
// Coordinated-turn feedforward: rudder-for-bank.
// Standard "pedal = tan(bank) * v / g" scaled into [-1, +1] command space.
const double pedal_ff = std::tan(bank_target) * (in.vcas_kts * 1.68781) / 32.2;
out.yaw_cmd = std::clamp(pedal_ff / 10.0, -1.0, 1.0);
```

The `/10.0` maps the physical pedal deflection to the normalized command space; calibrate against the EOM pedal authority at `eom.cpp:102`. This eliminates steady-state sideslip in turns and reduces the load on the PI yaw damper from A1.

Verification: 30° bank hold test — `beta` should stay within ±2° throughout the turn (was unbounded before A1; ±5° after A1 alone; ±2° after A2).

**A3 — Tighten takeoff lineup tolerance.** At `takeoff_module.cpp:316–341` (`check_runway_alignment`), tighten `centerline_align_tolerance_ft` from its current value to 5 ft and `heading_align_tolerance_rad` to 0.5° (~0.009 rad). The aircraft must enter the takeoff roll aligned, since even after A1 the high-speed roll has limited rudder authority at low qbar.

Verification: `test_takeoff_roll_heading_hold` — heading error stays within ±1° from 0 to 140 kts (was ±5°).

### Phase B — Localizer + intercept geometry

**Rationale.** Even with a coordinated aircraft, the localizer loop's 333-ft saturation + 8% beam undershoot bias mean a 1000-ft initial offset converges only to ~80 ft at touchdown. This is the "flies overhead offset" symptom.

**B1 — Raise localizer correction clamp.** `landing_module.hpp:214`: change `max_localizer_corr_rad` from 0.5 (~30°) to 0.87 (~50°). Saturation point moves from 333 ft to ~580 ft cross-track, giving the localizer authority to actually point at the centerline during intercept.

**B2 — Add beam-intercept lead angle.** In `landing_module.cpp:368–373`, replace the single-line localizer correction with:

```cpp
double LandingModule::localizer_heading_rad() const {
    const double xtrack = course_lateral_ft();
    if (std::abs(xtrack) > 1000.0) {
        // Far from centerline: aim at a point 1500 ft ahead on the centerline.
        // Standard ILS intercept geometry.
        const double ahead_ft = 1500.0;
        const double bearing_to_aim = std::atan2(
            -xtrack,  // toward centerline
            ahead_ft  // forward along the course
        );
        return runway_heading_rad_ + bearing_to_aim;
    }
    // Near centerline: standard proportional localizer correction.
    const double corr = std::clamp(localizer_gain * xtrack,
                                   -max_localizer_corr_rad, max_localizer_corr_rad);
    return runway_heading_rad_ - corr;
}
```

This is the standard ILS intercept geometry. Below 1000 ft cross-track, revert to the proportional correction.

**B3 — Drop the 8% beam undershoot bias.** At `landing_module.cpp:299–304`. Once B1 + B2 are in, the bias is no longer needed — it was a workaround for slow convergence. Removing it lets the localizer actually close to zero.

Verification: `test_landing_localizer_capture` — start 1000 ft off centerline at 5 nm; `course_lateral_ft` converges to < 50 ft within 3 nm of the threshold.

### Phase C — Flaps + energy-managed flare

**Rationale.** The "lands outside the runway" symptom is dominated by approach speed (210 kts with flaps retracted) and a flare law that has no concept of energy. A real F-16 lands at 130–150 kts with flaps; the 60+ kt difference doubles landing distance.

**C1 — Wire flaps through `AIControlOutput`.** Add to `ai_output.hpp`:

```cpp
double tef_cmd{0.0};   // [-1, +1] trailing-edge flap command
double lef_cmd{0.0};   // [-1, +1] leading-edge flap command
```

In `brain_component.hpp:251` (`map_to_pilot_input`), copy them to `pi.tefCmd` and `pi.lefCmd`. The FM already actuates these at `flight_model.cpp:453–454`.

**C2 — Set flaps on `on_enter(OnFinal)`.** In the OnFinal entry hook (around `landing_module.cpp:158–167`), emit a one-shot output with `tef_cmd = 1.0` and `lef_cmd = 0.6` (landing configuration). Subsequent `controls_for_onfinal()` calls maintain this.

**C3 — Lower `approach_speed_kts` from 210 to 160.** In `landing_module.hpp:144`. **C2 must land before C3** — lowering speed without flaps will stall on final. Also note the ALT-4 finding: the engine model's idle thrust is -900 lbf (likely a .dat conversion bug). Verify that approach-power-required at 160 kts with flaps produces positive thrust. If not, either fix the thrustIdle table in the .dat conversion pipeline or override the thrustIdle value in the FM config.

**C4 — Energy-managed flare.** Replace `controls_for_flare()` at `landing_module.cpp:653–662` with a touchdown-point predictor:

```cpp
AIControlOutput LandingModule::controls_for_flare() const {
    AIControlOutput out;
    out.gear_handle_down = true;
    out.throttle_cmd = 0.0;

    // Predicted touchdown point (linearized around current state):
    //   td_distance = (alt_agl_ft / max(vs_fpm, -50)) * vcas_kts * 1.68781 / 60
    //   td_along = course_along_ft + td_distance
    const double vs_eff = std::max(std::abs(current_vs_fpm_), 50.0);
    const double time_to_ground_s = current_alt_agl_ft_ / vs_eff * 60.0;
    const double td_distance_ft = time_to_ground_s * current_vcas_kts_ * 1.68781 / 60.0;
    const double td_along = course_along_ft() + td_distance_ft;

    // If predicted touchdown is outside runway bounds, go around.
    if (td_along > missed_along_ft || td_along < -500.0) {
        // Trigger transition to GoAround state (handled by sequencer).
        out.pitch_cmd = 0.3;   // climb away
        out.throttle_cmd = 1.0;
        return out;
    }

    // Otherwise, modulate flare pitch by predicted-vs-aim error.
    const double aim_along = beam_aim_offset_ft;  // 1500 ft past threshold
    const double td_err = td_along - aim_along;
    const double flare_pitch_adj = std::clamp(td_err / 500.0, -2.0, 4.0);  // deg
    const double target = (flare_pitch_deg + flare_pitch_adj) * D2R;
    out.pitch_cmd = std::clamp(flare_pitch_gain * (target - current_pitch_rad_),
                               -0.1, 0.5);
    out.roll_cmd = std::clamp(-2.0 * current_roll_rad_, -0.3, 0.3);
    return out;
}
```

This makes the flare law actively manage touchdown point instead of passively holding 8° pitch.

**C5 — Tighten `missed_along_ft`.** `landing_module.hpp:222`: 4000 → 2500 ft. Combined with C4's go-around trigger, this prevents plowing through the runway end.

Verification: `test_landing_touchdown_within_bounds` — touchdown along-track is within ±500 ft of aim point across `vcas` at flare entry varying ±10 kts.

### Phase D — Safety + test tightening

**D1 — Watchdog (hold-last on empty output).** In `brain_component.hpp` `update()`, if the active module's `AIControlOutput` is all-zero (no pitch/roll/yaw/throttle command) AND the aircraft is airborne, hold the previous tick's `PilotInput`. Log every instance so the underlying brain bug is visible. This prevents the 1-tick idle transient at phase transitions from being catastrophic.

Verification: `test_brain_empty_output_holds_last` — drive the brain with non-zero output for 10 ticks, then empty for 1 tick, then non-zero again. `PilotInput` is held during the empty tick.

**D2 — Tighten DigiMission tolerances.** In `test_digi_mission.cpp`, replace the loose 500 ft lateral / anywhere-on-runway touchdown tolerances with the acceptance criteria in §5 below. The current loose tolerances are why `DigiMission.FullLoopTaxiTakeoffNavigateApproachLandParks` "passes" despite the user-visible instability.

---

## 5. Acceptance Criteria

The mission passes when ALL of the following hold simultaneously, captured in CSV traces at both 1× and 4× time_scale:

| Metric | Target | Was |
|---|---|---|
| `phi` during FlyOut climb-out | ±5° | ±15° |
| `phi` during OnFinal | ±3° | ±10° |
| `k.p` steady-state | ±1 rad/s | unbounded |
| No `rstick` sign reversals in any 3-second window (steady-state) | 0 reversals | limit cycle |
| `vs_fpm` on final | ±300 fpm of beam-commanded VS | ±1500 fpm |
| `altitude_msl_ft` during waypoint nav | ±100 ft of target | ±500 ft |
| `course_lateral_ft` at threshold | < 50 ft | < 500 ft |
| `heading_rad` error during takeoff roll 0–140 kts | ±1° | ±5° |
| Touchdown along-track | 1000–2000 ft past threshold | 1000–4000 ft |
| Touchdown cross-track | ±50 ft | unbounded |
| Touchdown `vcas` | ±10 kts of `approach_speed_kts` (160 after C3) | unbounded |
| `tefCmd` from OnFinal entry through touchdown | > 0 | 0 |
| Phase-transition empty-output transients | 0 | undocumented |
| All above hold at both 1× and 4× time_scale | yes | 1× only, mostly |

---

## 6. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Un-stubbing yaw exposes a sign error → yaw damper becomes positive feedback | Medium | High — divergent beta | Test in isolated takeoff scenario at 1× first; verify `fcs.ky05` sign; if beta diverges, flip sign and re-run. DIGI-2 already fixed one NED→ENU sign error in this area |
| Lowering `approach_speed_kts` to 160 without flaps → stall on final | High if C3 lands before C2 | High — stall at low altitude | Enforce C2 → C3 ordering in PR review; add a stall-on-final test |
| Energy-managed flare predicts wrong touchdown point → over-flare → long landing | Medium | Medium | Linearize the flare around nominal conditions; add hysteresis so the predictor doesn't oscillate; test across ±10 kt approach-speed variation |
| Tightening `missed_along_ft` to 2500 → spurious go-arounds on nominal approaches | Medium | Medium | Test in isolated landing scenario; if go-arounds fire, the issue is flare timing, not the window |
| Coordinated-turn feedforward sign is wrong → rudder commands opposite to bank → worse | Medium | Medium | Compare against FreeFalcon's PedalInputs law; verify with a 30° bank hold test |
| Engine idle thrust is -900 lbf (ALT-4 finding) — at 160 kts with flaps, approach may need ~5% throttle but FCS's `throttle_min=0.15` floor is too high → overspeed | Medium | Low — affects touchdown point, not safety | Investigate thrustIdle table in the .dat conversion pipeline as a separate task; consider lowering `air_steering.throttle_min` from 0.15 to 0.05 for the landing tune |
| `test_digi_mission` will fail after D2 tightening until all fixes land | Certain | Low — expected | Mark the test as expected-fail during the transition; flip to expected-pass once §5 criteria are met |

---

## 7. Sequencing Summary

| Order | Step | Effort | Dependencies |
|---|---|---|---|
| 1 | Phase 0b — CSV trace exporter | 1 day | None |
| 1 | Phase 0c — Isolated scenarios | 1 day | None |
| 1 | Phase 0d — Trim-init at spawn | 0.5 day | None |
| 2 | Capture baseline traces (1× and 4×) | 0.5 day | 0b + 0c + 0d |
| 3 | A1 — Un-stub yaw channel | 0.5 day | CSV + isolated scenarios |
| 4 | A2 — Coordinated-turn feedforward | 0.5 day | A1 |
| 5 | A3 — Tighten takeoff lineup | 0.5 day | A1 |
| 6 | B1 + B2 + B3 — Localizer fix | 1 day | A1 (need yaw for intercept turns) |
| 7 | C1 + C2 — Wire flaps | 1 day | None (independent of A/B) |
| 8 | C3 — Lower approach speed | 0.5 day | C2 |
| 9 | C4 — Energy-managed flare | 2 days | C3 |
| 10 | C5 — Tighten missed-approach window | 0.5 day | C4 |
| 11 | D1 — Watchdog | 0.5 day | None |
| 12 | D2 — Tighten DigiMission tolerances | 0.5 day | All above |

**Total estimate: 9–10 days.**

Phases A and B are the highest-leverage — they address the ground-track and runway-alignment symptoms directly. Phase C is the longest because the flare law is new logic, not a gain change. Phase D is safety hardening and test tightening.

The critical path is **A1 → A2 → B1+B2 → C2 → C3 → C4**. Phase D can run in parallel with C.

---

## 8. Workflow Rules

1. **One fix per commit.** No batching. Each commit references the CSV signature it addresses.
2. **CSV before and after, attached to the PR.** No fix is merged without trace evidence.
3. **Isolated scenario first, full mission second.** A fix that passes the isolated scenario but fails the full mission is not ready — it means the fix interacts with another subsystem and that interaction must be diagnosed before merge.
4. **Update this document as fixes land.** Mark items in §2 as ✅ Done with the commit SHA. Add new root causes discovered during diagnosis to §4 with their own A/B/C/D sub-letter.
5. **Worklog entry per fix.** Append to `worklog.md` with Task ID `STAB-<phase><letter>` (e.g. `STAB-A1`), following the existing worklog format.

---

*This document supersedes the fix-sequencing portions of `FLIGHT_CONTROL_STABILITY_PLAN.md` §5. The root-cause catalogue in §4 of that document remains authoritative.*

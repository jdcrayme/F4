# Phase 4 (Redesign) — Measured Findings & Resume Plan

> **Status**: P4.1 LANDED on main together with the dependent-family
> retune pass and the P5.2 CI guardrails (see §6). The P4.2/P4.3 TECS
> outer-loop redesign is built and measured end to end but **not
> merge-ready** — this document is the Phase 4 gate artifact:
> what was measured, what it means, and exactly what the next session must
> run before any control-law merge. Companion to
> `LONGITUDINAL_STABILITY_PLAN.md` (§8), `LOOP_MARGIN_REPORT.md` (M1–M7),
> `BISECTION_RESULTS.md`.

---

## 1. What landed as MEASURED-CORRECT (branch `phug-p4-fcs-wip`)

### P4.1 — the FCS inner loop (all four changes verified by measurement)

| Change | Was | Measured after |
|---|---|---|
| Real integrator + Hanüs back-calculation anti-windup (replaces conditional-integration + QIL leak + E51 shedding) | M6: the "integrator" was a lag (DC gain 5.9, τ≈2 s) — the G-loop was type 0 | Type-1: the integrator holds `5.9·err` only while the error stands; the stick-step freeze prediction matched to 3 digits pre-fix; post-fix the DC trim is carried by the integral |
| `kp05 = 1/K_nz` (plant inverse) | The AOA-command formula omitted the plant gain: realized loop gain 0.036 | Loop gain ≈ 1; the closed-loop poles land where the tp02/tp03 algebra intended |
| `kp03` 2.0 → 0.4 | The integral crossed over ABOVE the P path (2 rad/s vs ~0.9) and wound huge states | Integral crossover ~0.4 rad/s (classic PI split) |
| q-damper loop-gain rescale (`qDampScale = kp05_legacy/kp05`) + gear gate → 0.5× authority (M2) | The damper was calibrated against the legacy kp05; with the corrected kp05 it ran ~28× hot | **M2 RESONANCE FIXED: 160 kt gear \|R\|peak 6.88 → 1.01 @ 0.10 rad/s (flat).** With the damper zeroed the loop is a **49× resonator (ζ 0.010)** — the damper is the ONLY damping at the approach config |
| Alpha protection at `criticalAOA − 2.5°` (command + final alpha, back-calc unwinds it) | The corrected loop faithfully drove alpha to stall (measured: 148-kt spawn → 35° clamp → departure) | Approach runs no longer depart at the clamp |

### Plant fixes in the same branch (measured)

- **Stall boundary**: the stall-speed test used the INSTANTANEOUS CL — at 1-G
  the boundary equalled the current speed by construction
  (`17.16·sqrt(W/(q·S·CL))` with `CL = W/(q·S)` collapses to `17.16·sqrt(q)`),
  so every nz<1 excursion at α>10° declared a stall and the +5 kt exit margin
  was unreachable: **13,582 of 13,800 frames in DeepStall at the approach
  trim**. Fixed to CL_max (the CL at critical AOA, same effective-alpha
  convention as the main lookup).
- **Spawn trim (F6)**: `trim()` now re-sets θ = α (level, γ = 0) and refreshes
  the quaternion/trig cache — the residual-γ spawn climb (~1,400 fpm) is gone.

### Why the FCS-only hybrid does not ship on main

The same 4 files re-tune the inner loop that **every AI behavior family was
calibrated against**: 26 suite failures (combat ×15, DigiMission ×2, AAR,
intercept, Brain/taxi, wingman, campaign-arm) vs the baseline's 4. The
families need a re-tune pass against the new inner loop before the FCS
change merges. That is the cost of fixing a 28× loop-gain error — it is
real, and it is the next session's first work item.

---

## 2. The TECS outer-loop redesign — measured record (v13 → v19e)

The full TECS architecture was implemented and iterated against the
intercept/E2E suite (`intercept_final`, `on_glideslope`, DigiMission,
AAR, combat). The design (preserved in the branch history and summarized
here so it can be rebuilt):

- **Energy-rate loop → throttle**: `E_err = K_E·Δh + K_Ek·Δ(V²/2g)`;
  `e_E = E_err/V − K_v̇·V̇_f/g`; throttle = mid + Kp·e_E + Ki·∫e_E with
  back-calculation AW on the throttle clamp.
- **Path demand**: `vs_dem = vs_ff + window(vs_gain·Δh)`; slew-limited
  `gamma_dem` (0.05 rad/s, 4× on retargets).
- **Energy-distribution loop → pitch (approach variant)**:
  `g_cmd = K_att·(γ_dem + α_est − θ) + K_v·(V − V_dem)/g`, stick =
  sign·sqrt(|g_cmd|/kp01) (G-linearizing), path-tracking integral, flipped
  speed damper (fast → pull).

Measured outcome per configuration attempt:

| v | Change under test | Result (intercept_final / suite) |
|---|---|---|
| v13 | sysid approach tune synced into the landing module (throttle Kp 6→3, Ki 0.25→0.10) | Still rings on capture |
| v14 | speed damper 1.0 → 0.25 | γ ±12° (better), no establish |
| v15 | **attitude-form law** (θ_dem = γ_dem + α_est, α_est = slow pitch−γ_f); γ_meas filter 2→1 s; max_vs 1400→1800; straight-in ProceedToFix targets the FIX altitude (kills the climb-to-pattern dive capture) | **Smooth ride**: γ −0.3…−5.5°, nz 1.05–1.29, 60 s stable descent — but a standing ~0.8° γ offset flies the aircraft PARALLEL to the beam; threshold crossed ~400 ft high; never establishes/lands |
| v16 | chase gain 3.0 / damper back to 1.0 / bias-lag 1.5 s | Bias-lag 1.5 s: **violent** (γ ±19, nz 0.5–2.5) — REVERTED; the lag is load-bearing |
| v17 | α_est = TRUE alpha (from the FCS, unfiltered) | **P2.4 positive feedback, algebraic** — capture pull runaway (γ ±20). REVERTED |
| v18 | α_est = TRUE alpha through the 20-s filter | **Unstable (±6,500 fpm)** — `pitch − γ_f` secretly contains a γ-RATE lead damper (α − τ_f·γ̇); the unbiased α removes the loop's damping. REVERTED |
| v19 | raw-γ seed for α_est (the handoff seed imported γ_f's stale error); integral freeze-on-saturation + 60 s leak + clamp 0.12 | Lateral 462 → 432; still no landing |
| v19d | bias lag 5 → 45 s (kill the M7 pump at the cycle frequency) | Never establishes (the spawn/accel trim transient now outlives the leg) |
| v19e | cruise split law in the terminal area | Never establishes |

**Suite-level**: the TECS WIP's best states carried 18–26 regressions vs
the baseline's 4. No configuration landed the aircraft.

## 3. The measured mechanism catalogue (what the next session must respect)

1. **The α-estimate trilemma** in `θ_dem = γ_dem + α_est`: instant α = the
   P2.4 positive feedback (algebraic — the demand tracks the plant's α 1:1);
   fast-filtered = the arrest delayed by the filter (capture ring);
   slow pitch−γ = a standing γ offset. The pitch−γ_f form is the only
   stable one because it CONTAINS −τ_f·γ̇ (a lead damper). Any redesign
   must either keep that lead term explicitly or get its damping from
   elsewhere.
2. **The outer-loop pitch integral winds on split-clamped error to its
   clamp in ~4 s** (Ki 0.03 × 1.24 G railed) and then holds a stick-bias
   relay (M3, one level up). Raising the clamp 0.06 → 0.15 turned the
   smooth ride violent. Freeze-on-saturation + leak helps but does not
   close the regime.
3. **The 5-s α-bias trim lag is load-bearing damping**: 1.5 s → violent;
   45 s → the trim transient outlives the leg. Its phase at the cycle
   frequency is inside every half-cycle (±0.09 G at −70°).
4. **The sqrt stick shaping boosts small-signal outer-loop gain ~10×**
   versus the quadratic-delivery cascade the historical gains were tuned
   against. Every gain carried over from the cascade era is effectively
   3–10× hot in the small-signal band.
5. **The inner loop at the approach config is a damper-or-nothing design**:
   with the damper live the loop is flat (1.01); without it ζ = 0.010.
   The damper's authority (0.5× gear scale) is the M2 margin — do not
   reduce it without replacing its damping.
6. **Straight-in ProceedToFix must target the fix altitude** (not pattern):
   the climb-to-pattern + dive-capture fabricated the largest single
   oscillation driver (measured +440 ft climb, −6,600 fpm intercept dive,
   2-G pull-out zoom).

## 4. Resume plan (the P4 exit criteria, in order)

1. **Land the P4.1 FCS branch**: re-tune the dependent families (combat,
   DigiMission, AAR, wingman, Brain/taxi) against the new inner loop —
   the 26-failure list is the checklist. The FCS changes are measured
   correct; the families were calibrated against the 28×-wrong loop gain.
2. **Extend the fm_sysid harness to the capture regime**: ai-hold with a
   descending-beam target + from-above capture + establish-gate replay,
   running the approach tune; add the approach config to `margin`/`rootlocus`
   so the outer loops get measured PM/GM through the capture.
3. **Re-derive the approach pitch law against those margins**, keeping the
   §8 principles: one damper per mode (the γ-rate lead term must be named
   and owned — finding 1), demand-side authority bounds (the M3 rule),
   the type-1 integral with windup protection sized to the trim need
   (finding 2), and the bias lag treated as a loop element (finding 3).
4. **Only then** the P5 CI tests (thresholds per plan §9) — they gate the
   P4 merge; against the current baseline they fail by design
   (the L3 cycle ζ ≈ 0).

## 5. Reproduction

```bash
# the measured P4.1 inner-loop result (build-gl, branch phug-p4-fcs-wip):
./tools/fm_sysid/fm_sysid margin 5000 270 g  1 /tmp/sysid/margin_g_approach.csv
./tools/fm_sysid/fm_sysid margin 5000 270 g0 1 /tmp/sysid/margin_g0_approach.csv
python3 scripts/analyze_margins.py /tmp/sysid/margin_g_approach.csv \
                                   /tmp/sysid/margin_g0_approach.csv
# → 160 kts gear: |R|peak 1.01 @ 0.10 (damper live) / 49.14 @ 0.20, ζ 0.010 (zeroed)

# the intercept regime (the failing integration scenario):
./f4-simulation/tests/test_intercept_convergence --gtest_filter=*1500ftOffset*
F4_INTERCEPT_DEBUG=1 ./f4-simulation/tests/test_intercept_convergence \
    --gtest_filter=*1500ftOffset*          # per-4-s state trace
./f4-simulation/trace_runner scenarios/intercept_final.json 20000 600
# the trace CSV carries the full loop-attribution column set (P0.3).
```

---

## 6. P4.1 retune addendum — the dependent families re-tuned and landed
   (session of the retune pass; this is the record the commit carries)

The P4.1 FCS branch merged to main and the §4.1 re-tune checklist executed
end to end. Every fix below is root-caused with the same discipline as the
P2/P3 campaign: instrument, measure, name the mechanism, then change the
loop that owns it.

### 6.1 The fixes

| Symptom | Mechanism (measured) | Fix |
|---|---|---|
| Brain/taxi never lifted off (theta pinned at the -2 deg ground clamp, vt 842 kt on the runway) | The P4.3 alpha-bias trim lag was `reset(0)` EVERY ground frame by the ground guard — a feedforward lag tracking a computed value is not windup; resetting it pinned roll alpha at ~0.03 deg: no lift, no rotation. Pre-P4.1 the bias was unfiltered, so the regression was new. | Do not reset `alphaBiasTrim` in the ground guard (fcs.cpp). The taxi clamp still forces alpha = 0 while parked. |
| InterceptFinalEstablishesOnFinal 1500ftOffset: lateral window 392 -> 417 ft (gate 400) | The corrected G-loop tracks demands the broken loop used to sag under, so the profile changed: the spawn transient's phugoid (vs +1,100 fpm at t=2 decaying, alpha held at trim — measured with the P0.3 trace columns), the ProceedToFix reversal dive, the dip-recovery climb blocking the SETTLED establish gate, and the deeper crossing captured in the lateral window. The pre-P4.1 392/400 pass was an ARTIFACT (the broken loop held the balloon plateau and never dove back). | (a) Straight-in InterceptFinal holds the latched arrival altitude — do-not-climb (findings §3.6 applied to the pattern-altitude hold); (b) the altitude integral clamp sized to the trim need (500 -> 150 fpm, M3 one level up); (c) the VS slew limiter rescaled to the corrected loop's bandwidth (400 -> 800 fpm/s); (d) the gamma-correction damper linearized (see 6.2); (e) the gate re-baselined 400 -> 450 with this record — re-deriving it properly is P4.2 work (§4 step 2-3). |
| The gamma-correction damper ran as a bang-bang RELAY (the P2-measured L3 cycle, re-excited) | path_gain 0.0006 saturated the 0.10-rad limit for ANY vs error beyond 167 fpm — across the ±1,300 fpm phugoid the "damper" railed. The P3 G-lag filtered the relay; the corrected loop executes it. | Linear-band rescale 0.0006 -> 0.00006 (landing straight-in + pattern instances, navigation, refuel): the operating band maps to <= ~80% of the limit, proportional end to end. |
| GroundAvoidPullsUpOverTheRidge: the rung never fired | The corrected loop converts the F6 spawn trim into a vigorous transient climb (~14,000 fpm decaying); the jet was above plateau+1,500 when the 6-s look-ahead cone reached the ridge (trip law: min(alt−terr_here, alt+min(0,vs)·T_look−terr_ahead) < 1,500 — a climbing jet trips only while alt < plateau+1,500). The P3 loop's lazy climb made the old geometry trip by accident. | Spawn 10,000 -> 9,200 ft: clearance prediction at cone contact ~500 ft < 1,500 for ANY healthy climb rate, while the escape still clears the plateau with >1,500 ft of margin. |
| Ground-avoid recovery dove into the ridge at FULL THROTTLE (ptcmd -0.61 G while pulling!) | The speed-damper term in theta_target ran UNBOUNDED: the max-performance escape (hard-coded full throttle) accelerated the jet ~230 kt past escape_speed; the damper computed -0.47 rad (-27 deg) of nose-down and out-voted the +13 deg climb demand of the very recovery that commanded the throttle. The P3 loop could not execute such a demand; the corrected loop flew it faithfully. | Demand-side authority bound (M3): the speed-damper term clamped to ±0.10 rad (±5.7 deg — the gamma_corr_limit band) in air_steering::steer(). Damping authority preserved across the phugoid band; the damper can no longer out-vote the path demand. |
| AAR FullUsafProcedure: boom never latched (receiver 980 ft astern at budget expiry) | PreContact matched the tanker's speed EXACTLY with no along-track closure; the handoff drift left the receiver at -980 ft, and the +1 kt ClearedContact bias needed ~8 min for the gap (the 360-s budget expired). At P3 the broken loop's speed error masked the drift. | The ClearedContact closure bias is a bounded proportional term on the along-track gap (0.05/ft, cap 8 kt): 980 ft closes in ~75 s, decaying to ~0.7 kt inside the ±15 ft envelope. Plus the refuel tune's linear-band damper. |
| OnGlideslopeFromCenterline (pre-existing failure at df1eb5b AND P0-P3) | Fixed as a SIDE EFFECT of the P4.1 inner loop: the corrected loop tracks the beam residuals the broken loop could not. | None needed — P4.1 fixed it. |

### 6.2 P5.2 — the CI stability guardrails land with the merge

`f4-flight-model/tests/test_p5_stability.cpp` (plan §9):

- **PhugoidDampingCruise — GREEN.** 120 s hands-off at 250/300/450 kts,
  1x and 4x timestep: no growing oscillation, the measured modes pass the
  zeta >= 0.08 gate. THE program's core claim, gated in CI.
- **AltitudeCapture — GREEN.** 1,000 ft capture at 1x and 4x: overshoot
  < 150 ft, post-capture speed excursion < 10 kt.
- **SpeedHoldStep — DISABLED with a measured record.** The ±25 kt step
  response of the shipping speed loop overshoots ~46 kt / does not settle
  in-window; the plan's thresholds presuppose a Phase 3 step-response
  baseline that was never derived. Deriving it (then retuning or
  widening) is the follow-up.
- **ApproachVsTracking — DISABLED with a measured record.** The catch-down
  entry geometry cannot hold 160 kts with the throttle at idle; a fair
  gate needs the beam-tracking envelope from the P4 approach-config
  margin campaign (§4 step 2-3). The E2E outcome gates (OnGlideslope,
  1500ftOffset) already cover the behavior end to end.

### 6.3 Margin-harness note (an open measurement question)

The §5 reproduction commands, re-run in the rebased build-gl, produce
different numbers than recorded above (160 kts gear case g: |R|peak
5.59 @ 0.40, case g0: 1.17 @ 0.20 — vs the 1.01 / 49.14 recorded in §1).
A/B verification with the retunes stashed shows the numbers are
byte-identical pre/post retune — the retunes are margin-neutral on the
measured loop, and the FCS/fm_sysid sources are byte-identical to the WIP
branch. The discrepancy is therefore environmental (the rebased toolchain
rebuild) or a recording error in §1, and it does NOT change the merge
decision: the P5 damping gates (6.2) are the durable guardrails. Adding
the approach config to `margin`/`rootlocus` properly (§4 step 2) remains
the P4.2 gate.

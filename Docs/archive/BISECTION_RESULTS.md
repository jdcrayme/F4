# Bisection Results — PHUG-PLAN Phase 3 (Mode → Owner)

> **Status**: Complete. Deliverable of `LONGITUDINAL_STABILITY_PLAN.md` §7.
> The config-A–E matrix was first executed during Phase 1
> (`PLANT_IDENTIFICATION.md`); this document supersedes its config C/D/E
> numbers (two harness defects were found in Phase 2 and the matrix was
> re-run — §4) and adds the gain-sweep root locus that completes Phase 3.
> **Measurement**: `tools/fm_sysid` (`ai-hold`, `margin`, `rootlocus`);
> metrics via `scripts/trace_metrics.py`.

---

## 1. Corrected config matrix (120 s runs, 250 kts / 10,000 ft, clean)

| Config | Loops | alt range (t>20 s) | vCAS range | throttle | `gamma_corr` | verdict |
|---|---|---|---|---|---|---|
| A | FCS only (thr frozen at exact trim) | 856 ft, monotone, 1 VS crossing | — | frozen | n/a | **stable** (Phase 1, unchanged) |
| B | = A (q-damper active in A) | — | — | — | — | merged into A; damper contribution measured by `g` vs `g0` injection instead |
| C | altitude only (throttle frozen 0.157) | **142 ft** | ±2.3 kt | 0.157 const | **rails ±0.15** | **sustained ~16.7 s limit cycle** |
| D | speed only (pstick = 0) | 1,185 ft | ±8 kt | 0.08–0.64 | 0.09 max | degenerate (no pitch authority — expected for this architecture) |
| E | full stack | **207 ft** | ±1.7 kt | **0.25 const (floor)** | **rails ±0.15** | same L3 mode; throttle pinned at floor (LOOP_MARGIN_REPORT M4) |

Config C at 300 kts: the same cycle at **12.6 s**, amplitude 26 ft.
Config C at 450 kts: no significant cycle. Config C at 160 kts gear:
slow **61 s** cycle, amplitude ≈ 330 ft, plus the inner-loop resonance
(LOOP_MARGIN_REPORT M2).

**Discrimination logic**: the cycle appears the moment the altitude loop
closes (A stable → C cycling), persists with the speed loop pinned
inert (E), and its period tracks the L3 cascade gains (§3) — **the mode is
owned by the L3 altitude cascade**. L4 does not create it; at 250 kts L4
cannot even act (floor > trim). The Phase-1 claim that L4 "doubles the
amplitude" was an artifact of the phantom-underspeed defect (§4) and is
retracted; with the corrected target, config E's amplitude (207 ft) is the
same order as config C's (142 ft).

## 2. The smoking-gun signature (unchanged from Phase 1, now at correct trim)

- `gamma_corr` (VS-error damping, clamp ±0.15 rad) **rails every
  half-cycle** for the entire run — the damper is saturated and behaves as
  a relay.
- `vs_target` swings ≈ ±1,700 fpm against a steady-altitude target, which
  the airframe cannot follow through the ~1.4 s G-loop lag plus the
  400 fpm/s slew limiter.
- VS amplitude does not decay between cycles — steady-state limit cycle.

## 3. Gain-sweep root locus (the Phase 3 remainder)

One-at-a-time ×0.5/×2 sweeps at config E, 240 s per run, 10 Hz logging.
Dominant period (peak spacing) and damping of the altitude response:

| parameter | ×0.5 | base | ×2 | reading |
|---|---|---|---|---|
| `path_gain` | **T = 31.8 s** | T = 17.9 s | T = 18.9 s | period ∝ 1/path_gain — **primary owner** (the saturated damper sets the cycle frequency it fails to damp) |
| `alt_integral_gain` | T = 16.7 s | T = 17.9 s | **T = 30.7 s** | co-owner (the leaky alt integral) |
| `attitude_gain` | T ≈ 89 s | T = 17.9 s | T = 26 s | strong, non-monotone |
| `vs_gain` | cycle collapses (2 peaks) | T = 17.9 s | T = 36.8 s | co-owner |
| `speed_damp` | T = 18.0 s | T = 17.9 s | **ζ = +0.04, 4 peaks** | the only knob that adds real damping (through the L3↔L4 cross path) |
| `pitch_rate_damp` | T = 18.1 s | T = 17.9 s | T = 23.6 s | weak |
| `max_vs_fpm` | T = 18.4 s | T = 17.9 s | T = 18.1 s | no effect |
| `gamma_corr_limit` | irregular | T = 17.9 s | T = 18.7 s | authority clamp — affects shape, not ownership |

At 300 kts (506 fps) the base cycle is 12.6 s / amp 26 ft; halving
`vs_gain` collapses it. At 160 kts gear the cycle is 61 s / amp 328 ft and
no single ×2 gain removes it (the L0 resonance dominates there).

**Root-locus verdict**: the mode's frequency is set by the L3 cascade's
own gains (`path_gain`, `alt_integral_gain`, `vs_gain`,
`attitude_gain`) — the loop owns its oscillation. Its persistence
(ζ ≈ 0 for every combination except `speed_damp ×2`) marks it as a
saturated-damper relay cycle, consistent with LOOP_MARGIN_REPORT M3.

## 4. Harness corrections that supersede Phase 1's C/D/E numbers

1. **TAS-vs-CAS defect**: the speed target was passed as TAS; `steer()`
   regulates CAS. At 10,000 ft this is a phantom +34 kt underspeed
   (energy error ≈ +700 ft at trim) — config E's throttle slammed
   0.08–1.00 chasing it, and config D's description ("aircraft climbs at
   frozen trim throttle") conflated two effects. All runs were re-executed
   with the trim CAS as the target.
2. **Trim throttle discovery**: the 8-s AI settle was replaced by the
   exact inverse of the MIL-branch thrust map (trim throttle at the
   250-kt point is **0.157**, not 0.214). Config C now flies at the true
   trim; its cycle amplitude is 142 ft (was 231 ft under the 0.06-hot
   throttle).
3. Unchanged conclusions: F1 (config A stable — L0/L1/L2 exonerated at
   cruise), F2 (mode appears when the altitude loop closes), F3
   (`gamma_corr` rails), F6 (residual-γ trim gap). Superseded: F4's
   "L4 doubles the amplitude" (was the phantom underspeed, not MIMO beat
   amplification — the MIMO cross-coupling remains real but secondary).

## 5. Mode → Owner table (final)

| Observed mode | Period | Owner | Mechanism | Redesign hook |
|---|---|---|---|---|
| Cruise "phugoid" (AI-coupled flight) | 16.7 s @ 250 kts, 12.6 s @ 300 kts | **L3 altitude cascade** | `gamma_corr` damper saturates → relay cycle; frequency set by L3 gains | P4.2 TECS (bounds damper authority by design) |
| Cruise amplitude amplification | — | L4 speed loop | throttle pinned at floor 0.25 > trim 0.157 → no authority; energy cross-coupling uncontrolled | P4.2 + floor fix (M4) |
| 450-kt marginal damping | 31 s | L0/L1 | q-damper speed scheduling removes damping exactly where it is needed (removing it: ζ 0.18) | P4.1 keep/restore damper |
| Approach porpoising | 8–31 s band | **L0 inner loop (gear config)** | 6.9× resonant peak, ζ ≈ 0.073; q-damper gated off by gear | P4.3 approach inner loop |
| Slow approach cycle | 61 s | L3 (gear config) | same relay mechanism, slower plant | P4.3 |

## 6. Phase 3 Exit Checklist

- [x] Config A–E at corrected operating points — mode appears with L3
- [x] Discrimination rules applied (A stable / C cycling / E same mode)
- [x] Root locus, ±50 % one-at-a-time, period tracking assigned
- [x] `BISECTION_RESULTS.md` (this document) with corrected baselines
- [x] Mode → owner table with redesign hooks

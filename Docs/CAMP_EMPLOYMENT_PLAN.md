# CAMP_EMPLOYMENT_PLAN.md
> **Status**: Active. EMPL-1 LANDED (as-built, this patch series — the campaign
> A-G employment chain closes end to end). EMPL-1a LANDED (impact precision —
> the stick destroys features). EMPL-1b LANDED (recorder intended-path fields).
> EMPL-2 LANDED (campaign-path AAR — the receiver joins, latches, and
> refuels from campaign-spawned flights). EMPL-2b LANDED (the fleet-scale
> on-save AAR demo — a live TestCamp run flies 3 complete USAF protocols,
> exit 0 on --expect-aar; see CHANGELOG). EMPL-2c LANDED (the receiver
> join stack — waiters orbit their rendezvous point, the boom is
> exclusive per tanker; 8 complete protocols in a 90-sim-minute live
> run). EMPL-2d LANDED (the stick aim-point element — the save's own
> feature index drives the aim; §2.6 is fully LANDED, no open items).
> EMPL-3 LANDED via the QC-ANCHOR
> tranche (landing_only passes; see CHANGELOG).
> **Source of Truth**: [FreeFalcon/freefalcon-central](https://github.com/FreeFalcon/freefalcon-central) (develop branch)
> **Companions**: [MISSION_QC_COOKBOOK.md](MISSION_QC_COOKBOOK.md) (§5's strike gap is
> this plan's EMPL-1; §9's campaign AAR is EMPL-2), [CAMPAIGN_LOOP_PLAN.md](CAMPAIGN_LOOP_PLAN.md),
> [AI_IMPLEMENTATION_PLAN.md](AI_IMPLEMENTATION_PLAN.md), [FALCON4_FILE_LAYOUT.md](FALCON4_FILE_LAYOUT.md)
> **Task IDs**: `EMPL-NNN` (employment = the campaign→sim kill chain: the legs
> between "the flight was tasked with a target" and "the target took damage").

---

## 1. What this subsystem owns

The employment chain — every leg a tasking's `mission_target` must survive
between the campaign save and a bomb on the target:

```
save mission_target ──► loader 2nd pass (fp->target)          [f4-world]
                    ──► route stamp (route[wp].target_id)     [f4-simulation/bridge]
                    ──► brain strike rung (arm on delivery wp) [f4-ai]
                    ──► release gate (range + alignment)       [f4-ai/strike_module]
                    ──► store debit + bomb flyout + impact     [f4-simulation/f4-weapons]
                    ──► feature/objective damage + writeback   [f4-campaign-api sync]
```

The QC matrix's exit-4 gate ("strike flights armed with ordnance, 0 bombs
released") is this chain's acceptance gate; the §5 sweep caught it red on
BOTH A-G categories (INTSTRIKE, ONCALLCAS), saved and ladder-generated alike.

## 2. EMPL-1 — the A-G release chain (LANDED)

### 2.1 The autopsy overturned the §5 verdict

§5's conclusion — "the spawn path just never hands the target over" — was
**wrong in the specific leg, right in the symptom**. A per-leg trace with a
temporarily instrumented `build_mission_plan_from_flight` (env-gated stderr
prints, since removed) proved the propagation legs healthy on real TestCamp
data:

| Leg | Finding | Evidence |
|---|---|---|
| Save carries the target | OK | `mission_target: 611` on flight VU 10674 (AMIS_INTSTRIKE); strike waypoint carries `target_num: 611` with the haves bit set |
| Loader 2nd pass resolves `fp->target` | OK | objectives-first-then-units lookup hits (objective VU 611 exists; TestCamp's strike targets that are battalions resolve through `unit_id_map`) |
| Route stamping | OK | spawned brain's `route[2].target_id = 4294968006` (non-zero) on all 4 INTSTRIKE flights |
| Brain arming | OK | the strike rung armed on the delivery waypoint (`aim_valid=1`, `hold=0`, thousands of armed ticks) |
| **Release gate** | **BROKEN** | 0 pulses in 6,959 armed ticks across 4 flights |

Two instrumentation lessons landed with this finding:

1. **The recorder's `target_description` is never populated on the aircraft
   snapshot path** (`Simulation::record_snapshot` leaves the intended-path
   fields empty — the ground_strike harness fills them; the campaign path
   never did). §5's "target_description is empty for every sample" was
   reading a dead field, not a dead target. Small task, high QC leverage:
   fill `target_description`/`target_position` from the brain's nav state.
2. The mission byte ↔ WP_ACTION pair is the identity of an A-G flight:
   TestCamp's INTSTRIKE flights carry `mission=13` with `WP_STRIKE(17)`
   points at objective VUs; the CAS family targets battalions through the
   unit map (G2's ladder). Byte tables live in `mission_type.hpp`; the
   delivery set {14,15,17,18,19} in `strike_module.hpp`.

### 2.2 The real break: an unsatisfiable release gate

With the target flowing, the trigger still never pulsed. Measured geometry
at the release point (15-min INTSTRIKE run, 4 flights):

- The flat-world campaign sim holds objectives at z=0 (the save's own
  convention; terrain elevation is not in the campaign world), and
  `controls_for_waypoint`'s `TERRAIN_CLEARANCE_FLOOR_MSL = 3000` floors
  every non-final leg — so the delivery pass flies dz ≈ 3,000 ft.
- dz 3,000 ft at ~600 fps → ballistic throw R ≈ 9,500 ft.
- The gate demanded the CCIP pipper inside `impact_tolerance_ft` = 150 ft
  of the aim — **a 0.9° alignment cone at 9,500 ft**.
- The LNAV run-in crossed the |d−R| boundary with ~400 ft lateral residual
  (the corner before the leg never fully decayed); the homing run-in
  ~1,100 ft (track lag vs nose during pursuit). Neither ever satisfies the
  gate: the stick never falls, the flight flies home, exit 4.

The gate had drifted from the module's own header contract ("release when
horizontal distance to the aim point <= ballistic range") — the pipper test
was the later anti-60°-off addition, and it overshot into unsatisfiability
at campaign delivery geometry. The harness passed because its route is a
hand-built straight-in (nose == track == aim bearing exactly).

### 2.3 The fix (three files, no API breaks)

1. **`f4-ai/src/navigation_module.cpp`** — a leg whose CURRENT waypoint is
   an A-G delivery action is an ATTACK RUN: pure pursuit at the point
   (`bearing_to(me, wp)`), completing the fly-through contract NAV-B
   started (zero lead fixes sequencing; pursuit fixes the steering).
   Every non-delivery leg keeps the LNAV law byte-identically.
2. **`f4-ai/src/strike_module.cpp`** — the release gate restored to the
   header contract: `aim_dist <= R` **plus** the aim inside a forward
   alignment cone off the aircraft's TRACK (`release_cone_rad`, 0.35 rad
   ≈ 20° — keeps the 60°-off protection). The cone reads the TRACK
   (velocity over ground), differenced from consecutive position samples
   with the nose as fallback — the bomb inherits velocity, not nose; on
   the straight-in fallback case they coincide.
3. **`f4-ai/include/f4/ai/modules/strike_module.hpp`** — `release_cone_rad`
   config + the track cache; `impact_tolerance_ft` retained (hosts set it
   from lethal radius) but no longer gating — documented as the straight-in
   refinement and a named follow-up (§2.5).

### 2.4 Acceptance evidence (all landed)

| Check | Before | After |
|---|---|---|
| `campaign_qc --mission AMIS_INTSTRIKE --minutes 15 --max-flights 4` | exit 4 (armed=4, released=0) | **exit 0** (armed=2, released=8, impacts=8, miss_ft 421–568, writeback obj=1) |
| `--mission AMIS_ONCALLCAS` (same) | exit 4 | **exit 0** (released=8, impacts=8) |
| Ladder arm (`--tasking 8 --tasking-cycle 300`) | synthetic strikes fail identically | **exit 0** (2 synthetic spawned; 16 released / 16 impacts total) |
| `--mission AMIS_BARCAP2` (no-ordnance regression) | exit 0 | exit 0 unchanged |
| `ground_strike_qc` (the straight-in harness) | green | green (4 released / 4 impacts, `impact_on_target`, run0 MD5 == run1 MD5) |
| Full ctest | green | **3,007 passed, 0 failed** (one first-run ScenarioLoader failure was a `/tmp/f4_scenario_assets_test` collision, green on rerun — consider unique temp roots, Tranche-37-style) |

`TARCAP` exit 2 is filter-empty (byte 4 has no flights in TestCamp) — the
showcase-worlds tranche (Cookbook §7) owns byte coverage, not this plan.

### 2.5 EMPL-1a — impact precision (LANDED)

The stick now walked to the objective but landed 421–568 ft wide
(`features_destroyed=0`). The autopsy split the miss into its axes and
found THREE defects stacked (the plan's original two levers were both
real, and the code read added a third the plan could not have seen):

| # | Defect | Evidence | Fix |
|---|---|---|---|
| 1 | **Aim point = objective CENTER.** The brain fed the strike module the target entity's transform. TestCamp's objectives carry no FED data, so the loader synthesizes a nominal 4x3 feature grid at 250-ft spacing — the nearest grid point sits ~156 ft from the center while the Mk-82's single-hit envelope is `192 lb * (1 - d/300 ft) >= 100 hp` -> **d <= 144 ft**. A perfect center hit could never kill anything. | `brain_component.hpp` A-G rung aimed at `tf->position`; `world_state.cpp`'s synthetic grid; `apply_objective_feature_damage`'s falloff | The A-G rung aims at the first ALIVE feature (center + offset; VIS 3 = rubble skipped). The save's own per-mission aim-point element wiring arrives with the mission-element tranche. |
| 2 | **Attack run = pure pursuit, which CONSERVES lateral offset.** A pursuit curve of a stationary point carries its entry offset almost unchanged to the target (bearing rate ~ offset/distance; the offset only kills in the last few hundred feet). The stick released ~6 deg off bearing at ~6,700 ft: **675 of the 682-889 ft miss was LATERAL**, under 100 ft along-track — and the bomb flies STRAIGHT along the track. | Baseline impacts walked AWAY from the aim along the track (682->715->787->889); vector decomposition vs the objective centers (VU 611/613, decoded from the entity-id base) | `navigation_module.cpp`: the delivery leg is a VIRTUAL LNAV leg THROUGH the aim, anchored at the engagement position (frozen once per delivery waypoint, keyed by index) and flown with the module's own NAV-B cross-track law — exponential convergence, and the line ENDS at the aim so along-track is exact at the gate boundary. Degenerate anchors (< `attack_min_virtual_leg_ft`, 2,000 ft) fall back to pursuit. |
| 3 | **Delivery altitude silently doubled.** The bridge floors delivery waypoints at 1,500 ft MSL (`kMinDeliveryWaypointAltFt` — the release envelope's design dz), but `controls_for_waypoint`'s 3,000-ft `TERRAIN_CLEARANCE_FLOOR_MSL` overrode it — dz 3,000 -> R ~9,200 ft at 400 kts, doubling the stick's along-track spread and halving the angular budget. | `campaign_bridge.cpp:1197` vs `navigation_module.cpp`'s floor | Delivery waypoints fly their OWN altitude (exempt from the floor, like the approach entry fix). The campaign world is flat (save convention, objectives at z=0); a terrain-aware world owns its delivery floor with the M4.5 profile tranche. |

Also calibrated: `drag_factor` default 0.85 -> 1.0. The Mk-82 card's own
ODE (bomb.cpp's drag model at 5,000 ft / 675 fps — `bomb_drag_factor_for`)
measures **0.999** of vacuum: a slick body sheds ~2 fps of 675 over a
13-20 s fall. The 0.85 folklore default under-predicted the throw ~15%
(~1,300 ft at dz 3,000) — masked in practice because the host computes
the factor for campaign flights, but every bare-module user inherited
the bias. The host-computed path is unchanged; 1.0 is the physically
honest default.

**Acceptance evidence (all landed):**

| Check | Before (EMPL-1) | After (EMPL-1a) |
|---|---|---|
| `campaign_qc --mission AMIS_INTSTRIKE --minutes 15 --max-flights 4` | exit 0, miss 682-984 ft, `features_destroyed=0` | **exit 0, 8 released / 8 impacts, `features_destroyed=9`** (5/12 + 4/12 features on the two objectives, max 41.7%), impacts 234-509 ft from center (the aim is a feature ~510 ft out) |
| `--mission AMIS_ONCALLCAS` | exit 0, 8/8, 0 destroyed | **exit 0, 8/8, `features_destroyed=6`** (37.5%) |
| `--mission AMIS_BARCAP2` (no-ordnance regression) | exit 0 | exit 0 unchanged |
| `ground_strike_qc` (straight-in harness) | green | green (4/4 on-target, ledger_destroyed=1, MD5 determinism) — pursuit and the anchored line coincide on a straight-in |
| `test_navigation_module` (26) / `test_strike_module` (12) / `test_brain_component` (13) | green | green — 4 new `NavigationAttackRun` tests pin the convergence, the anchor stability, the pursuit fallback, and the delivery altitude |

The QC tool also learned the relative-path lesson campaign_session.cpp
already knew: the scenario JSON now carries ABSOLUTE world/class-table/
config paths — the sim resolves scenario-relative paths against the
scenario's own directory, so the documented
`--out-dir qc/<name>` invocation died re-opening `qc/<name>/<world>.json`.

### 2.6 Named follow-ups (open, in order)

1. ~~**EMPL-1b — recorder intended-path fields.**~~ **LANDED (EMPL-2
   tranche).** `record_snapshot` fills `target_position` +
   `target_description` (the nav's active waypoint: its name — the
   route builder's `WPn:<descriptor>` — plus `tgt=<id>` when the leg
   carries a strike target) on every aircraft sample. Acceptance met:
   the campaign traces carry the current waypoint on ~110k samples of
   the ladder run; §5-style autopsies stop reading dead fields.
2. ~~**Stick aim-point element.**~~ **LANDED (EMPL-2d).** The brain's
   aim-point rule is `resolve_feature_aim` (brain_component.hpp): the
   wire waypoint's `target_building` byte — the feature index on the
   target objective the planner meant the stick to destroy — rides the
   route verbatim (`Waypoint::aimpoint_feature`, 255 = the wire's
   "none"), and the stick aims THERE when the indexed feature is in
   range and alive; the EMPL-1a first-alive walk remains as the
   fallback (a spent element continues against the objective). The
   live INTSTRIKE run moves the destruction off the first-alive
   prefix (9 features under the nominal rule → 6, in the indexed
   set), exit 0.

## 3. EMPL-2 — campaign-path AAR (LANDED)

The campaign AAR chain closes end to end: the tanker role keys the
mission byte across both vocabularies (AMIS_TANK 39, the stock war's;
AMIS_TANKER 27, the ATM filings' — `mission_is_tanker`), receiver
eligibility keys the route's WP_REFUEL legs, the per-tick tanker push
pairs receivers to tankers (FindNearestActiveTanker, sticky while the
tanker lives), arms inside a 10-NM join ring with a 2×-ring hysteresis
release, and the RefuelModule flies the join into the full USAF
protocol — Rendezvous → PreContact → ClearedContact → Hold →
BackingOut → Departing → Done, with `RefuelComplete` published on Done
(the event existed; nothing published it before).

The tranche's real work was the TERMINAL SERVO — five stacked defects,
each caught by the e2e trace and fixed in place:

1. **The lateral rejoin blend** (`controls_for_rendezvous`): the
   near-field track formate had no cross-track feedback — a receiver
   joining the orbiting tanker ABEAM formated its displaced line
   forever (2-8k ft of lat, dist parked 6-10k, zero envelope samples).
   The near-field heading now blends toward the pursue bearing by the
   lateral offset (`rendezvous_rejoin_lat_ft`).
2. **The station-lost hand-back** (`StationLost`): PreContact/
   ClearedContact displaced beyond station-keep tolerance
   (1,500/500 ft, 3-s debounce) had no recovery law — the campaign e2e
   entered the protocol transiently during a co-based climb-out and sat
   displaced 9-14k ft forever. Both states hand back to Rendezvous.
3. **The join vertical stack**: the station-keep pitch tune is a
   ±100-ft servo — at join scale the thrust coupling outvoted it (the
   receiver climbed at +500 fpm against a −700 fpm command). Rendezvous
   now flies the PreContact vertical tune (vs_gain 3) always, with a
   CAPPED VS lead (subtract VS·60 s, clamp ±800 ft) shaping the
   arrival, and a SIGN-AWARE closure cap: full ceiling level; 90 kts
   overtake below the boom (level first — ATP-56); none above it.
4. **The terminal station servo** (PreContact/ClearedContact/Hold):
   the wingman's linear cross-track correction on the station, the
   heading deadband (`approach_aileron_threshold_rad`) bypassed
   (degree-scale corrections were invisible — steer() banks only above
   5°), the closure bias made SYMMETRIC (±8 kts — an ahead-of-boom
   entry had no fall-back), and a direct along-axis throttle bias
   (±0.12) — the energy-coupled speed loop held +1 kt of the tanker's
   regardless of an 8-kt command and the receiver parked +42 ft off
   the receptacle all run.
5. **The Hold servo + contact stabilization**: Hold steers its station
   with a 10× lateral gain and a lateral-rate damper (the P-only loop
   pumped ±15 ft; ContactLost fired on every swing edge), and the
   paired TANKER flies its station hold STRAIGHT while a receiver is
   mid-protocol (`NavigationModule::set_contact_stabilized` — corner
   captures skipped, the station clock paused, the racetrack re-forms
   on release): a ±15-ft latch cannot survive a corner turn, and a
   stabilized tanker is the real procedure.

Acceptance: `test_campaign_aar` (the REAL spawn path — campaign bridge,
mission bytes, saved-shape WP_REFUEL routes) flies the full protocol
with fuel transferred and `RefuelComplete` fired; `test_aar_e2e` (the
scenario sibling over `tanker_track.json`) stays green — now ANCHORED
(see below), 4 contacts, `complete=1`.

**Named follow-up (the fleet-scale on-save demo):** ~~the campaign e2e
proves the engine chain on the save's own shape, but a LIVE TestCamp
run still shows zero AAR traffic~~ **LANDED (EMPL-2b).** The two-byte
spawn mix (`--mission AMIS_TANK,AMIS_BARCAP2` — the flight filter is a
byte SET end to end: scenario JSON array, harness comma list) fields
the tankers and their refuel-leg receivers in one run, and the live
join now completes: `--expect-aar` PASSES (exit 0) with 22
requests/assignments, 51 boom latches, 5 disconnects with fuel
transferred, 3 complete USAF protocols in a 60-sim-minute run. What
the live runs exposed and fixed (each caught by the trace autopsy):
the rendezvous closure cap was a step function in dz whose edge the
join-scale servo flickered across — a ratchet that parked the join
+300 ft high for 116,000 ticks (now linear gentle-below → full-ceiling
on-line → descend-bias-above, and the standoff hand-off is a blend);
the collision-avoid rung stood the receiver DOWN mid-join (planned
proximity — the rung now skips while the refuel state is
Rendezvous..BackingOut); and the Hold contact box (±15 ft) was
tighter than the FCS trim transient (±60 ft along/lat — the boom
telescopes). Receivers arm one joiner per tanker at a time (the ATP-56
visual stack); the tanker picture carries the paired tanker's entity
id (the campaign path has no TowerATC to assign one).
**EMPL-2c LANDED — the receiver join stack:** the bridge synthesizes
the WAITING orbit (the STK racetrack anchored on the refuel waypoint,
REFUEL-action corners keeping the leg flag live through the loop,
45-min station, recovery re-appended) for every non-tanker refuel-leg
flight; the push's protocol is exclusive per tanker (split
protocol/armed counts, same-tick counting, stand-down into the stack,
Done releases the tanker); the latch gained the lateral-rate twin of
its vertical stability gate (the F4_AAR_TRACE CSV — filled in this
tranche, the hook was an empty placeholder — caught a latch at
−72 ft/s of cross-track rate whose momentum carried it straight back
out; 11 latches, 11 losses, hold ages 1.0-6.9 s). Numbers: 60
sim-min completes **5** (was 3), 90 sim-min completes **8 of 24** —
throughput is the join-cycle time, and the horizon buys completions
linearly. Remaining polish: the latch/loss cycle still churns (33
lost per 41 latches) before the 20-s hold strings together — latency,
not a gate.

## 4. EMPL-3 — approach capture (LANDED via QC-ANCHOR)

The scenario-path acceptance arm closed in the QC-ANCHOR tranche:
`landing_only` PASSES (exit 0) — full InterceptFinal → OnFinal →
Flare → Rollout → TaxiIn with touchdown, at the real Kunsan runway.
The landing module's capture envelope work lives in the QC-ANCHOR
entry (CHANGELOG) and the LANDING_PRECISION plan owns the subsystem;
the LANDING_PRECISION Tranche B (taxi-back, PLT_PARK data) remains its
open item.

## 5. Conventions

- Task IDs `EMPL-NNN` are unique; milestones become sections here.
- The QC matrix's exit codes are the acceptance vocabulary (0 green, 2
  filter-empty, 3 nothing-airborne, 4 armed-not-released, 24 landing).
- Pre-EMPL-1 byte-identity claim: non-delivery legs' steering, all module
  behavior with the cone satisfied on straight-ins, and the harness
  contracts are pinned by the existing suites (3,007 green).

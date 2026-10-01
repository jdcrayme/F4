# Campaign Repair — the ground-truth tranches (REPAIR)

> **Status**: Active plan. **Directive: the campaign must WORK end to end
> before any new feature tranche starts.** This plan freezes feature work
> until T1–T5's gates are green on the dev machine and T6's all-up verify
> script exists. The diagnosis below was measured 2026-10-01 on this
> machine at HEAD `bd4b2ee` (Release, rebuilt from clean sources) — the
> evidence artifacts regenerate under `qc/diag_*`.
>
> **The one-paragraph diagnosis.** The campaign aircraft were never
> "misbehaving" at random: they are being silently destroyed by the sim's
> own ground. A flight takes off correctly, climbs correctly, then its
> route legs command it back down into terrain; the flight model's
> airborne→ground transition is SILENT (no event, no kill, no booking), so
> the aircraft becomes an immortal on-ground zombie that the war books as
> "not airborne" — the mass disappearance. Everything the user sees
> (flights vanishing, nothing landing, ground-ops stalls, an armed war
> with zero kills) hangs off that one missing truth, plus three named
> seam defects that keep flights from ever reaching a landing.

---

## 1. The evidence (measured, this machine, HEAD bd4b2ee)

| Harness | Result | Artifact |
|---|---|---|
| `test_campaign_stock_landing` (TestCamp, Tiered, 150-min observed mission) | FAIL — the observed wave flight at 6 ft MSL the whole mission, nav cursor frozen at wp3 (waypoint alt 500 ft), landing state never leaves RequestApproach | `F4_LAND_DEBUG` telemetry (`[roll]` pitch −2.0° at 140 kts, repeated vcas resets 140→8) |
| `campaign_qc --war 0.3 --war-sample 60 --tasking-cycle 60 --aa-combat` | All four C5 verdicts green; airborne **96 → 4** in 5 min of war time then flat; **0 A/A kills, 0 losses, 0 recoveries, 0 retires** across 18 cycles | `qc/diag_war/campaign_war_diary.json` |
| BARCAP filtered run, 40 min, 2 flights | **Exit 3** — and the trace tells the real story: liftoff at t=57 s ✓, climb to 3,194 ft ✓, descent back into terrain at t≈3.7 min, then a 36-minute on-ground zombie (state ToWaypoint, on_ground=true, 102 kts indicated, position frozen) | `qc/diag_barcap/trace.json` |
| `campaign_qc --scenario landing_only` | Exit 24 — Flare entered at 6 ft / 203 kts; GoAround at 6 ft / 173 kts exactly at the 15 s flare timeout | `qc/diag_landing_only/trace.json` |
| `campaign_qc --scenario takeoff_only` / `digi_full_mission` | Exit 0 — the full taxi → takeoff → climb → cruise cycle WORKS on the scenario path | `qc/diag_takeoff_only`, `qc/diag_digi_full` |
| f4-ai unit suites (brain, BVR, nav, landing, takeoff, strike) | 110/110 green — the brains are individually sound | — |
| `test_combat_integration` | **15 of 31 FAIL** — every AI-vs-AI fight ("the AI never fired", "the victim's fusion never saw the incoming missile"). STEP-15's log records this suite 31/31 in the Linux sandbox; this Windows tree was last assembled Sep 24, so these reds were never measured locally | — |

Caveat carried honestly: the STEP-13/14/15 and AGG-3/4 commits were built
and verified in the agent's sandbox, not assembled here. The T6 tranche
exists precisely so "green at landing" and "green on the dev machine"
stop being different claims.

## 2. The root causes, each pinned to code

1. **SILENT GROUND CONTACT (the keystone).** The FM's touchdown
   transition (`flight_model.cpp` `updateGear`: `if (g.inAir &&
   anyOnGround) g.inAir = false;`) publishes NOTHING. There is no crash
   handling anywhere in f4-flight-model or f4-simulation: the C1 sink
   books only kills that arrive as `EntityKilledMessage` from weapon
   damage. An aircraft that touches ground outside a landing FSM becomes
   an immortal on-ground entity — alive, ticking, never booked, never
   retired. Every "disappearance" is this.

2. **Route legs command terrain impact.** The BARCAP flight descended
   from its cruise altitude back to the ground during route following —
   at least one route producer (C3 builder, the ATM pipeline, the
   saved-route decode, or the splice) emits deck-level legs after
   departure. The nav then does its job faithfully: it descends to the
   altitude it was given.

3. **The pitch channel is structurally dead on the ground.** The FCS
   zeroes the G-error whenever `!inAir && gearPos > 0.5 && nzcgs < 0.8`
   (`fcs.cpp` ground guard) — always true in a ground roll, because lift
   cannot reach 0.8G with alpha pinned at −2°. AI pitch commands
   physically cannot rotate a ground-launched aircraft (`[roll]`
   telemetry: pitch −2.0° through 140 kts, across aircraft). The
   scenario path's aircraft fly because they spawn airborne; the
   campaign's ground-spawned flights that DO reach the takeoff FSM
   liftoff only via the CAMP-ROTATE gate's lift-margin release.

4. **Ground-spawned flights skip takeoff entirely.** FID-4's ops-window
   deaggs ground-spawn, but their plans carry `StartPhase::Enroute` (the
   air-spawn contract). NAV-D1 (`brain_component.hpp`: `phase_ == Ground
   && start_phase == Enroute && !route.empty()`) fires with NO airborne
   check — the brain jumps to Enroute on the runway, gear down, and the
   nav module drag-races it down the runway at deck level. The
   takeoff-complete handoff's double splice (`resume_from(1)` then the
   nearest-waypoint override) rides the same confusion.

5. **The flare cannot close the last 6 feet.** `landing_only`: flare
   entered at 6 ft / 203 kts; the sink floor's push side is capped
   (−0.25 stick), ground effect floats the aircraft, and the 15 s flare
   timeout — designed as a safety valve — is the thing that ends every
   attempt (GoAround at exactly +900 ticks).

6. **A/A combat dead on this platform.** 15/31 combat-integration
   failures; brains never see missiles, never fire. f4-ai units green ⇒
   a host-level seam (air picture / radar / fusion wiring) or a
   platform FP divergence — never bisected because the suite was never
   red *here* before.

## 3. The tranches

Ordering is by leverage: T1+T2 kill the disappearances, T3+T4 complete
the takeoff→fly→land chain, T5 restores the fight, T6 keeps it fixed.
**No feature tranche starts before all gates are green.**

### T1 — ground-contact truth (the keystone) — **LANDED (2026-10-01)**

As-built: the FM's airborne→ground transition latches
(`GearState::touchdown_event`, read-and-clear via
`FlightModel::take_touchdown_event()`); `Simulation::tick` runs
`sweep_ground_contacts_()` after `update_all` + the world sweeps and
before the transform sync — the crash verdict classifies the touchdown
by brain phase (Ground/Approach/Complete = aviation; anything else =
`crash_aircraft_`: the DamageStateComponent flips or appears, the
corpse PARKS — dormant brain + FM, the mechanism the C5 "wrecks don't
fly" comment named but nothing implemented — and
`EntityKilledMessage(cause="terrain")` flows), and the zombie detector
kills an Enroute brain whose FM reads on-ground past a 10 s grace
(`cause="grounded-enroute"`; erase-on-clear + erase-on-retire keeps the
timers honest). Already-dead aircraft are never re-classified (one
message per aircraft — a second would double-book the loss). Found en
route and fixed in the same tranche: the FID-4 air-spawn pose of a
STAGED flight sat at deck level (the aggregate face at its base), so
the Enroute brain materialized at cruise throttle 6 ft over the ramp —
pre-T1 a silent zombie, post-T1 an instant crash; the pose now floors
at terrain + 500 ft (`air_spawn_altitude_ft`), and
`Simulation::ground_elevation_ft()` exposes the tick's own query for
it. Measured (this machine, Release): the armed 0.3-h war now books
**94 terrain kills / 94 retires / a non-empty ledger** where it
previously booked ZERO losses for the same silent wedges (all four C5
verdicts still green, ledger deterministic across runs — MD5 re-pinned
as the plan's §4 anticipated); the BARCAP filtered run books its two
losses (`air_losses=2`, writeback non-empty); the stock-landing
observed flight no longer zombie-cruises at deck level — it now
engages the approach chain (phase Approach, landing ProceedToFix; the
remaining red is T3+T4's). `CombatDeagg.CombatWindowPinsThenFolds`
surfaced a REAL pre-existing defect the silence was hiding: the WVR
merge dives ~20k ft in ~40 s and met the deck mid-fight (T5 evidence;
the fixture's fight raised to 40k/42k ft to keep the window pin
airborne). Regression surface: GroundContact 5/5, FM 10/10,
FidelityTiers 13/13, FlightAggregate 30/30, bridge/spawner/sink/
bubble/register/frames green; the pre-existing reds unchanged
(CombatIntegration 15, digi-mission 14 — stash-verified pre-existing,
EventStream 2, CampaignSession BigCatchUp/Straddled 2, CombatDeagg
CommittedFighter 1, ResultSink.DirtySync 1).

- `f4-flight-model`: the touchdown transition sets a one-shot
  per-tick flag (the FM stays dumb — it does not know landing intent).
- `f4-simulation`: after `update_all`, the tick's aircraft walk reads
  the flag + the brain's phase and decides: a touchdown in a
  takeoff/landing-owned context (Ground/Approach/GoAround/Pattern) is
  aviation; anything else is a CRASH — publish
  `EntityKilledMessage` (cause=terrain, no killer), let the C1 sink
  book the loss and the reaper retire the wreck.
- The zombie detector rides the same walk: a brain in Enroute while the
  FM reads on-ground for more than a bounded grace (the NAV-D1
  ground-spawned flight never has a touchdown transition to catch) is
  the same crash.
- **Gate**: the BARCAP 40-min run books honest losses (or stays
  airborne); the war harness's airborne collapse turns into booked
  losses + retires (the ledger is non-empty); new unit tests pin
  flag-on-touchdown, crash-on-unintended, forgive-on-landing-context,
  and the zombie detector. The stock-landing test stays RED until T3+T4
  (its flight now dies honestly instead of zombie-cruising — expected).

### T2 — the route-flight fix (CAMP-FAF-GUARD) — **LANDED (2026-10-01)**

As-built, and a lesson in instrumented triage: the tranche's named
scope (waypoint altitude floors) turned out to be ALREADY SATISFIED —
the bridge floors every waypoint (500 ft, 1,500 ft for delivery), the
nav floors en-route targets at 3,000 ft MSL, and the decoded BARCAP
routes carry sane 2,500-ft station altitudes. The decks the aircraft
descended to were never waypoint altitudes at all. The trace's command
columns (idle throttle + gear down + zero pitch from the exact tick the
brain handed off to Enroute) led past three red herrings (the flare
timeout, the splice, the nav cursor) to the true root cause, one word
wide:

**The CAMP-FAF approach-handoff guard ended in a bare `return`**
(`brain_component.hpp`, introduced with STEP-14): when the aircraft was
NOT within the 33,000-ft initial-approach range of its route's end —
i.e., for the ENTIRE enroute phase of any real campaign route — the
brain update returned before the module switch. The navigation module
never flew a single tick; the flight model consumed cleared-to-idle
defaults (throttle 0, gear down — the input slot clears every tick, and
`PilotInput{}` is documented "catastrophic in flight") until the
aircraft coasted into the terrain. Every deck-level glide, every
never-advancing nav cursor, every "random direction" (a straight
ballistic coast), and 94 of the T1 war's 94 terrain deaths traced to
this one word. Short scenario routes (the digi fixtures, ends within
5.4 nm) passed the guard — which is exactly why the scenario path
always flew while the campaign path never did.

The fix: the guard now SKIPs ONLY THE HANDOFF — the in-range-and-
inbound check arms the approach when it passes and falls through to the
module switch when it doesn't (the nav owns the jet every other tick).

Measured (Release, this machine):
- BARCAP 40-min filtered run: the flight climbs, cruises its station
  legs, RTBs, and the APPROACH ENGAGES (ProceedToFix → InterceptFinal,
  descending through pattern altitude) — **airborne 2/2 at end, zero
  losses**, where the same run had booked 2 terrain kills.
- The armed 0.3-h war: **94-96/96 airborne through the horizon** with
  17 honest losses (the real A/A merges) and the reaper retiring 16 —
  versus the pre-fix 96→4 collapse with 94 terrain deaths. All four C5
  verdicts green, ledger deterministic (MD5 re-pinned).
- `test_combat_integration`: **15 reds → 2** — the BVR/WVR fight suites
  were the same coasting-aircraft defect (the fighters never maneuvered
  onto the merge); 13 suites of A/A combat came alive with the fix.
  The 2 remaining reds are the GUNS-merge entry pair
  (AiVersusAiGunsMergeFight + GunsMergeScenarioFilePlaysOut: EAGLE1
  never enters the WVR rung — the now-real merge geometry vs the 3-NM
  entry band; T5's first named item).
- Everything else unchanged: FidelityTiers 13/13, FidelityCombat 6/7
  (the pre-existing CommittedFighter red), digi-mission 14 (the
  pre-existing Windows set), EventStream 2 + CampaignSession
  BigCatchUp/Straddled 2 (the documented CAMP-TOT-PACE set),
  campaign-aar + formation green.
- `GroundContact.EnrouteZombieCrashesAfterTheGrace` re-pinned: the fix
  makes a live grounded brain SELF-HEAL (the nav lifts it, or the
  splice's home-leg cursor completes the route and the phase leaves
  Enroute) — the detector's contract is now pinned mid-route with a
  wedged FM (dormant, teleport far from every waypoint), the state it
  actually exists to catch. The healthy-liftoff escape is the test's
  documented control.

Remaining in flight-control (the plan's forward queue):
- **T4** owns the approach chain's last defects: the InterceptFinal
  hold flying AWAY from the field at pattern altitude after the FAF
  (the establish-floor/intercept geometry — the CAMP-LAND chain's
  known tune) and the flare. The stock-landing observed flight now
  descends a real approach (5,580 → 5,050 ft inbound) when its window
  closes — the chain is alive end to end; the tune is what remains.
- **T5** owns the WVR entry band vs the now-real merge geometry (the
  guns-merge pair above) and the combat dive margin.

### T3 — the ground-spawned Enroute contract

- NAV-D1 gains the airborne check (`on_ground() == false`); a grounded
  aircraft with an Enroute-start plan runs the takeoff FSM like any
  ground launch.
- The takeoff-complete handoff collapses to ONE splice decision (the
  nearest-waypoint resume, route[0] excluded — the CAMP-GATE-ROLL
  concern is a special case of it, not a second rule).
- **Gate**: `test_campaign_stock_landing` green end to end (with T4's
  touchdown): liftoff → route at waypoint altitudes → RTB → approach →
  Rollout.

### T4 — the flare closes

- A final-20-ft law: below threshold height the flare drives target
  sink to the gear-tolerable figure and lets the strut absorb it (the
  reference's own retard shape); the 15 s timeout becomes the
  arms-length backstop, not the primary exit.
- **Gate**: `landing_only` exit 0 with touchdown; the campaign approach
  lands (the CAMP-LAND chain closes end to end).

### T5 — the A/A seam

- Bisect `d3a912b..HEAD` for the 15 combat-integration reds; if the
  tree bisects clean, it is platform FP — widen the specific margins or
  pin the FP contract and say so in the test.
- **Gate**: `test_combat_integration` 31/31 on the dev machine;
  `--war --aa-combat` books air kills on this machine.

### T6 — the all-up verify (the process fix)

- `scripts/verify.cmd` (Windows-first): build Release → the fast sim
  tier → the three scenario gates (takeoff_only, landing_only,
  digi_full_mission) → the stock-landing harness
  (`F4_STOCK_WORLD=testcamp.world.json`) → a 0.3-h armed war — one
  command, loud exit code, run before every push.
- The documented CAMP-TOT-PACE re-pin set moves into an explicit
  known-red list file; anything red OUTSIDE that list fails the verify.
  Then the set is actually re-pinned — a permanently red suite hides
  real regressions (the exact mechanism that let the combat suite rot).

## 4. What does NOT change

- The FID tier machinery, the aggregate clock, the ledger's write
  model, the ATM pipeline — this plan repairs the sim's truthfulness,
  not the war's architecture.
- The CAMP-ROTATE liftoff gate (lift margin) stands.
- Determinism: the crash decision reads only sim state in walk order —
  no RNG, no wall clock; the war's ledger MD5s re-pin as the reapers
  start firing (a deliberate §5-style re-pin, recorded per tranche).

---

*Evidence artifacts regenerate via the commands in §1; the qc/diag_* dirs
are gitignored. Each landed tranche records its as-built notes in its own
section, the CHANGELOG gets its line, and the known-gaps ledgers
(CAMPAIGN_LOOP_PLAN §7) retire the entries this plan closes.*

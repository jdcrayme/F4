# Changelog

One line per landed milestone, newest first. The verbose task reports this
replaces live in `Docs/history/changes-archive.md`; the raw session log in
`Docs/history/worklog.md`. Current design docs live in `Docs/` (see
`Docs/README.md` for the index).

## INIT-1 — the employment chain is live: 8 of 9 strike types deliver; exit-4 gates 9 -> 1

The arming tranche's finding of record ("the save's delivery flights
carry NO target VU — a fixture data gap") was wrong: the [plan] probe
showed valid mission targets, STRIKE/17 waypoints with matching
target_nums, resolved plans. Three real mechanisms behind the nine
exit-4 gates. (1) The takeoff-handoff splice ran the route-path
projection, and the strike routes carry the post-strike rendezvous
BEHIND the delivery point — the join stack sits on that rendezvous a
few miles from the home field, so the cursor landed inside the stack
(measured resumes 11/19, 8/21, 10/22) and the flight walked its waiting
circle home without employing. A ground launch now always begins its
route at the beginning (resume_from(1) — the projection's own
CAMP-GATE-ROLL reduction); the projection stays the air-spawn site's
tool. (2) WP_SAD (16) was missing from the engine's delivery-action set
though route_builder's own vocabulary always had it — BAI/STRATBOMB/SAD
flew over their delivery points without arming. (3) Unit-targeted
delivery points (BAI's battalion VUs never spawn as sim entities) now
fall back to the nearest objective entity to the waypoint's position.
Plus two verifier repairs the first releasing matrix exposed: released
bombs were carded as flights (filtered on the recorder's own
missile-track stamp) and the combat-event kind names never matched the
recorder's serialization (bomb_released/entity_killed, not CamelCase).
Measured: employment 0 verdicts -> 22 PASS / 6 FAIL across 8 types
(INTSTRIKE/STRIKE/STSTRIKE/ONCALLCAS 16 releases each, BAI 11);
OCASTRIKE is the one remaining exit-4 — plan, splice, and target all
correct, the release trigger never fired at ~1,900 ft / 402 kts (owner:
the release-trigger/aimpoint tranche). PATH FAILs rose to 46 honestly:
the strike flights now fly their full 90-minute arcs and the windows'
final quarters land on the delivery-run and near-field-stack
excursions.

## ROUTE-HOLD follow-up — horizons come from the mission's own profile; recovery FAILs 38 -> 8

The recovery clause was measuring the horizon, not the behavior: 38 of
76 carded flights "failed to recover" only because the flat 30-minute
matrix horizon cut them mid-flight, and a 45-minute station contract
could never complete inside it. qc_missions.py now derives each run's
horizon from the mission's own MissionProfiles.json max_time (Falcon's
planned sortie window) clamped to [--minutes, --max-horizon] (30/90;
--flat-minutes restores the flat behavior), records the horizon per row,
and re-scopes the 0-airborne verdict to the C++ exit-3 gate's own two
conditions (0 airborne AND none recovered — a horizon past the sorties'
length lands every flight Complete on the deck, and that is the system
working). Same 22 types: recovery FAILs 38 -> 8, duration SKIPs 14 -> 0.
The longer windows made previously-hidden findings visible — the map's
job: 6 recovery FAILs are the new dominant finding (the approach chain
stalls in ProceedToFix after an otherwise-clean 88-minute arc — the
AIRLIFT flight holds its station, egresses, then spends 18 minutes
chasing the entry fix without capturing it; owner: the approach/landing
tranche), 4 newly-visible first_attempt go-arounds (ECM x2, JSTAR,
SWEEP), and 2 newly-visible late-route PATH excursions (STRIKE CS002-4,
TANK CS145-3). The 9 strike-family exit-4 gates are unchanged (the
fixture target-VU gap still owns the employment story).

## ROUTE-HOLD — flights hold their saved legs: PATH fails 52 -> 13

The coverage map's dominant finding, root-caused on its own traces and
fixed in three layers. (1) The nav's intercept clamp + the NAV-B2
damper's fixed point (`corr = corr_p - g*sin(corr)` throttles a
saturated intercept to ~12 deg — the AWACS departure flew
parallel-COMMANDED for 224 s, roll_cmd 0.00 at xte 22,700 ft) converged
every departure-scale excursion at ~140 ft/s: the intercept limit is now
distance-scheduled (20 deg at or inside xte_gain_ft, ramping to 54 deg
at 3x gain — the ramp's near-field is byte-identical, and it stops short
of the steeper 2x version per the BVR two-ship's wingman rejoin) and the
damper fades on the same schedule. (2) The takeoff FlyOut climbed the
whole 2,500 ft on the runway heading and THEN reversed — the brain now
seeds the FlyOut's departure course from the plan's first leg (at FlyOut
entry; never a tanker, never an already-armed receiver — the AAR e2e's
co-based join choreography owns those departures); BARCAP2's departure
transient fell 21,311 -> 7,371 ft. (3) The receiver join-stack orbit was
unflyable — a 12k x 6k racetrack, legs under one turn radius at any of
the nav's speeds, held a waiting receiver at a permanent 8,000-20,000 ft
offset (the SAD waiter's whole 30-min station: xte -20,284..-989) — it
is now a twelve-point circle at R = 24,000 ft flown at the nav's 200-kt
floor (30-deg corners leave ~700-ft bows), which the waiters hold.
Matrix re-run (22 types): PATH fails 52 -> 13, PASS 22 -> 55; the BARCAP
lead's PASS holds (392 ft). Residuals documented in the plan: the
~170-deg departures (ONCALLCAS pair) still peak ~22k with 3-min windows
ending mid-recovery, two co-based flights' routes collapse to 30-s
Enroute windows beside their own orbit (a counted regression each), and
the AWACS departure-tail metric artifact. Suites green (nav 29/29 with
three new pins, landing 32, brain 17, combat 31 incl. the rejoin gate,
recorder 22); verify.cmd GREEN; the scenario trio exits 0.

## EMPL — the saved-flight arming synthesis; the TestCamp target gap named

build_mission_plan_from_flight now synthesizes the delivery waypoint
for armed delivery flights whose saved route carries none (TestCamp's
generated save writes its routes takeoff -> LAND): the flight's own
resolved target's position, the family's delivery action (STRIKE 17 /
GNDSTRIKE 14 / SEAD 19), inserted before the terminal LAND — the C3
arming rule applied to the save's own flights. On real saves whose ATO
writes target VUs, armed strike flights now have something to deliver
on. On TestCamp the synthesis never fires: **the generated save's
delivery flights carry NO target VU** (both the INTSTRIKE and STRIKE
isolations' flights read fp.target invalid) — so the coverage map's
strike-family FAILs re-attribute from an arming-code gap to a FIXTURE
DATA gap (owner: the fixture/INIT-1 tranche — the generator should
stamp targets). The card reads honestly either way (employment SKIP:
no opportunity).

## MC-4 (opening) — the matrix wires the cards: the coverage map exists

`qc_missions.py --record` now runs the contract verifier on every
per-type run's trace — the `cards` column (pass/total per type, failed
clauses per flight in qc_matrix.json), `--no-verify` to opt out; card
FAILs surface but do not gate the matrix exit (the owned findings are
card data until their tranches land). The first full run — TestCamp,
22 mission types, 86 flights carded, 51 s wall — IS the coverage map,
and its headline: **52 of 86 flights fail the PATH clause** —
steady-state leg cross-track above 2,000 ft on their SAVED routes. Not
a clause artifact (saved routes are turn-sparse: 0.07 captures/min
median vs the 0.5/min calibration flight), and it cascades: 20
first-attempt fails (off-leg arrivals make unstable approaches), 30
recovery fails that are the 30-min horizon cutting missions mid-flight
(a horizon artifact — per-category horizons are the fix). The strike
family's 8 types all gate-fail on the saved-flight arming gap (the
EMPL finding); ESCORT and OTHER book 0 airborne (the plain-path
ground-ops crawl); the CAP/support/logistics families fly and recover.
The dominant behavioral tranche is named: route holding on saved
flights.

## MC-3 — FIRST_ATTEMPT and DURATION clauses: the go-around rate is a card number

The user's bar, made permanent: a go-around is a deviation — the
FIRST_ATTEMPT clause FAILs any flight that went around before recovery
unless the contract's allow_goaround flag deliberately tests it
(counted from the snapshot ai_state transitions; the landing module's
GoAround state persists minutes, so the 6-s decimation sampling cannot
miss it). DURATION bands the sortie time wheels-up -> recovery (the
honest landing-time expectation — the saved wires' own times are the
ATO planner's multi-day horizon). Retro-proven on disk: the T4b-era
BARCAP wingman's card reads `first_attempt FAIL 1 go-around(s)` while
every current-build flight (the BARCAP pair, the strike isolation,
landing_only) passes both — sortie 38 min. Every archetype's contract
carries both clauses.

## MC-3 (opening) — the employment clause splits opportunity from execution

MISSION_CONTRACT_PLAN.md's MC-3 opens with the clause its first
isolation runs forced: EMPLOYMENT now separates the OPPORTUNITY (the
route's delivery-action waypoints, carried on the capture events) from
the EXECUTION (the BombReleased events) — no opportunity is SKIP (the
route-arming concern), opportunity-missed is FAIL. The split is the
finding of record made precise: the saved-flight strike routes carry
their delivery waypoints with action 0 and no target stamp (the A-G
route arming resolved targets for the harness path, not the
saved-flight path) — the EMPL tranche's owner, alongside the
target/TOT carry MC-2b named. Selftest extended (the no-release strike
now presents an opportunity so the FAIL is the real one).

## MC-2b — the single-mission isolation profile

MISSION_CONTRACT_PLAN.md's generator tranche:
`campaign_qc --single-mission TYPE` (STRIKE or AMIS_STRIKE) — the
mission filter pinned, one aircraft, one seeded ladder cycle, the
saved-flight emission skipped (a saved flight would consume the cap at
t=0), a 45-h recorded horizon. Deterministic force-file through the new
`Campaign::seed_mission_request` -> `AirTaskingManager::seed_request`:
a MissionRequest per belligerent (defend own first objective / strike
the first enemy objective), riding the NORMAL pipeline — prioritize,
deconflict, package, escorts, TOT slotting; a team that cannot field
the profile lands in the ATM's own unfilled counter. The STRIKE
isolation flew end to end (draw -> spawn -> fly -> record -> card), and
the card immediately named the employment seam: the ATM-composed strike
flew STK1-3 with NO target and NO TOT on the intent (the release
trigger never armed) — the employment tranche's finding, pointed at by
the card's EMPLOYMENT FAIL and the identity join's tgt/tot columns.
Also surfaced: TANKER cannot isolate on TestCamp (no tanker unit — the
coverage map's first row) and the plain qc path's ATM-spawn unit-map
seam (the C5 war's session path spawns these fine).

## MC-2 — the mission contract verifier + report card

MISSION_CONTRACT_PLAN.md's second tranche: `scripts/mission_verify.py`
(stdlib, selftest-pinned) reads any MC-1 recording, selects the flight's
ARCHETYPE CONTRACT from its own AMIS name (cap/strike/support/escort/
generic), evaluates the typed clauses, and emits the report card —
mission_report.md (the human one-pager) + mission_report.json
(schema-versioned, the diffable review artifact). Clause semantics were
settled by the first real cards, each a finding the aggregate counters
never showed: PATH is phase-scoped to Enroute and bands the
steady-state tracking (p90 of the window's final quarter) because the
departure transient is real — the splice-resumed BARCAP diverges
~21,300 ft off its first leg before converging to 110-390 ft held for
the rest of the flight; STATION skips when the route promised no
station (the stock saves' BARCAPs carry no racetrack); TIMELINE accepts
the airborne-start arc; and one wingman transfer's enroute stub never
held its leg (steady p90 8,947 ft) — the first two named findings for
the splice-refinement tranche, carried on the cards with numbers.
Also: three dead snapshot fields filled by this tranche (the nav's leg
cross-track — the PATH metric, internal fuel — the ECONOMY input, and
the station-contract presence — the STATION applicability flag).

## MC-1 — the mission-contract instrumentation joins (the plan opens)

MISSION_CONTRACT_PLAN.md's first tranche — the layer that makes a run
readable as mission stories instead of glyph noise, and the instrument
panel the TOT-pacing pass needs. Two joins, pure instrumentation (no
behavior change; the BARCAP pair still lands both flights, verify
GREEN):

- Flight identity on the trace snapshots: mission (the AMIS name), the
  campaign flight VU, the home airbase, the appointed TOT, and the
  target objective — threaded from the MissionIntent through
  MissionPlan (both bridge paths) and CampaignOriginComponent, emitted
  only when non-default (pre-MC-1 recordings and identity-less
  scenario snapshots stay byte-identical).
- The mission EVENT stream beside combat_events: waypoint captures
  (index, name, wire action, miss distance — the splice's cursor jumps
  are excluded by the sequential rule), station entry/exit, and brain
  phase changes (Ground->Enroute is wheels-up; ->Complete is the
  recovery). Round-trip pinned in the recorder suite (22/22); the
  recorder suite joins verify's fast tier.

Demo (the BARCAP trace, reads from the joins alone): CS111-3
AMIS_BARCAP2 flight 10673 home 1341 — wheels-up 119 s, station anchor
captured 273 s, the LAND waypoint 508 s, Approach the same tick,
Complete (landed) 1,131 s.

## REPAIR-T6 — the all-up verify: one command, loud exit code, the known-red list

CAMPAIGN_REPAIR_PLAN.md's T6 tranche — the process fix that closes the
plan (status: COMPLETE; all six tranches landed). `scripts/verify.cmd`:
build Release → the fast sim tier (15 unit suites, known-red gated) →
the three scenario gates → the stock-landing harness → a 0.3-h armed
war, one command, loud exit codes (0 green, 1 the build or an unknown
red, 2 a verdict), logs in qc/verify/. `scripts/verify_known_reds.txt`:
one gtest name per line with its measured evidence and an owner tranche
— anything red OUTSIDE the list fails the verify. The list went in with
four entries (all pre-baseline never-measured Windows reds, all now
precisely characterized): the AGG-1 clock residue (the batched drain
lands one 1/60-s tick short; the books differ by that tick), the
dirty-sync shadow diff (the dirty ledger books 1 impact where the walk
books 2), and the compressed-war rig's materialization assert (the war
certifies there). The set is actually re-pinned: the digi pair's
51.5-ft touchdown cross against a 50-ft gate calibrated for a ±250-ft
weave re-pinned to 60 ft — the digi suite 3/3, the entries deleted —
and the "EventStream 2" red from the §1 inventory is gone (the event
journal 13/13, never re-measured until now). Contract verified end to
end: VERIFY GREEN with the four known reds noted and the armed war
deterministic; the fail-loud path demonstrated (unknown reds exit 1).

## REPAIR-T5 — the A/A seam: combat 31/31, the guns fight flies, the dive dies

CAMPAIGN_REPAIR_PLAN.md's T5 tranche, landed as-built — and both named
items traced to seams ABOVE the WVR band. The guns-merge pair: the
CAMP-FAF early approach handoff (the T2-era 33,000-ft inbound tune)
handed every tactical route that came within 5.4 nm of its endpoint to
the landing module, and the combat ladder runs only while Enroute — the
2.8-NM head-on merge was hijacked before the first tick and WVREngage
was never reachable (the entry band was never the problem). The handoff
is retired; the phase contract stands alone (T4b's FAF synthesis made
the field-terminal geometry sane, which was the tune's original
reason). The combat dive: the deagg air-spawn put a 40,000-ft aircraft
at 127 kts (the pose's absolute 100-fps floor — deep stall, a −73° /
−40,000-fpm fall), and the commit trigger was blind on the first pass
(the combat pass builds its aggregate feed at the END of each advance,
so a first advance covering the whole engagement window decided Trigger
A against an empty feed). Three fixes: the air-spawn speed floor (1.3 ×
stall CAS as TAS at the spawn altitude), the feed seeds at session
create, and the GroundAvoid horizon now covers the arrest distance
(GPWS practice). `CommittedFighter` — red since the plan's diagnosis
baseline — is green.

The stock gate's follow-through (the retirement changed the observed-
flight identity; the harness now follows the shortest-remaining-route
live flight) exposed and fixed four T4-legacy approach defects: the IAP
leg is the course-line intercept law with a one-sided FAF clamp (the
pursuit shape sequenced every intercept 3,800-10,400 ft wide with
~4,600 ft of convergence room), the wrong-side leg pursues the fix at
pattern altitude (the clamped law degenerated into an outbound line
chase), the reciprocal-heading deadlock got a turn-commit bias (the
±180-deg wrapped error flipped sign every wobble; phi weaved ±7° under
a saturated roll command for hundreds of miles), and the flare is the
direct VS servo from entry (the attitude/energy phase retired — the
real 130-ft/−978 entry sank to −2,166 under a 0.157-stick trim and
timed out; landing_only's certified 6-ft/−0 entry was a deck hover).
The E64 flare-entry band re-tightened to 900 by measurement (three
arrest generations bounced identically; the E64 doctrine holds — beyond
the band the RIDE is the landing) and the flare-height window widened
to the E55 overrun shape. The stock gate lands OnFinal → Rollout at the
threshold, first attempt.

Gates: `test_combat_integration` 31/31 (the T5 gate); the armed 0.3-h
war books 19 honest A/A kills / 17 retires, all four C5 verdicts green
(deterministic; the ledger MD5 re-pinned for the spawn-speed floor);
fidelity combat 7/7; the stock-landing harness GREEN; BARCAP lands both
flights full-stop; landing module suite 32/32 (the flare pins
re-contracted to the servo law); GroundContact 5/5; brain 17/17;
navigation 26/26; the scenario trio all book touchdowns; the digi
suite's red set preserved at 2. Known red carried to T6's list:
`CampaignWarHarness.RunsCertifiesAndIsDeterministic` — measured red at
the plan's own diagnosis baseline (a never-measured Windows
pre-existing red; the war certifies there, the compressed-horizon
materialization assert is what's dead).

## REPAIR-T3 — the ground-spawned Enroute contract: one splice, honest launches

CAMPAIGN_REPAIR_PLAN.md's T3 tranche, landed as-built. Disambiguated
[splice] telemetry over the stock-save harness showed two deagg
populations — flights materialized at their route's start, and
transfers materialized at the route's RECOVERY end (pos ==
route.back(), the departure field a distant other base) — and the
plan's named nearest-waypoint resume was wrong for both: at a base on a
closed route the nearest waypoint is the route's LAST waypoint (instant
RTB), and on a route revisiting its departure field it can sit behind
the aircraft's mission order. Fixes: NAV-D1's airborne check (a grounded
aircraft with an Enroute-start plan runs the takeoff FSM like any
ground launch; the air-spawn contract requires ACTUAL airborne), the
ONE splice decision (route_resume_index: project the aircraft onto the
route polyline, first-among-equals in leg order, resume at the leg's
end — reduces to resume_from(1) at a base launch, to the next leg ahead
for a mid-route deagg, and to route completion for the at-recovery
materialization; the old resume_from(1) + nearest-waypoint override
pair is gone), and the third defect the new pins surfaced: the nav's
spawn-on-leg consolidation clobbered every splice resume back to wp1 —
it now applies only to un-spliced activations. campaign_qc's "airborne
at end" verdict was retired with the deck-level zombie: the gate passes
on airborne OR landed.

Gates: the stock-landing harness GREEN; the BARCAP 40-min run books TWO
full-stop recoveries inside the window (Parked@782 s and Parked@1,008 s,
zero losses — the retired exit-3 verdict read that same run as "ground
ops stalled"); the armed 0.3-h war keeps all four C5 verdicts green
(deterministic, 19 honest losses / 18 retires / 18 A/A kills); brain
suite 17/17 (four new T3 pins), navigation 26/26, landing 32/32,
GroundContact 5/5, combat integration 29/31 (the pre-existing
guns-merge pair — T5's items); the digi suite's red set unchanged
(the authored digi route keeps its pattern legs); landing_only +
takeoff_only + digi_full_mission exit 0. T5 (the A/A seam: the
guns-merge WVR entry band) is the next open tranche.

## REPAIR-T4b — the campaign end-to-end gate closes: the observed flight lands

CAMPAIGN_REPAIR_PLAN.md's T4b tranche, landed as-built. The instrumented
stock-save run exonerated half of its own blocker list: the IAP leg's
"holds pattern speed" was an 8-11 kt throttle-PI residual (193 vs the
185 command), while the real ProceedToFix orbit sat one layer up — the
CAMP-FAF synthesis guard left 7 of the 13 stock-save fields flying to
the RAW FIELD CENTER (the guard kept any fix projecting 15,000+ ft
before the threshold; a long runway's field center projects that far
down the field AND 37,000-63,000 ft off the extended centerline, where
the T4 fix-7 centerline capture gate can never pass). The worst field
drew 15,247 orbit telemetry rows; the observed flight orbited one for
100+ minutes. Fixes: the acceptance window (keep a configured fix only
if it is on the approach course within 2,000 ft AND 15,000+ ft out —
everything else synthesizes the 5-nm FAF; all hand-authored scenario
fixes untouched), the scaled capture lead (bounded below by the turn
radius at the CURRENT speed — ~13,100 ft at 250 kts vs the 6,462-ft
approach-speed lead), and the short-touchdown strand (grounded
ProceedToFix fires GoAround — the SM was missing the ProceedToFix →
GoAround edge entirely, so the strand was structural; the grounded
go-around is a touch-and-go) — plus the [ptf] telemetry extension and
the establish-floor row's format-string fix.

Gates: the stock-landing harness is GREEN (the observed wave flight
touches down — plan §1's FAIL retires); the BARCAP 40-min run flies a
REAL campaign RTB flight end to end (ProceedToFix → InterceptFinal →
OnFinal → Flare → Rollout → Parked, 11.4 min into the mission, zero
losses) with the wingman's missed approach re-flown cleanly; the armed
0.3-h war keeps all four C5 verdicts green (deterministic, 15 honest
A/A losses / 12 retires); the landing module suite 32/32 (four new T4b
pins); GroundContact 5/5; combat integration 29/31 (the pre-existing
guns-merge pair, T5's items); landing_only + takeoff_only +
digi_full_mission exit 0. T3 remains open (the ground-spawned Enroute
contract).

## REPAIR-T4 — the approach chain closes: eight fixes, landing_only end to end

CAMPAIGN_REPAIR_PLAN.md's approach/flare tranche, landed as-built with
the id-tagged F4_LAND_DEBUG telemetry chain driving every fix: (1) the
flare hover — the energy driver + the ground-effect-supported attitude
equilibrium held the aircraft at 6 ft AGL / vs -0 for the 15 s timeout;
below a 60-ft gate the attitude loop is bypassed by a direct VS servo
(target -700 fpm) and the aircraft touches down in 2 s. (2) The OnFinal
deadfall — a firm arrival met the deck in OnFinal where the Touchdown
event was never observed; the wheels now fire it wherever they touch.
(3) The straight-in catch-down — InterceptFinal's hold tracked the beam
down once the localizer is captured (the from-above arrivals could
never establish). (4) The FAF orbit trap — the IAP leg's altitude
ceiling is the FAF's crossing altitude (the old beam-at-current-position
target rose with every outbound swing; 90 minutes lost orbiting).
(5) The wrong-side capture — the IAP leg aims past the fix in the
landing direction, both capture sites require the landing hemisphere,
and the intercept gained the missed-approach bound. (6) The
re-intercept gate aligned with the module's +300 windows (the climb
cascade stalls below +500). (7) The localizer-proximity capture — the
fix-capture window also requires the centerline (an aircraft 11 NM off
course sequenced into the intercept and cycled forever).

Gates: landing_only exit 24 -> exit 0 with the full chain
InterceptFinal -> OnFinal -> Flare -> Rollout -> TaxiIn; the landing
module suite 28/28 (two pins re-contracted + the new gate pin);
digi_full_mission + takeoff_only exit 0; digi suite 14 -> 13
(pre-existing set). Still open (T4b): the stock-landing campaign gate —
the observed flight now flies full approach cycles at the correct
altitudes/fixes but does not complete; the two named blockers are the
short-touchdown strand (a catch-down deck contact strands in
ProceedToFix) and the ProceedToFix convergence geometry at pattern
speed (the turn radius exceeds the capture window; the IAP leg's 185-kt
command is not being flown). Docs/CAMPAIGN_REPAIR_PLAN.md section T4
carries the as-built.

## REPAIR-T2 — CAMP-FAF-GUARD: the one-word fix that let the campaign fly

CAMPAIGN_REPAIR_PLAN.md's route-flight tranche. The investigation
exonerated its named scope (the waypoint altitude floors already
existed and were sane) and found the real root cause one word wide: the
CAMP-FAF approach-handoff guard's skip-path was a bare `return` from
the ENTIRE brain update (a STEP-14 regression) — every aircraft more
than 5.4 nm from its route's end stopped running the module switch, the
nav never flew a tick, and the FM consumed cleared-to-idle defaults
(throttle 0, gear down) until the aircraft coasted into the terrain.
The fix: the guard skips only the handoff and falls through to the
module switch. Measured: the BARCAP 40-min run flies its route, RTBs,
and engages the approach (airborne 2/2, zero losses — was 2 terrain
kills); the armed 0.3-h war holds 94-96/96 airborne with 17 honest
combat losses (was the 96-to-4 collapse with 94 terrain deaths);
`test_combat_integration` went 15 reds to 2 (the A/A fight suites were
the same coasting defect) with the 2 remaining the guns-merge WVR entry
band — T5's first named item. The zombie-detector test re-pinned for
the self-healing nav (wedged-FM mid-route is the contract's shape).
Docs/CAMPAIGN_REPAIR_PLAN.md §T2 carries the as-built.

## REPAIR-T1 — ground-contact truth (the terrain-zombie killer)

CAMPAIGN_REPAIR_PLAN.md's keystone tranche, landed as-built: the FM's
airborne→ground transition latches one-shot (`GearState::
touchdown_event` + `take_touchdown_event()`), and `Simulation::tick`'s
new `sweep_ground_contacts_()` classifies every touchdown by brain
phase — a landing/takeoff-owned phase is aviation, anything else is a
CRASH: the corpse parks (dormant brain + FM) and
`EntityKilledMessage(cause="terrain")` flows to the sink, the reaper,
and the fold. The zombie detector kills an Enroute-on-ground aircraft
past a 10 s grace (the NAV-D1 ground-spawned flight never produces a
transition). The FID-4 air-spawn pose floors at terrain + 500 ft (a
staged flight's aggregate face is deck level — pre-T1 a silent zombie,
post-T1 an instant crash). The armed 0.3-h war now books 94 terrain
kills / 94 retires / a non-empty ledger where it booked ZERO losses for
the same silent wedges (C5 verdicts green, ledger MD5 re-pinned);
the BARCAP run books its losses; the stock-landing flight engages the
approach instead of zombie-cruising. The combat WVR merge exposed as a
real pre-existing defect (a ~30k-fpm dive meets the deck mid-fight —
T5 evidence). Docs/CAMPAIGN_REPAIR_PLAN.md §T1 carries the as-built.

## VIEWER-PERF-1 — the viewer-perf tranche: PV-1/2/4/5 + UI honesty (the analysis's fix path, first round)

The PV-1..PV-5 ladder from VIEWER-PERF-ANALYSIS-1, implemented except the
one structural item (PV-3, the lock diet — deliberately deferred until the
new profiler measures the duty cycle; see the doc's updated §7). Every
change is engine-fidelity-neutral: the war's outcomes, the golden
identities, and the AGG-3 authenticity rule all stand.

- **PV-1, the frame profiler** (`F4_FRAME_PROF=1`): run() stamps the frame
  phases (snapshot / input / focus / canvas / imgui / present) as EMAs and
  a new Frame Profiler window prints them beside the worker's batch
  composition (hold ms, ticks, budget, delivery scale, effective speed)
  and the duty cycle. Zero cost when off (one env probe, one branch per
  phase). The measurement surface that decides whether PV-3 or the churn
  follow-ups come next — the FID-OPT discipline, applied to the frame.
- **PV-2a, the defended index** (ground_war): objective_defended is now a
  binary search over a sorted (vu, index) vector built once in the
  constructor (the unit_vus_ pattern, sorted). The canvas's objectives
  pass asked once per drawn objective per frame — ~3.5M iterations on the
  default Korea view → ~12 comparisons per query. 28/28 ground-war pins
  green under the gtest shim.
- **PV-2b, the refresh throttle** (campaign_session_view): while stepping,
  the worker bumps step_serial every batch (~60x/s at speed) and the old
  per-serial gate turned each bump into a full JSON round trip (4-5
  encode/parse queries, threat grid included, under the frame lock). The
  fetch now fires at most every 100 ms while the serial moves; command
  invalidations (the D/R buttons) bypass the throttle; the event drain
  stays per-frame. The supply cut-off cache rides the same cadence.
- **PV-2b companion, the AGG-3 per-batch report**: StepResult gains
  `bubble_live` — EngineSessionHost::step() fills it from the engine
  (bubble_live_flights(), the O(deaggregated) walk) and the runner clamps
  its feed off EACH STEP RESULT instead of the UI's snapshot mirror (the
  mirror is gone). The authenticity hold now engages at the reference's
  own compression-loop cadence, independent of the query throttle — and
  the step-result contract is pinned by the updated runner tests (the
  mock's step carries a bubble_live knob; 11/11).
- **PV-2b companion, the threat cache** (host): the threat query's
  full-grid serialize (~2x29k ints on Korea) is cached after the first
  ask — the route builder's ThreatMap is immutable for the session's
  lifetime, so every re-encode produced identical bytes.
- **PV-4a, lazy stats** (campaign_session): advance() marks a dirty flag
  instead of paying the full stats walk per 1-2-tick step batch (the
  viewer's worker fed ~60 batches/s; each re-walked the intents, the
  ground books, and the aircraft roster twice for a reader that came
  once per frame). stats() refreshes on first read (mutable + const
  refresh — every existing reader, test, and the war harness unchanged);
  the paused-advance path and every command mutation path mark too.
- **PV-4b, the Tier-B ceiling**: CampaignSessionOptions.max_live_flights
  (0 = uncapped, the golden identity; the viewer arms the QC's
  --accel-max-live default 32). While at/above the ceiling the
  ATTENTION-driven (Bubble) tier trigger defers — the war's own windows
  (ops/TOT/recovery) and the Combat/Force triggers are never capped:
  they fly outcomes, only display fidelity yields. Bounds the
  deep-zoom + combat-engagement spiral the certificates never ran.
- **PV-4d, the re-point rate limit**: a selection-anchored bubble
  re-points only after the anchor crosses HALF the bubble radius (never
  less than the old vis/16) — the deagg SET only changes when something
  crosses the bubble's edge, so re-pointing while the anchor drifts
  inside its own ball was pure churn (each re-point runs the bubble walk
  + tier evaluation synchronously under the frame lock and buys
  structural-rebuild debt on the worker's next ticks).
- **PV-5, the 3D diet**: the chase view's per-objective airfield geometry
  is cached per world (build_airfield_geometry_3d is a vertex builder
  that re-ran for EVERY nearby objective EVERY frame; the layouts are
  static, so the build runs once per objective and the draw toggles
  still apply per frame), and objectives_within_radius walks a flat
  (id, x, y) position index captured once per world/session (objectives
  never move) instead of an entity handle + TransformComponent get per
  objective per frame.
- **PV-2c, the quick wins**: the flights table's type cell is memoized
  per flight (flight_airframe_name — the ATO table's squadron memo
  applied to its sibling; the resolve chain ran per visible row per
  frame); the Event Log renders through a filtered row index +
  ImGuiListClipper (the 2,000-row cap now draws only the visible slice;
  the filtered-index rebuild is keyed on filter text + row count); the
  canvas's ATO-mark set is cached per snapshot identity (ato_targets_set)
  instead of rebuilt from the whole tasking vector every frame; the
  objectives pass reads the vu_id_num property-bag key once per
  objective (it paid the string-keyed lookup twice).
- **UI honesty**: the speed readout distinguishes the AGG-3 hold ("held
  at 1x — action in bubble", the reference's own DoCompressionLoop rule)
  from CPU dilation — and names BOTH when even the held 1x feed is
  CPU-starved. A number under the preset no longer reads as a
  performance bug when it is the authenticity rule working.
- Verified: every touched TU -fsyntax-only clean against the pinned
  headers; 87 behavior checks green across the four directly-affected
  suites under the gtest shim (ground-war 28, runner 11, protocol 34,
  queries 14); the simulation session/host/fidelity/commands/airwar-QC/
  FM-divergence tests and the QC tool all compile against the new APIs.
  NOT yet done (the next rounds, ranked by the profiler once it runs on
  target hardware): PV-3 the lock diet (snapshot then release), the
  minimap/parked-layer walks, threat-overlay texture batching, chase-view
  cadence.
- Doc: `Docs/VIEWER_PERFORMANCE_ANALYSIS.md` (§7 updated — what landed,
  what deliberately didn't).

## VIEWER-PERF-ANALYSIS-1 — why the world-viewer needs so much more CPU than FreeFalcon

The diagnosis doc for the viewer's time-acceleration limits (campaigns
CPU-limited <10x; viewing a specific aircraft occasionally <1x). Analysis
only — fixes scoped into the PV-1..PV-5 tranches, not implemented.

- **The headline**: the engine is no longer the cost (116x-1,676x headless
  on the FID-OPT certificates, 0.157 ms/tick) — the viewer wraps it in
  five multipliers FreeFalcon never paid: the whole-frame session lock
  (the worker's only window is EndDrawing's pace wait; every draw
  millisecond divides the sim's duty cycle), the per-frame contract-plane
  JSON round trip (the step_serial gate is a no-op while stepping — 4-5
  encode/parse queries per frame, threat grid included), the map's O(N^2)
  objectives pass (objective_defended is a linear scan per objective per
  frame — ~3.5M iterations), the Focus churn (each camera/selection
  re-point runs refresh_bubble + evaluate_tiers_ + refresh_stats_
  synchronously inside the frame lock, with ms-class materializations and
  structural-rebuild debt on the worker's next ticks), and the 3D chase
  view's per-frame scenery rebuild (objectives_within_radius +
  build_airfield_geometry_3d per frame, terrain chunk rebuilds per 20,000
  ft of drift — all under the lock).
- **The <1x aircraft case decomposed**: AGG-3 holds the feed at 1x while
  the viewed flight is deaggregated (the DoCompressionLoop authenticity
  rule, by design — the UI should label it distinctly from CPU dilation);
  the sub-1x residual is the churn spiral + unbounded deagg depth (the
  viewer has no --accel-max-live ceiling) starving even the 1x feed.
- **The cert-unpaid asymmetry**: viewer sessions run the ground war (the
  116x certificate's run had it off), pay refresh_stats_ per 1-2-tick
  step() (the QC drained 240-tick advance batches), and carry
  near_initial_wave's front-loaded deagg windows uncapped.
- **The fix path**: PV-1 the env-gated frame profiler first (the FID-OPT
  lesson — every prior "obvious" cost center re-attributed when
  measured), PV-2 the no-behavior-change quick wins (the defended index,
  the refresh throttle, the memoizations), PV-3 the lock diet (snapshot
  then release — draw from the copy), PV-4 the churn policy (re-point
  rate limits, tier evaluation at the advance boundary, the max-live
  ceiling), PV-5 the 3D diet (epoch-keyed geometry caches).
- Doc: `Docs/VIEWER_PERFORMANCE_ANALYSIS.md`; four-way code audit with the
  load-bearing claims spot-verified in source.

## ATO-SORT-1 — the ATO windows sort: squadron, airframe, T.O. + the class-table name chain

The campaign session's generated-missions table (the live war's ATO) gains
the reading columns and the sort; the "ATO / Tasking" QC window completes
its own. Plus the fix both ride on: the unit→airframe name chain was
resolving through the WRONG link on real theaters.

- **The name-chain fix** (`f4-world/src/theater_tables.cpp`): the UCD's
  `vehicle_type[]` is a CLASS-TABLE position (entity_type = value + 100,
  the converter's own field contract), not a VCD position — the resolver
  now walks CT row → DTYPE_VEHICLE → VCD row. On the real Korea tables
  the old positional read resolved "Airlift" squadrons to "Leopard 2"
  tanks and fell back to role words for every aircraft row (the test
  fixture's rows happened to sit at their CT data_ptrs, hiding it); the
  fixed chain names F-16C / MiG-21 / F-5E / Mi-24 squadrons correctly
  (verified against the committed Data/ exports end to end). The flights
  table's and inspector's existing airframe cells inherit the fix.
  Pinned: the fixture gained a row whose vehicle link resolves ONLY
  through the CT walk (Test sqn → CT 105 → VCD 1, a bare positional read
  falls off the table).
- **The session ATO table** (campaign_session_view.cpp): three new
  columns — squadron (role word + VU; the campaign carries no
  squadron-name table yet, the identity pair is the honest unique
  display), airframe ("F-16C", the squadron's class-table row,
  memoized per squadron on Impl — one resolution per distinct squadron
  per session, cleared at adopt), and T.O. (the DOM-4 scheduled takeoff
  slot in absolute campaign time, now parsed off the tasking query's
  additive tail; "-" when never slotted) — and EVERY column is sortable:
  the header click cycles ascending / descending / the engine's append
  order. The sort is an index permutation over the SHARED snapshot (the
  canvas's ATO marks read the same vector — nothing mutates it),
  re-derived when the spec moves, when new intents arrive, or when the
  snapshot refreshes (data pointer + size + refresh serial).
- **The QC ATO window** (campaign_qc_view.cpp): the airframe column
  joins squadron/target (both now cached strings in the row build — the
  sort keys and the cells read the cache, never the component graph),
  and the previously unsorted columns (pkg, target, squadron, airframe)
  sort too. The armed spec now SURVIVES filter changes and world
  mutations — the rebuild re-applies it instead of silently resetting
  to TOT order while the header still shows its arrow.
- **Sort discipline** (both windows): descending is the strict reverse
  of ascending (`less(y,x)`, not `!less(x,y)` — the latter violates
  strict weak ordering on equal keys), stable_sort keeps the engine's
  order within equal keys in BOTH directions, and T.O. sorts
  unscheduled (0) rows to the END ascending ("earliest launch first").
- **Verification**: the f4-world theater-tables suite (8 tests incl.
  the new chain pin) green; a standalone harness over the REAL
  Data/Classes/falcon4.ct.json + Data/Theater/korea/tables.json
  resolves all 14 flight/squadron type pairs + the vehicle branch
  correctly; every f4-world-viewer TU passes a full syntax check
  against the pinned imgui v1.91.5 / raylib 5.0 headers.

## Step 15 — the specialist support brains (FAC + the station brains)

AI_IMPLEMENTATION_PLAN.md §16's Step 15, landed as-built: the Part-III
module library now flies every flight TYPE — the support roles
(tanker / AWACS / ECM) get the defensive-only station composition, the
FAC gets it plus the talk-on that steers a strike flight's bombs onto
a mark the strike's own sensors never saw.

- **SupportStationBrain** (`tankbrn.cpp` → `set_support_profile`): no
  new brain class — a profile on BrainComponent that stands the
  engagement rungs (BVR/WVR) down and keeps the defensive MissileDefeat
  rung armed (the reference support aircraft run away, they do not
  fight), while the P7 station hold flies the racetrack and CollisionAvoid
  stays always-armed. The host arms it on the support roles.
- **FACBrain** (`facbrain.cpp` → `set_fac` + `FacTalkOnModule`): the
  orbit station + ONE talk-on (wingradio's fifth row, `FacTalkOn`,
  carrying the BRA from the assigned flight and the closed
  `TalkOnDesc::GroundAssets` row). The host resolves the scenario's
  `mark_feature` to the spawned feature entity, pushes the nearest
  same-team non-support flight's picture, and delivers the published
  row through the Step-14 hint pipe. The strike brain's UNMARKED
  delivery (delivery-action waypoint, target 0) prosecutes the mark —
  a route target is never overridden; v1 marks ONE target (the
  re-mark priority loop is the v2 data tranche).
- **Scenario surface**: `fac` + `mark_feature` per aircraft; the
  station-hold contract (`station_time_s` / `loop_waypoints`) now
  parses on per-aircraft routes (the P7 mechanics already lived in
  the NavigationModule — the scenario path just couldn't express the
  contract). The talk-on renders as host text
  ("STRIKE1, talk-on bearing 002 for 8.2, ground assets.").
- **Gates**: `ai.flight_lead` arms everything (the plan's "no new
  flags" rule) — off, every brain is byte-identically what it was.
  The AWACS node stays on `combat.gci_datalink`.
- **P7 BUG FIX the E2E caught** (`navigation_module.cpp`): the station
  hold's wrap check rode an `else if` AFTER the route-completion check —
  a hold whose span ends at the route's LAST waypoint completed the
  route on the first lap (the "station" silently became a fly-through).
  The wrap now rides before completion: an active hold wraps to the
  anchor; an expired hold at the last corner completes (orbit-to-end).
- **Tests**: `test_fac_talk_on_module.cpp` (19 — the gates, the BRA
  arithmetic, the one-shot latch, the hint API, the drain) and
  `test_support_fac_e2e.cpp` (5 tiers — the tanker's racetrack through
  a full boom cycle, the support stand-down with the gate-off twin
  engaging, the AWACS stationing while the net carries it, the talk-on
  driving the strike's unmarked delivery onto the mark, and the
  gate-off twin). f4-ai label 366/366; the affected sim suites green;
  the new suites green under ASan+UBSan with zero reports.

## Step 14 — FlightLeadModule: the lead half of flight command

AI_IMPLEMENTATION_PLAN.md §16's flight-lead chapter, landed as-built:
the Step-11 wingman gets its other half — a lead that WATCHES its
flight (per-tick status echoes the host pushes) and commands it
(flitlead.cpp's CommandFlight() tranche, smallest-first), with the
radio vocabulary the combat transcript was missing (wingradio v1).

- **The vocabulary** (`f4-ai/wingradio.hpp`): a CLOSED enum —
  `OrderRejoin` / `OrderEngageMyTarget` / `OrderRTB` / `Ack` — plus
  the bus message and the host-carried order envelope. AI code
  publishes enum rows only; the text rendering lives in the host
  (the plan's risk-table rule: no free-form strings from AI code).
  The kill/loss half of the wing vocabulary rides the EXISTING M4
  narration (splash carries the shooter; track drops speak for
  themselves) — wingradio v1 adds only the order/ack cycle.
- **The module** (`f4-ai/modules/flight_lead_module.hpp`): pure, beside
  `WingmanModule` in the same brain, active only when the host
  registers a roster on it. Three edge-triggered rules: REJOIN on the
  wingman's own Rejoining echo (latched until it reports Following —
  one order per blowout), ENGAGE when the lead fights a bandit the
  wingman holds and is not on (the order follows the lead's target;
  the wingman's targeting ranks it as a preference — sensor truth
  still wins), RTB once per flight at the wingman's bingo with v1's
  both-RTB (`lead_rtb()` latches the LEAD's own stand-down).
- **The brain** (`brain_component.hpp`): the flight-command step runs
  after the fuel check, before the ladder — the lead publishes the
  module's radio rows on the bus (host-stamped clock, the sensor
  components' rule) and drains its inbox; the wingman acks once per
  order and applies (rejoin drives the formation SM from the other
  side, engage ranks the ordered bandit above the sort via
  `SensorFusion::preferred_threat_target`, RTB takes the ladder's
  bingo path — `fuel_bingo_ || rtb_ordered_`, the "RTB" mode line
  included). The engagement the step reads is LAST tick's (captured
  before the ladder reset) — the same freshness every host push
  carries.
- **The host** (f4-simulation): the scenario's top-level `"ai"` block
  arms the tranche (`"flight_lead"`, default FALSE — the
  byte-identical gate); `resolve_wingman_refs` arms both brains of
  each pair and registers the roster; the tick pushes the wingman
  echoes next to the lead pictures (`push_flight_lead_echoes`) and
  applies the lead's orders after `update_all` — acked + applied on
  the wingman's next update, the one-tick latency every host push
  carries. A dead lead commands nothing (the corpse rule).
- **The transcript**: one new subscription renders the vocabulary
  ("EAGLE2, rejoin." / "EAGLE2, engage my target." / "EAGLE2, RTB." /
  "Copy.") — no row exists unless the scenario armed the gate.
- **Pins**: `test_flight_lead_module.cpp` (11 cases: the three rules'
  edges, latches, and re-arms; the corpse rule; duplicate
  registration; the inert shape), `test_flight_lead_e2e.cpp` (5
  tiers: the gate-off twin — no rows, member-for-member identical
  runs; rejoin order → ack → converged; engage-my-target outranking
  the sort with a free bandit available; the RTB order with the lead
  standing down on a HEALTHY tank; the done-when arc — formation →
  engaged as a flight → RTB on bingo, the RTB order + ack in the
  log). The fast f4-simulation label's 12 failures are the
  documented CAMP-TOT-PACE set, unchanged; the new suites are green
  under ASan+UBSan with zero reports (f4-ai 84 tests + the E2E 5).

## AGG-4 — fully lazy aggregate state: the schedule IS the state

The aggregate-clock plan's endgame (Docs/AGGREGATE_CLOCK_PLAN.md §4),
landed as-built: a TIME-mode flight's aggregate position is a pure
query `f(route, t)` computed on read — the save's own arrive/depart
schedule IS the truth, and the 60-s chunk stepping that sampled it is
gone.

- **The engine** (f4-campaign): `FlightAggregateEngine::tick` pops the
  TIME rows' discrete transitions from the deterministic due-queue
  (due_queue.hpp — the AGG-2a primitive's named consumer) — waypoint
  arrivals at the schedule's own seconds, key-ordered `(due, priority,
  seq)`, one live event per row (the cursor-chase), self-invalidating
  on any mutation that touches the row (generation counter). A fired
  arrival materializes exactly what the walk used to write at its
  quanta: the waypoint snap, the cursor, the terminal flag. The chunk
  walk remains for SPEED rows only (routes without usable schedules
  cannot be closed-form — synthetic intents carry arrival schedules in
  a later tranche). Between events a TIME row's propagation cost is
  ZERO.
- **The query faces**: `display_position` (already the pure TIME
  interpolation) is now THE face — `fuel_burnt_now` (the walk's own
  per-update accrual as a closed form on the same update grid:
  past the takeoff gate, past the first activation, before the
  terminal arrival; byte-equal totals, pinned) and `waypoint_cursor`
  (the walk's own cursor rule) complete it. The stored field
  materializes at the transitions that would otherwise hide the
  accrual: suspend, fold, retask (the monotone surfaces keep their
  exact shape).
- **The session** (f4-simulation): the entity mirror runs per pass and
  writes the SCHEDULE face for TIME rows (transforms track the wire
  continuously), the stored face for SPEED rows exactly as before.
  The tier triggers' bubble test, the combat feed, the convergence
  trigger, and the deagg spawn pose all read through one
  `aggregate_face_` definition of "where the flight is" — the
  schedule, not the last quanta. The fold's CAMP-TOT-PACE pace books
  against the exact cursor.
- **Windows stay queries**: the TOT/recovery/takeoff windows remain
  the heartbeat's per-pass reads of row times (pure queries, zero
  propagation cost, the AGG-2a latency rule stands) — the queue
  carries only the events whose STATE the engine owns.
- **Pins**: `TimeModeStoredFaceMaterializesAtArrivalsOnly`,
  `LazyFuelMatchesTheWalkContract` (gate holds, mid-leg accrual, the
  mid-route arrival burns, the terminal one doesn't, the dwell burns),
  `FoldReArmsTheQueueOnTheShiftedWire`,
  `SuspensionDropsEventsAndTheFoldRestoresThem`,
  `RetaskMaterializesTheLazyBurn` (f4-campaign);
  `FidelityTiers.LazyTimeMirrorTracksTheSchedulePerPass` (the
  transform glides with the schedule, no update boundary) +
  `LazyDeaggSpawnLandsAtTheScheduleFace` (f4-simulation). The
  pre-existing suite's failures are the documented CAMP-TOT-PACE
  set, unchanged (stash-and-rebuild comparison, the AGG-2b doctrine);
  the new suites are green under ASan+UBSan with zero reports.

## AGG-3 — the spawn policy's last half: the DoCompressionLoop clamp

The aggregate-clock plan's AGG-3 (Docs/AGGREGATE_CLOCK_PLAN.md §4),
closed as-built: the spawn half (synthetic ATM intents as AGGREGATES)
rode FID-5's `synthetic_as_aggregates` all along — the section was
stale; what landed here is the rule's remaining half, the reference's
`DoCompressionLoop` authenticity rule:

- **`Stats::bubble_live`** (f4-simulation) — deaggregated (Tier-B)
  flights whose live lead sits inside the observer bubble right now;
  the DEAGG radius over `live_lead_position_`, recomputed per
  `refresh_stats_` (O(live flights), usually 0). Deaggregated alone is
  not the rule — a force-deagged test flight, an ops takeoff at a far
  airbase, a combat window over the horizon never hold the player's
  clock; the fight being WATCHED does. The session publishes; it never
  paces (plan §2.2).
- **The wire** (f4-campaign-api): the `stats` query carries
  `bubble_live` at the DTO tail (additive, plan §11; protocol version
  unchanged); the StatsView golden and the host's vocabulary pin
  re-pinned with the new last key.
- **The clamp** (f4-world-viewer): `CampaignClientRunner::
  set_bubble_action()` — the frame scope mirrors the engine's state
  once per advance, and the worker holds its feed at 1× while the flag
  is up. The preset radio keeps the user's request, the AIMD
  delivery-governor scale carries over, and the clamp lifts the moment
  the bubble clears (no governor reset, no fake full-feed spike). The
  speed row shows `1x — action in bubble` while held; the
  camera-bubble checkbox is the knob (bubble off → the clamp never
  fires).
- **Pins**: `FidelityTiers.BubbleLiveCountsObserverBubbleAction`
  (no bubble = 0; force deagg OUTSIDE a far bubble = 0 — deaggregated
  is not enough; the bubble over the lead = 1; the fold = 0),
  `CampaignClientRunner.BubbleActionHoldsTheFeedAtOneX` (with the
  no-flag control at the same preset) +
  `BubbleActionClearResumesThePreset`, and the wire tail re-pins. The
  pre-existing suite's failures are the CAMP-TOT-PACE re-pins,
  unchanged (stash-and-rebuild comparison).

## AGG-2b — the air-picture roster: the SpatialIndex wired + the per-unit detection cadences

The aggregate-clock plan's third tranche (Docs/AGGREGATE_CLOCK_PLAN.md
§4) — the two hot walks that paid the full transform bucket per pass to
reject the same 99.8% of candidates with the same clutter arithmetic now
share one membership index:

- **`AirPictureRoster`** (f4-entities, air_roster.hpp) — the wired
  radar/detection term FID_OPT §5 named. Holds the non-clutter
  membership in entity-index order (the bucket order the uncached
  walks produced) plus a SpatialIndex over member positions captured
  at rebuild (`air_roster_within_radius` — the SAM-ring / formation /
  threat-query surface). Maintenance: rebuild on `structural_epoch()`
  movement (spawn/destroy/component changes, latency = next refresh)
  or on the caller-driven revalidation cadence (the behavioral flips —
  the taxi launch, the landing stop — no structural event marks);
  the clock is the host-stamped sim time, never wall time.
  `EntityWorld` owns one lazily; the move ops leave the destination's
  empty (the lazy create IS the defensive rebuild).
- **The radar scan walks the roster** (f4-sensors): the Search branch
  refreshes with its stamped clock and walks members with FRESH
  transform reads, re-applying the clutter + range gates idempotently
  — candidate sets, order, and the RNG streams the rolls consume are
  the uncached walks' (the FID-OPT-3 clutter-invariance pin passes
  through the roster path byte-for-byte: 0 vs 2,000 parked entities,
  identical per-seed detection timelines). Track mode untouched. The
  only observable delta: a behavioral flip joins within the
  revalidation window (1 s of sim, `roster_revalidate_s`, data) —
  pinned both directions (the taxi launch joins at the cadence; the
  landed member drops at the scan's fresh gate immediately).
- **The picture walk walks the roster** (f4-simulation,
  `push_air_picture_`): contacts ride the roster (same values, same
  order, fresh clutter gate per member); the Step-13 datalink nodes
  collect from their own populations — the AwacsComponent +
  RadarComponent ref buckets merged by slot index, reproducing the
  interleaved walk's node order and team-intern order exactly, with
  node liveness still a fresh per-walk read (the GCI-ghost kill keeps
  its ≤100 ms bound).
- **The per-unit detection cadences** — the AGG-2a primitive's named
  consumer: `RadarSimComponent::scan_phase_s` (default 0.0) primes
  the per-radar sweep timer inside its interval; the armed spawn
  paths fill it from the due-queue's FNV-1a `vu_hash` when the
  scenario's `stagger_sensor_phases` gate is on (the campaign path
  keys the flight's VU per arm index, the scenario path the radar
  seed + index; 1,000 phase slots). Default OFF = every pre-AGG-2b
  spawn schedule byte-identical; the armed war's 48 co-mounted radars
  stop sweeping on the same tick once a session opts in (the
  reference's HOTSPOT_FIX "spread the herd", made replay-stable).
- **Tests** — `test_entities_air_roster` (11 cases, label
  f4-entities: priming, epoch, cadence, both flip directions, order,
  the radius surface, move-op self-healing); 4 new radar pins
  (the flip cadence, the landed member, the phase schedule exactness,
  phase-moves-timing-not-outcomes); 2 new sim pins
  (`AirPictureRosterHoldsTheNonClutterPopulation` — the parked ramp
  bird never joins, the launched missile joins through the epoch;
  `StaggerSensorPhasesKeyPrimesTheRadarPhases` — the key's golden
  identity). The pre-existing suite's failures are the CAMP-TOT-PACE
  re-pins, unchanged by this tranche (verified clean-tree vs
  work-tree: the same set on both).

## AVIONICS-2 — the FCR page: the radar page as an SM + the lock hand-off

The avionics plan's second tranche (Docs/AVIONICS_PLAN.md §4) — the
fire-control radar page exists as avionics logic:

- **`fcr_page.hpp`** — the `RWS/TWS/VS` mode machine as a pure
  transition table on f4-state-machine (every transition AND refusal
  pinned); the lock state with the reference's rules (refused with the
  page Off, refused for untracked/Dropped targets, page lock ==
  radar lock by construction, mode switches keep the lock, PowerOff
  clears it, track death drops it) and the renderer-facing
  `FcrPageSnapshot` (page-relative azimuth wrap, slant range, signed
  closure, IFF, the Established-or-Coasting flag, the designated flag,
  the live scan frame; VS = velocity-only display semantics, no new
  radar physics).
- **The lock hand-off is the radar's own primitives** — the tranche's
  enabling discovery: `RadarSimComponent::command_track/command_search`
  already implement the reference's lock behavior and the AI's
  fire-control gate (`MissileModule::should_fire` via the policy's
  radar leg) already honors a live track. The page DRIVES those
  primitives and writes nothing else: zero sim-loop bytes changed,
  AI-only runs byte-identical by construction, and the AI still never
  consumes f4-avionics (the charter's parallel rule).
- **The done-when, proven in a scenario** — `test_fcr_page_flight`:
  the bandit starts inside the player's ±60° bar at 12 NM and files
  east out of it. Search alone: the track decays and drops, the radar
  leg dies, the gate closes. The page's designate parks the radar in
  Track mode (which scans the locked target regardless of the search
  volume): the leg stays lit the whole flight. break_lock closes it
  again. Page inputs → the AI's own can_fire path.
- **Wiring** — f4-avionics links f4-sensors (beside f4-flight-api/
  f4-geo/f4-state-machine; still never f4-ai). 18 unit tests
  (`test_avionics_fcr`, label `f4-avionics`) + the scenario test
  (`test_fcr_page_flight`, label `f4-simulation`).

## DATALINK-1 — Step 13 closed out: the sim-side datalink tiers

The AI Implementation Plan's DatalinkTier (§15 Step 13) is complete —
both halves landed. The f4-ai half (the `DatalinkNet` primitives, the
fusion's optional net leg, the 15-case geometry/gate suite) arrived in
f4 batch 2; this tranche lands the f4-simulation remainder:

- **The scenario node stamp** — the per-aircraft `"awacs"` flag now
  stamps `AwacsComponent` in `attach_combat_loadout`, the scenario-mode
  twin of the two campaign spawn paths' `mission_is_datalink` stamp
  (the header's documented claim, previously parsed-but-unconsumed).
  Unconditional, like the campaign stamps: the walk's `gci_datalink`
  gate is the fidelity switch.
- **Liveness on both sides of the mask** — the walk skips killed
  entities when collecting NODES (the shot-down AWACS stops
  broadcasting the same walk — no GCI-ghost through a dead radar) and
  when filling the contact bitmask (a splashed contact's mask entry
  answers 0 — the policy's corpse early-out, extended to the net leg;
  aggregate flight VUs are never resolved through the entity
  database). Verified to catch both bugs: with the fix reverted the
  new liveness tests fail exactly as the plan's Step-13 test list
  predicts.
- **`test_datalink_tiers.cpp` (8 cases)** — the sim-side suite the
  batch-2 CMake note named: the stamp, the no-nodes twin (gate on ==
  gate off, TargetInfo member-for-member), the coverage commit beyond
  own radar (151 NM contact seen through the net alone, `threat_target`
  commits), the per-team bitmask isolation (blue's node lights blue's
  leg, never red's, in one run), node death, the corpse rule, the live
  horizon clamp (the walk re-reads the component's geometry every
  walk), and the ground-site arm (`gci_ground_sites` collects
  radar-bearing objectives; the arm off leaves the same world dark).

Gate-off runs remain byte-identical by construction (every change lives
behind `datalink_gate_on`). Neighboring suites green: passive fusion
8/8, combat integration 29/29, sensor fidelity 7/7, countermeasures
4/4, the f4-ai datalink net suite 15/15.

## AVIONICS-1 — the f4-avionics scaffold: INS + steerpoint navigation

The avionics plan's first tranche (Docs/AVIONICS_PLAN.md §4) — the
engine-agnostic avionics layer exists. New header-only `f4-avionics`
(namespace `f4::avionics`), runtime-side in the boundary verifier:

- **The INS** (`InsUnit` + `InsConfig`) — alignment as a PURE transition
  table on f4-state-machine (`make_ins_machine()`: Off → Aligning →
  Aligned, Shutdown from both live states; side effects in `update()`, the
  stall-SM polling→event bridge). The align clock accrues only on the
  ground ("alignment completing on the ground clock"); completion zeroes
  the chain and seeds the walk. The stored heading/altitude/position
  chain reports truth-plus-drift; before Aligned it reports raw truth.
- **The drift integral is a seeded deterministic walk keyed on the
  airframe's nav data** — `nav_data_key` (FNV-1a) + `nav_data_age_days`
  seed and scale a bounded rate walk; the sampler is a fully specified
  splitmix64 (NOT std:: distributions) so the same seed fed the same
  update stream is byte-identical cross-platform. v1 is an error model
  over the IAircraftState seam (the strapdown-integrator tranche is
  named, not built).
- **Steerpoint navigation** — `SteerpointSequence` (loud
  `std::out_of_range`, `next()` reports the end wall), `to_steer()` (BRA
  over f4-geo: slant range, true bearing), `steer_cue()` (the HSI cue,
  dead-astern pins right — pinned), and `current_steer()` /
  `current_steer_cue()` reading through the drifting INS.
- **Tests** — 24 (label `f4-avionics`): the ground-clock align pins, the
  drift-zero twin equal to raw truth member-for-member, determinism per
  seed AND per update stream, nav-data keying, clamp bounds, the
  through-INS vs raw reads, the cue pins.

## AGG-2a — the transition-triggered publishers + the due-queue primitive

The AGG-1 follow-up (Docs/AGGREGATE_CLOCK_PLAN.md §4): with the pass
already off the tick stream, the two remaining O(theater)-per-pass walks
were the objective-damage sync (every snapshotted objective, every pass)
and the verdict compute (every objective, every pass, though its event
fires only on change). Both are now transition-gated, and the plan's
deterministic scheduler primitive is landed.

- **The damage sync goes dirty** — the sink subscribes to the bomb
  impact event (it already resolved strike targets there) and marks the
  touched objective's snapshot row; the session's repair mirror marks
  the rows it writes. The per-pass call is
  `sync_dirty_objective_damage()` — the shared per-row diff/book body,
  iterated ascending by snapshot index (which IS wire order), so the
  changed subset's records land in exactly the order the O(objectives)
  walk would have booked them. The full walk survives as the end-of-run
  form (QC, writeback, tests) and also consumes pending dirt. A quiet
  pass is an O(1) no-op; the delta buffer clears before the walk, so
  what the caller drains is what THIS sync collected.
- **The verdict emit is capture-gated** — the verdict's coarse state
  (band/leader/swing) moves only when a capture moves the territory
  census (`compute_theater_verdict` reads ownership flips; the ledger's
  loss/strength rows feed the query face's team rows, never the event),
  so the O(objectives) verdict compute runs only on passes whose
  capture-log tail advanced. The query face (`verdict()`) stays
  on-demand for the viewers. The gate starts TRUE: a mid-war save's
  loaded advantage still publishes its first verdict event.
- **The due-queue primitive lands** — `f4-campaign/due_queue.hpp`:
  `DueQueue<Payload>` keyed strictly `(due_time, priority,
  insertion_seq)` (determinism by construction; node-handle pops;
  visitor-scheduled re-arms fire within the same pass, key-ordered),
  plus `vu_hash`/`stagger_phase` — FNV-1a over the VU, NOT std::hash
  (implementation-defined) — phasing per-unit first-due times the way
  the plan's AGG-2 specifies (the reference's rand() jitter, made
  deterministic). Consumers: AGG-2b's per-unit detection cadences and
  AGG-4's discrete-event scheduler.
- **Verification** — a runtime probe drives two identical 3-objective
  worlds through the same five passes (quiet / one strike / two strikes
  / quiet / repair): world A per-pass full-syncs, world B dirty-syncs —
  the ledgers' byte-stable `to_json()` documents are byte-identical at
  every pass and at the end, and the negative surface (unknown-entity
  mark, target-less impact, markless sync) books nothing. 18/18 probe
  checks pass; 12/12 due-queue primitive checks pass. New tests:
  `ResultSink.DirtySyncIsTheFullWalksShadow`,
  `ResultSink.DirtySyncBooksTheRepairMark`, and the
  `test_due_queue` gtest (registered in f4-campaign/tests).
- **The δ1 threshold-deferral is retired, honestly** — the plan
  expected AGG-2's scheduler to defer sub-frame-rate drivers' δ1 passes
  past a debt threshold. With both fixed walks dirty-gated, a δ1 pass's
  remaining fixed cost is the engines' O(1) gated ticks plus the
  O(changes) emits plus the O(flights) tier heartbeat — and the
  heartbeat (the death fold, the deagg triggers) must NOT be deferred:
  it watches the sim, and deferral trades observable latency for
  nothing. The heartbeat's surface shrinks with AGG-3's bubble
  economics instead (see the plan's as-built note).

## AGG-1 — the campaign pass leaves the tick stream (the catch-up clock)

The time-compression ceiling's structural remainder
(Docs/AGGREGATE_CLOCK_PLAN.md §1, cost multiplier 3): the campaign
ladder, the ground/naval/flight engines, the damage sync, and the eight
`emit_*` event walks all rode the 60 Hz accumulator, sliced into whole
campaign seconds — the campaign layer paid O(theater) once per campaign
second, so its cost scaled with compression even in Tiered mode.

- **One pass per advance() call** — `advance()` drains the sim
  accumulator first (unchanged: fixed dt, step-capped, honest drop),
  then fires ONE campaign pass carrying the drained whole seconds as
  ONE engine delta (`ladder_->tick(delta)` big tick == N small ones,
  the C2 pin; each engine's due gates fire every update at its own
  boundary inside the big delta). A drain that completed no campaign
  second runs no pass. The per-second accumulators
  (`ground_sec_accum_`/`naval_sec_accum_`/`flight_sec_accum_`) are
  gone — `advance_ground_/naval_/flights_` take the pass's delta.
- **The booking is the product, not a sum** — the campaign delta is
  `steps × sim_dt` (240 × 1/60 == 4.0, bit-exact), not 240 additions
  of sim_dt (3.9999999999999907 — the ~1e-14 drift would quantize the
  pass boundaries a whole second off). The campaign clock cannot
  diverge from the sim clock: the delta IS the drained sim time.
- **Batch drivers win ∝ batch length** — the war harness's 4-second
  batches, the scenario player's drains, and the replay's runs now
  run ONE pass-set per batch instead of one per campaign second; the
  campaign layer's wall share tracks advance() calls (∝ frames), not
  campaign seconds consumed. Frame drivers at presets below the frame
  rate keep δ1 passes (the count there is the clock's; deferring
  passes past a threshold is AGG-2's due-queue's job).
- **The deterministic re-pin, deliberate** (AGGREGATE_CLOCK_PLAN.md
  §5): events batch per pass — emission TIMING moves, content and
  order do not (every walk is a cursor/log-tail diff; the tasking-cycle
  event already reported how many cycles rode together). A big delta
  straddling a tasking boundary fires the cycle at the pass's
  post-tick clock (the books' t_s moves to the batch edge; the one-pool
  totals do not). Pinned both ways:
  `BigCatchUpBatchesMatchAlignedSmallOnesByteForByte` (3 × advance(4.0)
  == 12 × advance(1.0), ledger JSON byte-identical when the big deltas
  land ON the boundaries) and `StraddledBigTickKeepsTheBooksTotals`.
- **The CAMP-HOST replay axis holds** — `step(ticks)` segments around
  the pending `apply_tick` boundaries in the host, untouched; commands
  still apply at exact engine-tick boundaries no matter how the
  session batches its passes.
- The war harness's drained-batch certificate comment now states the
  batch-shape contract (same-shape runs byte-identical; cross-shape
  emission timing re-pins deliberately); the C5 24-hour acceptance
  re-certifies on the next run.

## FID-P1B — the aggregate row follows a live flight; the fold re-anchors the wire; a dead lead folds on the pass

The viewer's campaign-map triad — flights pausing, teleporting, and
disappearing — traced to one cluster: while a flight was materialized,
its aggregate ROW froze at the deagg point, and everything downstream
read that frozen position.

- **The row tracks the lead (`update_live`)** — `sync_live_flight_rows_`
  now writes the lead's transform + monotone fuel into the suspended
  row each tier pass (a new `FlightAggregateEngine::update_live`; an
  aggregate row refuses the write). The reagg bubble, the deagg
  triggers, and the fold's not-killed path all read the row: a frozen
  row made the tier machinery judge a live flight by where it
  MATERIALIZED — folds fired by stale positions, respawn poses landed
  at one, and a dead lead's glyph fell back to one (the
  teleport-to-spawn, the frozen ghost, the fold→re-deagg flap).
- **The fold re-anchors a TIME-mode schedule (`reanchor_schedule_`)** —
  the fold lands the lead's true position, but a TIME-mode row's
  display/advance re-derive position from the wire schedule, so the
  very next read snapped the glyph back to where the schedule said the
  flight should be (the live window flew at real speeds while the wire
  modeled ~121 kts — tens of grids backward, to near the base it
  left). The fold now slides the whole arrive/depart schedule by one
  constant so it passes through the folded position AT the fold time:
  route shape, leg durations, and dwells are the save's own; only the
  clock they run on moves. A pre-departure fold keeps the wire (the
  takeoff gate still owns it).
- **A dead lead folds on the NEXT tier pass (FID-P0b)** — the tier
  pass now folds a flight whose kill is booked (the EntityKilled feed
  — aircraft deaths never touch the ALIVE tag) or whose lead cannot
  produce a live roll-up, BEFORE any pin/bubble/cooldown check. The
  old shape left a killed flight's row suspended at its pre-deagg
  position until the 2×600 s ops pin expired — a paused ghost drawn
  where the flight materialized, then a fold to `mark_destroyed`.
- **`ALIVE`-tag reads fixed** — `TagValue::as_bool()` is a `get_if`
  POINTER: the old `!alive->as_bool()` read the pointer's truthiness,
  so a present-but-FALSE tag read "alive". The shared
  `live_lead_position_` helper (fold verdict, tier pass, row sync,
  tier snapshot overlay) reads the value.
- **TIME-mode arrival fires past an unscheduled tail** — legs after
  the last scheduled waypoint have no arrival to fire; a route with an
  unscheduled tail never marked the flight arrived and it sat frozen
  at the last scheduled waypoint forever. The engine now pins each
  TIME-mode route's terminal arrival (construction-time index; retask
  flips the mode, so it stays valid) and arrives when it passes.
- **The map reads terminal rows honestly** — a destroyed flight draws
  a wreck cross sized with the glyph (the old fixed 3-px, 59%-alpha
  speck read as "vanished"), and aborted (scrubbed) rows dim like
  HOME; the viewer's FlightRow parses the row's additive `aborted` key.
- **CAMP-SAVE-WIRE — the stock saves' multi-day wires cannot drive
  motion** — the "none of the flights move until I click" report:
  every real-world save's waypoint times are the ATO planner's
  horizon, not a motion schedule (computable legs cross Korea at
  ~0.01 grid/min — weeks per leg; median first arrival 14.5 days
  out). Strict TIME-mode interpolation turned that into imperceptible
  creep for the whole war. A timed route whose computable legs ALL
  imply a crawl below `FlightAggregateConfig::
  min_leg_speed_grid_per_min` (1.0) — or that has no computable leg —
  now constructs as SPEED mode at the campaign cruise; sane wires and
  mixed wires (a slow loiter leg inside a fast route) keep the
  TIME-mode identity, and 0 disables the fallback. Pinned by
  GarbageWireConstructsAsSpeedMode + SaneAndMixedWiresStayTimeMode.
- **The fold keeps the pace the viewer watched** — the fold books the
  AIRBORNE lead's actual ground speed (clamped to [the config cruise,
  ~460 kts]) as the row's own SPEED-mode cruise (`reaggregate`'s
  `cruise_grid_per_min`; 0 = the config default). A flight that flew
  past the camera at 400 kts resumes its aggregate at that pace
  instead of braking to the 121-kt estimate — the "it moved for a bit,
  then it stopped" read. The deagg spawn pose, the air picture's
  contact velocities, and the convergence trigger's predictions all
  read the same per-row effective cruise.
- **An airborne lead re-anchors past a still-closed wire gate** — a
  bubble/ops deagg can take an unlaunched flight off the ramp early;
  when it folds back, the wire's depart is still in the future and
  the old guard froze the glyph at the fold point until the gate.
  `reaggregate` takes `lead_airborne`: a grounded complement keeps
  the wire, an airborne lead re-anchors (the shift lands the gate in
  the past — exactly where a flying sortie's gate belongs).
- **CAMP-SAVE-WAVE — the initial cycle's wave launches near** — the
  deeper half of the same report: the stock saves (save0/1/2 +
  Instant) decode ZERO flight entities (the user's save1: 683 units —
  524 battalions, 85 brigades, 72 squadrons — and not one flight), so
  the whole visible war is the tasking ladder's, and the ATM's TOT
  rule (the profile midpoints, median +120 min) filed the first
  wave's deliveries two hours out with gates riding TOT − 1200 s.
  Measured on the real install: `cycles 1, missions 100, live 1` two
  minutes into a running save1. `CampaignSessionOptions::
  near_initial_wave` (the viewer arms it with initial_tasking_cycle)
  clamps the INITIAL cycle's intents (issued at ladder time 0) to
  launch inside the first ops window; the delivery TOTs stay the
  planner's and later cycles keep the profile schedule. Same run
  after: `live 49 (48 airborne)` at campaign +2 min. Pinned by
  InitialWaveLaunchesNear (the bus-published initial-cycle intent
  gates ≤ 600 s; a later cycle's keeps TOT − 1200 s).
- **Pins**: 4 new engine tests (live tracking, the re-anchor + its
  pre-departure guard, the unscheduled-tail arrival) plus the
  wire-fallback / fold-pace / airborne-reanchor trio, 2 new session
  tests (the row tracks the lead and the fold sticks at the booked
  pace; a booked kill closes inside one pass through a live ops pin),
  and the A/B divergence's suspended-row fuel pin re-locked to the
  tracking contract. FullFidelitySpawnsAndHasNoEngine now requests the
  policy explicitly (the FID-DEF-GOV sweep missed it).

## CAMP-TOT-PACE — the generated air war learns when its own deliveries are scheduled

The 6-h stock-save QC (test_campaign_airwar_qc, F4_STOCK_WORLD-gated —
a 524-flight measured war) condemned the pacing: takeoffs worked
(353 launched, 350 airborne), recoveries worked (265 home, zero
lost), and **not one of 112 matured deliveries was inside ±5 min of
its TOT — median +23 minutes late**. Three causes, three fixes:

- **The gate paced nothing** — `TOT − 1200 s` flat, whatever the
  geometry. The gate now PACES THE LAUNCH: base → the route's
  delivery waypoint at the aggregate cruise
  (`gate = TOT − ingress/cruise`), so the aggregate arrives when the
  plan says. CAP racetracks (no delivery leg) keep the old window.
- **The ops pin flew the whole ingress live** — a takeoff-window
  deagg pinned its flight for 2×600 s: 20 minutes at real speed
  overflew the target long before the TOT, and the post-fold cursor
  only stumbled past the delivery waypoint minutes later. A pure
  takeoff-window deagg now takes `Trigger::OpsTakeoff` and the short
  `takeoff_pin_sec` (300 s default — the ATC sequence: hold,
  teleport, roll, climb); the delivery and recovery windows keep the
  full ops pin (the attack and the approach run live).
- **CAMP-SAVE-WAVE clamped the wrong end** — the initial wave's GATE
  moved under the planner's 2-h TOT, so the wave launched on load and
  transited its targets ~90 min early. The clamp now moves the WAVE'S
  TOT (`now + ingress + 2×ops_window`) with the recovery deadline
  shifted to match: launch and delivery stay consistent, and the map
  is still alive inside the first minutes (the takeoff window opens
  one ops window before the gate).

State after the first pacing tranche: the median swung to −12 min
(early), then +21 (the feasibility floor exposed the unreachable-TOT
cohort), and the ±15-min band holds ~20% of deliveries — the scatter
is structural (the live windows, the folds, and the mode
classification each shift individual flights). The measured war,
the harness, and the open bar live in
test_campaign_airwar_qc (F4_STOCK_WORLD-gated) and the plan ledger's
AIRWAR-QC finding; the remaining convergence is a documented design
tranche, not another constant.

- **CAMP-SAVE-WAVE rev 2 — the wave staggers per base** — the first
  clamp collapsed every initial-wave gate to the same second: a
  49-aircraft simultaneous launch, and every flights-table "window"
  countdown reading the same number (the user's "all the aircraft
  launch at the same time"). The wave now queues PER BASE (one
  runway, one departure per 15 minutes, bases concurrent, wire-order
  deterministic, the queue capped at 4 h), and each flight's TOT
  derives from its OWN gate (+ ingress + one ops window), so the
  takeoff times differ and the deliveries spread with the launches.
  Pins: InitialWaveLaunchesNear (first-at-base gates at
  now + ops_window; later cycles stay distance-paced).
- **CAMP-GATE-ROLL + the takeoff-time column** — "aircraft should
  take off on their takeoff time. Right now it seems like they take
  off before": the takeoff-window deagg materializes a flight one ops
  window early and the 45-s parking hold rolled it ~10 min before its
  gate. The parking hold now STRETCHES to the gate
  (`runway_wait_s = to_depart − 45`, clamped [45 s, 2 h] — the 45 s
  covers the teleport + roll), and the OpsTakeoff pin covers the wait
  (through liftoff + 2 min). The flights table's "window" column
  shows the gate as a takeoff TIME on the campaign clock
  (D375 09:15-style), with the old countdown in its own "count"
  column (countdowns are for waiting; schedules are for reading).

## VIEWER-QC-2 — the stock-save session's four: damage-event dupes, mystery rings, plan spaghetti, the landing flyaway

The save1 play-through QC's catch, four for four:

- **Duplicate "objective damaged:" events** — the result sink's damage
  diff ran against a snapshot that NEVER advanced: the first delta
  re-reported every pass, and the session re-published the event each
  second per damaged objective. The snapshot now advances to what was
  just reported (the diff is against the last SYNC), the event face
  pins empty on the second pass, and the ledger stops growing one
  damage record per objective per second.
- **The static yellow circles** — the objective pass drew a gold ring
  on every objective with priority >= 40 (most of the map on a stock
  save) — unexplained clutter. A ring now MEANS "a filed ATO mission
  targets this objective": the pass builds the target set from the
  snapshot's tasking rows each frame.
- **Flight-plan graphics (FF-MAP)** — the live plan drew one strong
  owner-color polyline with numbered dots, the approach included: the
  plan line TO the landing field read as a leg the flight will fly.
  FreeFalcon conventions now: a faint plan polyline, a faded gray leg
  from the aircraft to the waypoint it is FLYING (the pursue leg,
  `navigation().current_waypoint_index()`), NO plan line into the last
  landing waypoint, circles on airfields (filled = primary, hollow =
  an earlier/alternate), triangles on A-G delivery waypoints, boxes on
  AR (refuel) anchors, numbers only zoomed in.
- **The landing flyaway (CAMP-LAND)** — "aircraft come back, switch
  into approach and then just fly off into infinity": the campaign
  session built its per-airbase airfield map for the SPAWNER but never
  registered it with the ATC (the scenario path's
  register_campaign_airbase_airfields did; the campaign path had no
  counterpart). Every LandingRequest fell back to the stub's empty
  default field — threshold at the theater origin — and the recovering
  aircraft chased approach data off the map. `Simulation::
  register_airbase_airfields(map)` registers the campaign's map; the
  session calls it at create. Reproduced end to end over the canonical
  save1 export in test_campaign_stock_landing (F4_STOCK_WORLD-gated):
  the RTB'd flight's OWN clearance now names its own base's field
  (< 25 grid from the landing waypoint), the approach engages
  (RequestApproach -> ProceedToFix), and the flight never diverges
  from the field.

## FID-DEF-GOV — Tiered is the default fidelity policy; the runner's dilation becomes an AIMD delivery governor

- **The acceleration verdict becomes the default** — `CampaignSessionOptions::fidelity_policy`
  now defaults to `Tiered` (FID-6's certificate: 58.1× tiered vs 25.3×
  FullFidelity on the same war; FullFidelity's whole-war FM walk is the
  CPU limit the presets outrun long before 60×). This is the original
  game's own economics — aggregates everywhere, the flight model only
  near the eye — and the viewer's "fidelity tiers" checkbox already
  read this way. FullFidelity stays a first-class mode: every rig that
  pins the pre-FID spawn-at-init shape (test_campaign_session,
  test_strategy_layer, test_campaign_supply,
  test_campaign_personnel_session, test_flight_state_diag) now sets it
  EXPLICITLY, so intent is pinned rather than inherited.
- **The delivery governor (the HandleCampaignThread lesson)** — the
  viewer runner fed `wall × preset` and DROPPED whatever a capped
  batch couldn't drain: every unsustainable preset silently moved the
  clock at the same CPU-bound rate. The reference instead halves its
  compression when the campaign falls behind and restores it on
  catch-up ("Slow things down" / "Back to full speed",
  freefalcon-central campaign.cpp:2680). The runner now does the same
  with an AIMD delivery scale in (0, 1] under the preset: a capped
  batch halves the feed, `kCleanBatchesPerRecover` clean batches
  doubles it back, `set_speed` resets to full feed (a new preset is
  TRIED, then the CPU says what it sustains). Steady-state overload
  now SLOWS the clock instead of losing seconds; `time_dilated()`
  honestly reports only the residual case (drops resumed at the
  floor), and the new `delivery_scale()` readout gives the UI the
  governor's answer ("240x — delivering 6.2x").

## DEAGG-RWY-1 — deaggregated flights hold, teleport to the runway, and the stock-save ATC knows every field

- **The campaign takeoff fix, twice over** — the user report
  ("aircraft taxi to their target in the campaign and never take
  off") had two stacked causes. (1) STOCK-SAVE ATC: a save that decodes
  no Flight-class units (72 squadrons, 0 flights — every stock .cam)
  runs its session in ScenarioList mode, and the per-base ATC airfield
  registration lived only inside the CampaignFlights spawn path — so
  the ATC kept ONLY the theater-origin fallback field and answered
  every TaxiRequest with a cross-theater route no aircraft could ever
  finish taxiing. The derivation + registration is now
  `Simulation::register_campaign_airbase_airfields()` (member-cached,
  idempotent, shared by BOTH spawn paths), called in `initialize()`
  for ScenarioList right after `wire_atc()` and before any brain can
  publish a TaxiRequest. (2) DEAGG-RWY: the parking → hold-short taxi
  crawl itself is gone for deaggregated flights (see next bullet).
- **DEAGG-RWY — deagged flights hold a set time, then teleport onto
  the runway** — a ground-deaggregated campaign flight no longer taxis
  the parking → hold-short route at all. The TakeoffModule gains
  `wait_then_teleport` + `runway_wait_s` (default 45 s): the flight
  holds brakes at parking for the dwell, then publishes a new
  `RunwayTeleportRequest`, and the HOST (Simulation — the owner of the
  flight model + the per-airbase airfield data) snaps the airframe
  onto its field's runway threshold, lined up on the runway heading,
  via the new `FlightModelComponent::snap_to_ground()` (the FM's NED
  kinematics are the authoritative write — a transform-only write
  would snap back on the next FM → transform sync). From the threshold
  the unchanged FSM runs HoldShort → TakeoffClearance → lineup (the
  alignment gate passes on its first check) → roll → FlyOut → Done.
  FreeFalcon provenance: imminent-slot deaggregated flights are placed
  DIRECTLY ON THE RUNWAY (`GetVehicleDeagData`'s TakeoffPt branch →
  `ATCBrainClass::FindTakeoffPt`) — the teleport is that placement,
  applied after a configurable wait instead of at deagg time. The ATC
  protocol (TaxiRequest/TakeoffRequest/clearances) is untouched, and
  scenario-list spawns (hand-authored parking + taxi routes, the
  kunsan-style fixtures) keep the full taxi behavior. Spawn paths
  arming the mode: `spawn_aircraft_for_flight` + `spawn_aircraft_for_intent`
  (every ground spawn, `air_pose == nullptr`).
- **DEFAULT-FIELD — the world backs the ATC's default airfield too** —
  in ScenarioList mode `Simulation::apply_world_default_airfield()` now
  derives the DEFAULT airfield (the one airbase_id = 0 aircraft
  resolve — scenario-list spawns never tag a home base) from the
  world's registered per-base fields, choosing the one nearest the
  scenario aircraft's parking centroid. A hand-authored scenario
  airfield (a real taxi route) always wins; session-generated
  scenario_list scenarios keep their self-consistent origin field.
- The ScenarioAirfield → AirfieldConfig copy the three registration
  sites performed by hand is one shared `to_atc_airfield()` helper now.
- **Numbers** — the showcase world's 449-flight full-fidelity QC: the
  old behavior left 441/449 airborne after a 20-minute session (8
  still crawling in the ground regime); DEAGG-RWY puts 449/449 in the
  air within ~2.5 sim minutes of their deagg, every one walking
  Taxi → HoldShort → Takeoff → FlyOut → Done → ToWaypoint at the
  production 45 s dwell.
- Tests: `test_deagg_takeoff` — the session-level spawn → takeoff chain
  (ground flight holds at parking, jumps > 1000 ft to the threshold,
  reaches TakeoffState::Done — the first end-to-end takeoff regression
  the repo has) and the DEFAULT-FIELD pin (a world-backed scenario_list
  config with no hand-authored airfield answers TaxiRequest from the
  world's own base, not the origin).

## SVG-HAIRLINE-1 — the hairline pen stops rendering as a half-symbol bar

- **Widthless `vector-effect:non-scaling-stroke` imports as 1 screen px** —
  Inkscape's hairline pen writes `vector-effect:non-scaling-stroke` +
  `-inkscape-stroke:hairline` with NO `stroke-width`; the importer's
  1.0-viewBox-unit Style default fell through `add_stroke`'s ×32 width
  math, so every hairline shape imported at 32 px — half the symbol
  extent. The user-visible face: `unit_fighter.svg`'s closed squadron
  frame border (the dome) rendered as a fan of fat bars perpendicular
  to the arc (and any Inkscape-drawn hairline stroke would do the
  same). The width now seeds to 1 px at the 64 px reference when
  `non_scaling_px` turns on and no stroke-width has arrived
  (`Style::stroke_width_set` distinguishes the default from a written
  1.0); an explicit width (style or presentation, inherited down the
  tree) still wins, and `apply_style_attr` keeps applying
  vector-effect first so a later style width overwrites the seed.
- Blast radius was surgical: a corpus-wide import diff shows exactly
  three width changes (32 → 1) — the squadron domes in
  `unit_fighter.svg` + `unit_transport.svg` and the fighter's third
  glyph stroke; geometry (points/closed/roles) identical across all 75
  overrides, and both pinned width regressions hold (viewBox-unit
  0.1 → 3.2 px; explicit non-scaling `stroke-width:1` → 1 px).
- `symbols/unit_fighter.svg`'s title/desc/metadata said "Transport"
  (the clone the fighter was redrawn from) — now "Fighter"/"Squadron +
  fighter silhouette", matching the corpus JSON's description.
- Tests: `InkscapeHairlineWithoutStrokeWidthIsOnePx` (the dome imports
  closed at 34 points and 1 px) + `HairlineRespectsAnInheritedExplicitWidth`
  (explicit style width beats the seed; a widthless sibling under a
  vector-effect <g> inherits the 1 px hairline) in test_svg_import.

## SENSOR-FUSION-2 — the airframe card resolves, the blind spot is pinned, and CI grows the Windows + sanitizer legs

- **The airframe IRST card resolves from the host's library** — the
  fusion tranche attached the IrstComponent with the component's
  hard-coded defaults (which mirror the shipped generic card); now
  `attach_combat_loadout` / `arm_campaign_combat` take the host's
  loaded `IrstSensorData` (the scenario path and the campaign arm both
  pass `&ir_seeker_data_` — the library the missile-seeker side
  already parses) and `apply_irst_airframe_card` overlays its
  "generic" row onto the fresh component (az/el/range/ground factor;
  `flare_chance` deliberately NOT applied — that is the missile-seeker
  consumption number, meaningless for the airframe sensor). No
  library, or a library without the row, keeps the defaults —
  byte-identical, the golden identity. A host that points
  `ir_seeker_data_path` at a different card set now re-shapes every
  airframe's IRST eyes with the data, not a recompile.
- **The blind-spot geometry is pinned end to end** —
  `test_passive_fusion` (8 tests) drives the full `Simulation::tick`
  over the tranche's named shape: both C6-boresighted radar bars miss
  the bandit by 55° while the IRST/eyeball hold it (6 NM aft-quarter),
  the fold lights `detected_by_visual` with the radar leg false, the
  spawned brain makes the bandit its threat target sampled at FIRST
  contact (the passive-first ordering is part of the contract), the
  gate-off twin of the same geometry stays exactly the pre-FUSE
  silence, the airframe card test proves the library hookup, and the
  corpse rule covers both passive legs. `test_sensor_fidelity` pins
  the tranche's semantics over a hand-rolled policy world; this file
  pins its geometry over the real host.
- **CI-WIDE: the suite gains the windows (MSVC headless) and sanitize
  (ASan+UBSan) jobs** — the dev platform and the leak class the
  FID-OPT-1 UAF exposed were both uncovered: an MSVC-only conformance
  break landed untested, and nothing in CI would have caught the same
  leak class on main. Both jobs fly the headless shape (GUI targets
  off, boundary gate ON) with the Data/ manifest hygiene check up
  front; the sanitizer flags ride `CMAKE_CXX_FLAGS` so the FetchContent
  deps sanitize too.
- **BUILD-FIX: test_ecm compiles on GCC 14** — the `HeadOn` harness's
  default argument (`Jam jam = {}`) needed the nested aggregate's
  defaulted constructor while `HeadOn` was still incomplete; MSVC and
  Clang allow that, GCC 14 rejects it — the test did not compile on
  the repo's own CI toolchain. `Jam` hoists to namespace scope, every
  compiler reads the same file.
- **DATA-MANIFEST: the committed manifest matches the committed tree
  again** — the manifest had been regenerated on a machine where
  `f4import` had produced the gitignored `Models/koreaobj` files: it
  listed 3,962 files absent on every clone (both the CI hygiene step
  and `Sha256.ReproducesCommittedManifestFingerprints` failed on
  fresh checkouts of main) and missed the committed
  `Theater/korea/tables.json`. Regenerated against the committed
  tree: absent entries dropped, `tables.json` listed
  (`theater:korea`), fresh clones check green.
- **DOC-DRIFT: README per-library test counts re-pinned to actual TEST
  macros (11 stale rows; `scripts/check_readme_counts.py` added —
  pure-Python, exits 1 on drift)**, and FALCON4_FILE_LAYOUT rows
  corrected (`.tea` ATM block, `.pilot` roster, RCD, AII — all
  verified parsed in the current tree; the not-yet-parsed claims had
  drifted back over landed work).

## THREAT-TABLES-1 — the converted UCD paints the threat map

- **The C3 threat-map coverage gap closes with the data CAMP-SCALE-1
  already exported** — `ThreatMap` takes an optional `TablesContext`
  (the converted `TheaterTables` + the runtime ClassTable): a
  battalion whose world-JSON threat enrichment is all-zero (every
  committed campaign world) resolves its entity type through the CT's
  data pointer to its UCD row, and the full theater's air-defense
  rings paint with no world re-conversion. A unit WITH enrichment
  keeps it; no context (the tests' bare worlds) is byte-identical.
  The campaign session wires the context from the sim's
  `theater_tables_path` load (the route planner's threat map, the
  ATM's SEAD pairing, and the viewer's FLOT all read the same map).
  Pinned by test_threat_map's TablesFallback pair (fallback paints
  against the REAL falcon4.ct fixture; the unit's own enrichment
  wins over the table's row).

## SENSOR-FUSION-1 — the passive legs answer, the jammer degrades, the throttle picks the band

- **The radar-backed policy gains the passive optical legs** — the
  SensorFusion DetectionPolicy hook's remaining seats filled:
  `combat.passive_sensors` attaches an IrstComponent + VisualComponent
  to every armed aircraft (scenario path AND the campaign arm; the
  session carries `CampaignSessionOptions::passive_sensors`), and
  `RadarBackedDetectionPolicy` batch-caches them alongside the radar +
  RWR (the PERF-1 shape) — the `visual` verdict answers from their
  contact books. A fighter with a dead radar still sees, and fights,
  what its eye and IRST hold; the radar verdict stays radar-only (the
  IRST book cannot fabricate a radar track — a radar missile still
  needs the radar). No passive component attached = the lookups miss =
  the pre-fusion verdict, byte for byte. The perf certificate is the
  ARMED generated small war at the 60x preset with the gate on:
  `ArmedWarWithPassiveSensorsHoldsThe60xPreset` — zero dilation (the
  FID-OPT machinery un-collapsed), green, deterministic.
- **The ECM burn-through** — `combat.ecm` + the per-aircraft `"ecm"`
  fit (both must agree; no unit-data source exists, so the campaign
  arm fits nobody) attach an `EcmComponent` (f4-sensors: strength /
  burn-through range / IFF team / enabled). `RadarSimComponent::
  perform_scan` resolves the live enemy pods once per scan (friendly
  never, corpses never, disabled never) to bearing + weight (one-way
  noise, 1/r², saturating inside the pod's burn-through range); pods
  in the scan bar toward a candidate sum (capped 0.95) and the
  detection ramp reads the STRETCHED range `range/(1-W)` — the
  effective detection range degrades, and closing the range wins
  through. `update_rwr` hears the jammers: `RwrWarningType::Jamming`
  (rank Launch < Lock < Jamming < Search; own pod never warns itself;
  corpses stop; the lock/launch brain flags stay silent for noise;
  new strobes transition-publish). No `EcmComponent` in the world = an
  empty-bucket probe and untouched RNG — every pre-ECM fight byte
  identical.
- **Throttle-driven ir_power** — `combat.throttle_ir_power` stamps
  each active aircraft's IR band after update_all from the FM's
  last-flown throttle (≥1.05 → ir2 Max, ≥0.6 → ir1 Afterburner,
  below → ir0 Baseline; one tick of latency, deterministic). Gate off
  = the Afterburner default stands.
- Suites: f4-sensors (new `test_ecm` 9: the burn-through pins + the
  Jamming warning), f4-simulation (new `test_sensor_fidelity` 7: the
  policy legs, the attach gates, the ir_power stamp), the fast war
  harness +1 certificate — all green.

## ATO-START-1 — the war starts by planning (and the symbol color rule is mechanical)

- **Why save0-2 never generated ATO missions** — the ground war fires
  its first orders cycle at clock 0 ("the war starts by planning"),
  but the air tasking ladder's first cycle waited a full
  air_task_cycle_sec (1800 war-seconds ≈ 5-8 real minutes at the 10x
  preset). A loaded stock save carries no ATO in this engine (the
  wire's pre-planned missions are not decoded), so every session
  opened with an empty ATO that most runs never outlived.
  `Campaign::run_initial_tasking_cycle()` fires ONE cycle at the
  current clock — counted in cycles_fired_, scheduled boundaries
  untouched — gated behind `CampaignSessionOptions::
  initial_tasking_cycle` (default false: the QC ledgers keep their
  byte identity); the viewer turns it on. save1 now opens at
  `cycles 1 missions 100 routes 20`, flights table full from frame
  one. `InitialCyclePlansAtClockZero` pins it (tick tests 10/10).
- **The symbol color rule is now mechanical** — "a fill paints the
  background, a stroke paints the foreground, nothing is added or
  removed." Fixes two rendering bugs the Inkscape re-saves exposed:
  (1) the renderer drew an INVENTED 1px outline on every filled
  polygon (the intersection bars grew a stroke their file never had) —
  outlines now render only for outline-only shapes (hover/selection,
  unfilled polygons); (2) a filled+stroked path emitted ONLY the fill
  (the bridge road's author stroke was dropped, and its #333333 fill
  rendered in the contrast color ≈ invisible on the map) — a shape
  with both now emits both, and any fill paints the team color unless
  `data-color-role` says otherwise (the corpus's contrast glyphs carry
  explicit roles, so nothing regresses). `currentColor` strokes stay
  team-colored (the dashed-border convention). README + header
  contract rewritten to the mechanical rule.
- Suites: f4-campaign 372, f4-simulation 387, f4-renderer 15,
  f4-world 405, f4-world-viewer 99 — all green.

## SVG-TOLERANCE-1 — Inkscape re-saves import, and the name tables load from anywhere

- **Inkscape re-saves killed the symbols** — a round-trip through
  Inkscape (the authoring workflow's whole point) moved every paint
  into a CSS `style=""` attribute (`fill:#333333;stroke:#b3b3b3;...`),
  stamped `vector-effect:non-scaling-stroke` hairlines, wrapped the
  document in `sodipodi:`/`rdf:` vocabulary and `<defs>` — and the
  importer failed on all of it (`style` was a named-dangerous
  attribute; `<defs>`/`<sodipodi:namedview>` were unsupported
  elements). Now: the style attribute parses with CSS precedence
  (fill/stroke/stroke-width/fill-rule/opacity/dash identity rules kept,
  unknown properties fail by name); hex grays map by luminance
  (near-black → the contrast black, near-light → white, a MID-gray
  fails as ambiguous); non-scaling-stroke widths read as screen px at
  the 64 px reference; `<defs>`, `<metadata>`, and any namespaced
  element skip whole. Verified against the user's actual re-saves:
  `obj_intersection.svg`/`obj_bridge.svg` import (the export
  round-trip carries the 14-segment edited geometry, not the corpus's
  4 paths).
- **The name/table assets load from any working directory** — the
  loaders walked up only two levels from the CWD, so a viewer launched
  from a deeper directory silently ran with empty tables (no names, no
  types). `resolve_data_path` now adds the baked checkout dir
  (`F4_SOURCE_DIR`), and both loaders report what they loaded (or why
  not) on stdout: "theater names: 1631 (from ...)".
- **Static units gained their names** — the inspector's unit branch
  never resolved anything: header fell back to "Unit" and no Name row
  existed. It now shows the unit's instance name (the PropertyBag's
  `name_id` through the theater table), the CT type name
  (`resolve_entity_type_name` — "F-16C", "Armor"), and a Type name row;
  the header prefers class_name → type name → "Unit".
- f4-renderer 15/15 (svg 20/20 — four new Inkscape-tolerance tests),
  f4-world 405/405, f4-world-viewer 99/99; headless run from the exe
  directory (not the repo root) loads both assets and names captures
  in the Event Log.

## CT-NAMES-1 — the class table names things (the flights table + inspector speak "F-16C")

- **`Data/Theater/korea/tables.json`** — CAMP-SCALE-1's
  `cam2json --emit-tables` output (f4.theater.tables/1: 296 UCD + 285
  VCD + 203 WCD rows with their NAMES), exported from the install and
  committed with the manifest regenerated over it. The runtime reader
  (`f4-world`'s TheaterTables) existed and was tested; nobody loaded it
  for display until now.
- **`resolve_entity_type_name`** (f4-world, next to
  `resolve_countermeasures`): a CT entity type → its display name. A
  VEHICLE row names itself; a UNIT row names its FIRST vehicle when the
  group chain resolves (a squadron of F-16Cs displays "F-16C", not the
  UCD's generic role word "Attack"/"Airlift"), falling back to the
  unit-class name. The live-aircraft inspector gains a `Type:` row
  (flight's own row first, else its squadron's) and a header line; the
  Campaign Session's flights table gains a `type` column. The viewer
  loads the CT + tables at startup (same Data/ walk, fail-soft).
- `test_theater_tables` gains ResolveEntityTypeNameChain (5/5);
  f4-world 405/405, f4-world-viewer 99/99.

## NAMES-1 — the theater name table (the log names things)

- **`korea.idx` + `korea.wch`, decoded at last** — the strings behind
  `ObjectivePriorityComponent::nameid` (and `SquadronUIInfo::name_id`)
  are a per-theater pair in `campaign/SAVE/` (FreeFalcon
  `CAMPLIB/Name.cpp`: the `.idx` is `short count` + `short offsets[]`,
  the `.wch` is the raw string stream; entry 0 is "Nowhere"). The repo
  parsed the nameid BYTE everywhere but resolved it NOWHERE — the Event
  Log's `#NNN` rows were that gap made visible. Proven against the
  real install: nameid 744 → "Posong-ni".
- **`scripts/export_names.py` → `Data/Theater/korea/names.json`** — the
  pair rewritten as one `f4.theater.names/1` document (1631 names),
  committed with the manifest regenerated over it (`--check` green).
  The no-binary-runtime convention: the install's binary name data
  crosses as a fingerprinted JSON asset.
- **The viewer resolves** — `theater_names.hpp/cpp` (pure, tested) loads
  the document at startup (same Data/ upward-walk as the symbol
  library); `objective_display_name` gains the nameid hop:
  class_name (airbases/cities) → name table (small front objectives) →
  raw id. The Event Log, the session feed, the ATO target column, and
  the inspector's objective header/Name row all share the resolver.
  A nameid past the table (saves made under a larger install table —
  TestCamp's squadrons carry 3817+ vs this install's 1632) bounds-check
  to "" and render as ids, never OOB.
- `test_theater_names` 3/3 (parse, malformed rejects, bounds-checked
  lookup); f4-world-viewer ctest 98/98.

## EVENT-LOG-1 — the world viewer's running log (the war prints its book)

- **The Event Log window** — the original game printed a scrolling
  theater log; this viewer's only event faces were four-second yellow
  capture rings and a 14-row feed buried in the Campaign Session window
  that silently DROPPED the pilot/roe/slot families (`default: return
  false`). Windows > Event Log (auto-opens with every session start, "
  like the original") is the log: every drained campaign event frozen
  into display text AT ARRIVAL — the objective-name resolver runs while
  the session that owns the ids is alive, so the log reads back after
  the session is gone — newest at the bottom, team-colored via the map
  palette (`color_for_owner`), substring-filtered, follow-the-tail,
  clear button, 2000-row cap.
- **One formatter, two faces** — `event_log.hpp/cpp` is the ImGui-free
  model (store + envelope accessors + `format_campaign_event_label`);
  ALL 15 v1 families have a face now (`test_event_log` pins it — the
  log never drops a family). The Campaign Session window's compact feed
  delegates to the same formatter and gains the personnel/scheduling
  lines for free. The drain hook lives in `refresh_session_snapshot`
  next to the feed/capture-marker fills; a fresh adopt clears the log.
- **The capture rings get the same repair** — the yellow decaying rings
  resolved captured objectives through the SESSION engine's id map,
  which only carries mission targets, so ground captures (the vast
  majority) silently never drew their ring. The new
  `Impl::objective_entity` checks the pop map (every world objective)
  first and the session map second; the log's name resolution and the
  rings share it. Names still print as `#<vu>` for small front
  objectives — the theater name table behind `nameid` is loaded
  NOWHERE; that's the named data-pipeline gap this tranche leaves.
- `test_event_log` 7/7 (every-kind-formats, names, personnel faces,
  envelope accessors, cap/order, filter); f4-world-viewer ctest 95/95;
  a 330 s headless --session --play run shows the log filling with
  capture/verdict lines, team-dotted, follow pinned to the tail.

## SYMBOL-SVG-3 — the procedural vocabulary deletes (the library is the only render source)

- **`draw_symbol` and `draw_symbol_imgui` are gone** — the parity eyeball
  passed, so symbols.cpp keeps only the SymbolKind address space: the
  ObjectiveType/UnitClass mappers and the `symbol_key_for_kind` seam
  table (~960 → ~210 lines). Every icon on the canvas renders through
  `draw_library_symbol` now.
- **The fallback is a circle, not a vocabulary** — `RenderEntityIcon`
  draws a plain filled circle + outline when the library is null or a
  key is missing (broken corpus load, wild override). "An icon can never
  go blank" survives without 700 lines of bespoke shapes behind it.
- **The parity toggle deletes with it** — Layers loses `SVG map symbols`
  (`use_symbol_library` is gone); the canvas's three aggregate-fighter
  sites address their symbol through the same
  `symbol_key_for_kind(UnitFighter).primary` seam instead of the
  procedural switch.
- The planned legend follow-on turned out moot: the old Legend window is
  already gone (`draw_symbol_imgui` had zero callers) — it deleted with
  the vocabulary.
- Headless screenshot confirmed filled team-colored icons across the
  theater; f4-renderer 15/15 + f4-world-viewer 88/88 ctest green.

## SYMBOL-SVG-2 — the Inkscape authoring path (the Creator is removed)

- **The map symbols fill again** — the first live run of the library path
  rendered every icon as unfilled strokes in the dark outline color:
  `draw_library_symbol`'s convex-fill used raylib's `DrawTriangleFan`,
  which silently rasterized NOTHING for these vertex lists (the path was
  never exercised on a canvas — the Creator previewed through ImGui).
  The fill now draws as an explicit centroid fan of `DrawTriangle`
  calls, proven by the new GPU pixel-count tests (`test_symbol_draw`:
  corpus / viewer-merged / unit-kind each must produce > 400 fill
  pixels at 64 px — a fill-less render leaves ~0) and by a headless
  screenshot showing filled team-colored icons across the theater.
- **Scope change (review direction)**: the in-app Symbol Creator is
  REMOVED — its planned SVG Import/Export buttons were the old plan, and
  an on-disk editor plus file convention beats 1,026 lines of bespoke
  editor UI. SVG files are the authoring surface now.
- **The load path** — the viewer starts by loading `f4_symbols.json` (the
  committed corpus) merged with every `symbols/*.svg` override (filename
  stem = key: `obj_airbase.svg` replaces `obj_airbase`;
  `merge_symbol_svg_directory` in f4-renderer). Parse failures are
  collected and skipped — a half-finished Inkscape export never blanks
  the map — and the status bar reports corpus + overrides + errors.
- **Library-first rendering** — `RenderEntityIcon` gains an optional
  library (nullptr = the old procedural behavior); the canvas passes the
  loaded one: library-first, procedural fallback when a key is missing.
  The `SVG map symbols` toggle in Layers is the parity switch (flip it to
  compare against the procedural vocabulary), and View > Reload symbol
  library closes the edit-reload-look loop. `--symbols-dir` and headless
  `--export-symbols <dir>` are the CLI faces.
- **`symbols/` is committed** — all 75 corpus symbols exported as SVGs
  (the starter set: editing any map symbol in Inkscape takes effect on
  the next launch) plus a README documenting the workflow, the
  color-role convention, and the geometry rules. The procedural
  `symbols.cpp` vocabulary stays until the renders are eyeballed via the
  toggle — then it deletes (the last SYMBOL-SVG-1 queue item).
- `test_svg_import` 16/16 (override-by-stem, broken-file skip, missing
  dir); renderer + world-viewer ctest 102/102.

## SYMBOL-SVG-1 prep — the key seam

- **`symbol_key_for_kind(SymbolKind)` → `SymbolLibraryKey`** — the
  canonical SymbolKind → library-key table (static, `static_assert`ed
  against `SymbolCount`): objectives → `obj_*`, frames → `frame_*`,
  composed unit kinds carry both the standalone symbol (`unit_armor`) and
  the frame-agnostic glyph (`glyph_armor`); `UnitUnknown` falls back AT
  `frame_squadron` (the one documented duplicate — the corpus has no
  unit_unknown). `test_symbol_mapping` gains the `SymbolKeySeam` coverage:
  every kind's keys must exist in the committed `f4_symbols.json` corpus
  and primaries stay unique, so the coming wiring can never silently fall
  back for a kind the library actually defines. The vestigial
  `f4-world-viewer/src/symbols.hpp` alias wrapper is deleted (one
  includer, zero unqualified consumers left — canvas.cpp's 18 bare
  `RlColor` uses now explicit). Behavior unchanged; renderer 14 +
  world-viewer 88 ctest green.

## QC-SUITE-2 — the poles golden, honestly (1 red → 0)

- **The trim map is a function of x again** — the AI-closed Newton could
  not converge on ANY platform; "MSVC numerics" was the wrong theory.
  `diag_poles`' `Session::step` never applied the x vector's four AI-state
  coordinates before steering (setAiStates existed; only setFullState
  called it), so the map's AI rows read whatever the PREVIOUS evaluation
  left behind: F was path-dependent, the Jacobian's AI columns were
  fiction, and the line search compared incomparable states. Applied, the
  trim's residual fell from 1.17e3 chaos to physically-tiny motion.
- **The nonsmooth AI states leave the coordinate set** — ai_vsTgt (the
  slew limiter's state: algebraic near trim, a lambda_d=1 marcher when
  binding) and ai_prevA (overwritten by steer() from the current frame
  every evaluation — a zero column, always) singularized the Newton
  system. They stay APPLIED for flight continuity but are no longer
  coordinates or residual rows; the eigen report keeps all four AI rows.
  The AI trim gained the plant path's outer discipline (each pass
  re-settles from the template — the vt/z blend cures a diverged pass's
  wander — best residual wins) with the settle running the AI IN the loop
  (aiPilotInput is now the one shared input builder).
- **The slow-mode golden reads the time domain** — the FD Jacobian cannot
  resolve the AI-closed phugoid: the mode moves the state ~0.4% per major
  frame, at the FD noise floor of the stiff filter rows, and
  lambda_c = ln(lambda_d)/dt amplifies that noise per toolchain. The old
  +0.01656 golden was GCC's sludge — MSVC read +0.009 and +0.377 in the
  same code — and the 900 s verify run shows 12 phugoid cycles of FLAT
  envelope: sigma -0.0021 (a +0.0166 mode would double every ~42 s and
  cannot hide in that fit). `diag_poles` fits and prints the envelope
  rate (`--verify-sec` added); the golden is -0.0021 ± 5e-3 and the
  regression gate trips past +0.01 — an order of magnitude under
  pre-STAB-P1's +0.2196 re-growth. `PolesEnvelope` 5/5 on Windows/MSVC;
  the suite is fully green (`ctest -LE slow`).
- **Hygiene** — `receiver_pairing_` widened to uint64 (it narrowed
  EntityId::value past 2^32 — the C4244 was the symptom); the
  CombatRecording acquisition assertion dumps every track-event pair it
  recorded (QC-PASS-1's one-time failure passes 10/10 today —
  build-labile, and the next flip reads its own evidence).

## QC-SUITE-1 — the Windows suite repair (7 red tests → 1 named)

- **LF checkout pinned (`.gitattributes`)** — the byte-identity tests read
  the WORKING TREE bytes, but with `core.autocrlf=true` and no attributes a
  Windows clone materialized CRLF: every manifest fingerprint/size missed
  by the line count (f16.json 36559 on disk vs the manifest's 34434) and
  the dat-fixture byte-for-byte regen failed. The committed blobs were
  already all LF — `* text=auto eol=lf` pins the smudge; the Falcon
  binaries (`.cam`/`.ct`/`Falcon4.*`/`THEATER.*`/`KoreaObj.*`) carry
  `-text -eol`; `*.bat` keeps CRLF. The manifest-fingerprint and
  dat-fixture reproducibility gates pass on a CRLF-configured clone.
- **The emit-tables read stream closes before `remove_all`** — both
  `TheaterData.EmitTablesJson*` tests held the tables.json ifstream at
  test-body scope while deleting the temp dir: on Windows the stream's own
  handle is the sharing violation (MSVC opens without FILE_SHARE_DELETE;
  POSIX never refuses the unlink). 38/38.
- **The unarmed note only fires when nothing was delivered** —
  `strike_flights_armed` counts live Bomb stations POST-run, so a flight
  that released its whole stick ends at 0 and `qc_missions.py` rendered
  "unarmed (loadout concern)" on delivery rows (the Cookbook §7 known
  quirk). The note now gates on `released == 0` too; delivery rows render
  "released N, impacts M".
- **The strike certificate holds again** —
  `GroundStrikeHarness.StrikeRunsCertifiesAndIsDeterministic` had been
  failing since the runway-frame anchoring (6359dd1) rotated the shipped
  scenario's route to the real airbase: the harness kept injecting the
  target at the fixed world point (0, 30000, 0) — 131 NM off the route —
  so the gate never saw the aim and the strike window closed with the
  stick unfired (the end-state dump the harness now captures names exactly
  that: `nav=Done wp=3/3 delivery_wp=1 strike_target=0`). Three stacked
  repairs: (1) the trigger-stall verdict appends each striker's end state
  (phase, nav rung vs the delivery index, the module counters, the release
  geometry, both hold_fire gates); (2) the injected objective derives as
  the ground point under the FIRST armed striker's delivery waypoint
  (`target_position_explicit` pins the legacy verbatim point); (3) the
  release path keys the impact plane AND the recorded miss on the brain's
  RESOLVED aim (`CombatIntent::bomb_aim` — the EMPL-2d feature the planner
  named) instead of the objective center: the stick had been landing
  ~21 ft from the aimed feature while "missing" the center by 437-979 ft.
  `release_bomb` gains an optional `resolved_aim` (default = the old
  center behavior). 6/6 green; the precision gate (min_miss < the MK-82
  lethal radius) holds at ~20 ft.
- **Named, not fixed: the MSVC trim divergence** —
  `PolesEnvelope.AiCruiseNavTuneSlowModeGolden` fails on MSVC because
  diag_poles' damped-Newton trim does not converge (AI mode: residual
  1.17e3, the AI integrator states ai_vsTgt/ai_altI/ai_speedI top the
  residuals; plant mode: residual 1.5, throttle pegged 0.031, alt 333 ft
  low) — the golden was pinned on GCC. No `/fp:fast` anywhere; the
  divergence is codegen/libm-class. A numerics session on the Newton
  (seed from the analytic cruise trim; FD-scale audit against the GCC
  trace) is the named follow-up. The 24-hour CAMP-INIT harness (TIMEOUT
  900, "170 s real-time" on container hardware) times out on a 2-core
  Debug box — machine-bound; `ctest -LE slow` (the fast-tier twin) is the
  local substitute.

## EMPL-2 review round two — the contact FLOT, and territory that follows the armies

- **The contact front** — the FLOT is now the line between the closest
  opposing BATTALIONS, drawn only where they are actually in contact
  (`front_columns_from_battalions`, `kFrontContactRangeGrid` 12,
  smoothed by ±`kFrontSmoothColumns` 3 moving mean within each run).
  The garrison-gated OBJECTIVE front of round one still let lone
  garrisons yank the per-column extremes (TestCamp: seven chaotic
  runs, rows 326..572, swinging 190+ rows inside one run); the contact
  rule draws five tight runs in the two armies' interleave band (rows
  460..520, zero swing) — a FEBA is where the armies face each other,
  not where garrisons happen to sit. All three faces (the engine's
  display front, the Campaign interdiction pick, the ATM's
  unit_strike ranking) share the one computation; `objective_score_`'s
  outward scan clamps against the front's own span now (the battalion
  span can be narrower than the objective spread — a latent OOB the
  swap exposed). `front_columns_from_objectives` is gone.
- **Consolidation** — territory follows the armies. A new ground-war
  phase flips un-defended objectives to whichever belligerent army is
  STRICTLY nearer (`kConsolidatePerUpdate` 4 per update, wire order,
  ledger-captured like any flip): stock saves carry ownership no troop
  ever earned (TestCamp's 361 DPRK-owned southern towns, first_owner
  ROK), and the capture ladder can't reach them (no battalion within
  its 2-grid range, ever). Garrisoned holdings NEVER consolidate —
  they are captured through the combat ladder or they hold;
  equidistant pockets stay with their holder. The write-back lands
  the flips, so a session played long enough walks the save's
  ownership back to troop truth.
- **The viewer reads the same truth**: objectives render SOLID in
  owner color only when the engine's troop-gate stamp says garrisoned
  (`GroundWar::objective_defended`); un-garrisoned claims render
  hollow and dimmed — affiliation without a position.

## EMPL-2 review — troop-truth FLOT, real tankers, readable routes

- **The troop-gate (FLOT)** — the front line is TROOP truth now, not raw
  ownership bytes. Stock saves carry ownership no troop ever earned
  (TestCamp: 361 DPRK-owned objectives south of the ROK army,
  first_owner ROK — the "FEBA around Pusan" read), and
  `front_columns_from_objectives` drew that ghost line faithfully — the
  engine's display FLOT AND the G2 tasking front (CAS/interdiction
  ranking) both measured against a phantom. `FrontObjectiveView` gains
  `defended` (default true — unstamped views front exactly as before);
  `stamp_front_defended()` arms the gate at all three faces (the
  engine's mirror rebuild, the Campaign interdiction pick, the ATM's
  unit_strike ranking): a holding fronts only when the owner keeps a
  live Battalion within `kFrontGarrisonRangeGrid` (8, Chebyshev — a
  constant, not a knob: contested-column mean row moves 459→479 across
  radii 2..24 on TestCamp, vs 288 ungated with the line at row 129).
  Ownership bytes untouched — territory, capture history and supply
  keep their truth; the FLOT is the line the armies actually make.
- **The support gate + the rating-layout fix (tankers)** — fighters no
  longer fly the tanker orbit. Two stacked causes: (1) the rating chain
  read the wire's per-squadron rating[16] with the UCD Scores column
  index — the two tables have DIFFERENT layouts (wire = kAroNames
  order, data-proven: TestCamp's 3 real support squadrons carry
  68..70 in column 5; UCD = the reference MissionRollEnum), so support
  landed on out-of-range column 16 and EVERY tanker mission scored
  through the specialty fallback (unspecialized 60 — taskable
  everywhere); fixed: each table reads its own index, the decay seat
  remembers its seed's layout (`ratings_wire_layout`), and the wire
  row now outranks the UCD baseline. (2) `find_best_air_` gains the
  support gate — a squadron neither table rates for the role cannot
  fly the SUPPORT family (`has_table_rating_`), so stock wars' byte-39
  fighter filings no longer fly; the reference's own stock war did
  file them, which is the deliberate divergence the review directs.
  The sim side agrees: the tanker role (brain, station hold, AAR
  discovery) requires `SquadronComponent::role_ratings[kAroSupport] >
  0` (populated from the save for the first time) — an unrated byte-39
  flight spawns as a RECEIVER with its join stack, not a fake KC-10.
  Specialty byte 0 documented as UNSET (92 of TestCamp's 94
  squadrons), never a counter-air claim.
- **The route tail, readable** — the saved route rides past its first
  WP_LAND (the REFUEL hook + the DIVERT leg to the alternate field),
  and drawing that tail like mission legs is what made "all flight
  plans" read as spaghetti (371 of TestCamp's 449 flights end at an
  alternate field, not home). The viewer now draws the mission legs
  for everyone and the post-recovery tail only for the SELECTED
  flight, muted; the inspector's waypoint list resolves airfield
  target VUs to NAMES (the bare numbers were why home plate and the
  alternate were indistinguishable) and marks the home plate and the
  DIVERT leg — the route question answers itself in the panel.

## EMPL-2d — the stick aim-point element: the save's own feature index drives the aim

- **EMPL-2d** — the employment plan's last named open item closes. The
  wire waypoint's `target_building` byte — the feature index on the
  target objective the planner meant the stick to destroy — rides the
  route verbatim (`Waypoint::aimpoint_feature`, 255 = the wire's
  "none" sentinel; TestCamp carries real indices 0-12), and the
  brain's aim rule is `resolve_feature_aim`: the indexed feature when
  in range and alive, else the EMPL-1a first-alive walk (a spent
  element continues against the objective; no alive feature returns
  the objective center and the delivery gate aborts). Pure function,
  unit-tested (`test_strike_aimpoint`, 6 tests) — the pre-2d rule
  needed a brain/world harness. The live INTSTRIKE run (exit 0) moves
  the destruction off the first-alive prefix: 9 features under the
  nominal rule → 6, in the indexed set. `StrikeModule::last_aim()`
  exposes the resolved aim for the next diagnostic.

## EMPL-2c — the receiver join stack: 8 complete protocols, served one boom at a time

- **EMPL-2c** — the EMPL-2b residual closes: receivers now WAIT at their
  rendezvous point instead of flying their window off. The bridge
  synthesizes a compact WAITING orbit (the STK racetrack, anchored on
  the saved refuel waypoint, corners carrying the REFUEL action so the
  leg flag — and therefore the pairing and the arm — stays live while
  the nav cycles the loop, level above the nav's terrain floor, 45-min
  station, the post-hold recovery re-appended) for every non-tanker
  flight with a refuel leg. The push's protocol is now EXCLUSIVE per
  tanker with SPLIT counts: `protocol_count` (PreContact..Departing)
  is the boom's occupancy, `armed_count` the pursuit crowd; arming
  requires both empty and is counted the same tick it happens (the
  pre-fix map was built before the receiver loop — the whole in-ring
  stack armed in one tick and dogpiled the boom), and an armed
  receiver whose boom becomes occupied STANDS DOWN into its stack
  orbit (disarmed, pairing kept — still queued). Done releases the
  tanker. The latch gained the lateral twin of its vertical
  stability gate: ContactRequest requires the boom-frame lateral rate
  settled (`contact_latch_lat_rate_fps`, 20 ft/s) — the F4_AAR_TRACE
  CSV (filled in this tranche; the hook was an empty placeholder)
  caught a receiver tripping the contact envelope at −72 ft/s of
  cross-track rate and carrying its momentum straight back out
  (11 latches, 11 losses, hold ages 1.0-6.9 s). Numbers: 60 sim-min
  `--expect-aar` PASSES with 8 disconnects approved and **5 complete
  protocols** (was 3); 90 sim-min completes **8 of 24** receivers —
  the throughput is the join-cycle time, and the horizon now buys
  completions linearly. `F4_AAR_DEBUG=1` also prints the push path,
  each arm, and the latch/loss autopsy.

## EMPL-2b — the fleet-scale on-save AAR demo passes: 3 complete USAF protocols live on TestCamp

- **EMPL-2b** — the EMPL-2 named follow-up closes: `campaign_qc
  testcamp.world.json --mission AMIS_TANK,AMIS_BARCAP2 --minutes 60
  --expect-aar` **PASSES (exit 0)** — 22 refuel requests/assignments,
  64 PreContact/ClearedContact entries, 51 boom latches, 5 disconnects
  with fuel transferred, **3 complete protocols** (RefuelComplete fired)
  on the live save. The spawn mix is the tranche's opening move (the
  flight filter is a byte SET end to end: `FlightSpawnFilter::missions`,
  scenario JSON `"mission": [...]` arrays, harness comma lists — the
  `--war` arm refuses mixes, the session path keeps the single-byte
  face). The live runs then exposed four engine defects the e2e's
  pre-placed geometry never hits, each fixed from the trace autopsy:
  (1) the rendezvous closure cap was a STEP in dz (150 kts inside
  ±300 ft, 90 below, zero above — with the +3-kt bias floor overriding
  the zero): the join-scale servo's flicker at the band edge turned it
  into a ratchet that parked the join +300 ft high for 116,000 ticks
  and bounced it +6,282 ft off a 1.1-NM join; the cap is now LINEAR
  through those same three points (gentle below → full ceiling on the
  boom line → mirrored descend bias above) and the 6,000-ft standoff
  hand-off is a blend, so no crossing injects a step into the speed
  loop; (2) the collision-avoid rung stood receivers DOWN mid-join —
  4,159 Breaking samples per run, one 6.7 s after a LATCH — because
  planned proximity is what a boom is; the rung now stands down while
  the refuel state is Rendezvous..BackingOut (GroundAvoid still runs);
  (3) the Hold contact box (±15 ft along/lat) was tighter than the FCS
  trim transient — the live latch died at hold_t=6.6 s on along=57/
  lat=-16 with the VS damper one fpm from the skip gate; the box is
  ±60 ft (the boom telescopes); (4) the tanker picture now carries the
  PAIRED tanker's entity id (the campaign pairing has no TowerATC to
  assign one — the stub's answer carried 0), the receiver's refuel
  module tracks it, and the host arms ONE joiner per tanker at a time
  (the ATP-56 visual stack — mutual CPA between stacked receivers was
  the break storm's engine). Residual (the next tranche): 46 lost per
  51 latches — most receivers cycle hold/lose/re-join before the 20-s
  hold completes; 3 of 22 complete. The receiver join STACK (waiters
  hold at the rendezvous point instead of flying on) is the named
  follow-up. `F4_AAR_DEBUG=1` prints the latch/loss autopsy
  (boom-frame errors at loss) to stderr.

## EMPL-2 — the campaign-path AAR chain closes; the receiver joins, latches, and refuels

- **EMPL-2** — a campaign-spawned tanker/receiver pair flies the full USAF
  protocol: `test_campaign_aar` (the REAL spawn path — campaign bridge,
  mission-byte tanker role, saved-shape WP_REFUEL receiver) reaches
  Rendezvous → PreContact → ClearedContact → Hold with fuel →
  BackingOut → Departing → Done, `RefuelComplete` fired (the event
  existed; nothing published it). The tranche's work was the terminal
  servo, five stacked defects each caught by the e2e trace: (1) the
  rendezvous near-field track formate had NO lateral feedback — a
  receiver joining the orbiting tanker abeam formated its displaced
  line forever (the lateral rejoin blend fixes it); (2) PreContact/
  ClearedContact displaced beyond station-keep tolerance had no
  recovery — the new `StationLost` hand-back returns them to
  Rendezvous; (3) the join vertical stack — the station-keep pitch
  tune was outvoted by thrust at join scale (rendezvous now flies the
  PreContact tune always), the capped VS lead shapes the arrival
  (uncapped it crawled at deficit/60), and the closure cap is
  sign-aware (full ceiling level, 90 kts below the boom, none above);
  (4) the terminal station servo — the wingman's linear lateral law,
  the 5° heading deadband bypassed (degree-scale corrections were
  invisible), the closure bias symmetric (±8 kts), and a direct
  along-axis throttle bias (the energy-coupled speed loop held +1 kt
  of the tanker's regardless of an 8-kt command; the receiver parked
  +42 ft off the boom all run); (5) the Hold servo — a 10× lateral
  gain plus lateral-rate damping (P-only pumped ±15 ft and
  ContactLost fired on every swing edge) — and CONTACT
  STABILIZATION: the paired tanker flies its station hold straight
  while a receiver is mid-protocol (corner captures skipped, the
  station clock paused, the racetrack re-forms on release). Also:
  **EMPL-1b** — `record_snapshot` fills `target_position`/
  `target_description` on the aircraft snapshot path (the campaign
  traces' dead field that misled §5; ~110k live samples on the ladder
  run). **tanker_track re-anchors** to Kunsan (`airbase_source` +
  `waypoints_frame`, the last reverted template): exit 0 with 4
  contacts and `complete=1` — the full procedure including
  `RefuelComplete` at the real runway; the track lengthens to 500k ft
  so the tanker's recovery no longer interrupts the run. The named
  follow-up: a LIVE TestCamp run still shows zero AAR traffic — the
  single-byte `--mission` filter can't spawn a tanker and a refuel-leg
  receiver together, and the ladder's synthetic spawns don't carry
  their stamped refuel legs to the bridge (plan §3).

## QC-ANCHOR — every QC template anchors to its real runway; the landing capture gap closes

- **QC-ANCHOR** — the 16 runway-anchored QC templates (all but
  `tanker_track` and `on_glideslope`, see below) now fly in the actual
  world through the QC-WORLD overlay and the headless `campaign_qc`
  matrix alike: each gained an `airbase_source` block (Kunsan, the
  `digi_full_mission` anchor) and `"waypoints_frame": "runway"`.
  `derive_real_airbase` grew the machinery that makes this safe for
  the whole library: per-aircraft **routes and spawn-in-air spots
  rotate with the waypoints** (an anchored spawn-in-air scenario used
  to drop its aircraft at the raw frame origin — the theater datum, in
  the sea west of Korea), **spawn headings rotate too** (a receiver
  authored BEHIND its tanker silently became abeam — tanker_track's
  contact count went 4 → 0 before this), and a **start_in_approach
  spawn re-anchors its altitude onto the derived beam**
  (threshold_alt + along·tan(3°), the LandingModule's own formula —
  on_glideslope's authored 794-ft spawn presumed a 50-ft field;
  Kunsan's real elevation is 0). Results: **`landing_only` PASSES
  (exit 0)** — the approach-capture gap from the SHOWCASE-1 board
  (gate 24, InterceptFinal going around every run) is closed: full
  InterceptFinal → OnFinal → Flare → Rollout → TaxiIn, touchdown
  confirmed. Two templates REGRESSED under anchoring and were
  reverted to the sandbox frame, with the traces to autopsy the
  follow-up: `tanker_track` (the anchored airfield lets the tanker
  turn back and LAND after its route — the receiver station-keeps
  ~1,200 ft behind through the turn and the ±15 ft latch never
  aligns; contact 0, exit 22), and `on_glideslope` (all-GoAround:
  the 7,000-ft establish floor vs the rotated spawn range). Also in
  this tranche: **FlightRecorder snapshots carry callsigns now**
  (campaign flights: the CS%03u-%u origin stamp; scenario aircraft:
  the roster-order template callsign — the replay/QC menus no longer
  label tracks with raw entity ids); the **terrain auto-load
  resolution ladder** (the world JSON's bare `terrain_file` name now
  resolves through Data/Theater/<theater>/ instead of failing on
  every standard-layout load — the stale "Auto-load terrain failed"
  error is gone); and the **screenshot path fix**
  (`take_screenshot_to` early-returned on a STALE file from a previous
  run while the fresh shot sat orphaned in the CWD). And a
  disproof: the "map goes black at high zoom" report was the open
  ocean — the far-tile sea color, not a rendering bug (verified:
  terrain renders at zoom 24 over land,
  `Testing/zoom_land.png`). Follow-ups surfaced by this tranche, with
  evidence: the anchored `tanker_track` AAR latch (the receiver
  station-keeps ~1,200 ft behind through the tanker's route-complete
  turn; the ±15 ft latch never aligns — contact 0, exit 22; the
  un-anchored 4/3-contact baseline is restored and passes) and
  `on_glideslope`'s all-GoAround re-approach under anchoring (the
  7,000-ft establish floor vs the rotated spawn range; reverted).
  Strike-gap note: the campaign→sim target propagation the cookbook
  flagged (§5) already has its in-tree A-G tranche (loader
  mission_target → FlightPlanComponent::target → the plan builder's
  delivery-waypoint fallback), and the blocker on verifying it is
  FIXED here: the **Release `campaign_qc` crashed with 0xC0000409
  (fail-fast) on any world load** — root cause: the tool's default
  class table pointed at the BINARY `FALCON4.ct` fixture, the runtime
  `ClassTable::load_auto` (JSON-only by design) throws on it, and the
  throw escaped a main with no catch → abort() → the UCRT fail-fast
  that reads like a memory bug. Fixed two ways: the default is now
  the runtime-canonical `Data/Classes/falcon4.ct.json` (binary
  fixture demoted to fallback), and main routes every mode's
  exceptions through one reporter (a load error now prints its real
  message and exits 1). The Release matrix RUNS now — and the
  cookbook's strike catch turns out to be a BYTE MISREAD: §5's
  `--mission 39` filtered AMIS_TANK (a Support flight — taxi-bound by
  design, no strike leg); AMIS_INTSTRIKE is byte 13, and the first
  real INTSTRIKE matrix run on TestCamp flies the whole chain —
  armed=2, released=8, impacts=8, features_destroyed=9, 2 objectives
  damage-synced, exit 0. The saved INTSTRIKE route always carried a
  WP_STRIKE (17) leg with the mission target attached; the A-G
  tranche resolves it through to the StrikeModule. The strike gap is
  closed; §5's gate→ledger→trace workflow remains the template for
  the next catch.

## QC-WORLD — Mission QC flights fly in the actual world, on the world map

- **QC-WORLD** — the Mission QC window's **"Fly in world"** button (and
  the `--qc-world <scenario>` CLI flag) runs a scenario template as an
  OVERLAY on the campaign canvas instead of the sandbox scenario view:
  the aircraft take off from their real runway (the template's
  `airbase_source` anchoring — `derive_real_airbase` resolves the real
  objective's PHD runway threshold/heading/taxi/parking at
  initialize(); hand-authored templates fly their absolute-ENU route
  as-is), their **trails draw on the world map** (a new canvas QC layer:
  movement-gated per-aircraft polylines + the planned-route polyline +
  the active runway centerline; scenario ENU feet ÷ 1024 = canvas grid,
  no new transform), and they are **click-selectable** —
  `SelectionKind::QcAircraft` (the third entity-id space: the scenario
  Simulation's own EntityWorld, resolved through the new
  `Impl::qc_handle`), a QC branch in the Inspector (position/phase/
  velocity/route, same components as session aircraft), and the
  Inspector 3D tab's per-frame chase view follows the QC aircraft over
  the real theater terrain. The scenario player's own Simulation is
  untouched — same engine, same determinism, same FlightRecorder trace
  (forced to the `qc/<stem>/trace.json` convention, so **Open replay
  works on world runs too**; the epilogue and `stop_scenario_run`
  flush it, and both are throw-proof after an unwritable path was
  found to wedge the process mid-teardown). The QC panel (pause/
  resume/Speed/Follow-on-map/Center-on-flight/Stop; Space and G do
  what they say) drives the run; `stop_scenario_run` tears it down
  CPU-side (overlay runs never build the sandbox's GL resources).
  Mode dispatch in run() branches on `ScenarioPlayerState::
  world_overlay`: the canvas keeps its own input path while the
  in-frame fixed-timestep tick drives the sim. Verified live
  end-to-end: `digi_full_mission` flies its full ground cycle from the
  real Kunsan runway on the map — takeoff, Enroute at 230 kts,
  Approach at 115 kts, Landed/Complete — with the trail, selection,
  inspector, and 3D chase all exercised by hand; headless smoke
  `f4-world-viewer <world> <terrain> --qc-world <scenario> --speed 8
  --screenshot` covers it scripted. Fix en passant: the Mission QC
  trace conventions moved to shared `qc_build_root()/qc_trace_path()`
  helpers (viewer_state.hpp), and the recorder-log handle is now
  created inheritable with FILE_APPEND_DATA-only access (see
  MISSION-QC-RECORD below for why both matter).

## MISSION-QC-RECORD — the Mission QC menu records and re-records its missions

- **MISSION-QC-RECORD** — the Mission QC window's Record/Re-record button
  (one per template row) spawns the sibling `campaign_qc` recorder
  headlessly and writes the same artifacts as the CLI step:
  `<build root>/qc/<stem>/trace.json` + `scenario_qc_summary.json`
  (gates 20–24). The spawn is the exact CLI command with `--out-dir`
  pinned to the menu's first trace convention, so viewer-recorded and
  CLI-recorded traces land on the same file; the button reads
  **Re-record** when a trace exists (the rerun overwrites both
  artifacts). One record job at a time; the row shows `recording...`
  while it runs, and a finished job invalidates the scan cache, so the
  row flips to `Open replay` the moment the trace lands — verified live
  end-to-end: `tanker_track` (pass → Open replay/Re-record),
  `takeoff_only` + `kunsan_parking` (pass), and `landing_only`
  (gate 24 → "no touchdown" in the status line). Mechanics: the spawn
  lives in its own TU (`record_runner.cpp` — windows.h cannot coexist
  with raylib.h, the CloseWindow/ShowCursor extern "C" clash), runs on
  a detached thread with shared_ptr result flags (a viewer closed
  mid-record leaves the recorder running to finish its write), and
  captures the child's stdout/stderr to `qc/<stem>.log` (the parent is
  a windowless GUI process; CREATE_NO_WINDOW already suppressed any
  console). Three Windows subtleties the log's first cut got wrong, all
  fixed and pinned in comments: handles must be created INHERITABLE
  (bInheritHandles only duplicates so-marked handles — without it the
  child's output silently vanishes), FILE_WRITE_DATA in the access mask
  disables append-at-EOF semantics (the log overwrote itself head-first
  with a 7-byte "exit 0\n"), and the parent's header line goes through
  its own atomic-append open so the child's buffered CRT flush can't
  collide with a shared file pointer. Tool discovery walks up from the
  exe to the build root (the dir containing `f4-simulation/`) and
  prefers the viewer's own config layer (multi-config MSVC layouts);
  `f4-world-viewer` gains `add_dependencies(f4-world-viewer
  campaign_qc)` so a viewer build always produces its recorder, and a
  `--mission-qc` CLI flag opens the window for headless screenshot
  proofs (the `--ct-preview` pattern). Also fixed en passant:
  campaign_qc's console summary printed garbage on Windows
  (`fs::path::c_str()` is wchar_t* into a printf %s — hoisted narrow
  copies).
- 
## EMPL-1a — the strike stick destroys features (impact precision, A-G kill chain closes on damage)

- **EMPL-1a** — `features_destroyed` went 0 -> 9 on the INTSTRIKE repro
  (and 0 -> 6 on ONCALLCAS) with no exit-4 regression. The autopsy split
  the EMPL-1 miss (682-984 ft) into its axes and found three stacked
  defects: (1) the brain aimed the strike module at the objective CENTER
  while the loader's nominal feature grid sits 156+ ft off it and the
  Mk-82's single-hit envelope is 144 ft — the A-G rung now aims at the
  first ALIVE feature (the save's own aim-point element wiring comes with
  the mission-element tranche); (2) the EMPL-1 attack run flew PURE
  PURSUIT, which conserves its entry lateral offset almost to the target
  (675 of the miss was lateral, under 100 ft along-track) — the delivery
  leg is now a VIRTUAL LNAV leg THROUGH the aim, anchored at the
  engagement position and flown with the module's own cross-track law
  (degenerate anchors fall back to pursuit; 4 new `NavigationAttackRun`
  tests); (3) the 3,000-ft terrain floor silently overrode the bridge's
  1,500-ft delivery altitude, doubling the throw — delivery waypoints now
  fly their own altitude. Also calibrated: `drag_factor` default
  0.85 -> 1.0 (the Mk-82 card's own ODE measures 0.999 of vacuum — the
  0.85 folklore under-predicted the throw ~15% for every bare-module
  user). `ground_strike_qc` stays green with MD5 determinism (pursuit and
  the anchored line coincide on a straight-in); BARCAP2 no-ordnance
  regression unchanged. The QC tool also learned the relative-path lesson
  campaign_session.cpp already knew: scenario JSONs carry ABSOLUTE
  world/class-table/config paths, so the documented
  `campaign_qc --out-dir qc/<name>` invocation works again.

## CT-BROWSER-LAYOUT — the class table browser previews unit/squadron deaggregation layouts

- **CT-BROWSER-LAYOUT** — the deaggregation positioning math moved out of
  `campaign_bridge.cpp` into `f4-simulation/formation_layout.hpp`
  (wedge4/grid ground formations, the synthesized 8-spot/80-ft ramp row,
  overflow + runway-frame rotation), so the sim and the viewer share one
  definition (behavior unchanged; all spawn-path tests green). The class
  table browser's detail pane now previews CLASS_UNIT and CLASS_OBJECTIVE
  rows in 3D: unit rows lay the first populated instance's live vehicles
  out exactly where `spawn_vehicles_from_unit` would (UCD rows carry no
  vis and no vehicle list), squadrons draw the synthesized ramp row, and
  objective rows draw the whole feature layout — feature meshes where
  exports exist, plus the synthesized airfield plates (runway/taxiway
  geometry via `build_airfield_geometry_3d`) when the world carries PHD
  point lists. Models with no glTF export (nearly all vehicle/aircraft
  vis types — the koreaobj set is features only) draw as flat-colored
  placeholder boxes on a scaled ground grid, so the layout reads while
  staying visibly synthetic. Two supporting fixes: the WorldState
  adapters' `entity_type()` now falls back to `type` (JSON-loaded worlds
  had class_table_index 0 everywhere, keying nothing per-class), and the
  group preview composes instance transforms in raylib's row-vector
  order (rotate first, then translate — matching draw_vis_type_mesh).
  New `--ct-preview <entity_type>` CLI flag opens the browser
  pre-selected on a row for headless screenshot proofs (verified: unit
  170 wedge/grid, squadron 473 ramp row, vehicle 103 real mesh, radar
  2128 feature layout).

## SHOWCASE-1 — the mission-QC user concept made real (watch one aircraft fly one mission)

- **SHOWCASE-1** — the geometry half of the QC question ("follow an
  aircraft visually through a whole mission — is the AAR rendezvous
  sane? does the approach geometry look right?") got its tooling. The
  discovery: the engine already had ALL the pieces except the wiring —
  an 18-template flyable scenario library
  (`f4-scenario-player/scenarios/*.json.in` → `build/scenarios/`, AAR +
  glideslope + pattern + full ground cycle + intercepts + merges), a
  3D mission player (follow cam, speed, pause), the FlightRecorder
  trace format the viewer's replay mode renders with intended-path +
  cross-track-error coloring, and the AAR protocol's bus messages.
  Landed the three missing links:
  (1) `campaign_qc --scenario <json>` — the headless QC arm: runs a
  template with no window, writes `qc/<stem>/trace.json` +
  `scenario_qc_summary.json` (per-aircraft states walked, touchdown,
  fuel burn, the AAR protocol's message counts), and exits with a
  verdict derived from the scenario's own shape — gates 20–24 (no
  aircraft / frozen / AAR no contact / AAR incomplete / no touchdown),
  disjoint from the campaign ladder's 2–16.
  (2) the viewer's **File → Mission QC…** window (Windows menu toggle
  too) — the template roster with each template's recorded-trace
  status at the tool conventions; one click opens the geometry replay.
  (3) `f4-scenario-player --record <path> [--record-every <n>]` — keep
  the recording of what you just watched live.
  First sweep verdicts: `tanker_track` passes the full USAF procedure
  (PreContact → ClearedContact → Refueling → BackingOut → Departing →
  RefuelDone) but the summary surfaces the wobble — ContactMade 4 vs
  ContactLost 3 (the FM phugoid vs the ±60 ft contact envelope) and
  the receiver disconnecting at 209 lbs with RefuelComplete never
  firing; `on_glideslope` + `closed_traffic` land (OnFinal → Flare →
  Rollout → TaxiIn); `digi_full_mission` flies the full ground cycle;
  and `landing_only` FAILS exit 24 — InterceptFinal goes around every
  run, the approach-capture gap now a one-command reproduction with a
  trace to autopsy. Also documented in cookbook §9: AAR cannot engage
  from the CAMPAIGN path at all (set_tanker + the tanker-picture push
  are scenario-list-only) — the campaign AAR tranche is the next
  engine item after §5's strike-target gap. Determinism spot-checked
  (two runs, identical state sequences + protocol counts); the
  end-to-end loop verified headlessly under Xvfb (record → replay →
  screenshot). test_aar_e2e + test_flight_recorder stay green.
  Cookbook §8 is the user-concept chapter; §7's campaign-scale stage
  worlds remain the follow-on tranche.

## CAMP-OPT-1 — the campaign time-acceleration repair (the spinner-walk spike)

- **CAMP-OPT-1** — found and fixed the p7 animation patch's per-tick
  full-world walk that broke campaign time acceleration. The tick loop's
  ANIM spinner pass called `with_component_ref<VisualModelComponent>()`
  EVERY tick — a fresh bucket copy of every visual entity in the world —
  and resolved the powered/dormant check through an `EntityHandle` +
  `type_index` map lookup per entity per tick. A real campaign save
  (TestCamp) carries 4,063 visual entities (2,665 objectives, 1,719
  units' vehicles/features) even before a single aircraft spawns, and
  the walk measured **1.96 ms of every 2.02 ms tick (97%)** — capping the
  engine at ~420 ticks/s, so the viewer's certified 60× preset delivered
  **7.3×** and the 10× preset itself dilated. The FID-OPT-2-era
  certificate had cleared 60× at 61.07×; the regression shipped with
  `f4-p7-20260916` (6e6aae9) and every campaign since ran at ~1/8th its
  certified speed. The fix: `EntityWorld` grows a **structural epoch**
  (`structural_epoch()`, bumped on create/destroy/component
  add-replace-remove and both sides of every move — max+1 so a stale
  capture can never collide), and `Simulation::tick` keeps a cached
  spinner roster `(id, VisualModelComponent*, const FlightModelComponent*)`
  rebuilt only when the epoch moves (wars spawn/retire at most once per
  campaign second; scenario plays barely mutate after init). Between
  rebuilds the per-tick cost is three float integrations + one dormant
  read per entity — the cached pointers are as safe as the snapshot they
  replaced (component nodes are stable between structural changes, and
  every mutator in the tick runs BEFORE the spinner pass, so a rebuilt
  roster sees exactly the post-tick state the old per-tick snapshot
  saw). Measured, same save, same box: tick **2.09 ms → 0.157 ms (13×)**,
  spinner phase 1.96 → 0.11 ms; the **60× certificate sustains 116.1×**
  zero dilation (min sample 92.1×) vs 7.3× dilated before — 240× now
  delivers ~99× where 7× stood (CPU-bound, surfaced honestly by the
  runner's effective-speed readout); the FullFidelity 449-aircraft run
  drops 100 → 75 s for the same 7,200 ticks. Tests: 4 `StructuralEpoch`
  entity-core pins (queries never bump; every mutation does; both move
  sides land above every prior value — the move-assignment max+1 rule
  caught a real collision bug in the first cut; slot recycling bumps) +
  4 `SpinnerRoster` sim pins (features spin, dormant airframes hold
  their phase, a late spawn joins the next tick, a destroyed visual
  leaves the walk without a dangling read).

## CAMP-DOM-6 — task-force movement (the naval GroundWar sibling)

- **CAMP-DOM-6** — the DOM-5 "how deep" record's first named tranche
  lands: the wire's `dest_x`/`dest_y` — decoded on every domain-4 row
  since the .uni decoder and consumed by no engine — becomes the
  ORDER. `NavalWar` (f4-campaign, `naval_war.hpp` + `naval_writeback.hpp`)
  is the GroundWar structural twin, movement-only: every belligerent
  task force walks toward its wire dest at its movement speed (the UCD
  enrichment when present, else the sea family default table —
  carrier/battleship 25, cruiser/destroyer/frigate 30, patrol 35,
  amphib 12, replenishment 15 kph — the ground table's own
  documented-limitation pattern), in the ground move phase's exact
  fixed-point arithmetic (1/256 sub-grid, integer-truncated sqrt
  normalization, the arrival snap, the heading byte via
  atan2 ÷ 1.40625°), on its own 60-s update wheel (one big tick == N
  small ones), behind the shared `belligerent_pair` gate (neutrals
  stand down; a war-less world is inert). NO LEDGER — movement is not
  a war fact the books own (GroundWar syncs the ledger because it
  ATTRITES; NavalWar only moves); no orders cycle, no engage/capture,
  no supply doctrine — each stays in the "how deep" record. The
  session arms it with `naval_movement` (default OFF — no engine, no
  row touched, byte-identical everywhere): the moved rows sync live
  into the WorldState per update (`apply_naval_to`, the ground
  write-back's twin — activity-gated, identity-verified, dest consumed
  never written), the `taskforces` query serves them (the wire-state
  rule: the sync IS the serving face), the 3D task-force entities
  mirror the transform, the save carries the moved rows (the host
  save's idempotent second touch), and the DTO gains the additive
  `heading` tail (always present, at the END; kProtocolVersion stays
  1). The QC arms it with `--naval-movement`: the war block's
  `naval_move: updates/moved/arrivals/march` counters + the summary's
  naval block (armed-only) + exit 18 (armed & moved nothing — the
  exit-13 philosophy; arrivals NOT required, a fleet at its
  destination holds). Verified: 15 engine tests (the snapshot filter,
  the war-pair gate, the exact walk/heading/last-move pins, the
  arrival snap + hold, the static hold, the big-tick identity, the
  two-engine determinism, the sync's activity/loudness/idempotence
  gates), 5 new session tests over the kunsan pair (the frigate's
  1.4-grid haul SNAPS on the third 60-s update and the carrier's
  sub-grid truncation lands (753,264) → (752,265) — both served live
  on the query and carried by the save; movement-off leaves every
  wire row byte-identical; movement armed moves ONLY task-force
  rows; two armed runs answer identically), the DTO goldens, and the
  QC war run (kunsan 0.5 h: `naval_move updates=31 moved=31
  arrivals=1 march=14 grid`, deterministic=yes, and the arm-on/
  arm-off ledger MD5s IDENTICAL — movement alone does not move the
  books, by design). The carrier's full 319-grid haul arrives within
  the 24-hour certificate's horizon (engine-pinned).
  
## MAINT-2 — the mission QC cookbook + the per-type matrix runner

- **MAINT-2** — the answer to "how do we sanity-check the campaign at
  a glance?" made into tooling and a runbook. `Docs/MISSION_QC_COOKBOOK.md`
  writes down the three-layer QC discipline the codebase already
  implements — the C++ exit-code GATES (`campaign_qc` 2/3/4/5, ladder
  6–8, war 9–16), the LEDGERS (`campaign_qc_summary.json`'s
  missions_by_type / tasking / b3_loop / sim_run / ordnance / results
  blocks), and the viewer as the EYES (ATO/Tasking mission+team filters,
  click-to-pan rows, replay mode over the FlightRecorder trace) — with
  the per-category "what good looks like" table and the byte-exact
  filter traps spelled out (the stock war's CAP is AMIS_BARCAP2; its
  tanker missions are AMIS_TANK). `scripts/qc_missions.py` (stdlib
  only) runs the matrix: one `--mission`-filtered `campaign_qc`
  invocation per type, the category expectations layered on top of the
  gates (routes == spawned, airborne ≥ 1, Strike/SEAD/CAS
  "armed → released"), `qc_matrix.json`/`qc_matrix.md` artifacts, and
  a CI-able exit code. First sweep over TestCamp caught a live one
  (cookbook §5): strike flights — saved-tasking AND ladder-generated —
  are armed but never receive a ground target (`target_description`
  empty for every trace sample; the AI flies the route, then goes
  home through ProceedToFix/GoAround/Rollout), i.e. the campaign→sim
  boundary drops the `mission_target` propagation leg the A-G
  employment needs. Ground-war/supply verification via the same loop
  (`--war --ground-war [--unit-strike] [--objective-supply]
  [--replacement-stock]`, exits 13/14) documented with the ladder's
  window>cycle trap. §7 sketches the curated per-type showcase worlds
  as the follow-on tranche.

## CAMP-SEGF-2 — the segfault fix re-land (the engine + test half)

- **CAMP-SEGF-2** — CAMP-SEGF-1's data half landed inside MAINT-1, but
  the actual segfault fix did not: HEAD still authored every session's
  handoff scenario in the FIXED shared temp dir
  (`/tmp/f4_viewer_session/`), so two concurrent sessions (ctest -jN,
  two viewer instances) still raced on the same `scenario.json` —
  create fails on the other process's world path, a torn write, or a
  fresh delete, and the five rig factories that cannot ASSERT
  (non-void returns) walked their non-fatal `EXPECT_NE(host, nullptr)`
  on into a null-host deref (the flaky `CampCmdQueueRefusesTyped`
  SEGFAULT). Re-land on the post-MAINT-1 tree, byte-for-byte the
  audited fix: the session temp dir unique per instance (instance
  counter + steady-clock nanos — the rigs' own rule) in
  `campaign_session.cpp`, and the five factories
  (`HostRig::make`/`WarRig::make`/`WarRig::make_combat`,
  `CommandRig::make`/`KunsanRig::make`) now throw on a failed create —
  gtest reports the session's own error instead of the process dying.
  Verified on the fresh clone: full fast tier 2974/2974 green, 4×
  concurrent binary runs × 3 rounds clean, and the corruption hammer
  that deterministically segfaulted (exit 139) the pre-fix build
  passes the fixed one (the fixed sessions never touch the old shared
  path). ASAN+UBSAN was clean over the host + command binaries in the
  original audit; the engine hunk is unchanged.

## MAINT-1 — data hygiene + docs truthing (the green-baseline repair)

- **MAINT-1** — the fresh-clone baseline is green again and stays green
  by construction. Data/ had drifted three ways — the committed
  `Aircraft/f16.json` predated the parser's `criticalAOA` capture
  (`rawAuxAeroData: {}` where the fixture regenerates
  `{"criticalAOA": "25.0"}`), `manifest.json` recorded stale
  fingerprints for `kc10.json` + `Theater/korea/terrain.json`, and it
  listed `Weapons/falcon4.wcd.json`, which `.gitignore`'s Data/
  whitelist never allowed to be committed (the manifest was generated
  against a local export a fresh clone can never have). Fixed:
  f16.json regenerated from its committed fixture (byte-identity
  restored — 23 of 24 aircraft were already exact), the manifest
  regenerated from the committed tree (36 assets, zero phantom
  entries), and `!Data/Weapons/**` added to the whitelist so the next
  real export commits cleanly instead of poisoning the manifest. The
  `Sha256.Reproduces…`, `F16CriticalAOAIsTheDatOverride`, and
  `DatFixtureRegenerates…` failures are gone. Kept green:
  `generate_manifest.py --check` — a sub-second read-only
  manifest↔Data/ verifier (size/sha256/fnv1a + both directions of
  listed-vs-committed) wired as the first CI step of BOTH jobs
  (fail-fast before the toolchain install), because direct pushes to
  main can't be blocked post-hoc and the C++ gate only fires after a
  full build. Repo hygiene: the stray empty `main` file deleted.
  Docs truthing (README claims vs tree, audited): f4-ai's section
  rewritten planned → landed (16 modules, the f4-flight-api +
  f4-recorder deps, 311 tests), every stale **Tests** count corrected
  (geo 40, math 199, convert 139, data 105, entities 95, json 58,
  install 63, world-convert 179, world 94, flight-model 178), the
  four missing counts added (weapons 101, sensors 74, simulation 357,
  campaign 253), and a Supporting-libraries table added for the 18
  undocumented modules. `Docs/README.md`: the `ATM_STRATEGY_PLAN`
  "named next legs" sentence (CAMP-ATM-1/CMD-1/SCALE-1 landed since),
  the `ASSET_PIPELINE_SPEC` "pending implementation" row (f4-assets +
  f4-import are CI-gated), and the missing `AIRCRAFT_ANIMATION_PLAN`
  index row. `ARCHITECTURE PROPOSAL` §3: `f4-anim` +
  `f4-campaign-api` added to the as-built table (deps verified against
  CMake). `AI_IMPLEMENTATION_PLAN` banner: Draft → as-built (matching
  its index row).

## CAMP-DOM-5 — naval (the wrap-then-decide)

- **CAMP-DOM-5** — the campaign war's naval face becomes real to the
  tasking pipeline behind one default-off knob (`naval_tasking`, the
  session opt + the QC's `--naval-tasking`): the upstream
  NavalTaskingManager is a 15-byte flag shell on the wire, so the wrap
  maps the naval face onto the ATM pipeline's request vocabulary — a
  ranked pool of the enemy's task forces (`rank_taskforce_targets` in
  the new naval_tasking.{hpp,cpp}: sea-domain units at war, non-empty
  roster, own-shore distance ascending, wire-order ties), the anti-ship
  family (AMIS_ASHIP — `mission_is_naval_strike`, the name table's own
  position; ASW's submarines and TANK's armor stay honestly target-less)
  rotating across it through its own cursor, the targeted filings
  routing like strikes (the builder resolving the task force's grid
  position — the unit-resolution seam now the OR of the two arms), the
  per-target filing books (`book_naval_filing`, VU-ascending) exposed
  for the additive `taskforces` query (TaskForceView: the wire's own
  rows with or without the arm, overlaid with this run's books), the
  filings publishing on the SAME mission_filed event every other
  package rides (no new event family; kProtocolVersion stays 1), the
  legacy ladder's matching naval rung, the arm-gated
  `naval_requests`/`naval_filings` summary keys, and the "how deep"
  record (task-force movement, naval threat painting, carrier
  airbases, task groups/CVN ops — each its own tranche; the NTM's 15
  wire bytes stay captured verbatim). Tests (+15): the ranker pins
  (hostility, ties, the skips), the family split, the ATM arm
  (targeted ASHIP, the disarmed and empty-pool corners), the Campaign
  books (one-for-one with the filings, the based-squadron route pin,
  the two-run determinism, the disarmed summary), the session gates
  (the taskforces query serves the wire + the books, the filings ride
  mission_filed one-for-one, the arm-off identity, the two-run query
  determinism), the protocol whitelist + the DTO goldens. The
  medium-war gate with the arm ON: two runs one MD5; the kunsan
  fixture's 2 task forces are the session gates' raw material.

## CAMP-DOM-4 — airbase scheduling (FindTakeoffSlot depth beyond FID's airfield-ops windows)

- **CAMP-DOM-4** — the campaign war's slot grid becomes a living,
  visible, honest scheduler behind one default-off knob
  (`airbase_scheduling`, the session opt + the QC's
  `--airbase-scheduling`): the grid's anchor slides with the clock
  (`AirbaseSchedule::sync` — whole blocks drop off the front, past
  bits fall off with their time, the 160-minute horizon stops
  silencing every filing past 2.6 h; the campaign-start anchor stays
  the disarmed golden identity); FindBestAir's schedule gate applies
  the reference's own previous-block rule and its skips book
  (`schedule_denials` + the per-base `denied()` books + the ledger's
  slot-denial log); phase 7's horizon refusal counts instead of
  staying silent (`slot_overflows`, the flight keeps its estimate and
  still flies — the documented deviation stands); a scrubbed flight's
  still-future slot releases back to the grid (`slot_releases`,
  `release()` = fill's exact inverse); the grid is NOT written back to
  the save (the reference's scheduleTime was runtime state — so is
  this). The `slot_denied` event family (the fifteenth, the denied
  side's team gate) rides the stream; the `airfields` query joins the
  v1 whitelist (the 32-block grid as 64 hex chars, the `epoch_min`
  anchor, the booked count, the denial books — teamless rows, an empty
  set when the pipeline is off); the artifact's totals gain
  `slot_denials` (always, the honest 0) with the log array
  activity-gated; the summary's ATM block gains the three counters
  only when the arm is on. The seam: `MissionIntent` (and the
  `IntentView` DTO's additive tail) carries the flight's SCHEDULED
  takeoff, and the session's airfield-ops gate arms against the SLOT
  (`depart = takeoff_abs`) when the arm is on — the flight
  materializes one ops window before its grid minute and rolls on it,
  closing FIDELITY_TIERS §7's ~2× ops_window delivery-latency
  divergence; the TOT-anchored gate stays for slotless flights and
  disarmed sessions. Also restored here: the DOM-3 personnel test
  files the previous commit missed (they were untracked — the tree
  they shipped in could not build). +46 tests (the slide/release/
  overflow/gate units, the Campaign's denial books, the session's
  slot-anchored gate + airfields parity, the journal's fifteenth
  family, the DTO goldens, the whitelist); the C5 24-hour gate green
  with the knob off and on.

## CAMP-DOM-3 — personnel (the reference's AssignPilots, the rotation pressure)

- **CAMP-DOM-3** — the campaign war gains a personnel layer behind two
  default-off knobs (`pilot_assignment` / `rating_decay`, the session
  opts + the QC's `--pilot-assignment` / `--rating-decay`): every filed
  flight draws its crew from the squadron's decoded pilot roster — the
  lead scans the front third, the wingmen scan backward from the tail,
  a squadron that cannot crew is skipped at the pick gate and the
  request fills from the runner-up (the reference's flight-fails rule
  reshaped; `crew_denials` counts it) — the crew rides the
  `MissionIntent` and a crewed flight's LEAD sets the spawned brain's
  skill cadence when the SCALE-1 pilot-skill flow is armed; the
  squadron's per-role effectiveness table (the `.uni` tail's
  `rating[16]`, now typed through the world JSON pass) decays 25% per
  assignment — `new = (int)(0.75 × rating) + 1`, floored at 4 — and
  the live view re-prices FindBestAir so the sorties spread across the
  wing; the ledger books the personnel run (assignment / loss /
  recovery logs, per-slot dead/out/sortie deltas — losses consume
  slots in pick order, the lead first; recoveries credit the
  survivors' sorties); the pilot event trio (`pilot_assigned`,
  `pilot_lost`, `pilot_recovered` — the twelfth through fourteenth
  families) rides the stream and the `squadrons` query joins the v1
  whitelist with the personnel face (the wire's counts + the run's
  deltas, the live rating table when the decay fired); the write-back
  applies the roster face (dead slots → status 1, per-pilot
  `missions_flown` += the run's sorties, the decayed table) — a
  draws-only run writes nothing (the out is transient). With both
  knobs off, rosters are ignored beyond the SCALE-1 skill map and
  every golden stands byte-identically.

## WORLD-OCD-1 — the OCD join reads the right row (airbases get their features back)

- **WORLD-OCD-1** — cam2json's theater enrichment resolved an objective's
  ObjClassDataType row by (ObjectiveType − 1), but the OCD table's row 0
  is a zeroed placeholder and the game indexes ObjDataTable by the class
  table's dataPtr. Airbase (type 1) read the placeholder — no class name,
  no features, no ground layout, so the viewer drew nothing and the sim
  fell back to synthesized nominal feature grids — while airstrip (type
  2) and armybase (type 3) each read the row above their own (airstrips
  were rendering the airbase's 108 buildings). The join now goes through
  ClassTable::data_ptr_for (type − 1 kept only as fallback), fixing all
  three consumers in world_json.cpp (class enrichment, radar range,
  ground layout). Regenerated korea.world.json: all 50 airbases carry
  their real 66–145 FED placements + runway/taxiway/parking layouts, the
  42 highway strips show their own 13 features. Regression test
  (TheaterData.WorldJsonResolvesOcdRowByClassTableDataPtr) sweeps every
  save1 objective's emitted enrichment against its own OCD row.
- **Viewer** — the class-table browser now shows objectives as what they
  are: collections of features. Every CLASS_OBJECTIVE row has vis_type
  all-zero (the browser's 3D preview could never show anything for one),
  so the detail panel gains a feature-collection section built from the
  loaded world's objective entities (grouped per entity_type, rebuilt on
  world change via a world-generation counter): placements with FCD
  names, entity types, models, offsets, live damage state, and a Preview
  button per row that loads that feature's model into the orbit preview.
  Stale-canvas comments about "39 features each" and objective
  vis_types corrected.
  
## CAMP-DOM-2 — supply depth (the per-objective pool)

- **CAMP-DOM-2** — the war has a SUPPLY CHAIN
  (Docs/CAMP_HOST_PLAN.md §8). Upstream interdiction targets
  supply/fuel lines and objectives carry supply; the engine's C2 pool
  was source-less and repair did not exist. Now: the `.tea` strategic
  stocks (supply_avail/fuel_avail — decoded by the importer all along)
  parse into the WorldState and reach the engine through ITeamSource;
  with `objective_supply` armed the resupply fire becomes SOURCED —
  the team stock regenerates its held objectives' clamped (garbage-
  safe, kunsan's 235 → 100) supply/fuel stocks, battalions draw from
  the nearest own-held objective within the supply radius and are CUT
  OFF beyond it; the `last_repair` cadence (the third .cmp timer,
  bridged since C2) heals features at a supply-gated rate — flips to
  VIS_REPAIRED, spends stock, stamps the wire's own last_repair — and
  books the `objective_repaired` event family (the eleventh) whose
  post-repair bitmap rides the existing fstatus write-back; the
  strategic reserve (`replacements_avail`, exposed since C2, consumed
  since now) refills consumed reinforcement budgets behind
  `replacement_stock_flow`; the `objectives` query serves the LIVE
  mirror (the DOM-1 seam closes). Every knob defaults OFF — the G1/C2
  goldens stand byte-identically. GroundWarConfig gains the rates;
  campaign_qc gains --objective-supply/--repair-period/--replacement-
  stock and the summary echoes the supply books only when they moved.

## CAMP-DOM-1 — victory scoring (the books' projection)

- **CAMP-DOM-1** — the war can be SCORED
  (Docs/CAMP_HOST_PLAN.md §8). Upstream tracks victory points; F4 had
  books but no verdict. The verdict is a read-only projection of the
  books: f4-campaign's `war_verdict` (`compute_theater_verdict`) walks
  the LIVE owner of every objective (the ground war's mirror when one
  runs, the WorldState otherwise) against the session's OPENING owner
  (snapshotted at construction — the run scope the ledger keeps),
  weighted by each objective's own priority byte, and reports the
  ledger's team rows beside it. The `verdict` query lands additively on
  the contract (`VerdictView` at dto.hpp's tail — the threat precedent;
  the whitelist gains the name, `kProtocolVersion` stays 1) and the
  `verdict` event family (the tenth) fires only when the coarse state
  changes — band or leader — so the stream stays as sparse as the front
  moving. The band is territorial only: stalemate / advantage /
  decisive at kDecisiveSwing (100); a tie for the lead is no lead; the
  books alone never move it. The `.cmp` header's own
  `te_victory_points` rides the DTO as `threshold` context (no terminal
  semantics claimed — korea's full-campaign save carries 0). Gates: the
  pure model's 10 pins (swings both ways, neutral origins and
  reverts, the 99/100 boundary, ties, census rules, slot order), the
  quiet HostRig's exact query bytes + verdict-silence, and the
  generated small war's live front — a capture in the stream, a
  `verdict` event after it, and the query answering with the SAME
  coarse state the last event carried. campaign_qc and campaignd are
  untouched.

## CAMP-SCALE-1 — the Tier-3 full-data pass + the scale certificate

- **CAMP-SCALE-1** — the converted tables reach the fight
  (Docs/CAMP_HOST_PLAN.md §8). `emit_tables_json` (f4-world-convert) +
  `cam2json --emit-tables` write the COMPLETE UCD/VCD/WCD tables as one
  `f4.theater.tables/1` document; f4-world's `TheaterTables` reads it
  runtime-side (JSON is the contract — no importer link). The VCD's
  per-unit countermeasure counts resolve through the class-table → VCD →
  WCD chain (`resolve_countermeasures`; the WCD's name IS the dispenser
  identity) and stamp `CountermeasureSupplyComponent` at spawn — the arm
  path spends them instead of the documented 30/15. The pilot-skill flow
  (gated `pilot_skill_flow` / QC `--pilot-skill`, the countermeasures
  gate's own lesson): the squadron's converted pilot roster sets the
  spawned brain's SensorFusion cadence (0-2 Recruit / 3-5 Rookie / 6-7
  Veteran / 8-9 Ace — a documented monotone map). PLT_PARK decode closed
  with a test (the PD walk is type-agnostic; theaters that carry parking
  lists flow to the layouts). `campaign_qc --max-flights 0` = the
  uncapped fleet; `--theater-tables` + `--pilot-skill` arm the flows.
  Absent tables/rosters = the pre-SCALE identity byte-for-byte.

## CAMP-INIT-1 — create-from-parameters (the war can be born)

- **CAMP-INIT-1** — the scenario pack becomes a war
  (Docs/CAMP_HOST_PLAN.md §8). `ScenarioPack` (theater + OOB template +
  force levels + date/weather + seed) parses STRICT (unknown keys and
  vocabulary words are named errors; every cross-reference validated at
  the parse) and `CampaignInitializer` synthesizes the decode structs
  straight through the Task-70 encoder stack (`encode_cmp/obj/obd/tea/uni`
  + `CamWriter`): two builds of one pack are BYTE-IDENTICAL (the seed
  rides the `.cmp`'s CreationRand), and the world JSON the runtime
  consumes is the EXISTING reader's own projection
  (`CamArchive::load_from_memory` → `to_world_json` — no second
  projection). Fresh saves carry korea's own tasking priorities, one ATM
  airbase row per squadron home, non-zero airbase anchors, the canonical
  zero-delta `.obd`, and the stock 8-slot team block; the passthrough
  `.evt/.plt/.pst/.wth` ride empty (no codec exists; nothing reads them).
  The `campinit` CLI generates `.cam` + world JSON from a pack (importer
  side, exit 0/1/2). Four packs committed (small/medium/large/twinwars)
  and generated at build time into `generated_world_fixtures/`. Gates:
  the generated save decodes cursor-clean in the existing reader; the C5
  24-hour harness PASSES on a generated war (two runs, the MD5
  certificate equal); medium/large certify compressed; the G1
  two-war-pair limitation gets its engine-level bed (the second pair's
  battalions stand down while the first fights) plus a five-team
  harness-level run.

## CAMP-ATM-1 — the ACTION tables (the war reacts)

- **CAMP-ATM-1** — the strategy layer's named queue item lands
  (Docs/CAMP_HOST_PLAN.md §8; the ATM plan's own "what did NOT land").
  The ACTION tables scan the objectives' fstatus bitmaps at every
  strategy-armed generate_requests: an OWN objective with destroyed
  features files CAS over it, heavy damage (>= 25%) adds the garrison
  BARCAP station, an enemy objective at war files SEADSTRIKE against
  it — into a per-team pending queue (dedup vs the queue and the
  booked flights; capped; +25 priority bonus) drained ahead of the
  ladder walk. The SWEEP family gets a real enemy-objective target on
  its own rotation cursor and, under the sweep arm, the builder flies
  the LINE (the attack run extends through the target along the
  inbound axis, sweep action byte 22). AMIS_TANKER stations gain the
  refuel waypoint (WP_REFUEL turnpoint before the racetrack anchor).
  Every filing books the ledger's action-filing log (the optional
  `actions` JSON section — disarmed documents stay byte-identical) and
  publishes as `action_filed`, the ninth event family. The QC strategy
  line gains `actions=` (kunsan: 96 filings over 8 cycles). 20+ new
  ctest cases; the golden identity holds (disarmed runs byte-identical
  everywhere: summaries, ledger JSON, routes, events).

## CAMP-CMD-2 — retask / abort / priority (the command surface completes)

- **CAMP-CMD-2** — the v1 command set completes behind the CMD-1 wire
  (Docs/CAMP_HOST_PLAN.md §3.3/§8). `flight_retask` replans a flight
  FROM WHERE IT IS: the session rebuilds the route through its own
  RouteBuilder (home airbase → target, threat-aware), splices at the
  new plan's ingress point with the flight's CURRENT position as the
  head, recomputes TOT and the mission-over deadline with the ATM's own
  arithmetic (cruise-speed estimate + loiter + doubled reserve), and
  lands the write on EVERY shape the flight is in — the aggregate row
  (SPEED mode from the retask point; a TIME-mode save flight retasks
  into speed), the stored synthetic intent, a save flight's world
  WaypointPlanComponent (so a later deagg spawn flies the NEW route),
  the live brains (BrainComponent::retask — the one sanctioned
  mid-flight plan swap: Enroute hands the route straight to the
  NavigationModule with reset steering, Approach/Complete refuse), and
  the ATM booking (the recovery clock follows the new plan; the takeoff
  slot survives). `flight_abort` closes the sortie: not-yet-launched →
  the aggregate SCRUBS (a distinct terminal state — tick, tier
  triggers, ops windows, and the air picture all skip it; a parked
  complement folds back and retires) and airborne → the RTB leg home
  ([current position → the route's own landing waypoint]); either way
  the package's books close NOW — the ATM booking releases its
  survivors through the Campaign's scrub (the ledger's
  apply_mission_recovery at the current clock; save-carried flights
  have no booking in this session's ledger — operational abort only),
  the abort record keeps the tier triggers from ever resurrecting the
  sortie, and the flights row reports the additive `aborted` tail.
  `objective_priority` makes the commander's weight (0..100, the save's
  own scale) the FIRST runtime write of the objective priority byte —
  the same field every tasking score reads (the request target term,
  the CAP station ranking, the enemy target rotation, the legacy
  select_target), echoed by the `objectives` query and persisted by the
  contract save(); re-set replaces. Refusals gain `unknown_objective`
  (the wire's fourth typed reason). Gate: the M4/M5-style pinned
  retask (a flight retasked mid-crank closes on the new target — or
  the TOT window deaggregates it and the materialized aircraft carries
  the new plan), the RTB abort with the books closing exactly once,
  the next-cycle scoring moving with the priority write, and the CMD-1
  identity statement extended: a journal carrying retask + priority +
  abort replays into the SAME ledger fingerprint under different step
  chunkings; the tampered-flight-id replay exits 23 with both
  fingerprints named. Engine primitives pinned at their own level
  (FlightAggregateEngine retask/scrub, ATM scrub_flight/
  reschedule_flight). No commands → byte-identical; full ctest green
  (the 3 pre-existing upstream data-drift pins untouched).

## CAMP-CMD-1 — the command journal + RoE doctrine

- **CAMP-CMD-1** — the identity statement's command half lands
  (Docs/CAMP_HOST_PLAN.md §2.3/§3.3/§5/§8): `roe_set` rides the P7
  fire-control path — team/mission/flight scopes join the session's
  doctrine store (one level per scope; an aircraft's effective level is
  the tightest of its carried byte and every matching scope), and the
  write is `Simulation::set_flight_roe`, the FULL recompute from the
  armed doctrine baseline so a command can LOWER as well as tighten
  (the old `apply_flight_roe` was a tighten-only ratchet; the spawn
  cadence and the FID-5 deagg path now serve the store too). Refusals
  are typed data (team 0/mission 0 are non-targets; a flight scope must
  name a roster flight). Every applied command journals —
  `f4-campaign-api/command_journal.hpp` (writer + reader + byte
  goldens; the intent-body encoder gives every intent a canonical wire
  spelling) — and replays tick-exactly: `EngineSessionHost::step`
  segments around the journal's pending apply ticks so the replay's
  step CHUNKING is irrelevant, `campaignd --replay-commands` verifies
  the final identity against the journal's footer (drift/pending → 23,
  wrong war at load → 23, malformed → 24) and composes with
  `--verify-journal` for the full assertion. `roe_changed` publishes
  (the HOST-2-pinned encoder, unchanged bytes) from the command path.
  Gate: the fire-control levels pinned at the gate level (2→1→0
  recompute vs the armed baseline, gun budget untouched) and at the
  OUTCOME level (the combat rig's t=13 kill pair: holds at t=2.5 s
  kill nobody); the record's journal regenerates the record's
  `ledger_fnv` under three different step chunkings; no commands →
  byte-identical (the C5/C6 identity suites stayed green); 30+ new
  ctest cases; full ctest green (2796 passed; the 3 pre-existing
  upstream data-drift pins untouched).

## CAMP-HOST-3 — the viewer becomes a client

- **CAMP-HOST-3** — the world viewer's campaign session refactored onto the
  f4-campaign-api contract (Docs/CAMP_HOST_PLAN.md §8): every Campaign-window
  read is a QUERY (time/stats/flights/tasking/books/threat via a per-advance
  snapshot gated on the runner's step serial — "once per advance, never per
  draw"), every act is a typed COMMAND (camera bubble → `focus`/`clear_focus`,
  the flights table's D/R → `select_deagg`/`select_reagg` with refusals
  surfaced as data, Write Back → the runtime-safe `save()`). The runner left
  the engine (pacing is host-side composition, plan §2.2): `CampaignClientRunner`
  drives `step(ticks)` with the FIFO FairMutex discipline relocated and the
  wall→tick accumulator the engine's advance() used to own — ceiling-clamped,
  fixing the unbounded budget-doubling overflow the real-session advance cost
  always masked. The `threat` query lands (v1.1-additive: viewer_team,
  cell_grid echo, both density bands — 171×171 on the kunsan war) plus the
  additive `route_waypoints`/`flight_role` tail on tasking rows. The TWO
  PLANES rule is now explicit in the code: contract = campaign state; the
  live entity graph stays on the EntityWorld through a named render-plane
  seam (a remote roster gets a `vehicles` query in its own tranche). Gate:
  viewer parity — **981 deleted / 2327 added** (the engine sheds 825 lines:
  campaign_session_runner + its test, relocated and rewritten over the
  contract); 23 new ctest cases — the runner pinned on a MOCK session (no
  engine in the link), the query walks pinned on golden DTO JSON
  (additive-tolerant), the threat dispatch, ThreatView goldens, and four
  engine-backed HOST↔engine parity cases on the kunsan rig; full ctest green
  (2781 passed; the 3 pre-existing upstream data-drift pins untouched); the
  golden identity intact.

## CAMP-HOST-2 — the event stream + journal

- **CAMP-HOST-2** — the engine's typed event stream lands (Docs/CAMP_HOST_PLAN.md
  §3.4/§8): ONE bus message (`f4::campaign::api::CampaignEvent`, the tagged
  envelope of the v1 families pinned in HOST-1) published at the sites that
  move the ledger books — session-side for mission_filed / tasking_cycle /
  reinforcement_delivered / objective_captured / objective_damage (the sink
  collects the damage diffs, the session fills owner + time), sink-side for
  kill (EntityKilledMessage gains a defaulted `cause` literal —
  "missile"/"gun"), the WeatherSystem observer for weather_changed (scenario
  sessions; roe_changed waits for CAMP-CMD-1). The journal: `campaignd
  --journal war.jsonl` records the COMPLETE engine-rate stream as JSONL
  (identity header + event lines + identity footer); `--verify-journal`
  replays a golden byte-for-byte and exits 23 at the first divergence (the
  plan's identity-drift guard; drift outranks the refusal rule at EOF). The
  wire: `subscribe {"kinds":[...],"teams":[...]}` arms the stream (per-family
  team matching; a kill matches either side), step responses carry
  `"events":N` + N event lines, and an un-subscribed client's wire is
  HOST-1-identical (arms by use). Event `t` is the engine's relative seconds
  (the books' axis). Gate: journal replay of the C6 fight reproduces the
  stream AND the ledger fingerprint; empty-journal saves byte-identical; one
  golden line per family. 62 new ctest cases (57 contract + 5 e2e), all
  green.

## CAMP-HOST-1 — the engine contract + campaignd

- **CAMP-HOST-1** — the campaign engine's host contract lands
  (Docs/CAMP_HOST_PLAN.md): `f4-campaign-api` (header-only, f4-json + std
  only — the session iface, v1 query DTOs with byte-stable encoders, the
  typed CommandIntent/CommandAck wire, the v1 event vocabulary pinned
  pre-emission, the line protocol with the exit namespace 20/21/22/24/25),
  the `EngineSessionHost` adapter in f4-simulation (queries serve the
  engine's own views verbatim; the FID family forwards; the CAMP-CMD queue
  refuses with NotImplemented + the tranche named), `campaignd` (the stdio
  JSON reference host), 58 ctest cases green, the golden identity intact.

## P7 — the ATM strategy layer (support flights, racetracks, enemy CAP, RoE)

- **P7** — the C4 pipeline's named queue item "the strategy layer files
  them" LANDED (Docs/ATM_STRATEGY_PLAN.md is the as-built reference).
  ONE FLAG (`CampaignConfig::strategy_layer`, default OFF — the golden
  identity; `campaign_qc --strategy` arms it). FOUR LEGS. (1) The loiter
  racetrack: `RouteBuilder`'s TPROF_LOITER routes emit a closed 4-WP
  circuit anchored at the target (the anchor carries the station
  contract — `station_time_s` = the profile's loitertime,
  `loop_waypoints` = 4; corners WPF_TURNPOINT-protected), and f4-ai's
  `NavigationModule` gains the STATION HOLD — the AI plan's deferred
  rung 17 (LoiterMode/OnStation): the anchor capture arms a one-shot
  timer and the module loops the circuit until it expires, then flies
  on (no new fsm state; routes without the contract behave
  byte-identically). (2) CAP-family station targeting: the ladder's
  BARCAP/TARCAP/ALERT requests station over ranked OWN objectives
  (objtype_priority/2 + priority scaling, wire-order ties, rotation
  cursor) instead of staying target-less. (3) FindSupportFlights:
  ADDAWACS/ADDTANKER/ADDECM packages share-or-file the support family —
  a station is the own objective nearest the package target; an
  existing same-byte flight whose station (and TOT window) covers the
  request SHARES (one tanker feeds a whole raid), else a
  `FlightRole::Support` flight files with its OWN racetrack station
  route and the support profile's ADDESCORT fighter escort (the new
  FlightRole::Support = 3; `supports_filed`/`supports_shared` count).
  (4) RequestEnemyMission: a delivery package's ADDBARCAP files a
  defender BARCAP over the threatened objective for the enemy's NEXT
  cycle (pending queue, dedup, cap 4; `enemy_caps_filed` counts).
  PLUS the RoE carry: `AtmRequestState` gains
  action_type/context/roe_check (the reader stops skipping, the
  converter emits roe_check), the byte rides seed → request → flight →
  intent, and `Simulation::apply_flight_roe` gates the armed brain's
  fire controls post-arm (1 = weapons TIGHT: BVR suppressed; 2 = weapons
  HOLD: everything; 0 = free, the pre-P7 default). QC acceptance on
  TestCamp (`--tasking 240 --max-flights 96 --strategy`): stations=96
  supports=85 shared=115 enemy_caps=48, exit 0 (the strategy gate, exit
  17, guards a strategy run that stations nothing). 22 new tests; suite
  2,639/2,639.
- **P7 queue**: the ACTION tables' contextual filings (the
  objective-damage-driven CAS/BARCAP/SEAD requests — the reactive BARCAP
  above is its RequestEnemyMission slice), GetPriority's PO/package
  terms, the campaign RoE doctrine (per-team/per-mission editing, the
  threat map's 32000 overfly walls), SWEEP station lines, tanker
  waypoints, and the full-data scaling pass (the strategy war over the
  uncapped TestCamp fleet).

## P6 — IR/visual sensors + countermeasures (the seduction tranche)

- **P6** — the AI plan's named queue item "IR/visual sensor models +
  countermeasures" LANDED (Docs/SENSORS_COUNTERMEASURES_PLAN.md is the
  as-built reference). THREE LEGS. (1) The passive sensors: f4-sensors
  gains `IrstComponent` (the SENSDATA/IRST airframe card — gimbal
  gates, ground factor, the sqrt-of-intensity range law, the radar's
  0.75-knee Pd ramp, seeded rolls) and `VisualComponent` (THE ORIGINAL's
  documented signal law: gain × VIS-grid factor / range² ≥ 1 —
  deterministic, no RNG), both over a shared `PassiveTrackStore`
  contact book; `SignatureComponent` carries the full five-grid
  signature record (`sig_data` + `IrPowerMode` band selection, 1.0
  data-free — the golden identity at the component level). (2) The
  countermeasures: f4-weapons gains `CountermeasureComponent` (chaff
  30/flare 15, salvo 2/1, 0.5 s pacing — the MissileModule defeat
  intents' long-documented "no consumption model exists" gap closed),
  decoy ENTITIES (the FreeFalcon VuEntity shape: chaff bloom 25 m² /
  flare, TEAM-tagged, priority 42 — after radar, before missiles),
  and `make_decoy_aware_seeker_source` — the documented
  `seeker_source` hook's first production user: one honest roll per
  decoy inside the seeker cone (IR missiles roll their OWN card's
  `flare_chance` — aim9p 0.4, sa7 0.5 — via
  `find_ir_seeker_flare_chance` over the shipped irstdata.json; radar
  missiles roll chaff at 0.5), sticky through burnout, one-honest-
  chance, IFF-gated. The seduction exposed a latent terminal-math
  proxy bug (min_range_ measured the SEEKER's picture, so a flare hit
  read as a direct kill): with the decoy-aware seeker the miss
  distance is measured against the ASSIGNED target — every legacy path
  byte-identical. (3) The host: `combat.ir_seeker_data_path` +
  `combat.countermeasures` scenario keys, deploy intents in the combat
  pass, seduction seekers on every AI-guided release, the decoy ttl
  sweep. THE GATE IS THE STORY: the first cut armed dispensers
  unconditionally and six pinned fights failed (the merge-fight AIM-9
  flew against flares — correct fidelity, WRONG landing discipline);
  with `countermeasures` defaulting FALSE every pre-tranche fight is
  byte-identical and the pinned harnesses pass unchanged. Suite
  2,617/2,617 (36 new tests: the sensor models, the deploy/sweep/
  seduction units, the seduced-missile-survives integration, and the
  AI-vs-AI E2E — the AI fires on its own, the victim's RWR lights, its
  brain beams, the dispenser releases under fire). Queue: SensorFusion
  fusion of the passive legs, ECM/jamming, throttle-driven IR bands,
  VCD countermeasure counts.

## P5 — tree hygiene, save-write verification, AAR closure
- **P5** — the two pre-existing tree failures are FIXED, not apologized
  for: (1) `JsonReader.RegisteredEscapesStillDecode` — f4-json's
  `Reader` held a `const std::string&` to its source, so
  `Reader r("literal")` was silent dangling-reference UB (the
  temporary died at the end of the declaration statement; the parse
  read freed SSO stack and the test's document came back with a
  garbage first byte). `Reader` now OWNS its source (`std::string` +
  `std::string_view` ctor) — one copy per constructed Reader; the
  literal/temporary construction form is safe for every caller.
  (2) `Sha256.ReproducesCommittedManifestFingerprints` — the committed
  manifest had drifted (kc10/terrain/world fingerprints stale; 5
  runtime-generated `Temp/` entries + the never-committed
  `Weapons/falcon4.wcd.json` declared but absent — the weather patch
  had reverted the prior session's repair). Manifest surgically
  repaired (36 entries, every file present and matching);
  `generate_manifest.py` now hard-excludes `Temp/` and the hash test
  skips `Temp/` entries loudly instead of failing a fresh clone.
  En route: `campaign_qc`'s out_dir default hardened (bare relative
  world filename → empty parent_path → `create_directories("")`
  threw). `--save-write` verified end to end on TestCamp: decode →
  run → fight → apply → save → decode (campaign_after.cam 327,883
  bytes, round-trips with objectives/units/teams intact). AAR
  REDESIGN CLOSED: the ClearedContact closure-bias targeted the
  PRECONTACT station (bias → 0 at 50 ft astern) while the latch gate
  sits at ±15 ft — the receiver crept the last 35 ft at
  integrator-noise speed (120 s stuck at −52 ft on the F4_AAR_TRACE
  CSV) and expired 12 s into Hold; the bias now targets the
  receptacle (USAF: cleared contact closes to the boom) and the full
  procedure completes in 319.6 s — Done, 5,000 lbs transferred — with
  `test_aar_e2e` pinning Departing/Done/fuel instead of tolerating
  their absence. Docs as-built: ARCHITECTURE PROPOSAL refreshed to
  as-built (the §3 inventory = 30 targets with real CMake edges),
  AAR_REDESIGN_PLAN + DIGI_AI_PHASE2_PLAN archived with
  supersession banners, AI_IMPLEMENTATION_PLAN status banner as-built,
  SAVE_WRITE_PLAN + the docs index updated. Suite green (see the
  patch's ctest record), the two tree failures gone for the first
  time since Task 56.

## Fidelity tiers (most recent)

- **FID-OPT-3** — the sensor-sweep budget: the radar scan walks
  pointers, not maps + the RWR sweep's licensed cadence
  (Docs/FID_OPT_PLAN.md §4): the optimization tranche's third item.
  MEASURED FIRST (the temporary `F4_COMP_PROF` per-component profiler,
  removed before landing), and the budget re-attributed AGAIN, harder
  than §3: the ~228 s of post-OPT-2 "component work" is 62% RADAR SCAN
  (161.9 s of the instrumented 3-h/60× armed war) — each radar's
  once-per-second sweep resolves ~7,400 candidates through an
  EntityHandle + a component-map lookup (~1,259 µs/scan) to reject
  99.8% of them with two arithmetic checks that need only the
  transform pointer, and finds ~2 detections. LANDED, two levers:
  (1) `EntityWorld::with_component_ref<T>()` — the component-type
  index's pointer-carrying sibling (same bucket, same invariants,
  same entity-index order; dropped on world move; the replacing-add
  pointer refresh found-and-fixed en route) + the scan's Search walk
  applies the clutter/range pre-gates INLINE so only survivors build
  handles — byte-identical output (the pre-gates draw no RNG; the
  candidate set, its order, and the roll stream are unchanged; pinned
  by the detection-timeline-invariant-to-clutter-population test);
  per-scan 1,259 → 130 µs (9.7×), the radar term 161.9 → 20.8 s
  (7.8×); (2) the RWR sweep rides the licensed ≤100 ms cadence
  host-side (`kRwrCadenceTicks = 6`, the same bound the combat
  refresh and the picture walk carry; the sweep itself unchanged) —
  8.6 → 1.9 s. The deep-horizon 3-h/60× armed certificate: sustained
  61.07× → **137.1×** (the 57 gate now clears 2.4× over), min sample
  15.14× → 30.2×, dilated samples 60 → 30; the 20× baseline
  unchanged (54.6× → 54.1×); the 1-h armed ledger byte-identical
  pre/post across presets; the 2-h armed ledger returned to the
  ORIGINAL pre-OPT-1 value (`641174c7…`) — the RWR cadence's own
  ≤100 ms shift re-aligned the marginal event the fusion cadence had
  displaced. Full suite 2,574 green (2,565 + 9: six ref-bucket, two
  radar, one RWR-cadence test), the two pre-existing tree failures
  unchanged. The residual is measured and named (plan §5): the
  flight-model floor (physics), the brain's diffuse glue, the
  post-OPT-3 radar term, the picture walk.

- **FID-OPT-2** — the concurrent-fight budget: the fusion-refresh
  tiering + the shared picture's own cadence (Docs/FID_OPT_PLAN.md
  §3): the optimization tranche's second item. MEASURED FIRST, and
  the measurement re-attributed the plan's own budget: the deep-
  horizon cost is NOT the per-brain fusion rebuild the §3 arithmetic
  had closed on (~3–6 µs each) but the SHARED AIR-PICTURE WALK it sat
  next to invisibly (~1.4–1.7 ms per walk, 20–40× the fusion term —
  `push_air_picture_` runs outside the `update_all` window the
  FID-OPT-1 sub-profile had split, so the walk never showed). The
  mechanism: the legacy GCI rule sees every missile in the theater,
  so the beam-fight rule ("a visible hostile missile refreshes every
  tick") pinned EVERY combat brain at 60 Hz for as long as any red
  missile was airborne anywhere, and any single brain forced the walk
  every fight tick. LANDED, one bound (≤100 ms staleness — the
  design's own latency license), two cadences: (1) the fusion refresh
  is TIERED BY THREAT — imminent (inside the fusion's own 50 NM RWR
  band) keeps the every-tick beam-fight refresh, a distant theater
  missile rides the new 6-tick (10 Hz) combat cadence, quiet brains
  keep the skill timer; `will_rebuild_this_tick` mirrors all three
  exactly; (2) the shared picture walk is ALSO cadence-gated — at
  most one walk per 6 ticks while any brain demands it, the LAST
  snapshot handed out between walks. FOUND AND FIXED EN ROUTE: the
  push-null invariant (the first cut handed `nullptr` on
  demanding-but-not-walking ticks, dropping every rebuilding brain
  onto its ~1 ms world-query path — rebuilds measured at 977 µs
  before the fix) and the walk-gate off-by-one (walks 7 ticks apart →
  exactly 6). Measured on real TestCamp: walks 95,862 → 21,642 (4.4×
  fewer) and walk time 131.6 s → 36.2 s across 6 sim-hours of armed
  war with the missile-laden brain-seconds IDENTICAL (671,744 →
  672,770 — the war's shape preserved); the 60× deep-horizon armed
  certificate's sustained rate 52.36× → **61.07× (the 60× sustained
  gate now clears)** and its worst sample 7.92× → 15.14×; the 20× 1-h
  cert 1324× GREEN (baseline 54.6×, unchanged within noise), the 60×
  1-h cert **1707×** GREEN, the 2-h armed cert 410× GREEN zero
  dilation, the 4-h armed soak crash-free through 187+ deaggs / 48
  A/A kills / the hour-4 tasking wave. The 2-h armed ledger MD5
  changed (`73a06efd…`) — the first OPT patch that does: the throttle
  shifts detection timing within the licensed bound; re-pinned as the
  golden. The residual deep-horizon dilation (60 dilated samples,
  worst 15.1×) is now NAMED: ~228 s of active-component work (radar
  sim scans, steering, FMs, RWR over the materialized set) — the
  FID-OPT-3 budget, measured, not designed. Tests: 5 fusion + 1
  integration; full suite 2,565 green (the two pre-existing tree
  failures unchanged).

- **FID-OPT-1** — the active-cache walk + the ScopedSubscriptions fix
  (Docs/FID_OPT_PLAN.md): the optimization tranche's first item, driven
  by the FID-6 certificate's own finding ("the session's fixed per-tick
  cost over the ~8,400-entity theater walk"). Landed: the DORMANT FLAG
  on `BehavioralComponentBase` (per-component, routed through the
  owning world so a transition between ticks is picked up without a
  manual invalidate) + the ACTIVE behavioral cache (one rebuild sweep
  fills both lists; `update_all` walks the non-dormant subset — the
  ~8,126-component walk was ~99% of tick time, of which ~8,000 were the
  parked squadron inventory's documented no-op updates paying two
  virtual priority() dispatches each per tick). BrainComponent and
  FlightModelComponent delegate their dormancy to the base; their
  in-update early-returns stay as defense in depth. Measured on real
  TestCamp: update_all ~317 µs → ~0.1 µs/tick with zero aircraft; the
  1 sim-hour armed war's tick work 80.4 s → 0.12 s; the 20× tiered
  certificate sustained 1472× (was 58.1×); **the 60× preset — the plan's
  named target since FID-6 — now passes GREEN at 1331×**; the
  FullFidelity baseline itself lifted 25.3× → 55.3× (the same parked
  mass was taxing it). The 2-h armed war's ledger MD5 is IDENTICAL to
  the pre-OPT run — the war's behavior is byte-identical, only the
  host got faster. FOUND AND FIXED EN ROUTE: the per-entity AI modules'
  bus subscriptions (Takeoff ×2 / Landing ×2 / Refuel ×8) were never
  unsubscribed — a destroyed aircraft's handlers stayed in the bus and
  the next live brain's TaxiRequest publish invoked handlers whose
  captured `this` was freed memory (ASAN: heap-use-after-free on the
  4-hour armed war at ~3 sim-hours; latent since the modules landed —
  pre-OPT the deep-horizon war dilated so hard almost nothing
  materialized). Fix: `ScopedSubscriptions` (f4-messaging RAII bundle;
  bind at initialize, unsubscribe on destruction) adopted by all three
  modules; `Simulation`'s member order swapped so the bus outlives the
  world at teardown. Six new f4-entities tests (dormant skip, the
  unpark transition, active order, idempotence, the campaign
  spawn/unpark pattern, the passive edge); the full suite green, the
  two pre-existing tree failures unchanged. FID-OPT-2 (the
  fusion-rebuild budget — the per-materialized-aircraft cost the
  walk's removal exposed: ~52 µs/tick at 3 aircraft, ~700 at 21) is
  measured and designed in the plan §3, deliberately not in this patch.

- **FID-5** — event-driven combat deagg (Docs/FIDELITY_TIERS_PLAN.md §4.5–4.6):
  the last open milestone of the phase, and the certificate's own lever.
  Landed: the AGGREGATE AIR PICTURE (f4-ai `AggregateContact` +
  `Simulation::set_air_picture_aggregates` — the session publishes the
  engine's airborne aggregates as coarse contacts with team strings and
  cruise velocity; the campaign-flight entities are excluded from the
  world walk so the feed is the single publisher), the COMMIT TRIGGER
  (a Tier-B fighter's engagement id matched against the published set
  deaggregates the contact's flight; the radar-backed policy's coarse
  aggregate rule makes them detectable; the launch veto eats releases
  aimed at the phantom id — no missile ever flies at a non-entity), the
  CONVERGENCE TRIGGER (two opposing aggregates whose predicted tracks
  land inside the 30-kft envelope at the 120-s lookahead deaggregate
  both — wire order, deterministic), the TRANSIENT COMBAT WINDOWS (a
  Combat deagg pins for `combat_window_sec`, then the standard reagg
  rules fold it), and SYNTHETIC INTENTS AS AGGREGATES (the session's
  MissionIntent subscription + the engine's `register_synthetic` +
  the spawner's deferral arm — the generated war rides the tier
  machinery instead of spawning straight to Tier-B; the intent spawn
  path gained the flight path's AirSpawnPose airborne override; the ops
  trigger gained the TOT arm so deliveries still happen). Plus the A/B
  divergence harness the FID-2 acceptance deferred
  (`test_aggregate_fm_divergence` — the FM led the measured leg by
  2.03×, the ratio and the fuel gap pinned). The certificate's second
  run on real TestCamp: 20× tiered GREEN at 58.1× sustained vs the
  25.3× FullFidelity baseline (~2.3× the pre-FID-5 tiered war); the
  2-h armed war ran 59.5× with the tier machinery cycling the generated
  missions (16 deaggs / 4 reaggs, identity green). 7 new session tests
  + the A/B harness; the full suite green and unchanged.

- **FID-VIEW-1** — the campaign view shows the war (Docs/FIDELITY_TIERS_PLAN.md):
  the viewer's Tiered default ran the war but drew none of it — the canvas
  live layer rendered only materialized aircraft, so every aggregate flight
  was invisible and a fresh session read as dead. Landed: the aggregate air
  picture (a pass over `flight_tiers()` — AGG translucent, HOME dimmed, LOST
  a gray cross, LIVE skipped for the materialized aircraft, team filter +
  cull + click-pick with the flights-table selection ring), the tasking
  countdown (`Campaign::seconds_to_next_cycle` → `Stats::next_tasking_sec` →
  a "next tasking cycle in MM:SS" war-status line — the ladder's first
  generated missions land a full 1800-s cycle in), and the viewer
  `--smoke-seconds <n>` long-window smoke (the 6/12 s default can never
  cross the cycle). Engines untouched; one new session stats test.
- **FID-6** — the acceleration certificate (Docs/FIDELITY_TIERS_PLAN.md):
  `campaign_qc --accel <x>` runs the war harness under the TIERED policy
  at an interactive preset and gates exit 15 (DILATION — a sample or the
  sustained pass below x×(1−tolerance)) and exit 16 (DEAGG CEILING — the
  deaggregated set over `--accel-max-live`) on top of the C5 set (6–14);
  `--accel-baseline` measures the same war at FullFidelity for the
  before/after. The C5 roster identity gained the tier term
  (`+ tier_deaggs` — the deagg spawn path bypasses the spawner's
  synthetic counter), the diary gained the FID columns (agg_live,
  tier counters, sim_rate, dilated), and the harness validates the new
  knobs. First TestCamp run: tiered war passes every C5 gate
  (deterministic — identical ledger MD5 at 20× and 60×; drift/leak/alive
  ok); 20× green exit 0; 60× honestly fires exit 15 (sustained 31.7×:
  the ~8.4k-entity theater walk caps the host at ~48× empty, the
  synthetic Tier-B mass drops it to ~25×; FullFidelity baseline 20.7×).
  5 new harness tests; the fidelity-tiers test rig's temp-dir race
  (ctest -jN) fixed.
- **FID-1..4** — air agg/deagg (Docs/FIDELITY_TIERS_PLAN.md): the tiered
  session runs the war the game's way — flights are campaign aggregates
  (`FlightAggregateEngine`: the save's own arrive/depart schedule or the
  cruise walk, per-aircraft fuel burn) until the camera bubble, an
  airfield-ops window, or a click deaggregates them (`AirSpawnPose`
  airborne handoff; lead roll-up fold-back; a lost aircraft folds the
  flight destroyed). Viewer: fidelity-tiers checkbox (Tiered by default),
  the flights table (click-to-select + D/R per row), the tier summary
  line. Full fidelity untouched and bit-identical; 18 new tests
  (`test_flight_aggregate` 11, `test_fidelity_tiers` 7).

## Environment & data (most recent)

- **73** — Weather v1 + day/night model: 3-state condition Markov chain (seeded, deterministic), solar twilight bands, visual detection scales with weather×daylight. Suite 2,513.
- **72** — Complete AuxAeroData record (all 443 fields) across the fleet.
- **71** — TowerATC (AI Tier 3): sequencing tower behind the stub interface.
- **70** — `.cam` re-encoder reaches byte-identity (write side).

## Flight control

- **63–66** — Pole-based diagnosis program: `diag_poles` (trim → Jacobian → eigenvalues), unstable aperiodic speed mode measured at 18 trims; mechanism = the G-hold law, refuting the back-side-of-drag-curve and alpha-bias hypotheses. P4.1 inner-loop correction (`kp05 = 1/K_nz`, real integrator) shrinks the mode 26×; STAB-P1 `alt_integral_gain` 1.2→0.6; FCS pitch speed damper implemented, **refuted by measurement**, kept default-off as negative evidence. `test_poles_envelope` CI gates (5) pin goldens.
- **69** — Three pre-existing E2E failures closed: shipped `korea.world.json` was invalid JSON (8,016 dangling keys), landing STAB-E47/E48/E49/E57–E62. Suite 2,435.
- **STAB-E series (~55 fixes)** — instrumented, trace-verified flight-control fixes; full `digi_full_mission` passes end to end.
- **DIGI-1/2, ALT-2…5** — airspeed-rotated gamma-hold law; NED→ENU quaternion fix; altitude-loop tranches.

## Campaign loop

- **C1–C6** — War loop closes: result ledger + write-back (C1), one pool for tasking/losses/resupply (C2), threat map + A* routes (C3), campaign thread + 7-phase ATM tasking + full 3D coverage (C4), 24-hour war acceptance harness + starved-worker fix (C5), A/A combat live (C6).
- **V-CAMP** — Live campaign session in the world viewer (time controls, flying flights, route inspection); runtime fixtures become build outputs.
- **G1/G2** — Ground war (battalion maneuver, front line, books) and the interdiction link (CAS against real battalions).
- **PERF-1** — Shared air picture: merge-phase collapse, closed output-identically at 3–4×.
- **62** — WorldState→JSON emitter; the closed save loop (§6.1).

## Combat chain

- **M1–M5** — f4-weapons core (M1), f4-sensors (M2), AI tactics: BVR→WVR merge, guns, 2-ship wingman (M3), BVR intercept acceptance harness + combat events in the recorder (M4), A/G employment + WVR/guns merge harness (M5, incl. Task 68).

## Data & no-binary runtime

- **57–60** — AAR redesign reconciliation; `@asset:` resolver with manifest hash verification (Tranche 0e.3); runtime glTF rewire, `f4-models` cut from `f4-simulation` (Tranche 0d renderer+simulation halves); viewer on-demand conversions into `Data/Temp`; CI repair (LZSS use-after-free).
- **Tranche 0a–0e** — JSON subset of `Data/` committed (runs anywhere without an F4 install); CMake boundary enforcement; TEX→PNG + glTF materials; `f4import` model/texture exporters.
- **SIMDATA waves 1–2** — maneuver table, brain archetypes, formations; class table, sensors, signatures fly as data.

## Platform & hygiene

- **SYMBOL-SVG-1** — `f4-xml` (vendored pugixml) + SVG symbol authoring (import/export, color roles, holes, earcut fills).
- **QC-PASS-1** — Viewer QC sweep: route clutter gating, honest speed feedback, 3D for selections, mechanical cleanup.
- **TERRAIN-TEX-1/2, VIEWER-V71-1, GLV3D series** — textured terrain, campaign-save v71 decode, 3D viewer pipeline.
- **ECS Phases 0–6** — stabilize; type safety; `Cursor::check_and_throw()`; dedup; angle strong-type migration (Phase 4); deferred-item resolution; pre-AI hardening.
- **STEP-0** — Green suite + CI + repo hygiene.

---
*Going forward: one line per landed task, appended at the top. Narration
belongs in the as-built doc for the subsystem, not here.*

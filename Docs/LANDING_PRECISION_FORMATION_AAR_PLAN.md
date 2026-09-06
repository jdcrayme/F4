# Landing Precision, Taxi-Back, Formation & AAR — Tightened Acceptance

> **Status**: Active plan. The flight-control stability work
> (`FLIGHT_CONTROL_NEXT_STEPS.md`, `FLIGHT_CONTROL_STABILITY_PLAN.md`) landed
> the STAB-E series and Phase A–C tranches — the aircraft flies the full
> mission loop end to end. This plan picks up where that work left off: the
> user-visible imprecision the loose `test_digi_mission` tolerances were
> hiding, plus the two procedural-skill demonstrations the AI plan sequenced
> but never delivered (formation, AAR).
>
> **Progress (Tasks 46–48, code landed — build/test pending in the user's env)**:
> - **A4** ✅ — `tef_cmd`/`lef_cmd` in the replay JSON `FlightSnapshot` +
>   the full control-command block populated (the replay JSON was
>   control-blind before; the FCS CSV trace had the data, the replay
>   didn't). Round-trip + JSON-content tests pin the new keys.
> - **A1–A3** ✅ (earlier) — tightened tolerances + lateral bounds guard
>   + energy-based flare predictor all in code. **A precision residual**:
>   the `<50 ft` cross-track assertion (Tranche A1) is held as the gate;
>   Task 45 measured 93–162 ft actual. The residual is the next tuning
>   target (wings-level through the flare + centerline-hold in rollout,
>   iterated against the CSV trace) — needs the user's build env, not
>   silently relaxed.
> - **Tranche D (AAR)** ✅ code — `RefuelModule` (5-state SM) +
>   BrainComponent AAR rung (between Formation and Mission) + `tanker_track`
>   scenario + unit + E2E tests. Built on the pre-existing
>   ATC/ScriptedTanker/TankerConfig scaffold. Build/test pending.
> - **Tranche C (formation)** ✅ code — `formation_acceptance` scenario
>   (4-ship, 3 FORMDAT slots: trail/wedge/ladder) + slot-position
>   tolerance test. "Extend, don't build" — no WingmanModule changes.
>   Build/test pending.
> - **Tranche B (taxi-back)** ⏳ — not started (PLT_PARK data
>   investigation + Kunsan `taxi_in_route`/`parking_spots` wiring).
>
> **Predecessors**: `FLIGHT_CONTROL_NEXT_STEPS.md` (§2 table verified STALE
> by RECON-1 — 15 of 18 "not done" items are actually done), `AI_IMPLEMENTATION_PLAN.md` §5
> (Steps 1–5, 7–11 DONE; Step 6 AAR NOT STARTED; Step 12 DONE as BrainComponent),
> `NEXT_PHASE_PLAN.md` (Phase 2 campaign-derived scenarios landed).
>
> **Companions**: `COMBAT_CHAIN_PLAN.md` (M1–M3 landed), `CAMPAIGN_LOOP_PLAN.md`
> (C1–C6, G1, G2 landed), `AIRCRAFT_BINDING_DESIGN.md`.
>
> **Recon**: RECON-1 (landing + taxi-back) and RECON-2 (f4-ai module inventory)
> appended to `worklog.md` before this plan. Every "Status" claim below is
> verified against source, not against a plan doc.

---

## 1. The gap this closes

The flight-control stability plan's central lesson was *instrument before you
touch*. The STAB-E series then landed ~55 fixes — each verified by CSV trace
on an isolated scenario. The full-mission `test_digi_mission` passes. **But
the user sees the aircraft land away from the touchdown point and outside the
runway bounds, and never taxi back to parking.** Three root causes, all
verified by reading source:

### 1.1 No lateral runway-bounds guard

`LandingClearance` (`f4-ai/include/f4/ai/atc/messages.hpp:104`) carries
`runway_heading_rad` and `threshold_position` but **not** `runway_width_ft`.
`StubATC` (`f4-ai/include/f4/ai/atc/stub_atc.hpp:174-180`) builds the clearance
the same way. The landing module (`landing_module.cpp:585-591`) computes
`course_lateral_ft()` but **never consults any runway width** — there is no
field to consult. `check_flare_or_goaround()` (`.cpp:839`) and
`check_touchdown()` (`.cpp:865`) gate on along-track (`missed_along_ft`) and
altitude only. A landing 400 ft off the centerline on a 150-ft-wide runway
passes every gate the code has. **This is the "outside the runway bounds"
defect, exactly.**

### 1.2 `test_digi_mission` tolerances hide the imprecision (Phase 5b — the one genuinely undone item)

`test_digi_mission.cpp`:

| Check | Current tolerance | Honest target | Line |
|---|---|---|---|
| Touchdown cross-track | `< 800 ft` | `< 50 ft` (half a 150-ft runway + margin) | 286 |
| Touchdown along-track | `>= -2500 ft, <= rwy_len+800` | `1000–2000 ft past threshold` (the aim point ± 500) | 288-291 |
| Final lateral tracking | `< 2500 ft` | `< 250 ft` (the localizer beam is ±350 ft full-scale) | 281 |
| Taxi corridor | `< 250 ft` | `< 80 ft` (a taxiway is ~75 ft wide) | 261 |
| Parking-spot arrival | `±120 ft` | `±25 ft` (a parking spot is a point) | 299-300 |

The comment at line 279-280 — "at ~250 kts with a 20-deg bank limit the jet's
turn radius is ~13,000 ft, so intercept S-turns of a couple thousand feet are
honest flying" — is a rationalization. The localizer beam is ±350 ft
full-scale; a couple thousand feet of S-turn is **not** established on final,
it's still intercepting. The tolerance codifies a failure mode as expected
behavior. **This is why E2E "passes" despite the user-visible imprecision.**

### 1.3 The flare is sink-rate-driven, not point-precision

`controls_for_flare()` (`landing_module.cpp:1116-1203`) — STAB-E8 demoted the
touchdown-point predictor to a ±2 deg trim term; the pitch driver is a
sink-rate law (target −400 fpm). The predictor at `.cpp:1169-1175` is the
go-around arbiter only. A sink-rate flare lands *somewhere*; a point-precision
flare lands *at the aim point*. The STAB-E8 comment explains why: the
predictor's `time_to_ground` diverges at small sink (floored at 50 fpm it
predicted touchdowns 18,000 ft downrange). **The cure is a non-divergent
predictor, not abandoning prediction.** An energy-based predictor
(KE + PE → distance along the beam, with sink-floor applied to the *closure
geometry* not the time computation) stays bounded and can drive pitch.

### 1.4 Taxi-back-to-parking is a data issue (confirmed)

`LandingModule::TaxiIn` (`landing_module.cpp:906-920, 1215-1224`) exists and
works — it follows `taxi_in_route_` polylinally to `ParkedComplete`.
`campaign_bridge.cpp:322-347` derives `taxi_in_route` from the `PT_TAXI`
polyline for decoded airbases. **But**: (a) `campaign_bridge.cpp:349` documents
"this Korea PD has no PLT_PARK lists anywhere" — parking spots are
**synthetic** (ramp-area centroid, not a real spot); (b) the hand-authored
`kunsan_parking.json.in` scenario **omits** `taxi_in_route` entirely (the
aircraft parks on the runway). The user's "may be a data issue" instinct is
correct. The code is ready; the data isn't wired for the hand-authored
scenario, and the campaign-derived parking spots are approximations.

### 1.5 Formation is REAL; AAR has a scaffold but no module (confirmed)

RECON-2 verified: `WingmanModule` is a real 3-state SM with 5 built-in
formations + 9 FORMDAT data-driven formations, two-channel steering
(lateral heading-to-station + longitudinal PD speed law), battle-tested in
`test_combat_integration.cpp` 2v2 E2E. **Formation needs a dedicated
acceptance scenario and a slot-position tolerance test — not new code.**

AAR: `refuel_module.{hpp,cpp}` does **not exist** (not in `f4-ai/CMakeLists.txt`'s
14 sources). But the scaffold is pre-built: a 7-message ATC protocol
(`RefuelRequest`/`TankerAssigned`/`ContactMade`/`ContactLost`/
`RefuelComplete`/`Disconnect`), a `TankerConfig` struct with boom envelope
(±30 ft longitudinal, matching `AI_IMPLEMENTATION_PLAN.md` §3.8), a
`ScriptedTanker` that flies straight-and-level, and `StubATC` already
subscribes to `RefuelRequest` and emits `TankerAssigned`. **AAR is build-on-
scaffold, not build-from-scratch.**

---

## 2. Tranches

Four tranches, ordered by dependency. Each has its own acceptance criteria
(§3-6) and produces its own patch file. Tranches A and B are the flight-
control finish line; C and D are the procedural-skill demonstrations.

### Tranche A — Landing precision (the flight-control finish line)

**The forcing function**: tighten `test_digi_mission` Phase 5b tolerances
FIRST, watch them fail, then fix the code until they pass. This inverts the
STAB-E pattern (fix in isolation, verify in isolation) — the tightened
tolerances are the full-mission gate that makes isolation-fixes accountable.

**A1 — Tighten tolerances (the gate).** `test_digi_mission.cpp`:
- Touchdown cross: `< 800` → **`< 50`** ft
- Touchdown along: `>= -2500, <= rwy_len+800` → **`>= 500, <= 2500`** ft past threshold
- Final lateral: `< 2500` → **`< 250`** ft (tracking segment, along < −3000)
- Taxi corridor: `< 250` → **`< 80`** ft
- Parking-spot arrival: `±120` → **`±25`** ft

**A2 — Lateral runway-bounds guard (the "outside the bounds" fix).**
- Add `runway_width_ft` to `LandingClearance` (`messages.hpp:104`) and to
  `StubATC`'s `TankerConfig`-style airfield config (`stub_atc.hpp:50-52`).
- Populate from `ScenarioAirfield::runway_width_ft` (`scenario.hpp:120`)
  in `StubATC`'s landing-clearance builder (`stub_atc.hpp:174-180`).
- Store `runway_width_ft_` in `LandingModule` (initialize from clearance).
- In `check_flare_or_goaround()` and `check_touchdown()`: if
  `abs(course_lateral_ft()) > runway_width_ft_ / 2` while AGL < flare height
  × 2 (the near-runway environment), fire `GoAround`. The flare itself
  stays wings-level (the localizer was tracked in OnFinal; the flare is
  roundout, not tracking).
- In `OnFinal`: if `abs(course_lateral_ft()) > runway_width_ft_ / 2` and
  `course_along_ft() > -missed_along_ft`, the localizer has been lost
  inside the final approach gate — go around.

**A3 — Energy-based flare point-precision (the "away from touchdown" fix).**
- Replace the STAB-E8 sink-rate-only driver with an **energy-based touchdown
  predictor** that stays bounded at small sink:
  - `closure_speed_fps = max(vcas_fps * sin(glide_slope), 5.0)` — the
    *along-beam* closure, floored at a small positive constant (not the
    vertical sink). This is what closes the distance to the ground along
    the beam; it cannot diverge because it's bounded below by the beam
    geometry, not by a sink floor.
  - `td_distance_ft = alt_agl_ft / sin(glide_slope)` (the beam-distance
    to the ground) — exact for a beam ride, bounded for a flare.
  - `td_along = course_along_ft() + td_distance_ft`
- Pitch driver: `td_err = td_along - aim_along` (aim_along =
  `beam_aim_offset_ft`).
  - `td_err > 0` (long): pitch up more (bleed energy).
  - `td_err < 0` (short): relax toward the STAB-E8 sink law.
- Blend: `flare_pitch_adj = clamp(td_err / scale, -3, +5) + sink_trim`.
  The sink law becomes the *floor* (prevents diving), the energy predictor
  becomes the *driver* (lands at the aim point). Both bounded; neither
  diverges.
- Keep the go-around arbiter (the existing `td_along > missed_along_ft ||
  td_along < -500` check) — it now reads the non-divergent predictor.

**A4 — FlightSnapshot flap observability.** Add `tef_cmd`/`lef_cmd` to the
recorder JSON (`flight_recorder.cpp`). The FCS CSV has them; the replay JSON
doesn't. One-line fix; closes the observability gap for flare debugging.

**Acceptance (§3).**

### Tranche B — Taxi-back-to-parking (the data + wiring fix)

**B1 — PLT_PARK data investigation.** Read `f4-world-convert`'s airbase
decoder and `f4-world`'s `GroundLayoutList` loader. Confirm whether Korea PD
truly has no `PLT_PARK` lists, or whether the decoder skips them. Report
finding in the worklog. If the data exists but isn't decoded: decode it
(small tranche in `f4-world-convert`). If the data is absent: B2.

**B2 — Hand-authored parking overlay for Kunsan.** The `kunsan_parking.json.in`
scenario gets a real `taxi_in_route` (the reverse of the `taxi_route`, with a
turn into the parking apron) and explicit `parking_spots` (2–4 spots at the
Kunsan ramp, derived from the `GroundLayoutList` parking points if present,
else hand-placed from the real Kunsan layout — the airbase is well-documented).
The scenario already supports these fields (`scenario.hpp:115, 123`); they're
just empty in the hand-authored file.

**B3 — Taxi-back acceptance test.** New test `test_taxi_back.cpp` (or a new
case in `test_digi_mission.cpp`): after `ParkedComplete`, assert:
- The aircraft is within `±25 ft` of a `parking_spots` entry (not the ramp
  centroid).
- The aircraft followed `taxi_in_route` within `±40 ft` (the
  `taxi_wp_capture_radius_ft` is 40; the corridor should be tighter).
- The aircraft is stopped (`vt < 3 ft/s`).
- The aircraft's heading is within `±10 deg` of the parking-spot heading.

**Acceptance (§4).**

### Tranche C — Formation demonstration (extend, don't build)

**C1 — Dedicated formation acceptance scenario.** New scenario
`formation_acceptance.json.in`: a 4-ship (lead + 3 wingmen) in trail, 60-
second straight-and-level run, no bandits. Skill = Veteran. Each wingman in
a different slot (trail, echelon-R, wedge) to exercise multiple FORMDAT
entries. Template: `two_ship.json.in`.

**C2 — Slot-position tolerance test.** New test `test_formation_acceptance.cpp`:
- For each wingman, sample slot-position error (actual position − lead
  position − commanded slot offset) every tick for the 60-second run.
- Assert: 95th-percentile slot error `< 50 ft` lateral, `< 100 ft`
  longitudinal (the PD speed law is looser longitudinally).
- Assert: heading match within `±5 deg` (formation integrity).
- Assert: speed match within `±10 kts` (velocity matching).
- The 95th-percentile (not max) tolerates the join transient and turn
  entries; the steady-state is tighter.

**C3 — Screenshot demonstration.** The `--screenshot` path on the formation
scenario (once `f4-scenario-player` is buildable in the user's env) produces
a visible 4-ship. (We cannot run the GUI app here — no X11 dev headers — but
the scenario + test are the deliverable; the screenshot is the user's
verification.)

**Acceptance (§5).**

### Tranche D — AAR demonstration (build the module on the scaffold)

**D1 — `RefuelModule` (the 5-state SM).** New files
`f4-ai/include/f4/ai/modules/refuel_module.hpp` +
`f4-ai/src/modules/refuel_module.cpp`. State machine per
`AI_IMPLEMENTATION_PLAN.md` §5 Step 6:

```
NoTanker ──RefuelRequest──→ VectorTo
VectorTo ──BoomInRange(±30ft lat, ±50ft long, ±20ft vert)──→ Waiting
Waiting  ──ContactMade(boom contact)──→ Refueling
Refueling──FuelComplete OR Disconnect──→ Done
Refueling──ContactLost──→ VectorTo (re-rendezvous)
Any      ──Emergency(low fuel / collision)──→ done via BrainComponent ladder
```

- Consumes the existing ATC protocol (`RefuelRequest`/`TankerAssigned`/
  `ContactMade`/`ContactLost`/`RefuelComplete`/`Disconnect`).
- Consumes `TankerConfig`'s boom envelope (±30 ft longitudinal tolerance).
- Produces `AIControlOutput` — fine formation positioning (tighter than
  WingmanModule's fighting-wing: the boom envelope is ±30 ft vs formation's
  500+ ft). Reuses `AirSteering` with a tuned gain set, or a dedicated
  precision-formation law (PD on position error, no heading-to-station —
  the tanker is straight-and-level).
- The `ScriptedTanker` is the target (no FM needed for the tanker; it
  advances its position by velocity × dt).

**D2 — BrainComponent wiring.** Insert `RefuelModule` as a rung in the
BrainComponent priority ladder (RECON-2: GroundAvoid > CollisionAvoid >
fuel-gate > Defensive > WVR > BVR > Strike > Formation > Mission). AAR sits
**between Formation and Mission** — it's a mission-phase activity (the
flight plan says "refuel here"), not a tactical one. Triggered by a mission
waypoint of type `REFUEL` or an explicit `RequestRefuel` from the flight
plan.

**D3 — AAR scenario.** New `tanker_track.json.in`: one `ScriptedTanker` on a
60-NM racetrack (straight-and-level on the leg), one receiver spawning 5 NM
behind and below. 120-second run: rendezvous (VectorTo), close (Waiting),
contact (Refueling, 30 s of fuel transfer — the `ScriptedTanker` declares
`ContactMade` when the receiver is in the boom envelope), disconnect
(`RefuelComplete`), return to station.

**D4 — AAR acceptance test.** New `test_refuel_module.cpp`:
- Unit tests: each SM transition, the boom-envelope geometry check, the
  precision-formation law's position-hold within ±15 ft.
- Integration (`test_aar_e2e.cpp` or a case in the scenario test): the
  receiver achieves `Refueling` state within 90 s, holds for ≥ 30 s, the
  boom-envelope tolerance maintained throughout (`±30 ft` long, `±20 ft`
  lat, `±20 ft` vert — 95th percentile), clean disconnect.

**Acceptance (§6).**

---

## 3. Acceptance criteria — Tranche A (landing precision)

1. `cmake --build build` succeeds; `test_digi_mission` and
   `test_landing_module` build.
2. **Tolerances tightened** (A1): the five `test_digi_mission` thresholds
   are at the honest targets. The test FAILS at head before A2/A3 land
   (verified by running it on the pre-patch build).
3. **Lateral bounds guard** (A2): a new `test_landing_module` case — an
   aircraft on final 400 ft off centerline (outside a 150-ft runway) —
   fires `GoAround` before flare entry. No regressions in the existing
   `test_landing_module` cases.
4. **Flare point-precision** (A3): the `on_glideslope` isolated scenario
   (already exists) touches down within `±200 ft` of the aim point
   (`beam_aim_offset_ft` = 1500 ft past threshold). The `digi_full_mission`
   E2E touches down within `±500 ft` of the aim point. Both verified by CSV
   trace (the FCS CSV exporter is already wired — RECON-1 confirmed Phase 0b
   DONE).
5. **Full-mission pass** (A1+A2+A3 together): `test_digi_mission` passes
   with the tightened tolerances. Both straight-in and traffic-pattern
   variants.
6. **Flap observability** (A4): the replay JSON for a landing scenario
   contains `tef_cmd`/`lef_cmd` fields per snapshot.
7. **No regressions**: the existing 2,226 tests stay green. The combat
   chain (M1–M3), the campaign loop (C1–C6, G1, G2), and the campaign_qc
   MD5 certificates are byte-identical (combat/campaign are not touched by
   this tranche — the landing module is scenario-player-only).
8. **CSV trace attached**: a before/after CSV trace pair for
   `digi_full_mission` is committed under `download/traces/` (the
   instrumentation already exists; this is the verification reference).

## 4. Acceptance criteria — Tranche B (taxi-back)

1. `test_taxi_back` (or the new `test_digi_mission` case) passes: aircraft
   parks within `±25 ft` of a real `parking_spots` entry, stopped, heading
   within `±10 deg`.
2. The aircraft followed `taxi_in_route` within `±40 ft` (the capture
   radius).
3. The `kunsan_parking.json.in` scenario carries a real `taxi_in_route`
   and `parking_spots` (no longer parks on the runway).
4. If B1 found PLT_PARK data exists but was undecoded: the
   `f4-world-convert` decoder now emits parking points, and a campaign-
   derived airbase's `parking_spots` are the real decoded spots (not the
   synthetic ramp centroid). The `campaign_bridge.cpp:349` TODO is resolved
   or re-scoped.
5. No regressions in `test_digi_mission`'s straight-in and pattern variants.

## 5. Acceptance criteria — Tranche C (formation)

1. `test_formation_acceptance` passes: 4-ship, 60 s, 95th-percentile slot
   error `< 50 ft` lateral / `< 100 ft` longitudinal, heading `±5 deg`,
   speed `±10 kts`.
2. The `formation_acceptance.json.in` scenario exercises ≥3 distinct
   FORMDAT slots (trail, echelon-R, wedge).
3. No regressions in `test_wingman_module` or `test_combat_integration`.
4. The `--screenshot` path on the formation scenario is documented (user
   verification step).

## 6. Acceptance criteria — Tranche D (AAR)

1. `test_refuel_module` unit tests pass: all 5 SM transitions, boom-envelope
   geometry, precision-formation position-hold (±15 ft).
2. `test_aar_e2e` passes: receiver achieves `Refueling` within 90 s, holds
   ≥ 30 s, 95th-percentile boom-envelope tolerance maintained (`±30 ft`
   long, `±20 ft` lat, `±20 ft` vert), clean disconnect to `Done`.
3. The `tanker_track.json.in` scenario runs end to end through the
   `ScriptedTanker` + `RefuelModule` + `BrainComponent` ladder.
4. `RefuelModule` is wired as the AAR rung (between Formation and Mission)
   in BrainComponent; the fuel-gate and collision-avoid rungs preempt it
   (verified by a test where a collision threat during refueling breaks
   the receiver out).
5. No regressions in the existing 2,226 tests.

---

## 7. What does NOT change

- The aircraft entity component set (`Transform + FlightModel + VisualModel
  + Brain`) and the aircraft-binding design (`AIRCRAFT_BINDING_DESIGN.md`).
- The two-pass ECS tick contract (brains ≥ 75, physics < 75).
- The combat chain (M1–M3), the campaign loop (C1–C6, G1, G2), and the
  campaign_qc MD5 certificates. None of these touch the landing module,
  the scenario player, or the AI modules this plan extends.
- The `f4-state-machine` library. `RefuelModule` composes on it like every
  other module.
- The `ScriptedTanker`, `TankerConfig`, and the 7-message ATC protocol —
  these are the scaffold D1 builds on, not changes.
- The `WingmanModule` — Tranche C extends with a scenario + test, not code
  changes.

## 8. Out of scope (deferred, deliberately)

- **Multi-ship landing** (formation approach to landing): the formation
  scenario is straight-and-level; a formation approach adds the landing
  precision of Tranche A to the formation discipline of Tranche C. Lands
  after both are proven individually.
- **AAR with a real tanker FM** (6-DOF tanker instead of ScriptedTanker):
  the ScriptedTanker is the demo surface; a real tanker needs a tanker
  `.dat` → `AircraftConfig` and a tanker brain. Deferred to the real-data
  import tranche.
- **Boom-vs-drogue**: the `TankerConfig` envelope is boom-shaped; a drogue
  basket has different geometry. The module's state machine is the same;
  the envelope check is the swap. Deferred until a drogue tanker data
  exists.
- **F4-dis (DIS networking)**: unrelated; explicitly future per
  `AI_IMPLEMENTATION_PLAN.md` §1.4.
- **Flight-control fixes beyond landing**: the STAB-E series addressed the
  takeoff and enroute stability. This plan does not reopen those. If the
  tightened `test_digi_mission` tolerances surface a takeoff or enroute
  defect, it gets its own tranche — not a scope creep here.

## 9. Implementation order

1. **A1** — tighten `test_digi_mission` tolerances. Run. Watch fail. (The
   forcing function — 30 min.)
2. **A2** — lateral runway-bounds guard. Run `test_landing_module` + the
   new lateral-bound case. (Half a day.)
3. **A3** — energy-based flare predictor. Run `on_glideslope` + the flare
   unit tests. Iterate against the CSV trace. (1–2 days — the predictor is
   the real work.)
4. **A4** — FlightSnapshot flap fields. (30 min.)
5. **A integration** — run `test_digi_mission` with tightened tolerances
   until both variants pass. Capture the before/after CSV.
6. **A patch + ship.**
7. **B1** — PLT_PARK investigation. (Half a day.)
8. **B2** — Kunsan taxi_in_route + parking_spots. (Half a day.)
9. **B3** — taxi-back test. (Half a day.)
10. **B patch + ship.**
11. **C1** — formation scenario. (Half a day.)
12. **C2** — slot-position test. (Half a day.)
13. **C patch + ship.**
14. **D1** — RefuelModule SM + precision law. (2–3 days — the real work.)
15. **D2** — BrainComponent rung wiring. (Half a day.)
16. **D3** — tanker scenario. (Half a day.)
17. **D4** — AAR tests. (1 day.)
18. **D patch + ship.**

---

*This document supersedes the "out of scope" framing of
`FLIGHT_CONTROL_NEXT_STEPS.md` §5 (Phase 5b was the one genuinely undone
item; the rest of that table is stale per RECON-1). The formation and AAR
deliverables close `AI_IMPLEMENTATION_PLAN.md` Steps 6 and 11's
demonstration gaps. The landing-precision tolerances are the gate the STAB-E
series earned but never enforced.*

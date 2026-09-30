# Avionics Plan — the engine-agnostic avionics logic layer

> **Status**: Active — **AVIONICS-2 LANDED** (the `f4-avionics` FCR page: the RWS/TWS/VS mode SM as a pure transition table, the lock state with the reference's lock rules driving `f4-sensors`' own `command_track`/`command_search`, and the renderer-facing `FcrPageSnapshot` — 18 unit tests + the done-when scenario test; see §4's as-built notes. AVIONICS-1 landed the scaffold: the IAircraftState seam, the INS alignment SM, the seeded deterministic drift walk, steerpoint navigation — 24 tests, label `f4-avionics`). AVIONICS-3–4 open.
> **Source of Truth**: [FreeFalcon/freefalcon-central](https://github.com/FreeFalcon/freefalcon-central) (develop branch), `sim/avio*`, HUD/MFD source under `sim/`
> **Companions**: [Architecture Proposal](ARCHITECTURE%20PROPOSAL.md) §1 (scope), [Falcon4 File Layout](FALCON4_FILE_LAYOUT.md), [AI Implementation Plan](AI_IMPLEMENTATION_PLAN.md)

---

## 1. Why this exists (the charter boundary, drawn again)

The Architecture Proposal's §1 excludes "cockpit/instrumentation systems"
from the F4 libraries. Read precisely, that exclusion covers *rendering and
interaction* — drawing symbology, hit-testing switches, cockpit art. It
cannot cover the avionics **logic**: what the radar page computes, how an
INS drifts, what a steerpoint demands, which symbolic cue the HUD state
wants. That logic is pure simulation state, it is exactly what every host
cockpit must display, and today it exists only inside the AI's private
modules (the AI "knows" its radar picture through SensorFusion but no
player-facing instrument model exists — the world viewer's debug HUD is a
debug overlay, not an avionics model).

Without an avionics layer, the libraries cannot host a playable cockpit: a
host would have to re-derive targeting geometry, radar mode semantics, and
navigation state from raw components — and every host would derive them
differently. FreeFalcon's own avionics is the reference behavior; Falcon
BMS's documented avionics is the fidelity target named where the reference
is silent.

**Positioning**: `f4-avionics` sits beside `f4-flight-api` and consumes the
same components the AI consumes (`f4-sensors` radar/tracks, `f4-weapons`
weapon store, `f4-geo` BRA/bullseye, `f4-world-types` day/night + weather).
It produces **avionics state machines + pure view models** (symbology
element lists, page-model structs). A host cockpit (raylib, web, whatever)
is a *renderer of the view models and an emitter of `PilotInput`* — the
library never draws and never reads devices. The AI does NOT consume
`f4-avionics` (the digi brains keep their SensorFusion pipeline — same
relationship the reference's digi brains have with the player's avionics
code: parallel, not shared).

## 2. FreeFalcon reference anatomy

| Upstream area | What it holds | Approx shape |
|---|---|---|
| `sim/avio*` + HUD sources | HUD symbology state (pitch ladder, FPM, airspeed/alt boxes, cue lights) | per-frame recompute from flight state |
| MFD/FCR pages | radar page modes (RWS/TWS/VS, bore/scan raster), stores page, HSD, defensive page | mode state machines + page models |
| INS code | alignment states, position drift model, steering to steerpoints | SM + drift integrator |
| SMS | station select, weapon profile, release consent | per-station state machine |
| CCIP/DTOS/pre-designate | air-to-ground delivery cues | bomb-sight geometry over the FM's trajectory prediction |

The F4 repo already owns most *inputs* these need: `f4-sensors` (the radar
sweep + track files + RWR + IRST), `f4-weapons` (`WeaponStore` with
per-station counts — `expend()`), `f4-flight-api` (`PilotInput`,
`IAircraftState`), `f4-geo` (`to_bra`, `to_bullseye`, the datum lattice),
`f4-world-types` (`day_night`, weather). `f4-avionics` composes them.

## 3. Design principles

1. **View models, not pixels.** Every cockpit-facing output is a struct of
   numbers/flags/enums (element id, position, value, blink phase). The
   golden test for the whole subsystem: two different renderers consuming
   one page model must be able to produce the same *instrument behavior*.
2. **Mode machines are data, transitions are tests.** Same discipline as
   `f4-state-machine`'s transition tables — each page's mode SM is
   declared, and every transition (including every *refusal*) has a test.
3. **Determinism.** No wall-clock, no host randomness: blinking and
   alignment timers ride the sim clock the WeatherSystem/cadences already
   use.
4. **The AI stays parallel.** No shared mutable state with `f4-ai`; where
   both need the same computation (e.g., aspect geometry), it comes from
   `f4-sensors`/`f4-geo`, never from the other consumer.
5. **Gates where behavior feeds back.** Page logic that *writes* (e.g., a
   TWS lock feeding the fire-control gate) lands behind the existing
   combat gates so AI-only runs stay byte-identical. Pure view models need
   no gate — nothing consumes them in the sim loop.

## 4. Tranche ladder

### AVIONICS-1 — the library scaffold + INS + steerpoint navigation — **LANDED**
`f4-avionics` scaffold (the CMake target, the `PilotInput`/`IAircraftState`
seam), then the INS: alignment SM (align states, drift integral as a
seeded deterministic walk keyed on the airframe's nav data), the stored
heading/altitude/position chain, and steerpoints (the flight plan the
campaign bridge already stamps on flights is the route source; the INS
reports *aircraft position through the drifting INS* vs the steerpoint).
**Done when**: an INS test shows alignment completing on the ground clock,
drift accumulating deterministically, and steerpoint ranges/bearings
reading through the INS; the drift-zero case is byte-identical to raw
positions.

**LANDED (AVIONICS-1)** — as-built, against this done-when:

- **The scaffold** — `f4-avionics`, header-only (the f4-geo discipline),
  namespace `f4::avionics`, umbrella `f4/avionics/f4_avionics.hpp`. Links
  `f4-flight-api` (the seam), `f4-geo` (WorldPosition + BRA/to_bra),
  `f4-state-machine` (the alignment table). Nothing else. Marked runtime
  side in the boundary verifier. The seam is read-side for now: the INS
  consumes `IAircraftState`; `PilotInput` emission arrives with the page
  models (AVIONICS-2+).
- **The alignment SM is a pure transition table** (`make_ins_machine()`:
  Off → Aligning on PowerOn, → Aligned on AlignTimer, → Off on Shutdown
  from both live states — no captures). Side effects live in
  `InsUnit::update()`, the polling→event bridge, the stall-SM discipline:
  the align clock accrues ONLY while the fed truth is `on_ground()` (the
  ground clock), and `AlignTimer` is sent when it passes `align_time_s`.
  Completion zeroes the chain against the align truth (that is what
  alignment IS) and seeds the walk.
- **The drift integral is a bounded rate walk** — per update, each axis's
  error rate takes one clamped uniform step (fixed sample order: east,
  north, up, heading) and the position error integrates the rate. The
  seed is the config's `seed`, else FNV-1a over `nav_data_key` (the
  airframe's nav-database identity); `nav_data_age_days` scales the step
  and mixes into the seed. The SAMPLER is a fully specified splitmix64 —
  NOT std::mt19937 + std:: distributions, whose sequences are
  implementation-defined — so the same seed fed the same update stream is
  byte-identical on every platform. Clamps bound the rate (sigma scales
  with age; the clamp does not — old data saturates the limit).
- **The scope decision (v1, an error model)** — the INS maintains the
  BELIEVED chain as truth-plus-error over the `IAircraftState` the host
  feeds, not an open-loop acceleration integrator: every avionics consumer
  reads the believed chain relative to steerpoints, and the error model
  gives that read honestly and deterministically without inventing an
  Euler-integration divergence no consumer asked for. A full strapdown
  integrator behind the same interface is a named later tranche if a host
  ever pins one.
- **Steerpoints** — `Steerpoint`/`SteerpointSequence` (ordered plan + a
  selected point; overflight is the host's decision — `next()` reports the
  wall, empty/out-of-range are loud `std::out_of_range`), `to_steer()`
  (f4-geo's BRA over the supplied position — slant range, true bearing
  wrapped [0, 2π)), `steer_cue()` (the HSI cue: signed bearing error
  wrapped [−π, π], shortest-way flag, dead-astern pins right — pinned so
  two renderers agree), and the `current_steer()` / `current_steer_cue()`
  through-the-INS conveniences.
- **Tests** — 24 (label `f4-avionics`, `test_ins.cpp` +
  `test_steerpoint.cpp`): alignment completes on the ground clock at
  exactly `align_time_s` (and the clock freezes airborne); shutdown from
  both live states; re-alignment resets the walk; determinism per seed
  AND per update stream (same time, different dt stream → different
  walk); keyed on nav data (key and age both move it); clamps bound the
  drift; the drift-zero twin compares equal to raw truth member-for-member
  through 200 ticks of moving truth; the through-INS steer reads differ
  from raw under drift and equal it with drift off; the cue pins (shortest
  way, the dead-astern pin).

### AVIONICS-2 — the FCR page model (the radar page as an SM) — **LANDED**
RWS → TWS/VS mode SM over `f4-sensors`' radar component (the AI's
`RadarBackedDetectionPolicy` geometry, presented as page semantics:
azimuth/elevation bars, ranges, a lock state with the reference's lock
rules). Output: `RadarPageModel` + a `f4-sensors` lock hand-off the
fire-control gate already honors. **Done when**: the page model's lock
state drives the same `can_fire` path the AI's MissileModule uses, from
page inputs, in a scenario test.

**LANDED (AVIONICS-2)** — as-built, against this done-when:

- **The discovery that shrank the tranche**: `RadarSimComponent` ALREADY
  owns the lock primitives — `RadarMode{Search, Track}` +
  `command_track()`/`command_search()` with the reference's refusal rule
  ("cannot lock what the radar is not tracking") and the auto-drop ("the
  lock cannot outlive its track"). The fire-control chain was already
  end-to-end: live track → `RadarBackedDetectionPolicy`'s radar leg
  (`detected_by_radar`) → `MissileModule::should_fire`. So AVIONICS-2 is
  a PAGE over those primitives, not a new sensor concept: `f4-sensors`
  gained nothing, `f4-ai` gained nothing, and the whole tranche is
  additive to f4-avionics (zero bytes of sim-loop change — AI-only runs
  stay byte-identical by construction; the page writes only when a host
  drives it, the PilotInput-shaped seam).
- **`fcr_page.hpp`** — `FcrState{Off, Rws, Tws, Vs}` +
  `FcrEvent{PowerOn, PowerOff, Select*}` as a PURE transition table
  (`make_fcr_machine()`, the `make_ins_machine()` discipline: no
  captures; every transition AND every refusal pinned). `FcrPageModel`
  (the InsUnit pattern) owns the side effects: power_off clears the
  lock; mode switches KEEP it (the lock is orthogonal to the search
  display); `designate(id, radar)` refuses with the page Off and takes
  the radar's own `command_track` answer (page lock == radar lock, the
  no-divergence rule); `break_lock(radar)` parks the radar into Search;
  `update()` is a pure snapshot recompute with exactly ONE write — the
  track-death mirror (the locked track observed Dropped drops the page
  lock and defensively parks the radar, idempotent against the radar's
  own decay rule).
- **The snapshot** — `FcrPageSnapshot` + `FcrSymbol`: page-relative
  azimuth (bearing minus the live antenna center, wrapped [-π, π]),
  elevation, slant range, SIGNED closure (positive = closing, the
  radar-page convention), IFF hostile, the track-store's
  Established-or-Coasting "good track" flag, the designated flag, and
  the live scan frame (az half-width, el band, range scale). VS is
  velocity-only DISPLAY semantics (closing contacts only + a
  `velocity_only` flag) — no new radar physics (the plan's non-goal).
  Symbols in ascending entity_id; two renderers consuming one snapshot
  must agree frame for frame (pinned by an operator<=> determinism
  test).
- **The lock hand-off, honored** — the done-when, in
  `f4-simulation/tests/test_fcr_page_flight.cpp`: a two-ship scenario,
  the "player" brain combat-disabled (the radar belongs to the page),
  the AI's exact gate wired standalone (the radar's track store → the
  policy's radar leg → `MissileModule::should_fire`). The bandit starts
  INSIDE the player's ±60° bar at 12 NM and files east out of it:
  search alone lets the track decay and DROP — the radar leg dies and
  the gate closes; the page's designate (before the exit) parks the
  radar in Track mode, which scans the locked target regardless of the
  search volume — the leg stays lit the whole flight; break_lock kills
  it again. Page inputs driving the AI's own fire-control path.
- **Wiring** — `f4-avionics` now links `f4-sensors` (the radar
  component beside f4-flight-api/f4-geo/f4-state-machine; still never
  f4-ai). 18 unit tests (`test_avionics_fcr.cpp`, label `f4-avionics`)
  + the scenario test (label `f4-simulation`).

### AVIONICS-3 — the HUD view model
Symbology elements (airspeed/altitude boxes, heading tape, pitch ladder,
FPM, target boxes, cue arrows) computed from `IAircraftState` + the active
page model + `f4-geo` BRA. Pure recompute — no sim-loop feedback, no gate.
**Done when**: the element list for a pinned scenario frame matches a
golden element table (element ids, values, flags), and the same model
feeds both a text renderer in tests and the world viewer's HUD overlay
(replacing `sp_draw_hud`'s ad-hoc lines — the first real consumer).

### AVIONICS-4 — SMS + the HSD + the delivery cues
Stores page over `WeaponStore` (station select, profiles, release
consent), HSD (route + tracks + FLOT overlay from the shared picture),
CCIP/DTOS bomb-sight geometry from the FM's trajectory prediction.
**Done when**: a strike flow — steerpoint in, target designated, release
consent, pickle — expends the right station through the existing bomb
battery path under the campaign bridge's kCampaignWeaponMap.

## 5. Non-goals

- No rendering, no input devices, no cockpit art (charter boundary).
- No new sensor *physics* — `f4-sensors` is the only radar truth.
- No multiplayer datalink UI semantics (the Part-III datalink net is the
  sim-side picture; a datalink *page* is a later AVIONICS tranche).
- No BMS-only systems (DLINK, MVS) beyond a named future tranche.

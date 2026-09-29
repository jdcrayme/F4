# Avionics Plan — the engine-agnostic avionics logic layer

> **Status**: Active — the one plan for the avionics subsystem. Nothing landed yet; §4 is the tranche ladder.
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

### AVIONICS-1 — the library scaffold + INS + steerpoint navigation
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

### AVIONICS-2 — the FCR page model (the radar page as an SM)
RWS → TWS/VS mode SM over `f4-sensors`' radar component (the AI's
`RadarBackedDetectionPolicy` geometry, presented as page semantics:
azimuth/elevation bars, ranges, a lock state with the reference's lock
rules). Output: `RadarPageModel` + a `f4-sensors` lock hand-off the
fire-control gate already honors. **Done when**: the page model's lock
state drives the same `can_fire` path the AI's MissileModule uses, from
page inputs, in a scenario test.

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

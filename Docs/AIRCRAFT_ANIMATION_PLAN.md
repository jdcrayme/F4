# Aircraft Animation Plan — Tags, Rigs, and the Road to Animated Models

> **Status**: M0-M2 IMPLEMENTED (see §10 Implementation Status). M3-M5 open.
> **Created**: 2026-09-16
> **Implemented in**: f4-anim (channels/rig), f4-models (grouped extraction), f4-import (vocab + hierarchy emission), f4-gltf (anim map + evaluator), f4-renderer (parts + animated draw), f4-world-viewer (Animation Doctor section in the Class Table Browser).
> **Related documents**: `ASSET_PIPELINE_SPEC.md` (§6 glTF node tagging — the tag grammar this plan builds on), `AIRCRAFT_BINDING_DESIGN.md` (VisualModelComponent contract), `ARCHITECTURE PROPOSAL.md` (§13 f4-simulation), `FALCON4_FILE_LAYOUT.md`
> **Source references**: FreeFalcon `src/sim/include/dofsnswitches.h`, `src/sim/aircraft/surface.cpp` (RunSurfaces/RunGearSurfaces/RunLightSurfaces), `src/sim/airframe/airframe.cpp` (gearPos), `src/sim/airframe/gear.cpp` (RunLandingGear), `src/sim/simlib/simmover.cpp` (SetDOFangle/SetSwitchMask), `src/graphics/objects/drawbsp.cpp`

---

## 1. The problem

In the world-viewer today, aircraft models render but do not animate:

1. **Gears are in goofy positions** — gear legs appear baked at odd angles into the static mesh.
2. **Lights don't blink** — nav/strobe/landing light glow geometry is static.
3. **Effects geometry is locked in place** — afterburner plume, wing vapor, exhaust nozzle are stuck at their default baked pose.

### Root cause (one sentence)

`f4-import/src/gltf_emitter.cpp` bakes **each LOD into a single static mesh** with every DOF/switch/slot flattened at its default value, and emits DOF/switch/slot as **empty placeholder nodes** (`dof:unknown.N`) that are not connected to any geometry — so there is nothing left in the glTF for a runtime to animate.

Specifics from the current code:

- `gltf_emitter.cpp:526-535` — DOF nodes are emitted as empty scene-graph siblings with `"min": 0.0, "max": 0.0` (the real `BDofNode` ranges parsed by `f4-models` are thrown away, not just unmapped).
- `f4-renderer/src/mesh_builder.cpp:64` (`extract_gltf_lod_geometry`) — extracts one mesh per `lod:N` node and draws it statically; no transform application, no visibility switching.
- The `f4` extras schema, the `dof:/sw:/slot:/anchor:/lod:` grammar, and the loader that parses them (`f4-gltf`) **already exist** — the tag infrastructure is half-built; the geometry wiring and the runtime half are missing.

Meanwhile the behavioral ground truth lives in FreeFalcon as hard-coded DOF/switch indices and hard-coded presentation logic:

- `dofsnswitches.h` — the canonical enum of DOF/switch indices (complex, simple, helicopter, air-defense families).
- `surface.cpp:1690-1757` — gear sequencing: `gearPos ∈ [0,1]` moves **doors first** (`pos*2` in the first half), **then gear** (`(pos-0.5)*2` in the second half); show/hide switches are derived from DOF values with ~5° hysteresis; broken/stuck gear clamps the DOF at `0.6*range`.
- `airframe.cpp:883-891` — `gearPos` integrates at `0.3/sec` (≈3.3 s full cycle).
- `gear.cpp:49` (`RunLandingGear`) — wheel spin from ground speed / wheel radius; strut compression from terrain height under each gear; both applied to DOFs (`surface.cpp:1678-1679`).
- `surface.cpp:1498-1635` (`RunLightSurfaces`) — nav lights blink 0.4 s on / 0.5 s off with randomized phase; tail strobe 0.08 s on / 2.0 s off with randomized phase; landing lights gated on `gearPos == 1.0`; everything gated on main power. All timing constants come from `auxaeroData` (`animWingFlashOnTime`, `animStrobeOnTime`, …) — i.e. **per-airframe data, not code**.
- `surface.cpp:1061-1102` — afterburner: `COMP_AB` switch on `rpm > 1.0`, `COMP_ABDOF` scale DOF 0–1, exhaust nozzle as multi-state switch `1 << stage`.

---

## 2. Verdict on the tag proposal

**The tag scheme is the right call — and it is already half-designed.** `ASSET_PIPELINE_SPEC.md` §6 specifies exactly this: `dof:/sw:/slot:` node grammar, `f4` extras with provenance indices, one-time tag resolution per model at load, "simulation code animates by tag; missing tag = part absent = no-op." The proposal in this discussion is a confirmation of §6 plus the converter-side automation (drive the renaming from the hard-coded FreeFalcon list so humans never hand-tag hundreds of models). That automation is correct: the hard-coded list should be represented **once**, in the converter, and everything downstream should be data.

Two refinements are needed before the scheme actually fixes the three symptoms:

**Refinement A — tags without geometry wiring animate nothing.** The converter must stop baking models flat. DOF subtrees must become real glTF node hierarchies (geometry as children of a `dof:` node, pivot = `T(dof_translation)·dof_rotation`), switch children must become real child variants, and real min/max/mult/flags must land in the extras. Tag names are the addressing scheme; the hierarchy is the mechanism. Both are required.

**Refinement B — don't command tags directly from the flight model.** If the FM (or brain, or weapons system) writes raw DOF tags, the FreeFalcon presentation logic (doors-first sequencing, hysteresis, blink timers, broken clamps, wheel-spin integration) re-scatters across every caller, which is exactly the hard-coding problem we are trying to retire. Instead, insert one layer:

```
sim state (semantic, normalized)  →  animation rig (data-driven)  →  channels  →  tag map  →  renderer
```

The sim publishes *meaning* (`gear_pos = 0.62`, `per-gear damage flags`, `rpm`, `G`, `light flags`); a per-model-class **rig** turns meaning into channel values (sequencing, clamps, blink waveforms); channels address the model through the tag map. FreeFalcon's hard-coded *logic* then lives exactly once, generically, in the rig spec; FreeFalcon's hard-coded *index lists* live exactly once, in the converter's vocabulary tables.

Everything else in the original proposal survives unchanged: missing tag ⇒ no-op; one channel may drive many nodes (both main-gear legs); per-instance channel state lives beside the model state the way `VisualModelComponent.model_state` already does.

---

## 3. Architecture

### 3.1 Layer diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│ L1  SIM STATE (f4-flight-model, f4-simulation components)           │
│     gear_pos, gear_handle, per-gear flags (stuck/broken/door),      │
│     airbrake_pos, hook, dragchute, canopy, rpm, nozzle_stage,       │
│     g_load, light flags (nav/strobe/landing/master), wheel contact  │
│     exists today: gearPos (aircraft_state.hpp:145), switch sync     │
├─────────────────────────────────────────────────────────────────────┤
│ L2  ANIMATION RIG (new f4-anim, pure C++20, no rendering deps)      │
│     per model-class spec (JSON): state → channel mapping            │
│     gear sequencer, blink waveform evaluator, wheel-spin integrator,│
│     AB/vapor/nozzle rules, broken/stuck clamps                      │
│     deterministic: f(state history, sim_time, entity_seed)          │
├─────────────────────────────────────────────────────────────────────┤
│ L3  CHANNELS (f4-anim)                                              │
│     fixed enum of semantic channels; per-instance value array       │
│     command = (channel_id, value); unknown channel ⇒ no-op          │
├─────────────────────────────────────────────────────────────────────┤
│ L4  TAG MAP (f4-gltf / f4-assets)                                   │
│     built once per model at load: channel → [(node, op, params)]    │
│     nodes carry dof:/sw:/slot: names + f4 extras (§6 grammar)       │
│     FF dof index kept as provenance only                            │
├─────────────────────────────────────────────────────────────────────┤
│ L5  RENDERER (f4-renderer, raylib adapter)                          │
│     per frame: compose node matrices for tagged subtrees,           │
│     apply switch visibility, draw; shared static geometry           │
└─────────────────────────────────────────────────────────────────────┘
```

Dependency rule: L1–L3 are engine-agnostic libraries (L3 knows nothing about glTF; L2 knows nothing about channels' visual meaning beyond the vocabulary). L4–L5 are the only layers that touch glTF/matrices. This keeps `f4-anim` testable headlessly and the renderer swappable.

### 3.2 The channel vocabulary

A fixed enum in `f4-anim` (stable, additive-only), roughly 40 channels to cover everything `surface.cpp` does today:

| Channel group | Channels (abridged) | FF source |
|---|---|---|
| gear | `gear_pos`, `gear_door_pos.0..7`, `gear_leg_pos.0..7`, `gear_broken.0..7` (sw), `wheel_angle.0..7`, `strut_comp.0..7` | `ComplexGearDOF/DoorDOF/Switch/HoleSwitch/BrokenSwitch`, `COMP_WHEEL_*`, `COMP_GEAREXTENSION_*` |
| control surfaces | `stab.l/.r`, `flap.l/.r`, `tef.l/.r`, `lef.l/.r`, `rudder`, `airbrake.top/.bot.l/.r`, `spoiler.*` | `COMP_LT_STAB`…`COMP_RT_AIR_BRAKE_BOT` |
| engine | `ab_scale`, `ab_scale.2`, `nozzle_stage`, `nozzle_stage.2`, `throttle`, `rpm` | `COMP_AB(DOF)`, `COMP_EXH_NOZZLE(2)` |
| airframe | `canopy`, `hook`, `dragchute`, `refuel_probe`, `swing_wing`, `weapon_bay.0..4`, `intake_ramp.*` | `COMP_CANOPY_DOF`, `COMP_TAILHOOK`, … |
| lights | `light.nav`, `light.strobe`, `light.landing`, `light.wing`, `light.fuselage` | `COMP_NAV_LIGHTS`, `COMP_TAIL_STROBE`, `COMP_LAND_LIGHTS` |
| effects | `effect.vapor`, `effect.rotor.spin`, `effect.prop.spin` | `COMP_WING_VAPOR`, `HELI_*`, `COMP_PROPELLOR` |
| cockpit | deferred — 3D pit DOFs (100+) reuse the same mechanism later | `COMP_3DPIT_*` |

Conventions mirror §6.1: lowercase snake_case, `.l/.r/.0..N` instance suffixes. The enum is the *contract between rig and tag map*; FreeFalcon indices never appear at runtime.

### 3.3 Family-aware mapping tables (the "hard-coded list", done once)

The critical subtlety the single-list approach misses: **FreeFalcon's DOF/switch index spaces are per model family, and the same index means different things in different families** (`COMP_*` vs `SIMP_*` vs `HELI_*` vs `AIRDEF_*` in `dofsnswitches.h`). Within a family there are also per-model quirks (the header itself notes indices 6–8 and 25–27 were "used in some models" for gear compression bits).

Therefore the converter consumes **family tables + per-model overrides**, not one global list:

```
f4-anim/vocab/
  channels.json          # the channel enum + descriptions (advisory registry)
  family/complex.json    # FF index → channel: DOF 19→gear_leg_pos.0, SW 1→…
  family/simple.json     # SIMP_* mapping
  family/heli.json       # HELI_* mapping
  family/airdef.json     # AIRDEF_* mapping
  overrides/NNNNN.json   # per-vis-type deltas (F-16 index 6-8 = strut compression, …)
```

Converter behavior: classify the model into a family (slot/DOF/switch counts — the same signals `ModelRecord::visual_class()` already heuristically computes), apply the family table, apply per-model overrides, rename `dof:unknown.19` → `dof:gear_leg.0` / bind `channel: gear_leg_pos.0`, and emit anything unmapped as `dof:unknown.N` (preserved, linted as a doctor warning, exactly per §6.7). Unknowns still animate if a rig binds them by raw index — the escape hatch for community models.

### 3.4 Per-instance state

`VisualModelComponent` (or a sibling `AnimStateComponent`) gains:

```cpp
float         channel_value[kChannelCount];  // current values, written by rig
AnimTimeState time;                           // per-entity blink phase seeds
```

Tag maps are **shared const per model** (built at load); channel arrays are per instance. This mirrors `DrawableBSP`: shared node graph, per-instance `DOFData[]/switchData[]` (`simmover.cpp:531-544`).

---

## 4. Converter work (f4-import)

Behind a new option (`--hierarchy`, default on once golden), per model:

1. **Real hierarchy emission.** Walk the BSP tree as parsed (`f4-models` already returns the full node graph with `dof_rotation`, `dof_translation`, `dof_min/max/mult/flags`, switch children, slot transforms). Emit each DOF subtree as a glTF node `dof:<channel-or-unknown>` whose local transform is `T(dof_translation)·dof_rotation`, with geometry primitives attached beneath it. Nested DOFs (gear leg → strut → wheel) become nested glTF nodes — the transform composition order is already proven in `geometry_extractor.cpp` (`T·R·Rx(value·mult)`, rotation about **X**).
2. **Real ranges.** Extras carry the parsed `min/max/mult/flags` (fixes the current `"min": 0.0, "max": 0.0` emission at `gltf_emitter.cpp:530`).
3. **Switch children as variants.** Each switch child subtree becomes a glTF child node `sw:<id>.<child>`; runtime activates exactly one via visibility (per §6.4). The AB plume, vapor sheets, and gear-on/broken variants all become ordinary switch data.
4. **Light strings.** `BLightStringNode` chains export as `anchor:lightstring.<n>` nodes carrying color/point data so the renderer can draw blinking point sprites (§6 grammar already reserves `anchor:` for exactly this).
5. **Slots** keep their baked translation (already correct today) and gain `slot:` names from the family table where known (pylon/station mapping from class data).
6. **Provenance + lint.** Every renamed tag records the FF index and family in extras; `doctor` verifies: no `dof:unknown` for complex-family indices 19–24 on aircraft, min/max present, every `sw:` has ≥2 children. Golden round-trip tests: convert → parse → assert node structure and ranges.

Keep the flat-baking path as a fallback flag for tooling that wants static meshes.

---

## 5. Runtime work

### 5.1 Tag resolution (f4-gltf / f4-assets)

At load: walk the glTF, build `TagMap = { channel → vector<{node, op, min, max, mult, flags}> }`. One channel → many nodes (mirrored gear legs, 4 airbrake surfaces) is the normal case. This is the runtime equivalent of `DrawableBSP`'s per-node DOF list, built once and shared.

### 5.2 Animation application (f4-renderer)

Per frame, per instance: for each tagged node with a dirty channel, recompose the local matrix from the channel value using the **same math as `geometry_extractor.cpp`** (promote that DOF math into `f4-models` as a shared free function so offline extraction and runtime application cannot drift). Then rebuild subtree world matrices for affected nodes only (dirty-flag the chain to the root). Switches apply as child-visibility bitmasks (`SetSwitchMask` semantics, `simmover.cpp:531`). Apply channels to **all LOD tag maps**, not just the active LOD, so LOD flips never pop a half-extended gear; skip application entirely for billboard LODs.

### 5.3 f4-anim (the rig library)

Pure C++20, no glTF, no raylib. Inputs: semantic sim state struct + rig spec + sim time + entity seed. Outputs: channel value writes. Contains, as data-driven behavior:

- **Gear sequencer** — doors run `2·pos` clamped, legs run `2·(pos−0.5)` clamped, ranges per station from imported aero data, broken/stuck legs clamp at `0.6·range`, door/leg/hole visibility derived with 5°-equivalent hysteresis. (Port of `surface.cpp:1684-1757`, parameterized, not per-airframe.)
- **Blink evaluator** — waveform spec `{period, duty, phase_seed, jitter}` → 0/1 intensity; nav = 0.4 s on / 0.5 s off, strobe = 0.08/2.0; phase from a per-entity seeded PRNG so replays are deterministic (FreeFalcon's `rand() % FlashOff` de-sync becomes a seeded stream).
- **Wheel/strut integrator** — wheel angle integrates ground speed / wheel radius while in contact, RPS decays airborne; strut compression from (terrain height under gear − current height), clamped by imported `animGearMaxComp/Ext`. Needs per-gear ground queries — available in the viewer's terrain path; in the FM, the existing `computeMinHeight` gear ground logic.
- **Engine/effect rules** — `ab_scale` from rpm excess, `nozzle_stage` from nozzle position quantized to model's stage count, `effect.vapor` from G threshold.
- **Simple/heli/airdef rigs** — rotor spin, radar azimuth/elevation sweep, simple-model `SIMP_*` bindings.

Rig specs are small JSON per family, versioned in `f4-anim/vocab/`, overridable per vis type — the same shape as the converter tables.

---

## 6. Design decisions & trade-offs

| Question | Decision | Rationale |
|---|---|---|
| Tags embedded in glTF vs sidecar manifest per model | **Embedded** (§6 extras), registry advisory | Self-contained assets survive copy/rename; §6 already specifies it; sidecar would duplicate state |
| Sim commands tags directly vs rig layer | **Rig layer** | Sequencing/hysteresis/blink logic must exist once; keeps FM clean; ACMI compatibility |
| Channel enum vs free-form strings | **Enum + string registry** | Enum for the hot path and compile-time safety; registry for tooling/community extensions |
| Hierarchy emission vs flat + joint lists | **Hierarchy** | glTF-native, Blender round-trip free, nested gear works; joint lists would re-invent skinning |
| Blink in rig vs renderer | **Rig** | Determinism/replay; renderer stays dumb; viewer can scrub time |
| Animate all LODs vs active only | **All LODs** | Prevents pops on LOD flip; cost is negligible vs geometry |
| `Mesh_Destroyed` as separate tag kind | **No — switches + clamps** | FF destruction = switch variants (broken gear) + model swap on death; a new kind would duplicate `sw:` |

---

## 7. Known challenges / open questions

1. **Index collisions across families** — §3.3 tables + overrides; doctor lints suspicious bindings (e.g. a "complex" table applied to a model whose DOF count matches the simple family).
2. **Per-model quirks inside a family** — `dofsnswitches.h` admits indices 6–8, 25–27 were repurposed on some models. Overrides are per-vis-type; the world-viewer model-doctor panel is the discovery tool: it lists every tag + raw index, lets you drive each by slider, and screenshots disagreements.
3. **Dynamic coordinates** — Falcon animates some vertices directly (morph-style, `n_dynamic_coords`) and via `BTransNode`/`BScaleNode`. v1 bakes them at defaults (status quo); v2 maps them to glTF morph targets or per-frame vertex patches. Track which airframes visibly need it (wing flex, canopy morphs) before committing.
4. **Multiple texture-set swaps driven by switches** — §13 open question in the pipeline spec; orthogonal to motion animation, same tag map.
5. **Determinism & replay** — all rig time comes from sim time; all randomness from entity-seeded PRNG; the channel write stream is loggable in `f4-recorder` and is essentially the ACMI DOF/switch record format, which also gives us a conformance test source later.
6. **auxaeroData import** — the rig needs per-airframe numbers (gear ranges `NosGearRng`, `animWheelRadius`, strut limits, flash times). FreeFalcon reads them from per-airframe aero DATs (`airframe/readin.cpp`); `f4-import` must extend the airframe JSON export to carry them. Until then the rig uses FF's defaults (0.3/s cycle, 0.4/0.5 nav, 0.08/2.0 strobe).
7. **Performance** — worst case is a full-airframe DOF rattle; mitigation is dirty-flagging, shared tag maps, and LOD cut-off (no gear animation beyond LOD 2 — matches FF's far-LOD behavior anyway).
8. **Ground queries for strut compression in the viewer** — staged views have no terrain under the model; rig must accept a "no contact" input gracefully ( strut = fully extended, wheels stopped).
9. **3D cockpit** — 100+ `COMP_3DPIT_*` DOFs/switches reuse the identical mechanism; out of scope here, but the channel enum should reserve the range now so cockpit tags never collide.

---

## 8. Milestones

| M | Deliverable | Exit criteria |
|---|---|---|
| **M0 — Vocabulary** | `channels.json` + 4 family tables + doctor rules; channel enum in `f4-anim` skeleton | Every `dofsnswitches.h` entry has a table row or an explicit "unknown" disposition; zero behavior change |
| **M1 — Converter hierarchy** | `--hierarchy` emission: real DOF/switch/slot/lightstring nodes, real ranges, family-renamed tags; golden round-trip tests | `doctor` passes on a representative airbase set (F-16 + a bomber + a heli + a SAM); Blender round-trip preserves tags |
| **M2 — Runtime core** | Tag map at load; `f4-anim` rig eval; renderer matrix application; **model-doctor panel** (tag list, sliders, toggles, time-scrub) | F-16 gear cycles correctly end-to-end driven *by hand* from the doctor panel: doors → legs → wheels, LOD-stable |
| **M3 — Sim wiring** | FM publishes full semantic state; rig spec drives channels; viewer shows real gear cycles on taxi/takeoff/land; landing lights follow gear | Scenario-player taxi demo shows correct gear choreography without any per-model code |
| **M4 — Lights & effects** | Blink waveforms (nav/strobe), AB plume + nozzle stages + vapor via switches/DOFs | Formation flight shows de-synced (but deterministic) strobes; AB toggles with rpm |
| **M5 — Damage & fx** | Broken-gear switch variants + clamps; death = model swap (class data); slot-attached particle effects spec (separate plan) | Gear-up landing with a broken-struck leg renders per FF semantics |

M0–M2 are the fix for "goofy gears"; M4 fixes "lights don't blink" and "effects locked"; M5 completes the FF surface feature set.

---

## 9. Implementation status (M0-M2, this patch series)

| Milestone | State | Where |
|---|---|---|
| M0 vocabulary | **done** | `f4-anim` — Channel enum (110 channels), name↔id, `process_dof_value` (Process_DOFRot port), gear sequencer, blink evaluator, `vocab/channels.json`; 32 unit tests |
| M1 converter | **done** | `f4-import/vocab/family/*.json` (complex/simple/heli/airdef tables from `dofsnswitches.h`), `emit_model_hierarchy()` behind `--hierarchy` (flat path default-on, byte-compatible), family classification + doctor-visible `unknown.N` degradation; 7 end-to-end tests incl. triangle parity with the flat path |
| M2 runtime | **done** | `f4-gltf` anim map + pure-math evaluator (11 tests), `f4-renderer` parts pipeline + animated draw with switch-bitmask visibility, `VisualModelComponent::anim_values`, world-viewer **Animation Doctor** section in the Class Table Browser (sliders, toggles, scripted gear cycle) |
| M3 sim wiring | **started** | Gear rig wired (per-tick `eval_gear` → `anim_values`, rest states return to authored pose) + **spinner pass** (rotor.main / rotor.tail / radar.dish_spin integrated per tick with per-entity phase; dormant airframes hold). Family classifier now discriminates heli vs ground radar via the rotor pair (dofs 2+4); AIRDEF sweep (index 0) bound to `radar.dish_spin`. Still open: remaining channels (control surfaces from FCS, lights), AWACS radome binding (SIMP_0 semantic unconfirmed — one vocab row once verified), ground-unit turret/elevation dofs |

Key FreeFalcon semantics locked by tests: switch values are BITMASKS
(`BSwitchNode::Draw` walks `mask >>= 1`; `BXSwitchNode` inverts), doors run
the first half of `gearPos` / legs the second, visibility hysteresis at 5°,
broken legs clamp at 0.6×range, nav blink 0.4/0.5 s, strobe 0.08/2.0 s with
seeded deterministic phase. The fixture F-16 (KoreaObj model 1) drives end to
end: converter → glTF → anim map → doctor sliders.

Two renderer-side conventions are also locked by tests (the M3 integration
found both the hard way — each displaced every DOF part while every test of
the pure math passed):

- **Rotation-op axis conjugation.** The falcon→glTF basis B is improper
  (det = −1): `B·Rx(θ)·Bᵀ = Rot(−B·x̂, θ)` — the emitted axis is glTF
  **+Z**, not `B·x̂` = −Z. `AnimatedPoseMatchesFlatBake` fails at 0.5 m on
  the flipped axis.
- **Raylib is row-vector, and part vertices live in raylib space.**
  `MatrixMultiply(A, B)` applies A first; `Vector3Transform` is `v·M`.
  The node matrix must read S·R·T (translation LAST — `RaylibMatrixConventionLocked`),
  the chain fold must PREPEND each local matrix (innermost frame touches
  the vertex first), and the composed chain must be conjugated through the
  glTF→raylib basis bridge `diag(kMetersToFeet, −kMetersToFeet,
  kMetersToFeet)` (part vertices are stored raylib-space, chain math is
  glTF-space meters). The render harness (`DIAG_Model1052_RenderRestPose`)
  shows any of this regressing on sight.

## 9. Testing strategy

- **Golden converter tests** — per model: node structure snapshot (names, extras, transforms), reparse equality, doctor-clean.
- **Rig unit tests** — gear sequencer timeline (t=0 doors start, t≈1.65 s legs start, t≈3.3 s planted), blink duty-cycle statistics over simulated minutes, determinism (same seed ⇒ identical waveform), broken-gear clamp.
- **Model-doctor panel** (world-viewer) — manual actuation of every tag; this doubles as the community-model audit tool.
- **ACMI conformance (later)** — play a FreeFalcon ACMI tape's DOF/switch records through the rig/tag path and diff channel traces; the formats are near-isomorphic by construction.

# No Binary Runtime — Eliminate KoreaObj + FALCON4.ct from the Runtime

> **Status**: Active plan. Supersedes the "pragmatic Data/ bridge" framing
> from the prior discussion. The user's architectural decision: **no legacy
> binary formats in the runtime, period.** The 38 MB KoreaObj binary
> (HDR/LOD/TEX) and FALCON4.ct must be converted to JSON/glTF intermediates;
> the runtime loads only those. This is `ASSET_PIPELINE_SPEC.md` P2 (link-time
> isolation) made real, not aspirational.
>
> **Predecessors**: `ASSET_PIPELINE_SPEC.md` (Draft v1 — design agreed, not
> implemented; this plan is the implementation tranche), RECON-3 + RECON-4
> (appended to worklog before this plan — every claim below is source-verified).
>
> **Companions**: `LANDING_PRECISION_FORMATION_AAR_PLAN.md` (Tranche A landed;
> Tranche B/C/D queued — this tranche unblocks their E2E verification).

---

## 1. The gap this closes

`ASSET_PIPELINE_SPEC.md` designs a runtime that never touches legacy binary
formats. RECON-3 + RECON-4 verified the implementation reality:

| Spec principle | Status | Evidence |
|---|---|---|
| P2 (link-time isolation) | **FAILING** | f4-renderer, f4-simulation, f4-world-viewer all PUBLIC-link `f4-models` + `f4-world-convert`. `verify_boundary.cmake` does not exist. `F4_SIDE` set on zero targets. |
| KoreaObj → glTF producer | **DONE** | `f4-import/src/gltf_emitter.cpp` (482 lines), `f4import models` CLI, 7 round-trip tests pass. Tags DOF/switch/slot/lod per spec §6. |
| f4-gltf runtime loader | **DONE, UNUSED** | `f4-gltf/src/gltf_loader.cpp` (511 lines, complete for the f4import subset). NO runtime target links it — only f4-import. |
| Runtime BSP/TEX parsing | **ACTIVE** | `f4-renderer/feature_mesh.cpp:84`, `render_resources.cpp:100`, `texture_cache.cpp:32` call `ModelDatabase::extract_model_geometry`/`fetch_texture` lazily. `VisualModelComponent` carries `const ModelRecord*` directly. |
| FALCON4.ct → JSON | **NOT STARTED** | `ClassTable::load()` (150 lines, 2135 entries × 81 bytes) is a complete decoder in a library. The viewer's `class_table_browser::export_json()` is a reference. No `ct2json` CLI. No `falcon4.ct.json`. 18 consumer call sites. |
| KoreaObj.TEX → PNG | **NOT STARTED** | TEX decoder works (1290/1290 decode). No PNG writer. No `f4import textures` subcommand. glTF emitter has a TODO for materials. |
| SimData → JSON | **DONE** | f4-data loads only JSON. f16.json + 8 simdata/*.json are build artifacts. No residual binary dependence. |
| `Data/` directory | **NOT STARTED** | Gitignored. Spec §4 layout designed. `export-game-data.sh` doesn't exist. |

**The producer half of the pipeline exists. The consumer half — the runtime
rewire to load JSON/glTF instead of parsing binary — is the work.** And one
new producer (TEX → PNG) is needed.

## 2. Tranches (ordered by gap size, smallest first)

### Tranche 0a — ct2json + JSON Data subset (the immediate E2E unblock)

**The forcing function**: `test_digi_mission` SKIPs without `F4_INSTALL`
because `korea_real.world.json` doesn't exist. The converters that produce
the JSON subset ALL exist (`cam2json --theater-data`, `terrain2json`,
`dat2json`, the 6 SimData converters). The ONE missing piece is `ct2json`.
Commit the JSON subset + the new `ct2json` output, and E2E runs anywhere.

**0a.1 — `ct2json` CLI.** New `f4-world-convert/cli/ct2json.cpp` (~100 lines,
clone of `cam2json.cpp`'s structure). Calls `ClassTable::load()` then emits
`falcon4.ct.json` (one JSON object per `ClassTableEntry`: entity_type,
domain, cls, type, stype, vis_type[7], data_type, data_ptr_index). Plus
`ClassTable::load_json()` counterpart (~40 lines) so consumers can switch
from `.ct` to `.json` paths. CMake: 2 lines mirroring `cam2json`'s
`add_executable`.

**0a.2 — `scripts/export-game-data.sh`.** Pulls the JSON subset through the
CLIs into `Data/`:

```
Data/
├── manifest.json              # source paths, hashes, converter versions
├── World/korea.world.json     # cam2json --theater-data <install>/terrdata/objects
├── Theater/korea/terrain.json # terrain2json
├── Aircraft/f16.json          # dat2json (the convert_golden_fixtures output)
├── SimData/                   # the 6 SimData converters from SimData.zip
│   ├── maneuvers.json
│   ├── brains.json
│   ├── formations.json
│   ├── vehicles.json
│   ├── sensors.json
│   └── signatures.json
└── Classes/falcon4.ct.json    # ct2json (0a.1)
```

No binary. The 38 MB KoreaObj and the .ct binary stay OUT of `Data/` — they
get their own tranches (0c, 0d).

**0a.3 — Consumer switch (class table).** The 18 call sites RECON-4 found
switch from `ClassTable::load(.ct)` to `ClassTable::load_json(.json)`. The
`ScenarioAirfield` / scenario templates change `@F4_DIGI_CLASS_TABLE@` from
a `.ct` path to a `.json` path. `test_digi_mission`'s `GTEST_SKIP()` at
line 87 (the "world JSON not generated" guard) is deleted — the file is
always committed.

**0a.4 — `.gitignore` inversion.** `Data/` is currently gitignored in full
(spec: "generated, not committed"). Invert for the JSON subset: `Data/`
is committed, `Data/Models/koreaobj/` (the future glTF+PNG) stays gitignored
until 0c/0d land. The manifest records provenance so staleness is detectable.

**Acceptance (§3).**

### Tranche 0b — CMake boundary enforcement (makes the goal enforceable)

> **Status: LANDED** (Task 52). `cmake/verify_boundary.cmake` exists and runs
> at configure time. `F4_SIDE` set on 15 importer + 28 runtime targets. The
> verifier reports the expected violations (f4-simulation, f4-renderer,
> f4-world-viewer + downstream) as WARNING by default, FATAL_ERROR with
> `-DF4_ENFORCE_BOUNDARY=ON`. Each violation turns green as 0d decouples it.

**0b.1 — `cmake/verify_boundary.cmake`.** Implements the spec's P2 test:
no runtime target may link a legacy-binary parser (`f4-models`,
`f4-world-convert`, `f4-terrain-convert`, `f4-lzss`). The `F4_SIDE` target
property marks importer-only targets; the verifier errors on any non-`F4_SIDE`
target linking a parser.

**0b.2 — Set `F4_SIDE` on importer targets.** `f4-import`, `f4-convert`,
`f4-world-convert` (the converter libraries), `f4-terrain-convert`. The
CLIs (`cam2json`, `ct2json`, `dat2json`, `f4import`, etc.) are `F4_SIDE`.

**0b.3 — Wire the verifier into CMake.** Runs at configure time. **Will
fail loudly today** (f4-renderer, f4-simulation, f4-world-viewer link
f4-models). Each binary decoupled (0c, 0d) turns a failure green. This is
the gate that makes "no binary runtime" a contract, not a hope.

**Acceptance (§4).**

### Tranche 0c — KoreaObj.TEX → PNG extractor (the one new producer)

**0c.1 — TEX → PNG.** New `f4-import/src/tex_extractor.cpp`. The TEX
decoder exists (`f4-models/src/texture_cache.cpp`); add a PNG writer
(libpng, or stb_image_write for zero-dependency). Output:
`Data/Models/koreaobj/textures/NNNN.png` (one per TEX entry, 1290 entries).

**0c.2 — `f4import textures` subcommand.** Wraps 0c.1. Mirrors `f4import
models`.

**0c.3 — glTF materials emission.** `f4-import/src/gltf_emitter.cpp`'s
TODO: emit glTF materials referencing the PNG textures (pbrMetallicRoughness
with baseColorTexture). The geometry is already emitted; this closes the
visual loop.

**Acceptance (§5).**

### Tranche 0d — Runtime glTF rewire (the big refactor)

**The work that makes P2 pass.** The runtime stops linking `f4-models`;
it loads glTF via `f4-gltf` instead.

**0d.1 — `VisualModelComponent` rewire.** Replace `const ModelRecord*`
with a glTF model handle (the `f4-gltf` loader's output type). Every
consumer of `ModelRecord*` switches.

**0d.2 — `f4-renderer` rewire.** `feature_mesh.cpp:84`,
`render_resources.cpp:100`, `texture_cache.cpp:32` stop calling
`ModelDatabase::extract_model_geometry`/`fetch_texture` at runtime. The
mesh + texture data comes from the glTF load (done once at spawn, not
lazily per frame).

**0d.3 — `f4-simulation` link-cut.** Drop `f4-models` from
`target_link_libraries(f4-simulation ...)`. The simulation doesn't render —
it only needs the model ID for spawn resolution, which the class table
JSON (0a) provides. `VisualModelComponent` becomes a glTF handle the
renderer reads; the sim doesn't touch it.

**0d.4 — `f4-world-viewer` + `f4-scenario-player` link-cut.** The GUI apps
link `f4-gltf` + `f4-renderer` (rewired). The Hex Inspector's KoreaObj
decoder stays (it's a dev tool, `F4_SIDE`-exempt — it reads binary for
reverse-engineering, not runtime).

**0d.5 — `temp/KoreaObj.*` removal.** The 38 MB committed binary is
deleted. The gitignore exception is removed. The repo drops to ~76 MB
(114 MB - 38 MB).

**Acceptance (§6).**

### Tranche 0e — Full Data/ export (the destination)

**0e.1 — `export-game-data.sh` extended.** Now exports the full `Data/`
tree per spec §4: JSON subset (0a) + glTF models (0d, via `f4import models`)
+ PNG textures (0c, via `f4import textures`) + the manifest. No legacy
binary anywhere in `Data/`.

**0e.2 — Scenario portability.** Scenarios reference `@asset:` logical IDs
per spec §5 (e.g. `"@asset:theater:korea"`), not absolute paths. The
`@F4_BINARY_DIR@`/`@F4_INSTALL@` configure-time coupling is gone.

**0e.3 — `f4-assets` runtime resolver.** The spec §8 library that resolves
`@asset:` IDs to `Data/` paths. Consumers ask for an asset by ID; the
resolver finds it, checks the manifest hash, and returns the path.

**Acceptance (§7).**

---

## 3. Acceptance criteria — Tranche 0a (JSON Data subset + ct2json)

1. `ct2json` CLI builds and runs: `ct2json FALCON4.ct falcon4.ct.json`
   produces valid JSON. Round-trip: `ClassTable::load_json(falcon4.ct.json)`
   produces the same `ClassTableEntry` values as `ClassTable::load(FALCON4.ct)`
   for all 2135 entries.
2. `scripts/export-game-data.sh --install <path> --output Data/` produces
   the 8 JSON files (world, terrain, aircraft, 6 simdata) + falcon4.ct.json
   + manifest.json. No binary files in `Data/`.
3. The 18 class-table consumer call sites load `falcon4.ct.json`, not
   `FALCON4.ct`. No `ClassTable::load(.ct)` call remains in non-importer
   code.
4. `test_digi_mission`'s `GTEST_SKIP()` at line 87 is deleted. The test
   runs (and passes, with Tranche A's tightened tolerances) against the
   committed `Data/` — no `F4_INSTALL` required.
5. `Data/` is committed (the gitignore inversion). `Data/Models/koreaobj/`
   stays gitignored (pending 0c/0d).
6. No regressions in the existing test suite (the class-table consumer
   switch is behavior-preserving — same data, different format).

## 4. Acceptance criteria — Tranche 0b (CMake boundary)

1. `cmake/verify_boundary.cmake` exists and runs at configure time.
2. `F4_SIDE` is set on every importer/converter target.
3. The verifier FAILS at configure time today (documenting the P2
   violations: f4-renderer, f4-simulation, f4-world-viewer link f4-models).
4. After 0d lands, the verifier PASSES — no non-`F4_SIDE` target links a
   legacy-binary parser.

## 5. Acceptance criteria — Tranche 0c (TEX → PNG)

1. `f4import textures <install>/terrdata/objects/KoreaObj.TEX <output_dir>`
   produces 1290 PNG files.
2. `f4import models` now emits glTF materials referencing the PNG textures
   (the TODO in `gltf_emitter.cpp` is resolved).
3. A rendered model in `f4-models-viewer` shows textured geometry (visual
   verification — user's env, no X11 here).

## 6. Acceptance criteria — Tranche 0d (runtime glTF rewire)

1. `VisualModelComponent` carries a glTF handle, not `const ModelRecord*`.
2. `f4-renderer`, `f4-simulation`, `f4-world-viewer` no longer link
   `f4-models`. The CMake boundary verifier (0b) passes.
3. `f4-gltf` is linked by the runtime targets instead.
4. `temp/KoreaObj.HDR/.LOD/.TEX` is deleted from the repo. The gitignore
   exception is removed.
5. The scenario player and world viewer render correctly against
   `Data/Models/koreaobj/*.gltf` (user verification — no X11 here).
6. No regressions in the headless test suite (combat chain, campaign loop,
   campaign_qc MD5 certificates byte-identical — the sim doesn't render).

## 7. Acceptance criteria — Tranche 0e (full Data/ export)

1. `export-game-data.sh` produces the full spec §4 `Data/` tree: JSON
   subset + glTF models + PNG textures + manifest. No legacy binary.
2. Scenario JSONs reference `@asset:` logical IDs, not absolute paths.
3. `f4-assets` resolves `@asset:` IDs to `Data/` paths with manifest hash
   verification.
4. A fresh clone + `cmake -B build && ninja -C build` + `ctest` runs the
   full suite (including E2E) with no `F4_INSTALL` and no manual data
   generation. The data is in the repo.

---

## 8. What does NOT change

- The aircraft entity component set and the aircraft-binding design.
- The two-pass ECS tick contract.
- The combat chain, the campaign loop, the campaign_qc MD5 certificates.
  (0a's class-table switch is behavior-preserving; 0d's glTF rewire is
  rendering-only — the sim doesn't render.)
- The `f4-state-machine`, `f4-messaging`, `f4-entities`, `f4-geo`,
  `f4-math`, `f4-units`, `f4-json`, `f4-io` libraries. None touch binary
  formats.
- The Hex Inspector's binary decoders (in f4-world-viewer) — they're a dev
  tool for reverse-engineering, `F4_SIDE`-exempt. They read binary by
  design; they're not the runtime path.

## 9. Out of scope (deferred)

- **BMS theater support**: the spec mentions `Models/bms/…`. Korea-only
  for now; BMS lands when a BMS install is available for testing.
- **Multiple campaign saves**: `save1.cam` only. The script supports
  `--save`; enumerating all saves in an install is a follow-up.
- **Every aircraft `.dat`**: f16 only (the scenario-player need). The other
  23 `.dat` files land when their consumers exist.
- **Content-hash manifest verification at runtime**: the manifest is
  recorded (0a.2) but runtime hash-checking (spec P7's full enforcement)
  lands with `f4-assets` (0e.3). For now the manifest is provenance-only.

## 10. Implementation order

1. **0a.1** — `ct2json` CLI + `ClassTable::load_json()`. (Half a day. The
   decoder exists; this is a thin CLI + a JSON loader counterpart.)
2. **0a.2** — `scripts/export-game-data.sh` + `scripts/generate_manifest.py`.
   (Half a day. The converters all exist; this is orchestration.)
3. **0a.3** — class-table consumer switch (18 sites). (Half a day.
   Mechanical, behavior-preserving.)
4. **0a.4** — `.gitignore` inversion + commit `Data/` (JSON subset). The
   user runs the script, commits, pushes. From then on, E2E runs anywhere.
5. **0a patch + ship.** ← **This unblocks Tranche A E2E + Tranche B.**
6. **0b** — CMake boundary verifier. (Half a day. Will fail today; turns
   green per 0c/0d.)
7. **0c** — TEX → PNG extractor + glTF materials. (1–2 days. The one new
   producer.)
8. **0d** — runtime glTF rewire. (3–5 days. The big refactor —
   VisualModelComponent, f4-renderer, the link-cut. The boundary verifier
   turns green here.)
9. **0e** — full Data/ export + `@asset:` IDs + `f4-assets` resolver.
   (1–2 days. The destination.)

---

*This document implements `ASSET_PIPELINE_SPEC.md`'s P2 (link-time
isolation) as a sequenced tranche, not a design. RECON-3 + RECON-4 (appended
to worklog) verified the producer-side tooling exists; the consumer-side
rewire is the work. Tranche 0a is the immediate E2E unblock — everything
after it is the path to "no binary in the runtime, enforced."*

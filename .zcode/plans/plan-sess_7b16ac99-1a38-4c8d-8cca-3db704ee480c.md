## Goal

Make "set Falcon install → open .cam" work end-to-end in f4-world-viewer under the new no-binary-runtime pipeline: use existing `Data/` exports when present; convert whatever is missing **into `Data/Temp/`** (never overwriting canonical `Data/`); class table, terrain, world JSON parse fast; 3D models/textures auto-convert via f4import into Temp on first use (cached afterwards).

Root cause of the error you just hit, for the record: `terrain2json.exe` had never been built — building f4-world-viewer doesn't build the converter CLIs it shells out to, and `cmd.exe` returns exit 1 ("The system cannot find the path specified") for the missing exe. Two plan items fix that class of failure permanently.

## Changes

### 1. New helper module `f4-world-viewer/{include/f4/viewer/pipeline_io.hpp, src/pipeline_io.cpp}`
- `discover_data_dir()` — consolidate the duplicated `AssetRoot::discover()` / `F4_SOURCE_DIR/Data` logic (currently in `class_table_browser.cpp:156-164` and `ground_layout_3d.cpp:156-172`).
- `run_converter(exe, args, &captured_output)` — replaces `std::system`: `CreateProcess` with `CREATE_NO_WINDOW` on Windows (no console flash, no forward-slash cmd.exe quirks), output captured via pipes; `popen` fallback elsewhere. On failure the captured stderr is included in the thrown message so the "Campaign Load Failed" modal shows the real converter error.
- "Data first, else Temp" resolvers, each returning the path and converting on miss:
  - `ensure_class_table_json(install)` → `Data/Classes/falcon4.ct.json` if present; else run `ct2json <install FALCON4.ct> Data/Temp/Classes/falcon4.ct.json`.
  - `ensure_terrain_json(theater_key, theater_dir)` → `Data/Theater/<key>/terrain.json` if present; else `terrain2json <theater_dir> Data/Temp/Theater/<key>/terrain.json`.
  - `ensure_world_json(cam, stem, theater_key, objects_dir)` → candidates: manifest `campaign:<stem>` (via f4-assets, already linked), `Data/World/<stem>.world.json`, `Data/World/<theater_key>.world.json`; else `cam2json <cam> Data/Temp/World/<stem>.world.json --theater-data <objects_dir> --class-table <binary .ct if found>`.
  - `ensure_models_root(install)` → `Data/Models/koreaobj` if present; else run `f4import textures --all` + `f4import models --all --install <root> --data Data/Temp` and return `Data/Temp/Models/koreaobj`.

### 2. CMake — `f4-world-viewer/CMakeLists.txt:157-165`
- Add `F4_CT2JSON_EXE="$<TARGET_FILE:ct2json>"` and `F4_F4IMPORT_EXE="$<TARGET_FILE:f4import>"` defines.
- `add_dependencies(f4_world_viewer cam2json terrain2json ct2json f4import)` so building the viewer always builds/freshens the converter exes (permanent fix for the exit-1-missing-exe failure).

### 3. Rewire `load_campaign_from_install` (`install_flow.cpp:246-334`)
- Step order: class table → terrain JSON → (unchanged) `WorldView::load_theater` tiles → cam2json world JSON → models/textures. Each step uses the pipeline_io resolvers; status messages say whether the artifact came from `Data/` or was freshly converted into `Data/Temp/`.
- The multi-minute f4import models/textures step runs on a background `std::thread` with progress/status polled in the draw loop (new `Impl` fields: mutex-guarded progress string + done flag; no ImGui calls from the worker). On completion: `set_model_data_dir(Temp root)`, reset the `models_3d_load_attempted` retry flag, status "3D models ready". This avoids the app ghosting as "Not Responding" during first-time conversion.

### 4. Manual import paths (`file_ops.cpp:169-220`)
- `import_cam_archive`: output goes to `Data/Temp/World/<stem>.world.json` instead of next to the .cam (install dirs can be read-only).
- `import_terrain_binary`: output to `Data/Temp/Theater/<dirname>/terrain.json`.
- Both switch to `run_converter` and surface captured stderr on failure.

### 5. Fix broken campaign-session class table (`campaign_session_view.cpp:99-113`)
- Today it hands f4-simulation a **binary .ct path**, but runtime `ClassTable::load_auto` is JSON-only, so sessions always fail to load. Pass the resolved JSON path from `ensure_class_table_json()` instead.

### 6. Shared discovery in `class_table_browser.cpp:149-183` and `ground_layout_3d.cpp:147-199`
- Replace their private data-dir discovery with `pipeline_io::discover_data_dir()`; ground layout 3D accepts the Temp models root set by step 3.

### 7. Housekeeping
- `.gitignore`: explicit `Data/Temp/` entry (likely already covered by the `Data/**` block; make it self-documenting). Temp conversions do **not** touch `Data/manifest.json`.
- Short `CHANGES.md` entry documenting the Temp flow.

## Explicitly unchanged
- `WorldView::load_theater` (THEATER.L*/O*, TEXTURE.BIN, FArtILES.*) keeps reading binaries via f4-terrain — deferred per your answer.
- Canonical `Data/` files are never written by the viewer; only `Data/Temp/` gets new files.
- `WorldView` binary tiles: excluded. Hex Inspector/snapshot diagnostics stay binary-reading (intentional tooling).

## Verification
1. Full Debug rebuild of f4-world-viewer (now also builds cam2json/terrain2json/ct2json/f4import via the new dependency).
2. Run each converter command exactly as the new code generates it, against your install (the one referenced in `Data/manifest.json`), writing into `Data/Temp/`, and confirm outputs load (world JSON parses, terrain JSON parses, ct JSON loads via `load_auto`).
3. Delete nothing from canonical `Data/`; confirm the viewer prefers existing `Data/Theater/korea/terrain.json`, `Data/World/korea.world.json`, `Data/Classes/falcon4.ct.json`.
4. Final GUI smoke test (open campaign from install) needs you at the wheel — I'll hand off with exact steps.
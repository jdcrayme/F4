# Tranche 0d (renderer half) — Runtime glTF Rewire: Implementation Plan

> **Status**: Design plan — for execution in a GL-enabled environment.
> **Predecessor**: Tranche 0d simulation half (Task 54) — LANDED. The
> `f4-world-convert` link is cut from `f4-simulation`; the remaining
> boundary violations are `f4-models` + `f4-lzss`, both via
> `VisualModelComponent::model_record` (a `const ModelRecord*` resolved
> from `Simulation`'s `ModelDatabase`).
> **Goal**: Cut `f4-models` + `f4-lzss` from the runtime link closure.
> The renderer loads glTF + PNG (via `f4-gltf`) instead of parsing
> KoreaObj binary (via `f4-models`). `temp/KoreaObj.{HDR,LOD,TEX}` (38 MB)
> leaves the repo.

---

## 1. The remaining violations (the work to turn green)

The 0b boundary verifier reports (after the 0d simulation half):

| Target | Direct | Transitive |
|--------|--------|------------|
| `f4-simulation` | `f4-models` | `f4-lzss` |
| `f4-renderer` | `f4-models`, `f4-world-convert`* | — |
| `f4-world-viewer` | `f4-models`, `f4-world-convert`*, `f4-terrain-convert` | — |
| `f4-scenario-player` | — | (via `f4-simulation`, `f4-renderer`) |
| `trace_runner` | — | `f4-models`, `f4-lzss` |
| `campaign_qc` | — | `f4-models`, `f4-lzss` |

*\*Note: `f4-renderer` + `f4-world-viewer` still link `f4-world-convert`
for `ClassTable`. The 0d simulation half moved the runtime-safe
`ClassTable` to `f4-world-types`, but the renderer hasn't been switched
yet — that's part of this plan.*

After this plan lands, all six targets link neither `f4-models` nor
`f4-world-convert` nor `f4-lzss` nor `f4-terrain-convert`. The verifier
passes clean.

---

## 2. The four sub-tasks

### 2.1 `VisualModelComponent` rewire — `ModelRecord*` → vis_type identity

**Today** (`f4-simulation/include/f4/simulation/visual_model_component.hpp`):
```cpp
struct VisualModelComponent : entities::Component<VisualModelComponent> {
    const f4::models::ModelRecord* model_record{nullptr};  // ← the f4-models dep
    int16_t vis_type{0};       // already the identity (V-3DLIVE)
    int active_lod{0};
    f4::models::ModelState model_state{};  // ← also f4-models (SwitchState, DofState)
    int texture_set{0};
};
```

**After**: `model_record` removed. `vis_type` is the sole renderable
identity. `model_state` (DOF/switch animation) moves to a runtime-local
type in `f4-renderer` (the renderer owns the glTF scene graph; DOF/switch
state is a renderer concern, not a simulation concern). The simulation
sets `vis_type` at spawn and never touches `f4-models`.

```cpp
// f4-simulation/include/f4/simulation/visual_model_component.hpp (after)
struct VisualModelComponent : entities::Component<VisualModelComponent> {
    int16_t vis_type{0};       // the renderable identity (FALCON4.CT visType[0])
    int active_lod{0};
    int texture_set{0};
    // Gear switch (the only DOF/switch the sim animates): 0=down, 1=up.
    // The renderer maps this to the glTF switch node's child selection.
    uint8_t gear_switch_child{0};
};
```

**Migration**: ~20 call sites in `f4-simulation/src/simulation.cpp` +
`campaign_bridge.cpp` + `campaign_spawner.cpp` set `vis.model_record =
db.model(vis_type)`. Delete those lines (the db lookup is gone). The
`SwitchState gear_switch` construction (simulation.cpp:547) becomes a
simple `vis.gear_switch_child = 0`.

**Test impact**: `test_simulation_lifetime` + `test_feature_spawning`
assert `model_record != nullptr`. Update them to assert `vis_type != 0`
instead (the identity is vis_type now, not the pointer).

### 2.2 `f4-renderer` rewire — glTF geometry + PNG texture pipeline

**Today** the renderer's geometry pipeline (`render_resources.cpp`,
`feature_mesh.cpp`, `texture_cache.cpp`, `mesh_builder.cpp`) calls:
- `ModelDatabase::parse_lod` + `extract_model_geometry` → `ModelGeometry`
- `ModelDatabase::color_bank()` → vertex color resolution
- `ModelDatabase::fetch_texture(tex_id)` → `DecodedTexture` (RGBA)
- `build_raylib_meshes(geom, color_bank, transform)` → `vector<::Mesh>`

**After**: a new `RuntimeModelCache` (in `f4-renderer`) loads glTF by
vis_type, builds Raylib meshes from glTF accessors, and loads PNG
textures by URI. No `ModelDatabase` calls.

```cpp
// f4-renderer/include/f4/renderer/runtime_model_cache.hpp (new)
namespace f4::renderer {

/// One loaded glTF model: the GltfDocument + pre-built Raylib meshes
/// per LOD + the PNG textures it references.
struct RuntimeModel {
    std::shared_ptr<f4::gltf::GltfDocument> doc;
    std::vector<MeshEntry> lod0_meshes;  // one MeshEntry per glTF primitive
    bool built = false;
};

/// Loads glTF models by vis_type from the Data/Models/koreaobj/ tree.
/// Caches one RuntimeModel per vis_type (one GPU upload per unique model).
/// Replaces RenderResources::build_mesh_for_model's ModelDatabase path.
class RuntimeModelCache {
public:
    /// Set the Data/ directory (where Models/koreaobj/<NNNNN>.gltf lives).
    void set_data_dir(const std::filesystem::path& data_dir);

    /// Lazily load + cache the glTF model for a vis_type. No-op if cached.
    /// Requires GL context (UploadMesh). Sets built=true even on failure.
    void build_model(int vis_type);

    /// Look up a cached model. nullptr if not built or build failed.
    [[nodiscard]] const RuntimeModel* lookup(int vis_type) const;

private:
    std::filesystem::path data_dir_;
    std::unordered_map<int, RuntimeModel> cache_;
};

} // namespace f4::renderer
```

**The glTF → Raylib mesh conversion** (replaces `build_raylib_meshes`):
iterate `doc.meshes[*].primitives[*]`, for each primitive:
- POSITION accessor → `mesh.vertices` (via `doc.read_vec3_float`)
- NORMAL accessor → `mesh.normals`
- TEXCOORD_0 accessor → `mesh.texcoords`
- indices accessor → `mesh.indices` (via `doc.read_index_u32`)
- material index → `MeshEntry::tex_id` (mapped to a PNG texture load)

Coordinate transform: glTF is meters/Y-up; the model was exported with
the transform baked (`gltf_emitter.cpp` does feet→meters + Z-up→Y-up at
export). So the renderer's `model_vertex_to_raylib` becomes a no-op (or
a simple meters→feet scale if the sim stays in feet).

**PNG texture loading** (replaces `TextureCache::upload(db, tex_ids)`):
raylib's `LoadTexture` reads PNG directly. The glTF material's
`baseColorTexture` → image URI → `textures/NNNNN.png` → `LoadTexture`.
No `fetch_texture` / `DecodedTexture` / KoreaObj.TEX parsing.

**Files to change** (all in `f4-renderer/`):
| File | Change |
|------|--------|
| `include/f4/renderer/runtime_model_cache.hpp` | NEW — the glTF cache |
| `src/runtime_model_cache.cpp` | NEW — glTF load + mesh build |
| `include/f4/renderer/render_resources.hpp` | Add `RuntimeModelCache model_cache`; deprecate `build_mesh_for_model(db, ...)` |
| `src/render_resources.cpp` | `build_mesh_for_model` delegates to `model_cache.build_model` |
| `include/f4/renderer/feature_mesh.hpp` | Drop `ModelDatabase*` + `ClassTable*` from `FeatureMeshResources`; add `RuntimeModelCache*` |
| `src/feature_mesh.cpp` | `draw_vis_type_mesh` uses `model_cache.lookup(vis_type)` |
| `include/f4/renderer/texture_cache.hpp` | Drop `ModelDatabase&` from `upload()`; add `upload_png(path)` |
| `src/texture_cache.cpp` | PNG path via `LoadTexture` |
| `include/f4/renderer/mesh_builder.hpp` | Add `build_raylib_meshes_from_gltf(doc, mesh_idx)`; keep the ModelGeometry overload for the legacy path |
| `src/mesh_builder.cpp` | glTF accessor → Raylib Mesh conversion |
| `CMakeLists.txt` | Drop `f4-models` + `f4-world-convert` from `target_link_libraries`; add `f4-gltf` + `f4-world-types` |

### 2.3 Link-cut — drop `f4-models` + `f4-lzss` + `f4-world-convert` from runtime

After 2.1 + 2.2, the runtime no longer calls any `f4-models` /
`f4-world-convert` function. Cut the links:

| Target | Drop | Add |
|--------|------|-----|
| `f4-simulation` | `f4-models` | (nothing — vis_type is the identity) |
| `f4-renderer` | `f4-models`, `f4-world-convert` | `f4-gltf`, `f4-world-types` |
| `f4-world-viewer` | `f4-models`, `f4-world-convert`, `f4-terrain-convert` | `f4-gltf`, `f4-world-types` |
| `f4-scenario-player` | (transitive — drops automatically when f4-simulation + f4-renderer drop) | |

**`Simulation::model_db_` removal**: the `unique_ptr<ModelDatabase>`
member + `load_models()` + `model_db()` accessor all go. The scenario's
`models_hdr_path` / `models_lod_path` / `models_tex_path` fields become
 vestigial (scenarios reference `@asset:` IDs instead — already
 supported by the 0d simulation half).

### 2.4 `temp/KoreaObj.{HDR,LOD,TEX}` deletion + scenario migration

- Delete `temp/KoreaObj.HDR`, `temp/KoreaObj.LOD`, `temp/KoreaObj.TEX`
  (38 MB).
- Update `.gitignore`: remove the `!temp/KoreaObj.*` exception lines.
- Update all 17 scenario templates in
  `f4-scenario-player/scenarios/*.json.in`: replace
  `"models_hdr_path": "@F4_SOURCE_DIR@/temp/KoreaObj.HDR"` etc. with
  `"models_hdr_path": "@asset:koreaobj:models"` (a single asset ID
  that resolves to the Data/Models/koreaobj/ tree).
- Update `f4-world-viewer/src/viewer_state.hpp`: the lazy
  `model_db_3d` optional + `load_koreaobj()` path becomes a
  `RuntimeModelCache` configured from `Data/`.
- The Hex Inspector in `f4-world-viewer` (the KoreaObj binary decoder
  dev tool) stays — it's `F4_SIDE=importer`-exempt (reads binary for
  reverse-engineering, not runtime).

---

## 3. Test impact + migration

### Headless tests that assert on `model_record`

| Test | Current assertion | After 0d |
|------|-------------------|----------|
| `test_simulation_lifetime.cpp:323` | `EXPECT_NE(vis->model_record, nullptr)` | `EXPECT_NE(vis->vis_type, 0)` |
| `test_feature_spawning.cpp:173` | `EXPECT_NE(model_record, nullptr)` | `EXPECT_NE(vis_type, 0)` |
| `test_campaign_bridge.cpp:450` | `EXPECT_EQ(model_record, nullptr)` (db empty) | (delete — vis_type is always set) |
| `test_visual_model_component.cpp` | multiple `model_record` checks | rewrite for vis_type |

### Renderer tests (need GL)

The `f4-renderer` tests (`test_mesh_builder`, `test_texture_cache`,
`test_world_renderer`, etc.) currently link `f4-models` for
`ModelGeometry` / `ColorBank` / `DecodedTexture`. After 2.2, they link
`f4-gltf` instead and construct test fixtures from glTF JSON strings
(the `GltfDocument::load_from_string` path). This is a test rewrite,
not a behavior change.

### Visual verification (user env)

- `f4-models-viewer` renders a textured F-16 against
  `Data/Models/koreaobj/00002.gltf` + `textures/*.png` (0c verified the
  producer side; 0d verifies the consumer side renders).
- `f4-scenario-player takeoff_only.json` shows the F-16 taxiing with
  textured geometry (no `temp/KoreaObj.*` loaded).
- `f4-world-viewer` "Start Session" renders campaign aircraft with
  real models (vis_type → glTF cache).

---

## 4. Implementation order (estimated 3-5 days, GL-enabled env)

1. **2.2 RuntimeModelCache** (1.5 days) — the new glTF→Raylib pipeline.
   Build + unit-test headlessly (the cache loads glTF + builds meshes
   via `GltfDocument::load`; raylib `UploadMesh` needs GL but the
   geometry extraction doesn't). Verify against the 0c-produced
   `Data/Models/koreaobj/00002.gltf`.
2. **2.1 VisualModelComponent rewire** (0.5 day) — remove `model_record`,
   update `Simulation` spawn paths. Headless tests updated.
3. **2.3 Link-cut** (0.5 day) — drop `f4-models` + `f4-lzss` +
   `f4-world-convert` from runtime CMakeLists. Boundary verifier turns
   green.
4. **Renderer migration** (1 day) — switch `render_resources` /
   `feature_mesh` / `texture_cache` / `mesh_builder` to the
   `RuntimeModelCache` path. Visual verification in the viewers.
5. **2.4 temp/ deletion + scenario migration** (0.5 day) — delete the
   38 MB binary, update scenario templates to `@asset:` IDs.
6. **Full visual QA** (0.5 day) — scenario-player + world-viewer +
   models-viewer all render textured geometry from glTF+PNG.

---

## 5. Acceptance criteria (NO_BINARY_RUNTIME_PLAN.md §6)

1. `VisualModelComponent` carries `vis_type` (+ `gear_switch_child`),
   not `const ModelRecord*`. ✅ when 2.1 lands.
2. `f4-renderer`, `f4-simulation`, `f4-world-viewer` no longer link
   `f4-models`. The CMake boundary verifier passes clean. ✅ when 2.3 lands.
3. `f4-gltf` is linked by the runtime targets instead. ✅ when 2.3 lands.
4. `temp/KoreaObj.HDR/.LOD/.TEX` is deleted; `.gitignore` exception
   removed. ✅ when 2.4 lands.
5. Scenario player + world viewer render correctly against
   `Data/Models/koreaobj/*.gltf`. ✅ after visual QA (user env).
6. No regressions in the headless test suite (campaign chain, campaign
   loop, campaign_qc MD5 certificates byte-identical — the sim doesn't
   render). ✅ when 2.1-2.3 land.

---

*This plan is the renderer half of Tranche 0d. The simulation half
(Task 54) already cut `f4-world-convert` from the runtime; this plan
cuts `f4-models` + `f4-lzss`. Together they complete 0d — the runtime
loads only glTF + PNG + JSON, never legacy binary.*

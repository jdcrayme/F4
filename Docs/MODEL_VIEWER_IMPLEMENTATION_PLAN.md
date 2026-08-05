# F4 3D Model Viewer & Exporter Implementation Plan — f4-models + f4-models-viewer

> **Status**: Draft — For implementation reference
> **Source of Truth**: [FreeFalcon/freefalcon-central](https://github.com/FreeFalcon/freefalcon-central) (develop branch)
> **Companions**: [Architecture Proposal](ARCHITECTURE%20PROPOSAL.md) §15, [Falcon4 File Layout](FALCON4_FILE_LAYOUT.md), [f4-world-viewer source](../f4-world-viewer/)
> **Predecessor Lessons**: f4-world-viewer REFACTOR-1..5 god-file split — see §1.5

---

## Table of Contents

- [1. Goals & Non-Goals](#1-goals--non-goals)
- [2. Sequencing Rationale](#2-sequencing-rationale)
- [3. FreeFalcon Reference — 3D Model Pipeline Anatomy](#3-freefalcon-reference--3d-model-pipeline-anatomy)
- [4. Architecture Overview](#4-architecture-overview)
- [5. f4-models — Engine-Agnostic Model Library](#5-f4-models--engine-agnostic-model-library)
- [6. f4-models-viewer — Interactive 3D Viewer](#6-f4-models-viewer--interactive-3d-viewer)
- [7. glTF 2.0 Exporter](#7-gltf-20-exporter)
- [8. Implementation Steps](#8-implementation-steps)
- [9. FreeFalcon Validation Mapping](#9-freefalcon-validation-mapping)
- [10. Testing Strategy](#10-testing-strategy)
- [11. Observability & Tracing](#11-observability--tracing)
- [12. Hex Inspector Integration](#12-hex-inspector-integration)
- [13. Directory Layout & Build](#13-directory-layout--build)
- [14. Risks & Mitigations](#14-risks--mitigations)

---

## 1. Goals & Non-Goals

### Goals

1. **Engine-agnostic model parsing**: A new `f4-models` library parses FreeFalcon's `KoreaObj.Dxh` / `KoreaObj.Dxl` / `KoreaObj.Tex` trio into a typed in-memory `ModelDocument`. No rendering, no Raylib, no OpenGL. Testable from a unit test binary with nothing but a fixture file.

2. **Interactive 3D viewer**: A new `f4-models-viewer` subproject renders the parsed `ModelDocument` with Raylib `Mode3D` + Dear ImGui. The viewer exposes every runtime feature the original engine exposes: DOF sliders, switch bitmask toggles, texture-set picker, slot-child attachment, LOD switching, bounding-volume display.

3. **glTF 2.0 export**: A CLI tool (`dxh2gltf`) converts a `ModelDocument` to a standard `.glb` / `.gltf` file consumable by Blender, Three.js, Unreal, Unity, glTF Validator. Maps the BSP node tree to glTF scene nodes, DOFs to TRS animation channels, texture sets to materials, slots to node hierarchy children.

4. **Validated against FreeFalcon**: Every binary struct field in the parser has a one-to-one mapping to a FreeFalcon source file (e.g. `ParentFileRecord` ← `src/graphics/include/objectparent.h`). Every viewer feature maps to a FreeFalcon runtime API (e.g. `SetDOFangle` ← `DrawableBSP::SetDOFangle`).

5. **Mirror f4-world-viewer conventions**: Same pimpl `ViewerApp`, same `viewer_state.hpp` private header pattern, same `enum_text.hpp` inline decoders, same procedural symbol drawing where applicable, same `--screenshot` headless smoke test, same install-aware flow via `f4-install`.

### Non-Goals

1. **No legacy `.LOD` + `.HDR` support (for now)**: The pre-DX-engine path (`graphics/texture/objectlod.cpp`) is functionally equivalent to `.DXH`/`.DXL` with a different on-disk layout. Out of scope until a vanilla 1998 install becomes a real test target. Adding it later is a thin adapter at the file-reader layer.

2. **No `.DDS` textures (for now)**: Some mod packs ship `<basename>\<id>.dds` files alongside `KoreaObj.Tex`. The LZSS-paletted `.TEX` path is the canonical FreeFalcon default. DDS support is a future feature flag on `TextureBank` and requires a DXT1/3/5 decoder.

3. **No `.FLT` (OpenFlight) authoring path**: The original `bspbuild.exe` tool imports MultiGen `.FLT` files and emits the `.DXH`/`.DXL`/`.TEX` trio. Replicating it requires the proprietary MultiGen SDK or a from-scratch FLT reader. Out of scope.

4. **No round-trip re-encoding**: Writing `.DXH`/`.DXL`/`.TEX` from a `ModelDocument` (or from glTF) is out of scope. The writer interface (`ModelDocument → bytes`) can be stubbed for future work, but no implementation ships in this plan. Reason: re-encoding requires reimplementing `BNode` serialization, `BSplitterNode` tree construction (BSP partitioning), and FLT-equivalent comment-tag metadata (DARK/PERS/VERT/DYNA/TEXT/ANIM).

5. **No runtime simulation integration**: The viewer renders static snapshots of models. It does not consume `EntityWorld` from f4-entities, nor does it plug into a future f4-simulation frame loop. That integration is a separate consumer of `f4-models`.

6. **No multiplayer / cockpit / avionics**: This is asset inspection and conversion, not a playable game.

### 1.1 Decisions Locked In (from clarification round)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Format scope | `.DXH` + `.DXL` + `.TEX` only | Active FreeFalcon runtime path; covers FF, FF5, BMS-vanilla hybrid installs |
| Priority | Viewer first, then exporter | Visual validation of parser correctness before committing to a serialization format |
| Export targets | glTF 2.0 only | Industry-standard PBR format; works in Blender/Three.js/Unreal/Unity |
| Round-trip | Out of scope | Re-encoder needs full bspbuild reimplementation; defer until viewer/exporter is stable |
| Viewer layout | Standalone sibling subproject | Clean separation; can be built independently of f4-world-viewer |
| Plan format | Markdown in `Docs/` | Matches existing `AI_IMPLEMENTATION_PLAN.md` style; committable to the repo |

### 1.2 Success Criteria

The plan is complete when all of the following hold:

1. `cmake --build build --target f4-models-tests` passes ≥ 60 unit tests covering struct parsing, LZSS decode, XOR decrypt, BNode tree restoration, texture decode.
2. `cmake --build build --target f4-models-viewer` produces a runnable binary that loads a real `KoreaObj.Dxh` + `.Dxl` + `.Tex` from a FreeFalcon install, renders at least one parent's highest-detail LOD with textures, and lets the user toggle at least one switch and rotate one DOF.
3. `cmake --build build --target dxh2gltf` produces a CLI that converts a parent (by index) from the same fixture to a `.glb` file that passes `glTF-Validator` with zero errors and ≤ 5 warnings.
4. The `--screenshot` flag in `f4-models-viewer` produces a PNG under Xvfb that, when VLM-inspected, shows the expected aircraft silhouette (F-16, F-15, or whichever parent the smoke test targets).
5. Every public class in `f4-models` has a doc comment referencing the FreeFalcon source file/struct it replicates.

### 1.3 Predecessor Lessons (f4-world-viewer REFACTOR-1..5)

The world-viewer went through five refactor stages to escape the god-file trap. The model viewer inherits these lessons from day one:

| Lesson | World-Viewer Remedy | Model-Viewer Enforcement |
|--------|---------------------|--------------------------|
| One 1,500-line `viewer_app.cpp` is unmaintainable | Split into `viewer_app.cpp` (lifecycle) + `canvas.cpp` (render) + `imgui_panels.cpp` (UI) + `inspector_panel.cpp` (selection) + `file_ops.cpp` (I/O) + `install_flow.cpp` | §6.1 prescribes the same split up front; no `viewer_app.cpp` may exceed 300 LoC |
| Per-frame `DrawRectangleRec` for 16k terrain tiles killed FPS | Cached `RenderTexture` blit via `DrawTexturePro` | §6.5 mandates a single mesh upload per LOD, never per-frame vertex submission |
| `std::string` lowercase needle allocation ran 160k/sec during search | Cached lowercase buffer | §6.4 mandates that hot-path strings (parent names, DOF labels) are pre-computed once at load time |
| Tests couldn't link raylib+imgui, so testable code was untested | Model/view split: `HexModel` + `decoders.cpp` tested in isolation, `HexInspector` panel smoke-tested via `--screenshot` | §10.1 mandates that `ModelDocument`, BNode walkers, mesh extractor, and glTF writer all compile and test-link without raylib |
| Headless smoke tests are flaky without an orchestrator | Detached `std::thread` calls `std::exit(0)` 3s after `--screenshot` writes the PNG | §10.3 reuses the same pattern verbatim |

---

## 2. Sequencing Rationale

```
Parse → Render → Inspect → Export
─────────────────────────────────────
M1   →  M2  →  V1  →  V2  →  E1
```

**Why this order specifically:**

1. **M1 (structs + raw readers) first** — without a typed `ModelDocument`, nothing else can be built. The struct definitions are the contract every downstream consumer codes against.

2. **M2 (BNode tree → mesh) before V1 (viewer)** — the viewer needs *something* to render. A mesh extractor that walks the BNode tree and emits a flat vertex/index buffer is the minimum viable renderable artifact. Textures can wait; solid-shaded geometry proves the tree walk works.

3. **V1 (viewer MVP) before V2 (viewer features)** — get a window on screen showing one aircraft before adding DOF/switch/slot inspectors. Visual feedback is the fastest way to catch parser bugs that pure unit tests miss (e.g. wrong coordinate handedness, swapped normals, off-by-one vertex index).

4. **V2 (full viewer) before E1 (exporter)** — the viewer's inspectors are the validation surface for the exporter. If you can't toggle a switch and see the result in the viewer, you can't trust that the exporter is correctly serializing that switch to a glTF animation channel.

5. **E1 (glTF exporter) last** — glTF is a fiddly format with strict validation. Implementing it against a known-good `ModelDocument` (validated by the viewer) is far cheaper than implementing it against an unvalidated parser and debugging both layers at once.

**What we explicitly do NOT sequence:**

- **No parallel viewer + exporter development.** They share the `ModelDocument` contract; developing them in parallel risks the contract drifting. Viewer first locks the contract; exporter then trivially consumes it.
- **No "stub the writer for future round-trip"** — the stub adds API surface that has to be maintained without providing value. When round-trip becomes a real goal, the stub can be added then with full knowledge of what the writer needs to produce.

---

## 3. FreeFalcon Reference — 3D Model Pipeline Anatomy

This section catalogs the exact FreeFalcon structures that `f4-models` must replicate. It is the validation ground truth. File paths are relative to `freefalcon-central/src/`.

### 3.1 The Three On-Disk Files

| File | Role | Reader |
|------|------|--------|
| `KoreaObj.Dxh` | Master header: version magic, color bank, palette bank, texture index, LOD index, parent-object records | `ObjectParent::SetupTable` (`graphics/bsplib/objectparent.cpp:64`) |
| `KoreaObj.Dxl` | Per-LOD binary BSP node trees + vertex pools + lights | `ObjectLOD::SetupTable` (`graphics/bsplib/objectlod.cpp:105`) |
| `KoreaObj.Tex` | LZSS-compressed paletted texture blobs (8-bit, indexed into the palette bank) | `TextureBankClass::OpenTextureFile` (`graphics/bsplib/texbank.cpp:277`) |

The basename (`KoreaObj`) is theater-specific — `KoreaObj` for the default Korea theater; other theaters ship their own trio.

### 3.2 File Magic & Version

```cpp
// graphics/include/objectparent.h
static const UInt32 FORMAT_VERSION = 0x03087000;   // read by ObjectParent::VerifyVersion
```

```cpp
// graphics/dxengine/dxdefines.h
#define MODEL_VERSION 0x0002                       // DxDbHeader.Version self-check:
                                                   //   (Version & 0xffff) == (~Version >> 16)
```

### 3.3 `.DXH` On-Disk Layout

Read sequentially in `ObjectParent::SetupTable`:

```
[ UInt32  fileVersion = 0x03087000 ]              ← VerifyVersion
[ ColorBankClass pool    ]                         ← nColors, nDarkendColors, Pcolor[nColors]
[ PaletteBankClass pool  ]                         ← nPalettes, then DiskPalette[nPalettes]
                                                   //   DiskPalette = { DWORD paletteData[256];
                                                   //                   UInt32 palHandle; int refCount; }
[ TextureBankClass pool  ]                         ← nTextures, maxCompressedSize,
                                                   //   then TempTexBankEntry[nTextures]
[ ObjectLOD::SetupTable  ]                         ← maxTagList, TheObjectLODsCount,
                                                   //   per-LOD: 12-byte spare + fileoffset + filesize
                                                   //   (peeks DxDbHeader from .DXL to copy TexBank[])
[ ParentFileRecord array ]                         ← TheObjectListLength, then ParentFileRecord[N]
[ Per-parent: Ppoint[nSlots+nDynamicCoords] ]      ← slot positions then dynamic-coord positions
[ Per-parent, per-LOD: DiskLODrecord ]             ← { UInt32 objLOD; float maxRange; }
                                                   //   (objLOD low bit |1 is a marker;
                                                   //    real index = objLOD >> 1)
```

**`ParentFileRecord` (verbatim from `graphics/include/objectparent.h`):**

```cpp
typedef struct {
    float radius;             // bounding sphere
    float minX, maxX;         // AABB in object space (feet)
    float minY, maxY;
    float minZ, maxZ;
    float RadarSign;          // radar cross-section proxy
    float IRSign;             // infrared signature
    short nTextureSets;       // e.g. summer/winter/desert camo
    short nDynamicCoords;     // runtime-movable vertices
    unsigned char nLODs;
    unsigned char nSwitch;    // legacy 8-bit count (old KO.dxh)
    unsigned char nDOF;       // legacy 8-bit count
    unsigned char nSlots;     // child-attachment points
    short nSwitches;          // new 16-bit count (new KO.dxh)
    short nDOFs;              // new 16-bit count
} ParentFileRecord;
```

Old-vs-new auto-detection: a hack stored in `maxCompressedSize` of the texture pool (`nVer != 0xFEEF` → old 8-bit fields; else new 16-bit). See `texbank.cpp:147`. The `f4-models` reader must replicate this.

### 3.4 `.DXL` On-Disk Layout (per-LOD chunk)

Each LOD is `filesize` bytes at `fileoffset` into the memory-mapped `.DXL`. Layout:

```
[ DxDbHeader ]                                    ← 56+ bytes (see struct below)
[ DWORD Texs[dwTexNr] ]                           ← texture indices into the .DXH texture bank
[ DxNodeHeadType + body ] × dwNodesNr             ← the BSP node tree
[ Vertex pool @ offset pVPool ]                    ← dwPoolSize bytes
[ DXLightType[dwLightsNr] @ offset pLightsPool ]  ← lights (unused by viewer; parsed for completeness)
```

**`DxDbHeader` (verbatim from `graphics/dxengine/dxdefines.h`):**

```cpp
#define MAX_SCRIPTS_X_MODEL 2

typedef struct DxDbHeader {
    DWORD Version;             // self-check: (Version & 0xffff) == (~Version >> 16)
    DWORD Id;                  // model ID (XOR crypt key seed if CRYPTED_MODELS)
    DWORD VBClass;             // vertex-buffer class (ground/air/cockpit)
    DWORD ModelSize;           // total size of header+nodes+VP, used for crypt
    DWORD dwNVertices;
    DWORD dwPoolSize;          // size of vertex pool in bytes
    DWORD pVPool;              // offset to vertex pool from start of model
    DWORD dwNodesNr;
    DXScriptVariableType Scripts[MAX_SCRIPTS_X_MODEL];  // animation hooks
    DWORD dwLightsNr;
    DWORD pLightsPool;
    DWORD dwTexNr;
} DxDbHeader;
```

**Optional XOR encryption** (`#ifdef CRYPTED_MODELS` in `dxengine/dxvbmanager.cpp`):

```cpp
Key  = KEY_CRYPTER + Header.Id;
Size = Header.ModelSize / 4;
Key *= Header.VBClass;
Key += Header.ModelSize;
// XOR every DWORD after ModelSize with (Key * Size)
```

`f4-models` must support both encrypted and unencrypted DXLs. The encryption flag is implicit: if the Version self-check fails on a plain read, attempt decryption and re-validate.

### 3.5 Per-Node Header & Bodies

Every node starts with a 12-byte header:

```cpp
typedef struct {
    DWORD dwNodeSize;   // total size of this node record (incl. body)
    DWORD dwNodeID;
    ItemType Type;      // see enum below
} DXNodeHeadType;

typedef enum {
    DX_ROOT=0,          // BRoot — root of one LOD's tree, holds texture table + script
    DX_SURFACE,         // BPrimitiveNode / BLitPrimitiveNode — renderable geometry
    DX_MATERIAL,        // material state
    DX_TEXTURE,         // texture binding
    DX_DOF,             // BDofNode / BXDofNode / BTransNode / BScaleNode
    DX_ENDDOF,          // pop DOF scope
    DX_SLOT,            // BSlotNode — child-attachment point
    DX_SWITCH,          // BSwitchNode / BXSwitchNode — bitmask child selector
    DX_LIGHT,           // light
    DX_MODELEND         // sentinel
} ItemType;
```

**`DxSurfaceType` body** (renderable primitive):

```cpp
typedef struct {
    DXNodeHeadType h;
    DXFlagsType dwFlags;        // bitfield: Alpha/Lite/ChromaKey/VColor/Texture/SwEmissive/etc.
    DWORD dwVCount;
    DWORD dwStride;
    D3DPRIMITIVETYPE dwPrimType;  // POINTLIST/LINELIST/TRIANGLELIST/etc.
    DWORD dwzBias;
    float SpecularIndex;
    DWORD TexID[2];             // up to 2 textures (multi-texture)
    DWORD SwitchNumber, SwitchMask;
    DWORD DefaultSpecularity;
} DxSurfaceType;
```

**`DxDofType` body** (DOF node):

```cpp
typedef struct {
    DXNodeHeadType h;
    DWORD dwDOFTotalSize;
    DofType Type;               // NO_DOF/ROTATE/XROTATE/TRANSLATE/SCALE/SWITCH/XSWITCH
    union { int dofNumber; int SwitchNumber; };
    float min, max, multiplier, future;
    union { int flags; int SwitchBranch; };
    Ppoint scale;
    D3DXMATRIX rotation;        // 4x4
    Ppoint translation;
} DxDofType;
```

**`DxSlotType` body** (child attachment):

```cpp
typedef struct {
    DXNodeHeadType h;
    DWORD SlotNr;
    D3DXMATRIX rotation;        // includes slot origin in translation row
} DxSlotType;
```

### 3.6 The BNode Hierarchy (runtime)

`f4-models` does **not** replicate the virtual-method `BNode::Draw()` tree — that's a render-time concern owned by the viewer. Instead, `f4-models` builds a **typed, owned `BNode` variant tree** from the `DXNodeHeadType` stream. The viewer then walks this tree to extract meshes.

```cpp
namespace f4::models {

enum class BNodeType : uint8_t {
    Root, SubTree, SlotNode, DofNode, XDofNode, SwitchNode, XSwitchNode,
    SplitterNode, PrimitiveNode, LitPrimitiveNode, CulledPrimitiveNode,
    SpecialXform, LightStringNode, TransNode, ScaleNode, RenderControlNode,
};

struct BSubTree {
    std::vector<Ppoint> coords;          // vertex positions (object space, feet)
    std::vector<Pnormal> normals;        // vertex normals
    int dynamic_coord_offset = 0;        // index into instance.DynamicCoords
    int n_dynamic_coords = 0;
    std::vector<std::unique_ptr<BNode>> children;
};

struct BRoot : BSubTree {
    std::vector<int> tex_ids;            // indices into TextureBank
    int script_number = -1;              // index into ScriptArray (-1 = none)
};

struct BDofNode  : BSubTree { int dof_number; Pmatrix rotation; Ppoint translation; };
struct BXDofNode : BSubTree { int dof_number; float min, max, multiplier, future; int flags;
                              Pmatrix rotation; Ppoint translation; };
struct BTransNode: BSubTree { int dof_number; float min, max, multiplier, future; int flags;
                              Ppoint translation; };
struct BScaleNode: BSubTree { int dof_number; float min, max, multiplier, future; int flags;
                              Ppoint scale; Ppoint translation; };
struct BSlotNode { Pmatrix rotation; Ppoint origin; int slot_number; };
struct BSwitchNode { int switch_number; std::vector<std::unique_ptr<BNode>> sub_trees; };
struct BXSwitchNode { int switch_number; int flags; std::vector<std::unique_ptr<BNode>> sub_trees; };
struct BSplitterNode { float A, B, C, D; std::unique_ptr<BNode> front, back; };
struct BPrimitiveNode { Prim prim; };                 // see polylib.h
struct BLitPrimitiveNode { Poly poly; Poly back_poly; };
struct BCulledPrimitiveNode { Poly poly; };
struct BLightStringNode { float A, B, C, D; int rgba_front, rgba_back; Prim prim; };
struct BSpecialXform { std::vector<Ppoint> coords; BTransformType type;
                       std::unique_ptr<BNode> sub_tree; };
struct BRenderControlNode { BRenderControlType control; int iarg[4]; float farg[4]; };

struct BNode {
    BNodeType type;
    BNodePayload payload;                // std::variant of the above
};

}  // namespace f4::models
```

### 3.7 Polygon Types (`graphics/include/polylib.h`)

The `Prim`/`Poly` family is what the mesh extractor must flatten:

```cpp
typedef enum PpolyType {
    PointF, LineF,
    F, FL, G, GL, Tex, TexL, TexG, TexGL, CTex, CTexL, CTexG, CTexGL,
    AF, AFL, AG, AGL, ATex, ATexL, ATexG, ATexGL,
    CATex, CATexL, CATexG, CATexGL, BAptTex,
    PpolyTypeNum
} PpolyType;
// F=flat-color, G=gouraud, L=lit, C=chroma-keyed, Tex=textured, A=alpha, CA=chroma+alpha

typedef struct Prim   { PpolyType type; int nVerts; int* xyz; };                  // xyz indexes pCoords
typedef struct Poly   : Prim { float A, B, C, D; };                               // plane eqn for back-face cull
typedef struct PolyFC : Poly { int rgba; };                                       // flat colour (index into ColorBank)
typedef struct PolyVC : Poly { int* rgba; };                                      // per-vertex colour
typedef struct PolyFCN: PolyFC { int I; };                                        // flat + lit (intensity index)
typedef struct PolyVCN: PolyVC { int* I; };                                       // gouraud + lit
typedef struct PolyTexFC : PolyFC { int texIndex; Ptexcoord* uv; };               // textured (texIndex → TextureBank)
typedef struct PolyTexVC : PolyVC { int texIndex; Ptexcoord* uv; };
typedef struct PolyTexFCN: PolyFCN { int texIndex; Ptexcoord* uv; };
typedef struct PolyTexVCN: PolyVCN { int texIndex; Ptexcoord* uv; };
```

In `f4-models`, the `int* xyz`/`rgba`/`I` index arrays become `std::vector<int32_t>` (indices into the parent `BSubTree::coords`). The `Ptexcoord* uv` becomes `std::vector<Vec2>`.

### 3.8 Coordinate System & Units

From `graphics/include/grtypes.h`:

- **Coordinate system**: left-handed, Z-up (terrain in X-Y plane, +Z up, +X north, +Y east)
- **Object space**: same axes as world (Z-up)
- **Units**: feet throughout (bounding boxes, LOD switch-in distances, slot positions)
- **Rotation units**: radians for DOF angles (`SetDOFangle(dof, radians)`)
- **Constants**: `PI = 3.14159265359f`, `TWO_PI`, `PI_OVER_2`, `PI_OVER_4`

The viewer must convert left-handed Z-up to Raylib's right-handed Y-up at render time (see §6.6).

### 3.9 Texture Storage

`.TEX` per-texture blob (at `fileOffset`, `fileSize` bytes):

- LZSS-compressed 8-bit palettized image data (decompresses to `dimensions × dimensions` bytes)
- Each byte is an index into the linked `Palette` (256 ARGB entries, indexed via `TexBankEntry::palID`)
- Square dimensions only: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
- Default chroma-key (transparent pixel) color: `0xFFFF0000` (blue)

`f4-models` decompresses via the LZSS algorithm already implemented in `f4-world-convert/src/lzss.cpp`. The extraction plan: lift LZSS into a shared `f4-lzss` (or move it into `f4-data` as a public utility) so both `f4-world-convert` and `f4-models` share one implementation. (See §5.4.)

### 3.10 Texture Sets

A parent can have `nTextureSets` texture sets (e.g. summer/winter/desert camo). The `BRoot` stores `pTexIDs[nTexIDs]` where `nTexIDs / nTextureSets` textures belong to each set. At draw time:

```cpp
int texOffset = instance->TextureSet * (nTexIDs / max(1, ParentObject->nTextureSets));
TheStateStack.SetTextureTable(pTexIDs + texOffset);
```

The viewer exposes a "Texture Set" spinner; the exporter exports each set as a separate glTF material variant (or a glTF extension if the consumer supports it).

### 3.11 Animation Scripts

Each `BRoot` carries a `ScriptNumber` (-1 = none). When drawn, if `ScriptNumber > 0`, `BRoot::Draw` invokes `ScriptArray[ScriptNumber]()`. The script table is populated in `bsplib/scripts.cpp`; recognised names (`bsputil/scriptnames.cpp`):

| Script | ID | Use case |
|--------|----|----------|
| `UH1.ANS` / `Rotate.ANS` | 0 | simple rotor |
| `AH64.ANS` | 1 | 4-blade rotor |
| `Hokum.ANS` | 2 | coaxial rotors |
| `OneProp.ANS` | 3 | single propeller |
| `C130.ANS` | 4 | 4-engine prop |
| `E3.ANS` | 5 | radar dome |
| `VASIF` / `VASIN` | 6 / 7 | variable approach |
| `Chaff.ANS` | 8 | chaff dispenser |
| `Beacon.ANS` | 9 | rotating beacon |
| `CHUTEDED.ANS` | 10 | drag chute |
| `LongBow.ANS` | 11 | LongBow mast |
| `Cycle2/4/10.ANS` | 12/13/14 | wheel cycles |
| `TStrobe.ANS` | 15 | strobe light |
| `TU95.ANS` | 16 | 4-engine contra-rotating |
| `Meatball.ANS` | 17 | carrier approach lights |
| `ComplexProp` | 18 | multi-axis prop |

The viewer implements these as named callbacks that mutate `instance.DOFValues[]` (e.g. `Rotate.ANS` increments `DOFValues[rotor_dof].rotation` by `dt * rotor_rpm`). For Phase V2, implementing scripts 0, 3, 12, 13, 14 (the most common) covers ~80% of in-game vehicles. Full coverage is a Phase V3 stretch goal.

---

## 4. Architecture Overview

### 4.1 Two New Subprojects

| Subproject | Type | Deps on f4-* | External deps | Purpose |
|------------|------|---------------|----------------|---------|
| `f4-models` | STATIC + CLI | `f4-json`, `f4-install` | none | Parse `.DXH`/`.DXL`/`.TEX` → typed `ModelDocument`; JSON I/O; glTF writer |
| `f4-models-viewer` | STATIC + CLI | `f4-models`, `f4-install`, `f4-json` | Raylib 5.0 + ImGui 1.91.5 + rlImGui 9acdbbf + tinyfiledialogs (vendored) | Interactive 3D viewer |

Both mirror the existing convert+viewer pattern (parse lib is engine-agnostic and unit-testable; viewer is the rendering boundary).

### 4.2 Dependency Graph Position

The two new subprojects slot in alongside the existing convert/viewer pair:

```
              f4-json   f4-install   f4-world-convert (owns LZSS today)
                 │           │              │
                 │           │              │  (LZSS extracted to shared utility — see §5.4)
                 │           │              ▼
                 │           │           f4-lzss (NEW, leaf)
                 │           │              ▲
                 │           │              │
                 └─────┬─────┴──────────► f4-models (NEW)
                                      │       ▲
                                      │       │
                                      │   f4-models-viewer (NEW)
                                      │
                              (future) f4-simulation (consumes ModelDocument)
```

The viewer does NOT depend on f4-world-viewer — they are siblings. Both can be built independently; both share Raylib+ImGui versions via the same `FetchContent` pins.

### 4.3 The `ModelDocument` Contract

The single class every downstream consumer codes against:

```cpp
namespace f4::models {

struct ModelDocument {
    // From .DXH
    ColorBank         color_bank;          // Pcolor[nColors] + nDarkendColors
    PaletteBank       palette_bank;        // Palette[nPalettes] (each = 256 ARGB)
    TextureBank       texture_bank;        // TexBankEntry[nTextures] (offset+size+meta)
    std::vector<ObjectLODRecord>  lods;    // fileoffset+filesize+tex_ids (peeked from .DXL)
    std::vector<ObjectParent>     parents; // ParentFileRecord + slots + dynamic coords + LOD refs

    // From .DXL (lazily loaded per-LOD on demand)
    std::unordered_map<int, LodData> loaded_lods;  // key = LOD index

    // From .TEX (lazily decompressed per-texture on demand)
    std::unordered_map<int, DecodedTexture> loaded_textures;

    // Source provenance (for the viewer's status bar + the exporter's metadata)
    std::filesystem::path dxh_path;
    std::filesystem::path dxl_path;
    std::filesystem::path tex_path;
    std::string theater_basename;  // e.g. "KoreaObj"

    // Public API (see §5.5 for full signatures)
    static ModelDocument load(const std::filesystem::path& basename);
    static ModelDocument load_from_install(const f4::install::Installation& install,
                                           std::string_view theater_key);

    const ObjectParent* parent(int index) const noexcept;
    const LodData* fetch_lod(int lod_index);                 // lazy load
    const DecodedTexture* fetch_texture(int tex_index);      // lazy load

    void to_json(f4::json::Writer& w) const;
    static ModelDocument from_json(f4::json::Reader& r);
};

}  // namespace f4::models
```

The contract is: **once `ModelDocument::load()` returns, the parent table, color/palette/texture banks are fully populated. LODs and textures are fetched on demand by index.** The viewer and exporter never touch the `.DXH`/`.DXL`/`.TEX` files directly.

---

## 5. f4-models — Engine-Agnostic Model Library

### 5.1 Public Headers

```
include/f4/models/
├── f4_models.hpp                # UMBRELLA — includes all public modules
├── model_document.hpp           # ModelDocument + load() + load_from_install()
├── types.hpp                    # Ppoint, Pnormal, Pmatrix, Pcolor, Ptexcoord (typed aliases)
├── banks.hpp                    # ColorBank, PaletteBank, TextureBank, TexBankEntry
├── parent.hpp                   # ObjectParent, ParentFileRecord, LODrecord
├── lod.hpp                      # ObjectLODRecord, LodData (BNode tree + vertex pool + lights)
├── bnode.hpp                    # BNodeType enum + BNode variant tree (see §3.6)
├── polylib.hpp                  # Prim, Poly, PolyFC, PolyVC, PolyTex*, PpolyType enum
├── texture.hpp                  # DecodedTexture (RGBA8 + dimensions + chroma_key + flags)
├── dxdefines.hpp                # DxDbHeader, DxNodeHeadType, DxSurfaceType, DxDofType, DxSlotType,
│                                #   DXFlagsType, DXScriptVariableType, ItemType enum
│                                #   (verbatim from FreeFalcon, with x64-safe layouts)
├── json_io.hpp                  # to_json / from_json (uses f4-json Reader/Writer)
└── gltf_writer.hpp              # write_gltf(doc, parent_index, output_path, options) (Phase E1)
```

### 5.2 Source Layout

```
src/
├── model_document.cpp           # load(), load_from_install(), parent(), fetch_lod(), fetch_texture()
├── dxh_reader.cpp               # read_dxh(path, ModelDocument&) — VerifyVersion + banks + parents
├── dxl_reader.cpp               # read_dxl_chunk(bytes_view, LodData&) — DxDbHeader + nodes + VP + lights
│                                #   (handles both encrypted and unencrypted; XOR decrypt path)
├── tex_reader.cpp               # read_tex_blob(bytes_view, palette, DecodedTexture&) — LZSS decompress
├── bnode_builder.cpp            # walk DxNodeHeadType stream → typed BNode tree
├── lzss.cpp                     # LZSS decompressor (extracted from f4-world-convert, see §5.4)
├── json_io.cpp                  # ModelDocument ↔ JSON
└── gltf_writer.cpp              # ModelDocument → .glb (Phase E1)
```

### 5.3 The On-Disk Struct Shims (x86 → x64 safety)

FreeFalcon's on-disk structs include pointer-sized fields that grew from 4 bytes (x86) to 8 bytes (x64). The original code has `#pragma pack` shims (`DiskLODrecord`, `DiskPalette`, `DiskTempTexBankEntry`, `DDSDiskHeader`). `f4-models` defines its **own** packed structs with explicit sizes:

```cpp
namespace f4::models::disk {

#pragma pack(push, 1)
struct DiskLODrecord {            // 8 bytes on disk, both x86 and x64
    uint32_t objLOD;
    float    maxRange;
};
static_assert(sizeof(DiskLODrecord) == 8);

struct DiskPalette {              // 1032 bytes on disk (256*4 + 4 + 4), not the runtime Palette
    uint32_t paletteData[256];
    uint32_t palHandle_slot;      // ignored on read; pointer-sized on x86, we treat as 4 bytes
    int32_t  refCount;            // ignored on read; runtime refcount only
};
static_assert(sizeof(DiskPalette) == 1032);

struct DiskTempTexBankEntry {     // x86 layout (we read as x86 even on x64 — the original code does the same)
    int32_t  fileOffset;
    int32_t  fileSize;
    int32_t  dimensions;          // square texture
    uint32_t flags;               // MPR_TI_* bits
    uint32_t chromaKey;           // default 0xFFFF0000
    int32_t  palID;
    int32_t  refCount;            // always 0 on read
};
static_assert(sizeof(DiskTempTexBankEntry) == 28);
#pragma pack(pop)

}  // namespace f4::models::disk
```

The reader never trusts `sizeof(SomeCppStruct)`; it always reads via the explicitly-sized `disk::` struct and then constructs the typed runtime representation.

### 5.4 LZSS Extraction

Today, `f4-world-convert/src/lzss.cpp` + `lzss.hpp` implement LZSS decompression for the `.cam` campaign archive. The same algorithm (with a different byte window size and look-ahead — see `freefalcon-central/src/utils/lzss.cpp` and `lzssopt.h`) decompresses `.TEX` texture blobs.

Two options:

1. **Extract LZSS into a new leaf library `f4-lzss`** with two parameterized entry points (`decompress_window12` for `.cam`, `decompress_window16` for `.TEX`). Both `f4-world-convert` and `f4-models` depend on it. Cleanest; breaks no existing code (the `f4-world-convert` API stays the same, just delegates).

2. **Copy the LZSS code into `f4-models`** as a private detail. Faster to ship; creates a duplicate implementation that can drift.

**Recommendation: Option 1.** LZSS is small (~200 LoC), well-tested, and the duplication risk is real. The extraction is a one-time refactor that touches `f4-world-convert/CMakeLists.txt` (replace internal `src/lzss.cpp` with `target_link_libraries(... f4-lzss)`) and adds a new leaf subproject.

### 5.5 Public API Signatures

```cpp
namespace f4::models {

// === types.hpp ===
struct Ppoint    { float x, y, z; };
struct Pnormal   { float i, j, k; };
struct Pmatrix   { float M11..M33; };                  // row-major 3x3
struct Pcolor    { float r, g, b, a; };
struct Ptexcoord { float u, v; };

// === banks.hpp ===
struct ColorBank {
    std::vector<Pcolor> colors;
    int n_darkened_colors = 0;                          // first N get TOD light modulation
};
struct PaletteBank {
    struct Palette { std::array<uint32_t, 256> argb; }; // ARGB, 4 bytes per entry
    std::vector<Palette> palettes;
};
struct TextureBank {
    struct Entry {
        int32_t  file_offset;                           // into the .TEX file
        int32_t  file_size;                             // compressed size in bytes
        int32_t  dimensions;                            // square: dimensions × dimensions
        uint32_t flags;                                 // MPR_TI_* bits
        uint32_t chroma_key;                            // transparent pixel (default 0xFFFF0000)
        int32_t  palette_id;                            // index into PaletteBank
    };
    std::vector<Entry> entries;
};

// === parent.hpp ===
struct ObjectParent {
    // From ParentFileRecord
    float radius;
    float min_x, max_x, min_y, max_y, min_z, max_z;
    float radar_sign, ir_sign;
    int16_t n_texture_sets;
    int16_t n_dynamic_coords;
    uint8_t n_lods;
    int16_t n_switches;                                 // resolved (max of legacy 8-bit / new 16-bit)
    int16_t n_dofs;
    uint8_t n_slots;

    // Followed in .DXH by slot + dynamic-coord positions
    std::vector<Ppoint> slot_positions;                 // size = n_slots
    std::vector<Ppoint> dynamic_coord_positions;        // size = n_dynamic_coords

    // Followed by per-LOD records
    struct LODRef {
        int   lod_index;                                // index into ModelDocument::lods
        float max_range;                                // switch-in distance in feet
    };
    std::vector<LODRef> lods;                           // size = n_lods, sorted near→far
};

// === lod.hpp ===
struct LodData {
    DxDbHeader header;                                  // see dxdefines.hpp
    std::vector<uint32_t> tex_ids;                      // dwTexNr entries
    std::vector<std::unique_ptr<BNode>> nodes;          // owned tree
    std::vector<Ppoint>    vertex_pool;                 // dwPoolSize / sizeof(Ppoint)
    // Lights parsed but unused by viewer (Phase V3 might add light visualization)
};

// === texture.hpp ===
struct DecodedTexture {
    int32_t  dimensions;                                // square
    uint32_t chroma_key;                                // from TexBankEntry
    std::vector<uint8_t> rgba8;                         // dimensions × dimensions × 4 bytes
    bool     has_alpha = false;                         // true if any pixel's alpha < 255
};

// === model_document.hpp ===
class ModelDocument {
public:
    static ModelDocument load(const std::filesystem::path& basename);
    static ModelDocument load_from_install(const f4::install::Installation& install,
                                           std::string_view theater_key);

    // Inspectors (O(1))
    [[nodiscard]] const ColorBank&   color_bank()   const noexcept;
    [[nodiscard]] const PaletteBank& palette_bank() const noexcept;
    [[nodiscard]] const TextureBank& texture_bank() const noexcept;
    [[nodiscard]] std::span<const ObjectParent> parents() const noexcept;
    [[nodiscard]] const ObjectParent* parent(int index) const noexcept;

    // Lazy fetch (O(1) after first call; thread-safe via std::mutex)
    [[nodiscard]] const LodData*        fetch_lod(int lod_index);
    [[nodiscard]] const DecodedTexture* fetch_texture(int tex_index);

    // Provenance
    [[nodiscard]] const std::filesystem::path& dxh_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& dxl_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& tex_path() const noexcept;
    [[nodiscard]] std::string_view theater_basename() const noexcept;

    // JSON I/O (uses f4-json)
    void to_json(f4::json::Writer& w) const;
    static ModelDocument from_json(f4::json::Reader& r);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace f4::models
```

### 5.6 JSON I/O Strategy

The `.DXH`/`.DXL`/`.TEX` trio is ~50–200 MB per theater. Dumping the entire decoded tree to JSON is wasteful and slow. Instead, `ModelDocument::to_json()` emits a **manifest + parent table + banks** (the cheap, fully-decoded parts) and skips LOD trees and texture blobs:

```json
{
  "format": "f4-models/v1",
  "theater": "KoreaObj",
  "dxh_path": "...",
  "dxl_path": "...",
  "tex_path": "...",
  "color_bank": { "n_colors": 256, "n_darkened": 64, "colors": [...] },
  "palette_bank": { "n_palettes": 12, "palettes": [...] },
  "texture_bank": {
    "n_textures": 4521,
    "entries": [
      { "file_offset": 0, "file_size": 1024, "dimensions": 64, "chroma_key": "0xFFFF0000", "palette_id": 0 },
      ...
    ]
  },
  "parents": [
    {
      "index": 0,
      "radius": 35.5,
      "aabb": { "min": [0,0,0], "max": [40,40,15] },
      "n_lods": 4,
      "n_switches": 3, "n_dofs": 8, "n_slots": 12, "n_texture_sets": 1,
      "lods": [
        { "lod_index": 0, "max_range": 5000.0 },
        ...
      ],
      "slot_positions": [...],
      "dynamic_coord_positions": [...]
    },
    ...
  ]
}
```

The full LOD trees + texture blobs stay in the binary files; the JSON is a ~1–5 MB index. The viewer loads the JSON for fast parent browsing, then lazily fetches LODs/textures on demand.

A separate `dxh2json` CLI ships in `cli/` for headless conversion (mirrors `cam2json` and `terrain2json`).

### 5.7 Error Handling

Follows the existing convention: throwing `std::runtime_error` on I/O / parse errors, caught at the viewer/CLI boundary.

Specific errors documented in the public headers:

| Condition | Exception | Caught at |
|-----------|-----------|-----------|
| `.DXH` file missing or unreadable | `std::runtime_error("f4-models: cannot open .DXH: <path>: <strerror>")` | Viewer `load_model_dialog()` |
| `FORMAT_VERSION` mismatch | `std::runtime_error("f4-models: bad .DXH magic: expected 0x03087000, got 0x...")` | Viewer status bar |
| `.DXL` Version self-check fails after both plain and decrypted reads | `std::runtime_error("f4-models: .DXL chunk at offset <N> failed version check")` | Viewer status bar |
| LOD index out of range | `std::out_of_range` | Caller (programming error) |
| Texture index out of range | `std::out_of_range` | Caller |
| LZSS decompression produced wrong byte count | `std::runtime_error("f4-models: LZSS decompression of texture <N> produced <X> bytes, expected <Y>")` | Viewer texture panel |

---

## 6. f4-models-viewer — Interactive 3D Viewer

### 6.1 File Layout (mirrors f4-world-viewer post-REFACTOR-5)

```
f4-models-viewer/
├── CMakeLists.txt
├── include/f4/models_viewer/                # PUBLIC headers
│   ├── viewer_app.hpp                        # pimpl: ViewerApp class, ~15 public methods
│   ├── settings.hpp                          # ViewerSettings + load/save (cross-platform)
│   ├── file_dialog.hpp                       # pick_open_file / pick_save_file / pick_folder
│   ├── model_inspector.hpp                   # pure data: ParentSummary, LodSummary, DofSummary, ...
│   └── enum_text.hpp                         # inline const char* decoders (node_type_name, dof_type_name, ...)
├── src/                                      # PRIVATE
│   ├── viewer_app.cpp                        # lifecycle: ctor/dtor/run() + state mutators
│   ├── viewer_state.hpp                      # PRIVATE HEADER: ViewerApp::Impl struct
│   ├── file_ops.cpp                          # load_model / import_dxh_dxl_tex / load_from_install
│   ├── install_flow.cpp                      # set_install_path* / pick_parent_from_install
│   ├── camera3d.cpp                          # Camera3D orbit/pan/zoom + fit_to_model + reset_view
│   ├── scene.cpp                             # build_raylib_meshes(doc, parent, lod, instance_state)
│   │                                         #   → vector<RaylibMesh> + vector<RaylibTexture>
│   ├── canvas3d.cpp                          # BeginMode3D/EndMode3D + DrawMesh + grid + gizmos
│   ├── imgui_panels.cpp                      # menu bar, parent browser, status bar, modals
│   ├── inspector_panel.cpp                   # selected parent detail (ParentFileRecord fields)
│   ├── dof_panel.cpp                         # DOF sliders (rotation + translation per dof_number)
│   ├── switch_panel.cpp                      # switch bitmask toggles
│   ├── texture_set_panel.cpp                 # texture-set picker
│   ├── slot_tree_panel.cpp                   # slot hierarchy + attach child model
│   ├── lod_panel.cpp                         # LOD switch + max_range display + force-LOD override
│   ├── texture_panel.cpp                     # texture browser (decoded RGBA thumbnails)
│   ├── materials_panel.cpp                   # color bank + palette bank viewer
│   ├── bounding_volume_overlay.cpp           # sphere + AABB draw toggles
│   ├── animation_panel.cpp                   # script picker (Phase V2: scripts 0, 3, 12-14)
│   ├── settings.cpp                          # hand-rolled JSON (uses f4-json)
│   └── file_dialog.cpp                       # tinyfiledialogs wrapper impl
├── cli/
│   └── main.cpp                              # --install, --parent <N>, --screenshot, --hex-inspect, ...
├── tests/
│   ├── CMakeLists.txt                        # FetchContent gtest; links model_inspector.cpp + enum_text
│   ├── test_model_inspector.cpp              # pure data tests (no raylib)
│   └── test_settings.cpp                     # JSON round-trip
├── assets/
│   └── (procedural — no PNGs needed for 3D viewer; icons generated via raylib DrawCircle etc.)
└── third_party/tinyfiledialogs/              # vendored dep (same version as f4-world-viewer)
```

### 6.2 Public API (`viewer_app.hpp`)

```cpp
namespace f4::models_viewer {

class ViewerApp {
public:
    ViewerApp();
    ~ViewerApp();

    void run();                                                  // blocking Raylib event loop

    // === Install-aware API (mirrors f4-world-viewer) ===
    bool set_install_path_dialog();
    bool set_install_path(const std::filesystem::path& path);
    const std::optional<f4::install::Installation>& installation() const noexcept;

    // Pick a parent by walking the install's theater list (Korea, etc.) and the parent table
    void open_parent_picker_dialog();

    // === Direct file API ===
    void load_model(const std::filesystem::path& basename);     // e.g. ".../KoreaObj"
    void import_dxh_dxl_tex(const std::filesystem::path& dxh,
                            const std::filesystem::path& dxl,
                            const std::filesystem::path& tex);

    // === Parent / LOD selection ===
    void select_parent(int index);
    void select_lod(int lod_index);                             // -1 = auto (by camera distance)
    void force_lod(int lod_index);                              // overrides auto-select

    // === Instance state (drives DOFs/switches/slots) ===
    void set_dof_angle(int dof_number, float radians);
    void set_dof_offset(int dof_number, float offset);
    void set_switch_mask(int switch_number, uint32_t mask);
    void set_texture_set(int set_index);
    void attach_child(int slot_number, int child_parent_index);
    void detach_child(int slot_number);

    // === Display toggles ===
    void show_bounding_sphere(bool on);
    void show_aabb(bool on);
    void show_grid(bool on);
    void show_axes(bool on);
    void show_wireframe(bool on);
    void show_normals(bool on);

    // === Hex Inspector integration (Phase V2) ===
    void open_hex_inspector_with_file(const std::filesystem::path& path);

    // === Test/script helpers (mirrors f4-world-viewer) ===
    void set_initial_camera(const float eye[3], const float target[3]);
    void schedule_screenshot(float delay_sec, const std::filesystem::path& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace f4::models_viewer
```

### 6.3 The `Impl` Struct (`viewer_state.hpp`)

The private header groups `ViewerApp::Impl` fields by concern (same pattern as `f4-world-viewer/src/viewer_state.hpp`):

- **Window/camera**: `window_w/h`, `Camera3D camera`, `camera_orbit_yaw/pitch/dist`, `camera_target`, `dragging`, `panning`, `should_exit`, `initial_camera_set`
- **Data**: `f4::models::ModelDocument doc`, `doc_loaded`, `doc_path_display`, `selected_parent_index`, `selected_lod_index`, `forced_lod` (-1 = auto)
- **Instance state**: `std::vector<float> dof_rotations`, `std::vector<float> dof_translations`, `std::vector<uint32_t> switch_masks`, `int texture_set`, `std::vector<int> slot_children` (parent index per slot, -1 = empty)
- **Render cache** (per parent + per LOD): `std::vector<RaylibMesh> meshes`, `std::vector<RaylibTexture> textures`, `bool meshes_dirty` (set true by any instance-state mutation, cleared by `rebuild_meshes()`)
- **Layer toggles** (12 booleans): `show_bounding_sphere/aabb/grid/axes/wireframe/normals/slots/dof_axes/texture_thumbnails/...`
- **Status / install / settings / modals / pending dialog / screenshot / hex inspector**
- **Inline color helpers**: `RlColor`, `color_for_dof(int)`, `color_for_switch(int)`, `to_rl(Pcolor)`, `to_rl(uint32_t argb)`

### 6.4 Mesh Building (`scene.cpp`)

The hot path. Walks the BNode tree and produces one `RaylibMesh` per material/texture combination (not per primitive — that would be thousands of draw calls).

```cpp
// Pseudocode for scene.cpp's build_raylib_meshes()
struct MeshBuilder {
    std::unordered_map<int /*texture_id*/, VertexAccumulator> by_texture;
    std::unordered_map<uint32_t /*rgba*/, VertexAccumulator> by_flat_color;
    VertexAccumulator unlit_untextured;

    void walk(const BNode& node, const Pmatrix& parent_xform, const InstanceState& state) {
        switch (node.type) {
            case BNodeType::Root:
                // Set texture table from state.texture_set
                walk_children(node.as<Root>(), parent_xform, state);
                break;
            case BNodeType::DofNode: {
                auto& dof = node.as<DofNode>();
                Pmatrix xform = parent_xform * dof.rotation *
                                rotation_x(state.dof_rotations[dof.dof_number]);
                walk_children(dof, xform, state);
                break;
            }
            case BNodeType::SwitchNode: {
                auto& sw = node.as<SwitchNode>();
                uint32_t mask = state.switch_masks[sw.switch_number];
                for (int i = 0; i < (int)sw.sub_trees.size(); ++i) {
                    if (mask & (1u << i)) walk(*sw.sub_trees[i], parent_xform, state);
                }
                break;
            }
            case BNodeType::SlotNode: {
                auto& slot = node.as<SlotNode>();
                if (state.slot_children[slot.slot_number] >= 0) {
                    // Recurse into child parent's LOD 0 with slot's transform
                    int child_parent = state.slot_children[slot.slot_number];
                    auto& child_doc = ...;  // same ModelDocument
                    auto& child_lod = child_doc.fetch_lod(child_parent.lods[0].lod_index);
                    walk(*child_lod.nodes[0], parent_xform * slot.rotation, ...);
                }
                break;
            }
            case BNodeType::PrimitiveNode: {
                auto& prim = node.as<PrimitiveNode>().prim;
                emit_primitive(prim, parent_xform, state);
                break;
            }
            // ... etc for LitPrimitive, Splitter, SpecialXform, etc.
        }
    }

    void emit_primitive(const Prim& prim, const Pmatrix& xform, const InstanceState& state) {
        // Look up the parent BSubTree's coords/normals
        const auto& subtree = ...;
        for (int i = 0; i < prim.nVerts; ++i) {
            Ppoint local = subtree.coords[prim.xyz[i]];
            Ppoint world = transform(xform, local);
            // Pick the right accumulator: by texture_id if textured, by rgba if flat, else unlit
            // Push (position, normal, uv, color) into that accumulator
        }
        // Push indices (triangulate if prim is a fan/strip)
    }

    std::vector<RaylibMesh> finalize() {
        // Upload each accumulator's vertex/index buffers via rlgl UploadMesh
    }
};
```

**Key invariants** (enforced by tests in `test_model_inspector.cpp`):

1. **No per-frame vertex submission.** Meshes are rebuilt only when `meshes_dirty` is set (instance-state mutation or LOD switch). The render loop just calls `DrawMesh` per cached mesh.
2. **One draw call per texture.** Primitives sharing a texture are batched into one mesh.
3. **Pre-computed lowercase strings.** Parent names (if any), DOF labels, switch labels are uppercased once at load time. The imgui panels render them directly with no per-frame allocation.

### 6.5 Texture Upload (`scene.cpp`)

`DecodedTexture` (RGBA8) → `RaylibTexture` via `rlgl`'s `LoadTextureFromImage`:

```cpp
RaylibTexture to_raylib(const DecodedTexture& tex) {
    Image img = {
        .data = tex.rgba8.data(),
        .width = tex.dimensions,
        .height = tex.dimensions,
        .mipmaps = 1,
        .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
    return LoadTextureFromImage(img);
}
```

The texture is cached in `Impl::texture_cache[int tex_id]` for the lifetime of the loaded `ModelDocument`. Switching `texture_set` does not invalidate the cache (same texture IDs are reused, just different selection).

### 6.6 Coordinate Conversion (Left-Handed Z-Up → Right-Handed Y-Up)

Raylib's `Camera3D` and `DrawMesh` assume right-handed Y-up. FreeFalcon's data is left-handed Z-up. Conversion happens **once per vertex** at mesh-build time, not per-frame:

```cpp
// LH Z-up (x, y, z) → RH Y-up (x, z, -y) — equivalent to a 90° rotation about X
inline Vector3 to_raylib(Ppoint p) {
    return { p.x, p.z, -p.y };
}
```

This is the same convention used by every Z-up → Y-up asset importer (Blender's glTF importer does the same). The viewer documents this in `scene.cpp`'s file-top comment so future maintainers don't try to "fix" the inversion.

Normals get the same treatment. Tangents/bitangents (if added in Phase V3) follow.

### 6.7 Camera Controls (`camera3d.cpp`)

- **Left-drag**: orbit (yaw + pitch around `camera_target`)
- **Right-drag**: pan (translate `camera_target` in screen plane)
- **Mouse wheel**: dolly (zoom toward `camera_target`)
- **`F` key**: frame the selected parent (set `camera_target` to AABB center, `dist` to `radius * 2.5`)
- **`R` key**: reset view to default
- **`1`/`3`/`7` keys**: front/right/top orthographic views (Blender convention)
- ImGui's `io.WantCaptureMouse` guards against the canvas stealing clicks when an ImGui panel is hovered

### 6.8 Animation Scripts (`animation_panel.cpp`)

Phase V2 implements scripts 0, 3, 12, 13, 14 (the common ones):

```cpp
void run_script(int script_number, float dt, InstanceState& state) {
    switch (script_number) {
        case 0: // Rotate.ANS — single rotor
            state.dof_rotations[rotor_dof] += dt * 2.0f * PI * 8.0f;  // 8 rev/sec
            break;
        case 3: // OneProp.ANS — single propeller
            state.dof_rotations[prop_dof] += dt * 2.0f * PI * 30.0f;  // 30 rev/sec
            break;
        case 12: // Cycle2.ANS — 2-wheel landing gear rotation
        case 13: // Cycle4.ANS — 4-wheel
        case 14: // Cycle10.ANS — 10-wheel (C-5 galaxy)
            for (int dof : wheel_dofs) state.dof_rotations[dof] += dt * 2.0f * PI * 5.0f;
            break;
        default:
            // Unknown script: no-op (logged once per parent load)
            break;
    }
}
```

The script runs in the render loop's update phase (before `rebuild_meshes_if_dirty()`). User can pause/resume via the animation panel.

### 6.9 CLI Entry (`cli/main.cpp`)

Mirrors `f4-world-viewer/cli/main.cpp`:

```
Usage: f4-models-viewer [OPTIONS] [basename]

Options:
  --install <path>          Use a Falcon install path to resolve theater basename
  --theater <key>           Theater key (e.g. "korea") when --install is set
  --parent <N>              Select parent by index on startup
  --lod <N>                 Force LOD on startup (-1 = auto)
  --screenshot <path>       Write a PNG after 1.5s and exit (headless smoke test)
  --hex-inspect <path>      Open the Hex Inspector with a file (Phase V2)
  --eye <x,y,z>             Initial camera eye position
  --target <x,y,z>          Initial camera target
  --width <W> --height <H>  Window dimensions (default 1600×900)

Positional:
  basename                  Path to KoreaObj (without the .Dxh/.Dxl/.Tex extension)
```

VS-debugger properties (`VS_DEBUGGER_WORKING_DIRECTORY`, `VS_DEBUGGER_COMMAND_ARGUMENTS`) are set so F5 in Visual Studio launches against bundled fixtures.

---

## 7. glTF 2.0 Exporter

### 7.1 CLI Tool (`dxh2gltf`)

Lives in `f4-models/cli/` (alongside the future `dxh2json`):

```
Usage: dxh2gltf [OPTIONS] <basename> --parent <N> --out <path>

Options:
  --parent <N>              Parent index to export (required)
  --lod <N>                 LOD index to export (default: 0 = highest detail)
  --texture-set <N>         Texture set to use (default: 0)
  --format <glb|gltf>       Output format (default: glb)
  --embed-textures          Embed textures in the .glb (default: true for .glb)
  --no-textures             Skip texture emission (geometry only)
  --no-animation            Skip DOF animation channels (static pose only)
  --validate                Run glTF-Validator on the output and report warnings
```

### 7.2 Mapping FreeFalcon → glTF

| FreeFalcon | glTF 2.0 |
|------------|----------|
| `ObjectParent` | One `scene` containing a root `node` |
| `BRoot` (LOD root) | `node` with `mesh` reference |
| `BDofNode` (ROTATE) | `node` with `ROTATION` TRS + `animation.channel` targeting `.rotation` |
| `BDofNode` (TRANSLATE) | `node` with `TRANSLATION` TRS + `animation.channel` |
| `BScaleNode` | `node` with `SCALE` TRS + `animation.channel` |
| `BSwitchNode` | `node` with multiple child nodes; visibility driven by `animation.channel` on `.extras.switchMask` (custom extension) — OR: export each switch combination as a separate mesh primitive, with material alpha mask |
| `BSlotNode` | `node` with empty `children` (placeholder for runtime child attachment) + `extras.slotNumber` |
| `BPrimitiveNode` (textured) | `mesh.primitive` with `POSITION`, `NORMAL`, `TEXCOORD_0`, `material` (with `baseColorTexture`) |
| `BPrimitiveNode` (flat color) | `mesh.primitive` with `POSITION`, `NORMAL`, `COLOR_0` (vertex color), `material` (with `baseColorFactor`) |
| `TextureBank` + `PaletteBank` | `image` (decoded RGBA8 PNG) + `sampler` + `texture` per texture ID |
| `ColorBank` | `material.baseColorFactor` (per primitive) OR `mesh.primitive.attributes.COLOR_0` |
| `ParentFileRecord` (bounding sphere/AABB) | `node`'s `mesh`'s bounding box (auto-computed) + `extras.boundingSphere` |
| `ScriptNumber` (animation script) | `animation` with `channel`s per DOF (keyframes generated procedurally) |

### 7.3 glTF Writer API (`gltf_writer.hpp`)

```cpp
namespace f4::models {

struct GltfExportOptions {
    enum class Format { Glb, Gltf };
    Format format = Format::Glb;
    int    parent_index = 0;             // required
    int    lod_index = 0;                // -1 = all LODs as separate meshes
    int    texture_set = 0;
    bool   embed_textures = true;
    bool   emit_textures = true;
    bool   emit_animation = true;        // DOF channels + script-driven keyframes
    bool   validate = false;             // invoke glTF-Validator if available
};

struct GltfExportResult {
    std::filesystem::path output_path;
    int   n_meshes = 0;
    int   n_materials = 0;
    int   n_textures = 0;
    int   n_animation_channels = 0;
    int   n_warnings = 0;
    std::vector<std::string> warnings;
};

GltfExportResult write_gltf(const ModelDocument& doc,
                             const std::filesystem::path& output_path,
                             const GltfExportOptions& opts);

}  // namespace f4::models
```

### 7.4 glTF Validator Integration

The exporter runs `gltf-validator` (if installed and on PATH) on the output and reports warnings/errors. The validator is fetched via CMake `FetchContent` (binary distribution) and cached at configure time. The `--validate` flag forces the check; otherwise it's a no-op.

If the validator isn't available, the exporter still produces output but logs `"glTF-Validator not found; skipping validation"` to stderr.

### 7.5 Animation Channel Strategy

For each `BDofNode` in the tree, the exporter emits one `animation.channel`:

```json
{
  "sampler": 0,
  "target": {
    "node": <dof_node_index>,
    "path": "rotation"  // or "translation" or "scale"
  }
}
```

The sampler's keyframes are generated by walking the DOF's `[min, max]` range at 8 discrete steps (so the consumer can scrub the DOF). For script-driven DOFs (rotor, propeller), the exporter emits a 1-second animation at 24 fps showing one full rotation.

This is **not** a perfect preservation of FreeFalcon's animation system — it's a faithful approximation that lets glTF consumers scrub through the model's range of motion. Full animation preservation would require exporting the script bytecode, which is out of scope.

---

## 8. Implementation Steps

### Phase M1 — f4-models Foundation (Weeks 1–3)

**Goal**: `ModelDocument::load()` returns a typed object from a real `KoreaObj.*` fixture.

**Steps**:
1. Create `f4-models/` subproject skeleton (CMakeLists, umbrella header, empty `.cpp`s, gtest scaffold).
2. Extract LZSS into `f4-lzss` (refactor `f4-world-convert` to delegate).
3. Define `disk::` packed structs (§5.3) with `static_assert`s on sizes.
4. Implement `dxh_reader.cpp`: `VerifyVersion`, `ColorBank::ReadPool`, `PaletteBank::ReadPool`, `TextureBank::ReadPool`, `ObjectLOD::SetupTable` (peek `DxDbHeader` from `.DXL` for `TexBank[]`), `ReadParentList` (with old-vs-new auto-detect).
5. Implement `dxl_reader.cpp`: open `.DXL` as memory-mapped (`mmap` on Linux, `CreateFileMapping` on Windows — or just `std::ifstream::read` for simplicity in v1, optimization deferred). Slice per-LOD chunks. Implement XOR decrypt path. Validate `DxDbHeader.Version` self-check.
6. Implement `bnode_builder.cpp`: walk `DxNodeHeadType` stream, placement-construct typed `BNode` variants.
7. Implement `tex_reader.cpp`: LZSS-decompress `.TEX` blob, apply palette, produce `DecodedTexture` (RGBA8).
8. Wire `ModelDocument::load()` + `load_from_install()`.
9. Add fixtures: slice a real `KoreaObj.*` trio from a FreeFalcon install into `tests/fixtures/` (small enough to commit — ~5 MB if we pick a low-LOD parent). Add `fixture_manifest.json` with known parent counts, LOD counts, texture counts for ground-truth validation.
10. Write ≥ 30 unit tests covering: struct sizes, magic/version validation, old-vs-new parent detection, LZSS round-trip, XOR decrypt, BNode tree depth, texture decode byte-count.

**Exit criteria**: `ctest --test-dir build/f4-models/tests` passes ≥ 30 tests. `ModelDocument::load("tests/fixtures/KoreaObj")` returns a non-empty doc with the expected parent count.

### Phase M2 — Mesh Extraction (Weeks 3–4)

**Goal**: A pure-function `extract_mesh(doc, parent_index, lod_index, instance_state) → MeshData` works without raylib.

**Steps**:
1. Define `MeshData` struct (vertices, indices, material_id per primitive-group).
2. Implement `mesh_extractor.cpp`: walk BNode tree (§6.4 pseudocode), accumulate vertices/indices per texture/flat-color bucket, triangulate fans/strips.
3. Implement DOF transform composition (`Pmatrix` multiply, `rotation_x`).
4. Implement switch bitmask filtering (only emit children whose bit is set).
5. Implement slot child resolution (recurse into child parent's LOD 0).
6. Write ≥ 20 unit tests: triangle count for known parents, vertex positions for known DOF=0 state, switch-mask filtering behavior, slot child mesh inclusion.

**Exit criteria**: `extract_mesh()` on the fixture produces a mesh with the expected vertex count (±2% to allow for triangulation ambiguity). The mesh is renderable in a unit test by manual inspection of its data.

### Phase V1 — Viewer MVP (Weeks 5–6)

**Goal**: A window opens and renders one aircraft as a solid-shaded mesh.

**Steps**:
1. Create `f4-models-viewer/` subproject skeleton. Copy `CMakeLists.txt` structure from `f4-world-viewer/`. Pin Raylib 5.0 + ImGui 1.91.5 + rlImGui 9acdbbf + tinyfiledialogs (same versions).
2. Implement `viewer_app.cpp` lifecycle (ctor, dtor, `run()`).
3. Implement `viewer_state.hpp` private header (full `Impl` struct).
4. Implement `camera3d.cpp`: orbit/pan/zoom, `fit_to_model`, default camera.
5. Implement `scene.cpp`: `build_raylib_meshes()` calls `f4::models::extract_mesh()` and uploads via `rlgl UploadMesh`. Cache meshes per (parent, lod, instance_state_hash). Set `meshes_dirty = true` on any state mutation.
6. Implement `canvas3d.cpp`: `BeginMode3D`/`EndMode3D`, `DrawMesh` per cached mesh, grid, axes.
7. Implement `file_ops.cpp`: `load_model(basename)`, `import_dxh_dxl_tex(dxh, dxl, tex)`.
8. Implement `install_flow.cpp`: `set_install_path_dialog`, `set_install_path`, `pick_parent_from_install`.
9. Implement `cli/main.cpp`: parse `--install`, `--theater`, `--parent`, `--screenshot`, positional `basename`.
10. Add `--screenshot` smoke test (mirrors f4-world-viewer's headless PNG capture).
11. Manual visual check: F-16 (or whatever parent the fixture contains) renders as a recognizable aircraft silhouette.

**Exit criteria**: `f4-models-viewer --parent 0 tests/fixtures/KoreaObj` opens a window showing the aircraft. `--screenshot out.png` under Xvfb produces a PNG that a VLM can identify as "an aircraft".

### Phase V2 — Viewer Features (Weeks 7–9)

**Goal**: Every runtime feature (DOFs, switches, slots, texture sets, animation scripts) is exposed in the UI.

**Steps**:
1. `dof_panel.cpp`: list `n_dofs` sliders; on change, mutate `dof_rotations`/`dof_translations`, set `meshes_dirty`.
2. `switch_panel.cpp`: list `n_switches` bitmasks as checkboxes; on change, mutate `switch_masks`, set `meshes_dirty`.
3. `texture_set_panel.cpp`: spinner 0..`n_texture_sets-1`; on change, mutate `texture_set`, set `meshes_dirty` (texture IDs shift).
4. `slot_tree_panel.cpp`: list `n_slots` rows, each with a parent-picker combo box; on change, mutate `slot_children`, set `meshes_dirty`.
5. `lod_panel.cpp`: list `n_lods` with `max_range`; auto-select by camera distance or force-LOD override.
6. `texture_panel.cpp`: scrollable grid of decoded RGBA thumbnails (lazily fetched via `fetch_texture()`).
7. `materials_panel.cpp`: color bank + palette bank viewer (clickable swatches).
8. `inspector_panel.cpp`: selected parent's `ParentFileRecord` fields, AABB visualization, radar/IR signatures.
9. `bounding_volume_overlay.cpp`: draw bounding sphere (`DrawSphereWires`) and AABB (`DrawCubeWires`) toggles.
10. `animation_panel.cpp`: script picker (scripts 0, 3, 12, 13, 14); play/pause; per-script speed slider.
11. `enum_text.hpp`: inline `const char*` decoders for `BNodeType`, `DofType`, `PpolyType`, `ItemType`, `MPR_TI_*` flags.
12. Hex Inspector decoders for `.DXH`, `.DXL`, `.TEX` (see §12).
13. Write ≥ 15 unit tests for `model_inspector.cpp` (pure data: parent summaries, DOF ranges, switch counts).

**Exit criteria**: User can toggle landing gear (a switch), rotate a rotor (a DOF), swap texture sets, and attach a child model to a slot — all from the UI, all visible in real time.

### Phase E1 — glTF Exporter (Weeks 10–12)

**Goal**: `dxh2gltf` produces a `.glb` that loads in Blender and passes glTF-Validator.

**Steps**:
1. Implement `gltf_writer.cpp`: JSON chunk builder for `.gltf` mode, binary chunk builder for `.glb` mode.
2. Map BNode tree → glTF node hierarchy (§7.2).
3. Map `TextureBank` + `PaletteBank` → glTF `images`/`samplers`/`textures` (PNG-encode the RGBA8 data).
4. Map `ColorBank` → vertex colors (`COLOR_0`) or `material.baseColorFactor`.
5. Map DOFs → glTF `animation` channels (8 keyframes per DOF, scrubbing `[min, max]`).
6. Map script-driven DOFs → 1-second animations at 24 fps.
7. Map slots → child placeholder nodes with `extras.slotNumber`.
8. Map switches → either (a) separate meshes per switch combination (memory-heavy but portable) or (b) custom `F4_switch` extension (lighter but requires consumer support). Start with (a); document (b) as future work.
9. Implement `--validate`: shell out to `gltf-validator` (fetched via CMake `FetchContent`), parse JSON output, report warnings/errors.
10. Add `dxh2gltf` CLI in `f4-models/cli/`.
11. Write ≥ 10 unit tests: glTF JSON structure correctness, binary chunk alignment, texture PNG encoding, animation channel keyframe counts.
12. Manual validation: open output `.glb` in Blender 4.x, verify mesh count, material count, texture count match the source. Run `gltf-validator` standalone; verify zero errors, ≤ 5 warnings.

**Exit criteria**: `dxh2gltf tests/fixtures/KoreaObj --parent 0 --out f16.glb --validate` produces a `.glb` that passes glTF-Validator with zero errors. Blender 4.x opens the `.glb` and shows the aircraft with textures applied.

---

## 9. FreeFalcon Validation Mapping

Every public class/function in `f4-models` documents its FreeFalcon source. This table is the validation ground truth — if a behavior diverges from FreeFalcon, it's a bug.

### 9.1 Parser Layer

| `f4-models` artifact | FreeFalcon source | Validation |
|---------------------|--------------------|------------|
| `ModelDocument::load()` | `ObjectParent::SetupTable` (`graphics/bsplib/objectparent.cpp:64`) | Same field order; same version check |
| `dxh_reader.cpp:VerifyVersion` | `ObjectParent::VerifyVersion` | `0x03087000` magic |
| `dxh_reader.cpp:ReadColorBank` | `ColorBankClass::ReadPool` (`graphics/bsplib/colorbank.cpp`) | `nColors` + `nDarkendColors` + `Pcolor[]` |
| `dxh_reader.cpp:ReadPaletteBank` | `PaletteBankClass::ReadPool` (`graphics/bsplib/palbank.cpp`) | `nPalettes` + `DiskPalette[]` (1032 bytes each) |
| `dxh_reader.cpp:ReadTextureBank` | `TextureBankClass::ReadPool` (`graphics/bsplib/texbank.cpp`) | `nTextures` + `maxCompressedSize` (with old/new `nVer` hack) + `TempTexBankEntry[]` |
| `dxh_reader.cpp:ReadLODTable` | `ObjectLOD::SetupTable` (`graphics/bsplib/objectlod.cpp:105`) | `maxTagList` + `TheObjectLODsCount` + per-LOD (12-byte spare + offset + size) + TexBank peek |
| `dxh_reader.cpp:ReadParentList` | `ObjectParent::ReadParentList` (`graphics/bsplib/objectparent.cpp:195`) | `ParentFileRecord` + `Ppoint[nSlots+nDynamicCoords]` + `DiskLODrecord[]` |
| `dxl_reader.cpp:ReadChunk` | `CDXVbManager::SetupModel` (`graphics/dxengine/dxvbmanager.cpp`) | `DxDbHeader` + `Texs[]` + nodes + VP + lights |
| `dxl_reader.cpp:XorDecrypt` | `CDXVbManager::Decrypt` (same file, `#ifdef CRYPTED_MODELS`) | Key formula: `(KEY_CRYPTER + Id) * VBClass + ModelSize`, XOR every DWORD with `Key * (ModelSize/4)` |
| `bnode_builder.cpp:Walk` | `BNode::RestorePointers` (`graphics/bsplib/bspnodes.cpp:41`) | `tagList[]` → placement-`new` correct subclass; pointer fixup |
| `tex_reader.cpp:Decode` | `TextureBankClass::ReadImageData` (`graphics/bsplib/texbank.cpp`) | LZSS decompress to `dimensions × dimensions` bytes; apply palette |
| `lzss.cpp` | `utils/lzss.cpp` + `lzssopt.h` | Same window size, same lookahead, same output |

### 9.2 Viewer Layer

| `f4-models-viewer` feature | FreeFalcon API | Validation |
|----------------------------|----------------|------------|
| DOF slider | `DrawableBSP::SetDOFangle(int dof, float radians)` | `instance.DOFValues[dof].rotation = radians` |
| DOF offset slider | `DrawableBSP::SetDOFoffset(int dof, float offset)` | `instance.DOFValues[dof].translation = offset` |
| Switch bitmask toggle | `DrawableBSP::SetSwitchMask(int switch, uint32_t mask)` | `instance.SwitchValues[switch] = mask` |
| Texture set picker | `DrawableBSP::SetTextureSet(int set)` | `instance.TextureSet = set`; `texOffset = set * (nTexIDs / nTextureSets)` |
| Slot child attach | `DrawableBSP::AttachChild(DrawableBSP* child, int slot)` | `instance.SlotChildren[slot] = &child->instance` |
| LOD auto-select | `ObjectParent::ChooseLOD` (`objectparent.cpp:493`) | Walk near→far; first `range < maxRange` wins; `Fetch()` must return true |
| Animation script 0 (Rotate.ANS) | `ScriptArray[0]` (`graphics/bsplib/scripts.cpp`) | `DOFValues[rotor].rotation += dt * 2π * rpm` |
| Bounding sphere draw | `instance.Radius()` from `ParentObject->radius` | Sphere centered at instance position |
| AABB draw | `ParentObject->Box*()` accessors | Cube from `(minX..maxX, minY..maxY, minZ..maxZ)` |

### 9.3 Exporter Layer

| `f4-models` glTF mapping | FreeFalcon source | Validation |
|--------------------------|--------------------|------------|
| `BRoot` → glTF `node` with `mesh` | `BRoot::Draw` (`bspnodes.cpp:315`) | Same texture-table offset logic |
| `BDofNode` → glTF `node` with `rotation` channel | `BDofNode::Draw` (`bspnodes.cpp:363`) | `mlSinCos` → quaternion conversion |
| `BXDofNode` → glTF `node` with `rotation` channel + min/max | `BXDofNode::Draw` + `Process_DOFRot` | Clamp to `[min, max]`; respect `XDOF_NEGATE`/`XDOF_SUBRANGE` |
| `BSwitchNode` → multiple child nodes (one per bit) | `BSwitchNode::Draw` (`bspnodes.cpp:655`) | Only children whose bit is set are emitted |
| `BSlotNode` → empty child node with `extras.slotNumber` | `BSlotNode::Draw` (`bspnodes.cpp:348`) | Slot origin + rotation preserved |
| `BPrimitiveNode` (textured) → `mesh.primitive` with `TEXCOORD_0` + `material.baseColorTexture` | `BPrimitiveNode::Draw` + `DrawPrimJumpTable[Tex]` | UV coords + texture index preserved |
| `BPrimitiveNode` (flat color) → `mesh.primitive` with `COLOR_0` | `BPrimitiveNode::Draw` + `DrawPrimJumpTable[F]`/`[G]` | Color index → RGBA |

---

## 10. Testing Strategy

### 10.1 Model/View Split (Same as f4-world-viewer)

The viewer's full Raylib+ImGui path is hard to unit-test, so testable code is extracted into pure-data / pure-function modules:

| Testable | Untestable (smoke-tested) |
|----------|---------------------------|
| `ModelDocument` (load, fetch_lod, fetch_texture) | `ViewerApp::run()` |
| `mesh_extractor::extract_mesh()` | `scene.cpp::build_raylib_meshes()` (calls extract_mesh + uploads) |
| `gltf_writer::write_gltf()` | `canvas3d.cpp` (rendering) |
| `model_inspector.cpp` (pure summaries) | `imgui_panels.cpp` / `dof_panel.cpp` / etc. |
| `enum_text.hpp` (inline decoders) | `camera3d.cpp` (input handling) |
| `settings.cpp` (JSON round-trip) | `file_dialog.cpp` (native dialogs) |

The unit tests link only the testable modules — no raylib, no imgui. The viewer lib's CMakeLists uses `target_link_libraries(f4-models-viewer PRIVATE raylib imgui ...)` so the test executable doesn't transitively pull them in.

### 10.2 Test Fixtures

```
f4-models/tests/fixtures/
├── fixture_manifest.json        # known parent counts, LOD counts, texture counts, ground-truth AABBs
├── KoreaObj.Dxh                 # sliced from a real FreeFalcon install (~500 KB)
├── KoreaObj.Dxl                 # ~5 MB (one or two parents' LODs)
├── KoreaObj.Tex                 # ~1 MB (textures for the same parents)
└── synthetic/                   # hand-crafted minimal files for edge cases
    ├── empty_parents.dxh        # nParents = 0
    ├── single_parent_no_lods.dxh
    ├── encrypted_chunk.dxl      # CRYPTED_MODELS = ON
    └── corrupted_magic.dxh      # wrong FORMAT_VERSION
```

The slicing script (`scripts/extract_model_fixtures.py`, modeled on `extract_rosetta.py`) takes a full FreeFalcon install path + a parent index range, writes the fixture files, and updates `fixture_manifest.json`.

### 10.3 Headless Smoke Test

The `--screenshot` CLI flag (mirrors f4-world-viewer):

1. Initialize the viewer with `--parent 0 tests/fixtures/KoreaObj`.
2. Schedule a screenshot 1.5s after launch.
3. After the screenshot is written, a detached `std::thread` sleeps 3s then calls `std::exit(0)`.
4. The orchestrator runs under `xvfb-run -a -s "-screen 0 1600x900x24"` with `LIBGL_ALWAYS_SOFTWARE=1`.
5. The PNG is VLM-inspected: "Does this image show an aircraft?" → pass/fail.

The same pattern has been reliable for f4-world-viewer (per the worklog).

### 10.4 glTF Validator Integration

The `dxh2gltf --validate` flag shells out to `gltf-validator` (binary fetched via CMake `FetchContent` at configure time). The validator's JSON output is parsed:

```json
{
  "asset": { "version": "2.0" },
  "issues": {
    "errors": [],
    "warnings": [
      { "code": "UNUSED_TEXTURE", "message": "Texture 3 is not referenced by any material." }
    ]
  }
}
```

The exporter's exit code is non-zero if any errors are present. Warnings are logged to stderr but don't fail the build.

### 10.5 Test Counts (Targets)

| Subproject | Target test count | Coverage focus |
|------------|-------------------|----------------|
| `f4-lzss` | 8 (carried over from `f4-world-convert`) | Decompression correctness on real `.cam` chunks |
| `f4-models` | ≥ 60 | Struct sizes, magic/version, old-vs-new detection, LZSS, XOR, BNode tree, texture decode, mesh extraction, glTF JSON structure, glTF binary alignment |
| `f4-models-viewer` | ≥ 15 (pure data only) | `model_inspector` summaries, `settings` JSON round-trip, `enum_text` decoders |

---

## 11. Observability & Tracing

Following the F4 project's "Observability" principle (Architecture Proposal §18.1):

### 11.1 Parse Trace

Every `ModelDocument::load()` emits a greppable trace to `stderr` (or a `std::ostream` injected for tests):

```
[f4-models] load basename=".../KoreaObj"
[f4-models]   .DXH opened: 2,456,789 bytes
[f4-models]   VerifyVersion: 0x03087000 OK
[f4-models]   ColorBank: 256 colors (64 darkened)
[f4-models]   PaletteBank: 12 palettes
[f4-models]   TextureBank: 4521 textures (maxCompressed=8192, nVer=0xFEEF → new format)
[f4-models]   LODTable: 1247 LODs
[f4-models]   ParentList: 856 parents
[f4-models]   .DXL opened: 45,234,567 bytes (memory-mapped)
[f4-models]   .TEX opened: 12,345,678 bytes
[f4-models] load OK (856 parents, 1247 LODs, 4521 textures)
```

### 11.2 Viewer Status Bar

The viewer's bottom status bar shows (updated once per frame, no per-frame allocations):

```
KoreaObj — parent 0/856 (F-16C) — LOD 0/4 (auto, 2340 verts, 12 textures) — 60.0 FPS — 45.2 MB GPU
```

### 11.3 Exporter Log

`dxh2gltf` emits a structured log:

```
[dxh2gltf] input: .../KoreaObj (parent=0, lod=0, texture_set=0)
[dxh2gltf] extracted mesh: 2340 verts, 4567 tris, 12 texture groups
[dxh2gltf] emitted 12 materials, 12 textures (PNG-encoded)
[dxh2gltf] emitted 8 DOF animation channels (8 keyframes each)
[dxh2gltf] emitted 1 script animation (Rotate.ANS, 24 keyframes, 1.0s)
[dxh2gltf] wrote f16.glb: 1,234,567 bytes
[dxh2gltf] glTF-Validator: 0 errors, 2 warnings
[dxh2gltf]   warning: UNUSED_TEXTURE — Texture 7 is not referenced by any material.
[dxh2gltf]   warning: ANIMATION_CHANNEL_NO_WEIGHTS — Animation 0 channel 3 has zero-weight keyframes.
```

---

## 12. Hex Inspector Integration

The existing `f4-world-viewer`'s Hex Inspector (model/view split: `HexModel` + `decoders.cpp` + `hex_inspector.cpp`) supports adding new decoders via a documented 3-step pattern. `f4-models-viewer` inherits the same Hex Inspector and adds four new decoders:

| Decoder | File | Annotations |
|---------|------|-------------|
| `decode_dxh` | `hex/decoders.cpp` | `FORMAT_VERSION` magic, color bank pool header, palette bank pool header, texture bank pool header (with `nVer` old/new detection), LOD table entries, parent records, slot/dynamic-coord positions, LOD refs |
| `decode_dxl` | `hex/decoders.cpp` | `DxDbHeader` (Version self-check, Id, VBClass, ModelSize, dwNVertices, dwPoolSize, pVPool, dwNodesNr, Scripts, dwLightsNr, pLightsPool, dwTexNr), `Texs[]` array, per-node `DxNodeHeadType` (with `ItemType` enum decoder), `DxSurfaceType` body, `DxDofType` body, `DxSlotType` body |
| `decode_tex_blob` | `hex/decoders.cpp` | LZSS stream header (if any), per-byte palette index annotations (first 16 bytes only — too many to annotate all) |
| `decode_palette` | `hex/decoders.cpp` | 256 ARGB entries with hex + RGB decimal display |

Each decoder is a pure function `(const HexModel&) → std::vector<Annotation>`, unit-tested against the same `KoreaObj.*` fixtures used by the parser tests.

The Hex Inspector opens automatically when the user clicks "Inspect raw bytes" on any parent/LOD/texture in the viewer's inspector panels.

---

## 13. Directory Layout & Build

### 13.1 New Subprojects in Root CMakeLists.txt

Insert into `/home/z/my-project/CMakeLists.txt` in dependency order (after `f4-world-convert`, before `f4-world-viewer`):

```cmake
# --- f4-lzss (NEW — extracted from f4-world-convert) ---
add_subdirectory(f4-lzss)

# --- f4-models (NEW) ---
add_subdirectory(f4-models)

# --- f4-models-viewer (NEW, gated) ---
option(F4_BUILD_MODELS_VIEWER ON "Build the 3D model viewer")
if(F4_BUILD_MODELS_VIEWER)
    add_subdirectory(f4-models-viewer)
endif()
```

### 13.2 `f4-models/CMakeLists.txt` (Sketch)

```cmake
cmake_minimum_required(VERSION 3.20)
project(f4-models VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "FreeFalcon .DXH/.DXL/.TEX model parser")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(f4-models STATIC
    src/model_document.cpp
    src/dxh_reader.cpp
    src/dxl_reader.cpp
    src/tex_reader.cpp
    src/bnode_builder.cpp
    src/lzss.cpp                 # private impl delegating to f4-lzss (or self-contained if extraction deferred)
    src/json_io.cpp
    src/gltf_writer.cpp
)
target_include_directories(f4-models
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
target_link_libraries(f4-models PUBLIC f4-json f4-install f4-lzss)

# CLI tools
add_subdirectory(cli)

option(F4_MODELS_BUILD_TESTS "Build unit tests" ON)
if(F4_MODELS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

### 13.3 `f4-models-viewer/CMakeLists.txt` (Sketch — mirrors `f4-world-viewer`)

```cmake
cmake_minimum_required(VERSION 3.20)
project(f4-models-viewer VERSION 0.1.0 LANGUAGES C CXX DESCRIPTION "Interactive 3D viewer for FreeFalcon models")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Third-party deps (same pins as f4-world-viewer) ---
include(FetchContent)
FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG 5.0)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.5)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(rlimgui GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git GIT_TAG 9acdbbf)
FetchContent_MakeAvailable(rlimgui)

# tinyfiledialogs (vendored)
add_library(tinyfiledialogs STATIC third_party/tinyfiledialogs/tinyfiledialogs.c)
set_target_properties(tinyfiledialogs PROPERTIES C_STANDARD 11)

# --- Viewer lib ---
add_library(f4-models-viewer STATIC
    src/viewer_app.cpp
    src/file_ops.cpp
    src/install_flow.cpp
    src/camera3d.cpp
    src/scene.cpp
    src/canvas3d.cpp
    src/imgui_panels.cpp
    src/inspector_panel.cpp
    src/dof_panel.cpp
    src/switch_panel.cpp
    src/texture_set_panel.cpp
    src/slot_tree_panel.cpp
    src/lod_panel.cpp
    src/texture_panel.cpp
    src/materials_panel.cpp
    src/bounding_volume_overlay.cpp
    src/animation_panel.cpp
    src/settings.cpp
    src/file_dialog.cpp
    src/hex/hex_model.cpp
    src/hex/decoders.cpp
    src/hex/hex_inspector.cpp
    # ImGui sources compiled directly (same pattern as f4-world-viewer)
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp
)
target_include_directories(f4-models-viewer
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${rlimgui_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyfiledialogs)
target_link_libraries(f4-models-viewer
    PUBLIC  f4-models f4-install f4-json
    PRIVATE raylib tinyfiledialogs)

# CLI
add_subdirectory(cli)

option(F4_MODELS_VIEWER_BUILD_TESTS "Build unit tests" ON)
if(F4_MODELS_VIEWER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

### 13.4 Build & Test Commands

```bash
# Configure (from repo root)
cmake -B build -S .

# Build everything
cmake --build build

# Run all model-viewer tests
ctest --test-dir build/f4-models/tests --output-on-failure
ctest --test-dir build/f4-models-viewer/tests --output-on-failure

# Run the viewer against bundled fixtures
./build/f4-models-viewer/cli/f4-models-viewer --parent 0 f4-models/tests/fixtures/KoreaObj

# Export to glTF
./build/f4-models/cli/dxh2gltf f4-models/tests/fixtures/KoreaObj --parent 0 --out download/f16.glb --validate

# Headless screenshot smoke test
xvfb-run -a -s "-screen 0 1600x900x24" \
  LIBGL_ALWAYS_SOFTWARE=1 \
  ./build/f4-models-viewer/cli/f4-models-viewer --parent 0 --screenshot /tmp/smoke.png f4-models/tests/fixtures/KoreaObj
```

---

## 14. Risks & Mitigations

### 14.1 Technical Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| **x86 vs x64 struct layout shims wrong** — `DiskPalette`, `DiskTempTexBankEntry`, `DiskLODrecord` all carry pointer-sized fields that grew on x64 | High | Parser produces garbage; viewer shows nothing | §5.3 mandates `static_assert(sizeof(DiskStruct) == N)` for every on-disk struct. Validate against real fixture before adding any logic on top. |
| **LZSS extraction breaks f4-world-convert** | Medium | Existing tests fail; blocks the whole plan | Do the extraction as the very first commit (Phase M1 step 2). Run `ctest --test-dir build/f4-world-convert/tests` immediately after; if anything fails, revert and copy instead. |
| **`.DXL` XOR encryption path not exercised by fixtures** — fixtures are sliced from unencrypted FF installs | Medium | Encrypted mods (some FF5 variants) fail to parse | Add a synthetic `encrypted_chunk.dxl` fixture (hand-crafted) that exercises the decrypt path. Unit-test both plain and decrypted reads. |
| **BNode tree has cycles or null siblings** — malformed `.DXL` could crash the walker | Low | Viewer crashes on edge-case mods | `bnode_builder.cpp` enforces a max tree depth (e.g. 64) and treats null siblings as a hard error (throw `runtime_error`), not a silent skip. |
| **glTF Validator not available in CI** | Medium | `--validate` flag silently passes | CMake `FetchContent` downloads the validator binary at configure time. If download fails, `--validate` logs `"validator not found"` and exits 0 (non-fatal). CI runs `--validate` and asserts the log contains `"0 errors"`. |
| **Raylib 5.0 3D mode perf on huge parents** — some FF parents have 50k+ vertices per LOD | Medium | Viewer drops below 30 FPS | §6.4 invariants enforce one draw call per texture (not per primitive). For >100k vertices, fall back to wireframe-only mode with a status-bar warning. |
| **Coordinate handedness confusion** — left-handed Z-up vs right-handed Y-up | High | Model renders upside-down or mirrored | §6.6 documents the single conversion function `to_raylib(Ppoint)`. The first viewer smoke test (Phase V1) checks "is the aircraft right-side up?" via VLM. |
| **Texture chroma key not respected** — blue pixels show as opaque | Low | Visual artifacts only | `DecodedTexture::rgba8` premultiplies alpha: any pixel matching `chroma_key` gets alpha=0. The viewer's mesh material uses `BLEND_ALPHA` if any texture in the LOD has `has_alpha == true`. |

### 14.2 Scope Risks

| Risk | Mitigation |
|------|------------|
| **Scope creep into `.DDS` support** — "just one more format" pressure | §1.2 explicitly lists `.DDS` as Non-Goal. Adding it requires a separate plan amendment and a Phase V3 milestone. |
| **Scope creep into round-trip re-encoding** — "while we have the writer, just add the encoder" | §1.2 explicitly lists round-trip as Non-Goal. The writer interface is **not** stubbed — it doesn't exist. Adding it later is a clean greenfield, not a stub-completion. |
| **f4-world-viewer feature requests leak into f4-models-viewer** — "add the campaign map too" | The two viewers are siblings, not layers. f4-models-viewer never depends on f4-world-viewer. If a shared feature emerges (e.g. procedural symbols), extract to a shared utility lib — don't couple the viewers. |

### 14.3 Schedule Risks

| Risk | Mitigation |
|------|------------|
| **Phase M1 slips because real fixtures are hard to slice** | The slicing script (`extract_model_fixtures.py`) is Phase M1 step 9, but it can be parallelized: write the parser against synthetic minimal fixtures first, swap in real fixtures once the slicer is ready. |
| **Phase V1 visual validation is subjective** | The VLM smoke test (§10.3) gives a binary pass/fail. If the VLM is unreliable, fall back to a human reviewer for the first parent, then trust the test for regressions. |
| **glTF-Validator integration takes longer than estimated** | The validator is a Rust binary with a JSON output mode. If `FetchContent` is flaky, fall back to a `find_program(gltf_validator)` lookup and document "install manually" in the README. |

---

## Appendix A — Open Questions

These are flagged for resolution during implementation, not blockers for starting:

1. **Fixture sourcing**: Which FreeFalcon install do we slice `KoreaObj.*` from? Default: the latest `freefalcon-central` installer's `res/terrdata/KoreaObj.*`. Need to confirm licensing allows committing ~7 MB of fixtures.

2. **`f4-lzss` API shape**: Should it expose one `decompress(bytes_view, params)` with a params struct, or two separate `decompress_cam` / `decompress_tex` functions? Default: one parameterized function; the `.cam` and `.TEX` paths differ only in window size.

3. **Switch node export strategy**: §7.5 proposes (a) separate meshes per switch combination. For a parent with 8 switches each having 2 children, that's 2^8 = 256 meshes — likely too many. Fallback: export only the switch combinations that have non-zero mask in the default instance state (usually 1–4).

4. **Script-driven DOF identification**: How do we know which DOF index is the "rotor" for `Rotate.ANS`? FreeFalcon's `ScriptArray[0]` hardcodes the DOF index per parent. For `f4-models`, we either (a) hardcode the same per-parent DOF indices (brittle) or (b) require the user to pick the DOF in the animation panel. Default: (b) for v1; revisit if (a) becomes necessary.

5. **glTF extension for switch masks**: §7.5 mentions a custom `F4_switch` extension. Should we draft it? Default: no — keep the exporter standard-glTF-only for v1. Custom extensions are a Phase E2 stretch goal.

---

## Appendix B — Reference: Existing f4-world-viewer Patterns to Copy Verbatim

| Pattern | Source file | Copy to |
|---------|-------------|---------|
| Pimpl `ViewerApp` lifecycle | `f4-world-viewer/include/f4/viewer/viewer_app.hpp` | `f4-models-viewer/include/f4/models_viewer/viewer_app.hpp` |
| `viewer_state.hpp` private header | `f4-world-viewer/src/viewer_state.hpp` | `f4-models-viewer/src/viewer_state.hpp` |
| `settings.cpp` JSON round-trip | `f4-world-viewer/src/settings.cpp` | `f4-models-viewer/src/settings.cpp` (rename `WorldSettings` → `ModelSettings`) |
| `file_dialog.cpp` tinyfiledialogs wrapper | `f4-world-viewer/src/file_dialog.cpp` | `f4-models-viewer/src/file_dialog.cpp` (verbatim) |
| `enum_text.hpp` inline decoders | `f4-world-viewer/include/f4/viewer/enum_text.hpp` | `f4-models-viewer/include/f4/models_viewer/enum_text.hpp` (new decoders for `BNodeType`, `DofType`, etc.) |
| `--screenshot` detached-thread exit | `f4-world-viewer/cli/main.cpp` | `f4-models-viewer/cli/main.cpp` (verbatim) |
| Hex Inspector model/view split | `f4-world-viewer/src/hex/` | `f4-models-viewer/src/hex/` (verbatim, then extend with new decoders) |
| Tests link only pure-data .cpps | `f4-world-viewer/tests/CMakeLists.txt` | `f4-models-viewer/tests/CMakeLists.txt` (same pattern: link `model_inspector.cpp` + `enum_text.hpp` + `settings.cpp` directly, not the full viewer lib) |
| VS_DEBUGGER_* properties | `f4-world-viewer/CMakeLists.txt` | `f4-models-viewer/CMakeLists.txt` (same F5-launches-fixtures pattern) |

# Falcon 4.0 / FreeFalcon — On-Disk File Layout

This document is the working reference for the F4 engine-agnostic reimplementation
project. It catalogs every Falcon 4.0 / FreeFalcon data file we know about,
where it lives in a typical install, what it contains, and whether we already
parse it.

It is **living documentation** — when a new file format is reverse-engineered,
add a section. When a parser is added or extended, update the "F4 status" line.
The snapshot tool (`Tools > Snapshot Install Files...` in the viewer, or
`--snapshot <path>` on the CLI) is the canonical way to get real bytes from a
real install to ground-truth struct layouts while writing parsers.

A companion tool — the recursive file listing (`Tools > List All Install Files...`
in the viewer, or `--list-files <path>` on the CLI) — produces a plain-text
manifest of every regular file under the install root (relative path + size in
bytes, sorted within each directory). Running it on multiple installs (vanilla /
FreeFalcon / BMS) and sharing the outputs lets us document each layout
side-by-side and simplify the file-search logic in `f4-install`. The output
format is:

```
=== F4 INSTALL SNAPSHOT ===
generated:                2026-08-04T12:34:56Z
install_root:             C:/Falcon4
install_valid:            yes
per_file_byte_cap:        8192
include_tail:             no
full_recursive_listing:   yes
skip_curated_dumps:       yes
file_count:               17

--- Resolved install paths ---
  class_table:  C:/Falcon4/FALCON4.CT
  aircraft_dir: C:/Falcon4/sim
  ...

--- Theaters (1) ---
  Korea (korea)
    dir: C:/Falcon4/terrdata/korea
    THEATER.MAP: present (1024 bytes)
    ...

--- Campaigns (1) ---
  save1  [korea]
    save1.cam: present (524288 bytes)

=== FULL RECURSIVE FILE LISTING ===
walk_root: C:/Falcon4
  FALCON4.CT                                                 12345 bytes
  sim/F-16.dat                                                8192 bytes
  sim/F-18.dat                                                8192 bytes
  terrdata/objects/Falcon4.OCD                               73728 bytes
  terrdata/objects/Falcon4.PHD                                5120 bytes
  terrdata/korea/THEATER.MAP                                 65536 bytes
  terrdata/korea/THEATER.MEA                               8388608 bytes
  ...
total_files: 1247
total_dirs: 87
total_bytes: 4567890123

=== END OF SNAPSHOT ===
files_dumped: 0
files_missing: 0
files_listed: 1247
dirs_traversed: 87
total_bytes_listed: 4567890123
```

The recursive listing uses forward slashes for the relative path (cross-platform
readability), does NOT follow symlinks (so symlink loops can't crash the walk),
and reports any per-entry errors inline rather than aborting the walk.

---

## 1. Install root layout

A standard Falcon 4.0 / FreeFalcon install has the following top-level shape:

```
<install>/
├── falcon4.exe                  (Windows) or Falcon4.ai (Allied Force)
├── FALCON4.ct                   Class table — entity_type → ObjectiveType/UnitSubtype
├── FALCON4.PHD                  (sometimes at root, usually under terrdata/objects/)
├── sim/                         Aircraft .dat files (one per flyable airframe)
├── campaign/                    Saved campaign files (.cam archives)
├── terrdata/                    Per-theater static data — the bulk of the install
│   ├── theater.lst              Plain-text list of theater directory names
│   ├── objects/                 Static per-theater object data (see §3)
│   ├── ai/                      AI / simulation tuning (see §4)
│   ├── weather/                 Weather templates
│   ├── terrain/                 (some installs) shared terrain textures
│   └── <theater-key>/           One directory per theater (korea, balkans, …)
│       ├── THEATER.MAP          Header + palette (required)
│       ├── THEATER.MEA          Elevation grid (required)
│       ├── THEATER.O2           Secondary overlay (optional)
│       ├── THEATER.L0..L5       Per-LOD post data (optional, large)
│       ├── theater.ini          Display name + config
│       └── …                    Textures, models, sounds, coast/road/rail
├── art/                         3D models + textures (some installs)
├── sounds/                      Audio assets
├── ui/                          UI art + config
└── config/                      User settings
```

**Detection** is handled by `f4-install` (`f4::install::Installation::detect(root)`).
A directory is considered a valid install if it contains `FALCON4.ct` **and/or**
a `terrdata/` subdirectory. We do not require `falcon4.exe` so dev/CI trees
that only ship data files still validate.

**Case sensitivity**: Falcon 4.0 ships uppercase filenames, FreeFalcon ships
mixed case, and Linux/Wine installs may have any case depending on extraction.
All path resolution in `f4-install` and in the snapshot tool is case-insensitive.

---

## 2. Save files (`.cam` archives)

| Path                          | Format             | F4 status                       |
|-------------------------------|--------------------|---------------------------------|
| `campaign/<stem>.cam`         | LZSS archive       | **PARSED** by `f4-world-convert/cam_archive` |
| `campaign/<stem>.world.json`  | Our JSON export    | **PARSED** by `f4-world`        |

A `.cam` is an LZSS-compressed archive containing several inner files, each
of which is a typed binary record stream:

| Inner file | Records                       | F4 parser                       |
|------------|-------------------------------|---------------------------------|
| `.cmp`     | CampaignClass                 | `campaign_decoder.cpp`          |
| `.tea`     | TeamClass + ATMAirbaseClass   | `team_decoder.cpp` (partial — ATM block not yet decoded) |
| `.obj`     | ObjectiveClass + features     | `objective_decoder.cpp`         |
| `.unit`    | UnitClass                     | `unit_decoder.cpp`              |
| `.pilot`   | PilotClass                    | (not yet parsed)                |
| `.team`    | (team bits)                   | `team_decoder.cpp`              |
| `.victory` | VictoryClass                  | (not yet parsed)                |
| `.oob`     | Order-of-battle               | (not yet parsed)                |
| `*.key`    | (encryption keys, if present) | skipped                         |

**Curated**: the EXPOSE-1 task (see `worklog.md`) added `fstatus[]`,
`RadarRangeClass.detect_ratio[]`, `waypoints[]`, `airbase_id`, `roster`, and
team-name resolution. Still-missing `.cam` subdata: ATM airbase schedule
(`.tea` post-TeamClass block), pilot roster, OOB tree, victory conditions.

---

## 3. Static per-theater object data — `terrdata/objects/`

This is the highest-leverage remaining gap. Every file in this directory is
**static** (loaded once at startup, never written to by the game) and contains
the type/feature/vehicle definitions that the `.cam` save format references
by index.

| File              | C struct (FreeFalcon)         | Records                | F4 status                  |
|-------------------|-------------------------------|------------------------|----------------------------|
| `Falcon4.ct`      | `ClassTableEntry[]`           | entity_type → type/subtype map | **PARSED** by `f4-world-convert/class_table` |
| `Falcon4.PHD`     | `PtHeaderDataType[]`          | per-airbase layout header (runway heading, runway length, parking counts, links into PD) | ✅ **PARSED** — `theater_data.hpp` (verified against real data) |
| `Falcon4.PD`      | `PtDataType[]`                | runway / taxi / parking points (linked-list traversal) | ✅ **PARSED** — `theater_data.hpp` (verified) |
| `Falcon4.OCD`     | `ObjClassDataType[]`          | objective class name + data (radar ranges, features, unit slots, supply) | ✅ **PARSED** — `theater_data.hpp` (verified) |
| `Falcon4.UCD`     | `UnitClassDataType[]`         | unit class composition (vehicle type + count per group, default roster, icons) | ✅ **PARSED** — `theater_data.hpp` (verified) |
| `Falcon4.VCD`     | `VehicleClassDataType[]`      | per-vehicle data (name, speed, armor, weapons, 3D model index) | ✅ **PARSED** — `theater_data.hpp` (verified) |
| `Falcon4.FED`     | `FeatureEntry[]`              | feature placements (offset/facing per feature on each objective) | ✅ **PARSED** — `theater_data.hpp` (verified) |
| `Falcon4.FCD`     | `FeatureClassDataType[]`      | feature class definitions (name, hit points, repair time) | ✅ **PARSED** — `theater_data.hpp` (verified) |
| `Falcon4.OTD`     | `ObjectiveTypeData[]`         | objective type table (may be absent in some installs) | **NOT PARSED** — low priority |
| `Falcon4.ORD`     | `ObjectiveRadarData[]`        | per-objective radar data (may be absent) | **NOT PARSED** — low priority |
| `Falcon4.RCD`     | `RadarClassData[]`            | radar class definitions (range + detection ratios) | **NOT PARSED** — 56 records × 60 bytes confirmed in snapshot |

### 3.1 `Falcon4.PHD` — PtHeaderDataType

Per-airbase ground-layout header. **One record per point-list** (runway, taxi
chain, parking row, etc.) — NOT one per airbase. An airbase with 2 runways +
3 taxiways + 1 parking row has 6 PHD records chained via `nextHeader`.

**Verified source**: `src/falclib/include/entity.h:177-191` in the FreeFalcon tree.

**Verified on-disk record size**: 28 bytes (MSVC default 8-byte alignment).

```
off 0: objID        (short, 2)  — ID of the objective this layout belongs to
off 2: type         (uchar, 1) — PointListType (1=Runway, 8=RunwayDim, 11=Parking, ...)
off 3: count        (uchar, 1) — # of PtData points in this list
off 4: features[5]  (uchar[5]) — feature indices this list depends on (255 = unused)
off 9: *1 byte pad*             — MSVC: aligns `data` to 2-byte boundary
off10: data         (short, 2) — type-specific (e.g. runway heading in degrees)
off12: sinHeading   (float, 4) — sin(heading) — precomputed for runtime speed
off16: cosHeading   (float, 4) — cos(heading)
off20: first        (short, 2) — index of first PtData point in Falcon4.PD
off22: texIdx       (short, 2) — texture index for runway rendering
off24: runwayNum    (char, 1) — which runway this list applies to (-1 if N/A)
off25: ltrt         (char, 1) — left/right offset flag
off26: nextHeader   (short, 2) — index of next header in chain (0 = end)
```

**Verification**: For record 1 of the real Falcon4.PHD, `data=20` (heading
20°) and `sinHeading=0.342`, `cosHeading=0.940` — these match sin(20°) and
cos(20°) exactly. ✓

The ATC runtime class (`ATCBrain` in FreeFalcon) is **not serialized** — it
is rebuilt from PHD + PD + the objective `fstatus[]` byte array at campaign
load time.

### 3.2 `Falcon4.PD` — PtDataType

One record per ground-layout point. **Verified source**:
`src/falclib/include/entity.h:193-198`.

**Verified on-disk record size**: 12 bytes.

```
off 0: xOffset  (float, 4) — X offset (feet) from objective tile center
off 4: yOffset  (float, 4) — Y offset (feet) from objective tile center
off 8: type     (uchar, 1) — PointType (see §3.3)
off 9: flags    (uchar, 1) — PT_FIRST / PT_LAST / PT_OCCUPIED
off10: *2 bytes trailing pad* — MSVC: struct size must be multiple of 4
```

The linked-list traversal lives in the PHD side: PHD's `first` + `count`
fields define a contiguous slice of PD records that form one logical path
(runway centerline, taxiway, parking row, etc.). This is NOT a per-record
`next` pointer as previously documented — it's a slice indexed by
`PHD.first .. PHD.first + PHD.count - 1`.

**Verification**: For record 1 of real Falcon4.PD, `xOffset=2699 ft`,
`yOffset=2956 ft`, `type=1` (RunwayPt), `flags=1` (PT_FIRST) — sensible
runway threshold coordinates. ✓

### 3.3 `PtDataType.type` enum

Verified against `src/falclib/include/ptdata.h:40-60` in the FreeFalcon tree.

| Value | Constant             | Meaning                          |
|-------|----------------------|----------------------------------|
| 0     | `PT_NOT_USED`        | empty slot                       |
| 1     | `PT_RUNWAY`          | runway threshold (takeoff/landing end) |
| 2     | `PT_TAKEOFF`         | takeoff position (held short of runway) |
| 3     | `PT_TAXI`            | taxiway node                     |
| 4     | `PT_SAM`             | SAM site placement               |
| 5     | `PT_ARTILLERY`       | artillery placement              |
| 6     | `PT_AAA`             | AAA placement                    |
| 7     | `PT_RADAR`           | radar placement                  |
| 8     | `PT_RUNWAY_DIM`      | runway dimensional point (length/width marks) |
| 9     | `PT_SUPPORT`         | support vehicle placement        |
| 10    | `PT_STATIC_RADAR`    | static radar (building-sized)    |
| 11    | `PT_SMALL_PARK`      | small parking spot (fighters)    |
| 12    | `PT_LARGE_PARK`      | large parking spot (transports/bombers) |
| 13    | `PT_SMALL_DOCK`      | small dock (small boats)         |
| 14    | `PT_LARGE_DOCK`      | large dock (capital ships)       |
| 15    | `PT_TAKE_RUNWAY`     | runway access point (taxiway → runway) |
| 16    | `PT_HELICOPTER`      | helicopter pad                   |
| 17    | `PT_FOLLOW_ME`       | follow-me truck rendezvous       |
| 18    | `PT_TRACK`           | track/path point (ground vehicle routes) |
| 19    | `PT_CRIT_TAXI`       | critical taxiway intersection    |

### 3.4 `Falcon4.OCD` — ObjClassDataType

Per objective class. **Verified source**: `src/falclib/include/entity.h:66-79`.

**Verified on-disk record size**: 54 bytes.

```
off 0: Index          (short, 2)   — descriptionIndex pointing back into FALCON4.ct
off 2: Name[20]       (char[20])   — class name (e.g. "02_20 Airbase 2", "Highway Strip NS",
                                       "Armybase 1", "Border", "044 Bridge 6", "304 BioWeapons")
off22: DataRate       (short, 2)   — sort/recovery rate
off24: DeagDistance   (short, 2)   — distance (m) at which to deaggregate
off26: PtDataIndex    (short, 2)   — index into PtHeaderDataTable (airbase layout)
off28: Detection[8]   (uchar[8])   — electronic detection ranges per movement type
off36: DamageMod[11]  (uchar[11])  — damage modifiers per damage type
off47: *1 byte pad*                 — MSVC: aligns IconIndex to 2-byte boundary
off48: IconIndex      (short, 2)   — index into icon sheet
off50: Features       (uchar, 1)   — # of features in this objective
off51: RadarFeature   (uchar, 1)   — ID of the radar feature (255 = none)
off52: FirstFeature   (short, 2)   — index of first FeatureEntry in FED
```

**Verification**: Real Falcon4.OCD record 1 has name "02_20 Airbase 2",
features=108, pt_data_index=1, first_feature=1. Record 4 ("Border") has
radar_feature=255 (no radar). All names parse as clean ASCII. ✓

### 3.5 `Falcon4.UCD` — UnitClassDataType

Per unit class. **Verified source**: `src/falclib/include/entity.h:30-54`.

**Verified on-disk record size**: 336 bytes.

```
off   0: Index                (short, 2)
off   2: *2 bytes pad*        — MSVC: aligns NumElements to 4-byte boundary
off   4: NumElements[16]      (int[16], 64)   — per-group vehicle count
off  68: VehicleType[16]      (short[16], 32) — class-table index per group
off 100: VehicleClass[16][8]  (uchar[128])    — 8-byte entity class descriptors
off 228: Flags                (ushort, 2)     — VEH_ capability flags
off 230: Name[20]             (char[20])      — e.g. "Airlift", "Patrol", "Supply",
                                                 "Air Defense", "Armored"
off 250: *2 bytes pad*        — MSVC: aligns MovementType to 4-byte boundary
off 252: MovementType         (int, 4)        — MoveType enum (1=Foot, 2=Wheeled, 3=Tracked,
                                                 4=LowAir, 5=Air, 6=Naval, 7=Rail)
off 256: MovementSpeed        (short, 2)      — cruise speed (kph)
off 258: MaxRange             (short, 2)      — movement/flight range (km)
off 260: Fuel                 (long, 4)       — internal fuel (lbs)
off 264: Rate                 (short, 2)      — fuel usage (lbs/min at cruise)
off 266: PtDataIndex          (short, 2)      — index into PtHeaderDataTable (formation)
off 268: Scores[16]           (uchar[16])     — score per mission role
off 284: Role                 (uchar, 1)      — standard mission role
off 285: HitChance[8]         (uchar[8])      — best hit chance per movement type
off 293: Strength[8]          (uchar[8])      — full strength per movement type
off 301: Range[8]             (uchar[8])      — firing range per movement type
off 309: Detection[8]         (uchar[8])      — electronic detection ranges
off 317: DamageMod[11]        (uchar[11])     — damage modifiers per type
off 328: RadarVehicle         (uchar, 1)      — ID of radar vehicle (group index)
off 329: *1 byte pad*         — MSVC: aligns SpecialIndex to 2-byte boundary
off 330: SpecialIndex         (short, 2)      — for squadrons: index to max stores table
off 332: IconIndex            (short, 2)      — icon index into icons sheet
off 334: *2 bytes trailing pad* — MSVC: struct size must be multiple of 4
```

**Verification**: Record 1 has name="Airlift", movement_type=5 (Air),
movement_speed=999, max_range=400, fuel=30, rate=100, role=20. Record 2 has
name="Patrol", movement_type=6 (Naval), num_elements=[1,1,0,...],
vehicle_type=[578,578,0,...]. All sensible. ✓

### 3.6 `Falcon4.VCD` — VehicleClassDataType

Per vehicle class. **Verified source**: `src/falclib/include/entity.h:137-167`.

**Verified on-disk record size**: 160 bytes. **No mid-struct padding** — every
field is naturally aligned at its current offset.

```
off   0: Index             (short, 2)
off   2: HitPoints         (short, 2)
off   4: Flags             (uint, 4)       — VEH_ flags
off   8: Name[15]          (char[15])      — e.g. "An-70", "E-3", "M-1A1", "A-10",
                                              "B-52G", "MiG-29", "C-130", "M-60"
off  23: NCTR[5]           (char[5])       — NCTR (non-cooperative target recognition) string
off  28: RCSfactor         (float, 4)      — log2(1 + RCS relative to F-16)
off  32: MaxWt             (long, 4)       — max loaded weight (lbs)
off  36: EmptyWt           (long, 4)       — empty weight (lbs)
off  40: FuelWt            (long, 4)       — max fuel weight (lbs)
off  44: FuelEcon          (short, 2)      — fuel usage (lbs/min)
off  46: EngineSound       (short, 2)      — SoundFX sample index
off  48: HighAlt           (short, 2)      — in hundreds of feet
off  50: LowAlt            (short, 2)
off  52: CruiseAlt         (short, 2)
off  54: MaxSpeed          (short, 2)      — kph
off  56: RadarType         (short, 2)      — index into RadarDataTable (Falcon4.RCD)
off  58: NumberOfPilots    (short, 2)      — # of pilots (for eject)
off  60: RackFlags         (ushort, 2)     — bit per hardpoint: needs a rack?
off  62: VisibleFlags      (ushort, 2)     — bit per hardpoint: visible?
off  64: CallsignIndex     (uchar, 1)
off  65: CallsignSlots     (uchar, 1)
off  66: HitChance[8]      (uchar[8])      — per-movement-type hit chance
off  74: Strength[8]       (uchar[8])
off  82: Range[8]          (uchar[8])
off  90: Detection[8]      (uchar[8])
off  98: Weapon[16]        (short[16], 32) — weapon ID per hardpoint (or weapon list ID)
off 130: Weapons[16]       (uchar[16])     — # of shots per hardpoint (full supply)
off 146: DamageMod[11]     (uchar[11])     — damage modifiers per type
off 157: *3 bytes trailing pad* — MSVC: struct size must be multiple of 4
```

**Verification**: Record 1 has name="An-70", hit_points=150, flags=1105,
rcs_factor=3.4594. Record 2 has name="E-3", max_wt=325000, empty_wt=170277,
fuel_wt=155450, fuel_econ=235, max_speed=853, radar_type=18. Record 4 has
name="A-10", max_wt=50000, max_speed=680. All match real aircraft. ✓

### 3.7 `Falcon4.FED` / `Falcon4.FCD` — Features

`FED` (FeatureEntry) is the per-objective-type feature placement table —
which features an objective has, and where they go (offset from center,
facing). `FCD` (FeatureClassDataType) is the feature class table — name,
hit points, repair time, radar type.

**Verified source**: `src/falclib/include/entity.h:56-64` (FeatureEntry),
`src/falclib/include/entity.h:122-135` (FeatureClassDataType).

**Verified FED record size**: 32 bytes.

```
off 0: Index      (short, 2)    — entity class index of the feature
off 2: Flags      (ushort, 2)
off 4: eClass[8]  (uchar[8])    — 8-byte entity class descriptor
off12: Value      (uchar, 1)    — % loss in operational status for destruction
off13: *3 bytes pad*            — MSVC: aligns Offset to 4-byte boundary (vector is 4-aligned)
off16: Offset.x   (float, 4)    — X offset from objective center (feet)
off20: Offset.y   (float, 4)    — Y offset from objective center (feet)
off24: Offset.z   (float, 4)    — Z offset from objective center (feet)
off28: Facing     (Int16, 2)    — facing angle (degrees)
off30: *2 bytes trailing pad*   — MSVC: struct size must be multiple of 4
```

**Verified FCD record size**: 60 bytes.

```
off 0: Index        (short, 2)   — descriptionIndex pointing here
off 2: RepairTime   (short, 2)   — seconds to repair from destroyed to operational
off 4: Priority     (uchar, 1)   — display priority
off 5: *1 byte pad*               — MSVC: aligns Flags to 2-byte boundary
off 6: Flags        (ushort, 2)   — FEAT_ flags
off 8: Name[20]     (char[20])    — e.g. "Bridge", "Bush", "Control Tower", "Fuel Tank",
                                     "Hangar", "Runway", "Tree", "Barracks", "High rise",
                                     "Mayday Stadium"
off28: HitPoints    (short, 2)
off30: Height       (short, 2)    — height of vehicle ramp (if any)
off32: Angle        (float, 4)    — angle of vehicle ramp (if any)
off36: RadarType    (short, 2)    — index into RadarDataTable (Falcon4.RCD)
off38: Detection[8] (uchar[8])
off46: DamageMod[11](uchar[11])
off57: *3 bytes trailing pad*     — MSVC: struct size must be multiple of 4
```

**Verification**:
- FED record 2 has index=987, offset=(1368, 152, 0) ft, facing=20°.
- FED record 3 has index=995, offset=(3193, 2838, 0) ft, facing=20°.
- FCD record 1 has name="Bridge", repair_time=72, hit_points=500.
- FCD record 3 has name="Control Tower", repair_time=48, radar_type=32,
  detection=[0,0,0,0,40,100,0,0] (has radar detection capability).
All match expected real-world values. ✓

`fstatus[]` (the byte array on each objective, exposed in EXPOSE-1) is a
2-bit-per-feature damage bitmap indexing into FED's feature list for that
objective's class. So FED + `fstatus[]` together give you the live damage
state of every building/runway/feature on every objective.

---

## 4. AI / simulation tuning — `terrdata/ai/`

| File             | Format          | Contents                                | F4 status                  |
|------------------|-----------------|-----------------------------------------|----------------------------|
| `Falcon4.AII`    | INI (text)      | `SIM_BUBBLE_SIZE`, `GROUND_BUBBLE_SIZE`, bubble scale factors, ATC tuning | **NOT PARSED** — referenced from FreeFalcon source but not yet loaded |
| `Falcon4.AIL`    | binary (table)  | AI logic table (skill modifiers, target priorities) | **NOT PARSED** — low priority |
| `Falcon4.SAI`    | binary          | Strategic AI weights                    | **NOT PARSED** — low priority |

### 4.1 `Falcon4.AII`

Standard Windows INI format. Key sections we care about:

```ini
[Sim]
SIM_BUBBLE_SIZE    = 2.5    ; air-sim deagg bubble, grid units
GROUND_BUBBLE_SIZE = 1.0    ; ground-sim deagg bubble, grid units
```

`SIM_BUBBLE_SIZE` is read by `UnitClass::Deaggregate()` to decide when to
promote a low-fidelity unit (one icon, no individual vehicles) to high
fidelity (full vehicle entities). Without parsing `Falcon4.AII`, we cannot
match the game's deaggregation behavior — this blocks Phase 6 of
`f4-simulation`.

---

## 5. Terrain data — `terrdata/<key>/`

Already documented in `f4-install/include/f4/install/theater.hpp` and parsed
by `f4-terrain` + `f4-terrain-convert`. Included here for completeness:

| File            | Format          | Contents                                | F4 parser            |
|-----------------|-----------------|-----------------------------------------|----------------------|
| `THEATER.MAP`   | binary header + palette | Theater dimensions, color palette | `f4-terrain`         |
| `THEATER.MEA`   | binary (elevation grid) | Per-grid-post elevation, in meters | `f4-terrain`         |
| `THEATER.O2`    | binary (overlay) | Secondary overlay (coast lines, road/rail polylines) | `f4-terrain` (partial) |
| `THEATER.L0..L5`| binary (LOD posts) | Per-LOD elevation / texture data   | `f4-terrain` (loaded but not yet rendered) |
| `theater.ini`   | INI (text)      | `[Theater].Title`, `[Theater].DataDir`   | `f4-install`         |
| `theater.lst`   | text            | List of theater keys (one per line)     | `f4-install`         |

---

## 6. Aircraft data — `sim/`

| File              | Format          | Contents                                | F4 parser            |
|-------------------|-----------------|-----------------------------------------|----------------------|
| `sim/<aircraft>.dat` | binary | Flight-model coefficients (aero derivatives, engine tables, gear, FCS) | `f4-convert/dat_parser` |

The `f4-convert` library already parses these into JSON for `f4-flight-model`.
Sample aircraft are bundled in `f4-convert/tests/fixtures/`. The full set
(~30 flyable airframes in a typical FreeFalcon install) is enumerated by
scanning `sim/`.

---

## 7. Snapshot tool

The snapshot tool is the bridge between this document and the actual
parser-writing work. It produces a single plain-text file containing
hex+ASCII dumps of the first 8 KB of every interesting data file in a
real install, so the dev team can ground-truth struct layouts without
needing direct access to a Falcon install.

### 7.1 Usage

**GUI**: With an install set, `Tools > Snapshot Install Files...` opens a
save-file dialog. The default output is
`<install_root>/f4_install_snapshot_<UTC-timestamp>.txt`.

**CLI**: `f4-world-viewer --install /path/to/falcon4 --snapshot out.txt`
writes the snapshot to `out.txt` and exits (no GUI).

### 7.2 What gets dumped

The curated list is hard-coded in `f4-world-viewer/src/snapshot.cpp` and
mirrors §2–§4 of this document:

1. `FALCON4.ct`
2. `terrdata/objects/Falcon4.PHD`
3. `terrdata/objects/Falcon4.PD`
4. `terrdata/objects/Falcon4.OCD`
5. `terrdata/objects/Falcon4.UCD`
6. `terrdata/objects/Falcon4.VCD`
7. `terrdata/objects/Falcon4.FED`
8. `terrdata/objects/Falcon4.FCD`
9. `terrdata/objects/Falcon4.OTD`
10. `terrdata/objects/Falcon4.ORD`
11. `terrdata/objects/Falcon4.RCD`
12. `terrdata/ai/Falcon4.AII`
13. `terrdata/ai/Falcon4.AIL`
14. `terrdata/ai/Falcon4.SAI`
15. `terrdata/theater.lst`
16. `sim/F-16.dat`
17. `sim/F-18.dat`

For each: file header (path, size, bytes dumped), then a classic xxd-style
hex+ASCII dump (8-hex offset, 16 bytes/line, ASCII column with `.` for
non-printables).

A catch-all section at the end lists (filenames + sizes only) every file
in `terrdata/`, `terrdata/objects/`, `terrdata/ai/`, `terrdata/weather/`,
`terrdata/terrain/`, `sim/`, and `campaign/` — catches anything the
curated list missed.

### 7.3 Snapshot file format

```text
=== F4 INSTALL SNAPSHOT ===
generated: 2026-08-04T12:34:56Z
install_root: C:/Falcon4
install_valid: yes
per_file_byte_cap: 8192
include_tail: no
file_count: 17

--- Resolved install paths ---
  class_table:  C:/Falcon4/FALCON4.CT
  aircraft_dir: C:/Falcon4/sim
  ...

--- Theaters (1) ---
  Korea (korea)
    dir: C:/Falcon4/terrdata/korea
    THEATER.MAP: present (1024 bytes)
    ...

--- Campaigns (3) ---
  save1  [korea]
    C:/Falcon4/campaign/save1.cam: present (482310 bytes)
  ...

=== CURATED FILE DUMPS ===

=== FILE 1/17: terrdata/objects/Falcon4.PHD ===
path: C:/Falcon4/terrdata/objects/Falcon4.PHD
size: 5120 bytes  (dumping first 5120 bytes)
--- hex dump ---
00000000  50 48 44 01 00 00 80 00  00 00 00 80 00 00 00 80  |PHD.............|
00000010  ...

=== FILE 2/17: ... ===
...

=== DIRECTORY LISTINGS (catch-all) ===
--- terrdata/objects/ ---
  ...
--- terrdata/ai/ ---
  ...

=== END OF SNAPSHOT ===
files_dumped: 12
files_missing: 5
```

---

## 8. Worked example: writing a parser from a snapshot

When the dev team receives a snapshot file, the workflow to add a new
parser is:

1. **Identify the C struct** from the FreeFalcon source (see the table
   in §3 — each row links to the source file).
2. **Open the snapshot** to the corresponding `=== FILE N/M: <path> ===`
   section.
3. **Map the struct fields to byte offsets** by reading the hex dump.
   MSVC pack rules: structs are packed to the alignment of the largest
   member; `char[]` members have no padding; `int`/`float` are 4-byte
   aligned; `short` is 2-byte aligned. Doubly-linked-list indices are
   typically `int` (4 bytes).
4. **Write a cursor-based parser** in `f4-world-convert/` following the
   pattern of `objective_decoder.cpp` / `unit_decoder.cpp`:
   - Add a struct to a new header under `include/f4/world_convert/`.
   - Add a `parse_*.cpp` source under `src/`.
   - Add `parse_*()` to the cursor, called from `cam_archive.cpp` or
     from a new `static_data_loader.cpp`.
5. **Wire parsed data into `WorldState`** under `f4-world/` so the viewer
   and JSON export pick it up automatically.
6. **Add tests** under `f4-world-convert/tests/` that build a synthetic
   fixture (using the snapshot bytes as the gold reference) and assert
   the parser produces the expected struct values.
7. **Add a worklog entry** under the appropriate task ID.

The first parser to land this way will be `Falcon4.PHD` + `Falcon4.PD`
(task `STATIC-1`), giving us airbase ground geometry (runways, taxiways,
parking spots) for the ATC system.

---

## 9. Open questions / known unknowns

These are gaps in this document that the snapshot tool will help close:

- **OTD/ORD/RCD** — some installs have these, some don't. Need a snapshot
  from a Balkans + Iceland + Vietnam install to confirm what's universal.
- **Falcon4.AIL / Falcon4.SAI** — never seen the contents; the snapshot
  will tell us whether they're tabular or free-form.
- **ATM airbase schedule** — the post-TeamClass block in `.tea`. We know
  it's an `ATMAirbaseClass` array (40 bytes each: VU_ID + `schedule[32]`),
  but the format of the schedule bits is undocumented. A `.cam` snapshot
  (not yet supported by the snapshot tool, but easy to add) would help.
- **Objective instance names** — the `.obj` records have a
  `objective_name[]` field we already expose; the question is whether
  the snapshot's `Falcon4.OCD` dump matches the class names we'd expect
  for those instances.
- **BMS vs vanilla layout** — the `--list-files` flag (or its menu
  counterpart `Tools > List All Install Files...`) is the canonical way
  to ground-truth this. Run it on a vanilla install, a FreeFalcon
  install, and a BMS install, share the three output files, and the dev
  team can extend §1's install-root diagram to cover all three variants
  and simplify the file-search logic in `f4-install` (e.g. detect BMS
  installs by the presence of `Data/` or `Falcon BMS.cfg`, instead of
  scanning for `terrdata/`).

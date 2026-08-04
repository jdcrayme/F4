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
| `Falcon4.PHD`     | `PtHeaderDataType[]`          | per-airbase layout header (runway heading, runway length, parking counts, links into PD) | **NOT PARSED** — top priority |
| `Falcon4.PD`      | `PtDataType[]`                | runway / taxi / parking points (linked-list traversal) | **NOT PARSED** — top priority |
| `Falcon4.OCD`     | `ObjClassDataType[]`          | objective class name + data (radar ranges, features, unit slots, supply) | **NOT PARSED** — top priority |
| `Falcon4.UCD`     | `UnitClassDataType[]`         | unit class composition (vehicle type + count per group, default roster, icons) | **NOT PARSED** — high priority |
| `Falcon4.VCD`     | `VehicleClassDataType[]`      | per-vehicle data (name, speed, armor, weapons, 3D model index) | **NOT PARSED** — medium priority |
| `Falcon4.FED`     | `FeatureClassDataType[]`      | feature class definitions (name, model index, armor) | **NOT PARSED** — medium priority |
| `Falcon4.FCD`     | `FeatureDataType[]`           | feature instances on objectives (offset, heading, class index) | **NOT PARSED** — medium priority |
| `Falcon4.OTD`     | `ObjectiveTypeData[]`         | objective type table (may be absent in some installs) | **NOT PARSED** — low priority |
| `Falcon4.ORD`     | `ObjectiveRadarData[]`        | per-objective radar data (may be absent) | **NOT PARSED** — low priority |
| `Falcon4.RCD`     | `RadarClassData[]`            | radar class definitions (may be absent) | **NOT PARSED** — low priority |

### 3.1 `Falcon4.PHD` — PtHeaderDataType

Per-airbase ground-layout header. One record per airbase objective. Source:
`src/sim/include/atcbrain.h:154-200` in the FreeFalcon tree.

Each record contains:
- `ID` (VU_ID) — should match the airbase objective's VU_ID
- `runwayIdx` (int) — index into `Falcon4.PD` of the first runway point
- `runwayNum` (int) — number of runways (1 typically, 2 for big fields)
- `runwayHeading` (float) — heading in radians
- `dimension` (PtHeaderDimensions) — runway length / width
- `parkingSpots` (int) — count of SmallParkPt + LargeParkPt entries
- `taxiways` (int) — count of taxi links
- `links` — indices into `Falcon4.PD` forming the runway/taxi/parking graph

The ATC runtime class (`ATCBrain` in FreeFalcon) is **not serialized** — it
is rebuilt from this file + the objective `fstatus[]` byte array at campaign
load time.

### 3.2 `Falcon4.PD` — PtDataType

One record per ground point. Source: `src/sim/include/atcbrain.h:201-318` in
the FreeFalcon tree.

Each record contains:
- `type` (uchar) — point type (see §3.3)
- `x`, `y` (float) — local grid coords relative to airbase center
- `next` (int) — index of next point in linked list (-1 = end)
- `prev` (int) — index of previous point
- `branch` (int) — index of branch point (taxi intersections)
- `dist` (float) — distance to next point (used for ATC speed commands)

The linked-list traversal is the core data structure: a runway is a chain of
`RunwayPt` (type 1) terminated by `RunwayDimPt` (type 8). A taxiway is a
chain of `TaxiPt` (type 9). Parking spots are leaf nodes of type
`SmallParkPt` (11) / `LargeParkPt` (12).

### 3.3 `PtDataType.type` enum

| Value | Constant             | Meaning                          |
|-------|----------------------|----------------------------------|
| 0     | `NotUsed`            | empty slot                       |
| 1     | `RunwayPt`           | runway centerline point          |
| 2     | `RunwayExtPt`        | runway extension / overrun       |
| 3     | `TaxiPt` (ext)       | taxiway point (extended)         |
| 4     | `TaxiIntersectionPt` | taxi intersection                |
| 8     | `RunwayDimPt`        | runway threshold / dimension     |
| 9     | `TaxiPt`             | taxiway point                    |
| 10    | `TakeOffPt`          | takeoff position                 |
| 11    | `SmallParkPt`        | small parking spot (fighter)     |
| 12    | `LargeParkPt`        | large parking spot (cargo/bomber)|
| 13    | `RunwayPt` (alt)     | alternate runway point           |

### 3.4 `Falcon4.OCD` — ObjClassDataType

Per objective class. Source: `src/campaign/include/campobj.h` in the FreeFalcon
tree (the struct is large — ~560 bytes/entry).

Each record contains:
- `Name` (char[20]) — objective class name (e.g. `"Airbase"`, `"Port"`,
  `"Army Base"`)
- `Domain` (uchar) — air/ground/naval
- `RadarRange` (RadarRangeClass, 32 bytes) — 8 arcs × detect_ratio float
- `FeatureEntries` (int) — count of feature slots
- `Features` (FeatureEntry[]) — feature class index + offset within objective
- `Priority` (int) — strategic priority (affects AI target selection)
- `RepairTime`, `RebuildTime` (floats)
- `SupplyLevel`, `FuelLevel` (ints)

**Naming**: This is **the** source of objective names. Currently
`objective_type_name()` returns a static `"Objective#N"` string because we
don't load OCD. Once OCD is parsed, the inspector / canvas should display
the real name (`"Kunsan AB"`, not `"Objective#9"` — well, the per-instance
name actually lives in the `.obj` records as `objective_name[]`, but the
**class** name comes from OCD).

### 3.5 `Falcon4.UCD` — UnitClassDataType

Per unit class. Source: `src/campaign/include/campunit.h` in the FreeFalcon
tree.

Each record contains:
- `Name` (char[20]) — unit class name (e.g. `"M1A1 Tank Platoon"`)
- `Domain` (uchar)
- `Type` (uchar) — armor/infantry/AAA/SAM/…
- `VehicleType` (VehicleClassData[] indexed) — what vehicles this unit fields
- `TotalVehicles` (int) — total vehicles in unit
- `GroupCounts` (int[16]) — default per-group vehicle count (16 groups max)
- `Formation` (int) — default formation index
- `IconImage` (int) — icon index into the icons sheet

**Combines with `roster`** (uint32, 2 bits/group × 16 groups = live per-group
vehicle count, exposed in EXPOSE-1) to give per-group vehicle type + count at
runtime.

### 3.6 `Falcon4.VCD` — VehicleClassDataType

Per vehicle class. Source: `src/campaign/include/vehclas.h` in the FreeFalcon
tree.

Each record contains:
- `Name` (char[20])
- `Domain`, `Type` (uchar)
- `VehicleID` (int) — index into 3D model table
- `HitChance` (int) — base to-hit modifier
- `MaxSpeed`, `MaxRange` (int)
- `Fuel` (float)
- `SensorClass` (int) — radar/IR/visual flags
- `WeaponSlots` (WeaponSlot[]) — count + types of weapons carried

### 3.7 `Falcon4.FED` / `Falcon4.FCD` — Features

`FED` is the feature class table (names + 3D model index + armor + repair time).
`FCD` is the per-objective-type feature placement template (which features an
airbase has, and where). Together they define what gets rendered when an
objective is deaggregated: runway sections, hangars, fuel tanks, radar
antennas, etc.

`fstatus[]` (the byte array on each objective, exposed in EXPOSE-1) is a
2-bit-per-feature damage bitmap indexing into FCD's feature list for that
objective's type.

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

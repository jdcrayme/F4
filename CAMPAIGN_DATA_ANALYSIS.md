# Falcon 4.0 / FreeFalcon Campaign and Theater Information Analysis

This document provides a comprehensive analysis of the files and formats needed to load and display all information for a Falcon 4.0 combat theater (like Korea), as well as the corresponding C++ source files/classes in `FreeFalcon` responsible for parsing and handling them.

---

## 1. High-Level Loading Flow and Configuration

When FreeFalcon loads or switches to a theater like Korea, it relies on global structures to resolve directories for terrains, campaigns, 3D objects, and assets.

### Key Configuration Files:
- **`theater.lst`**: A plain text registry file located in the game's root directory listing the paths to all available theater definition files (e.g., `terrdata\theaterdefinition\korea.tdf`).
- **`<theater>.tdf` (Theater Definition File)**: A plain text configuration file specifying theater-specific metadata and resource directory paths (e.g., `campaigndir`, `terraindir`, `objectdir`, `3ddatadir`, `sounddir`, etc.).

### Responsible Source Files:
- **`src/falclib/theaterdef.cpp`**:
  - Implements `LoadTheaterList()`, which parses `theater.lst`.
  - Implements `LoadTheaterDef()`, which parses individual `<theater>.tdf` files using the descriptor `InputDataDesc theaterdesc[]`.
  - Implements `TheaterList::SetNewTheater()`, which does all the work of swapping theaters, resetting path variables (e.g., `FalconTerrainDataDir`, `FalconCampaignSaveDirectory`), re-attaching resource zip archives (`ziplist.lst`), reloading the class table (`LoadClassTable("Falcon4")`), and reloading the terrain (`TheaterReload()`).
- **`src/falclib/include/theaterdef.h`**: Declares the `TheaterDef` class and structure holding path variables.

---

## 2. Terrain & Map Data Loading

The geographical and structural world of the combat theater is represented by terrain files, elevation files, and cover grids.

### Key Terrain Data Files (typically in `<terraindir>`):
- **`<theater>.map` (Terrain Map)**: Holds segment and tile indices for the visual terrain renderer.
- **`<theater>.ter` (Terrain Elevations)**: Contains high-resolution elevation post heights.
- **`L2` / `O2` Files**: Represent Level-of-Detail (LOD) elevation and texture tile mapping.
- **`<scenario_name>.thr` (Campaign Terrain Cell Database)**:
  - Represents the campaign-level simplified grid database.
  - Dictates movement characteristics, cover, and strategic networks (roads and rails) for campaign-level AI routing.

### Responsible Source Files:
- **`src/campaign/camplib/campterr.cpp`**:
  - Implements `LoadTheaterTerrain(char *name)`, which reads `<scenario_name>.thr` via `ReadCampFile`.
  - Parses grid bounds `Map_Max_X` and `Map_Max_Y` and loads the 1-byte structured cell database `TheaterCells` (arrays of `CellDataType`).
  - Provides getters `GetRelief()`, `GetCover()`, `GetRoad()`, and `GetRail()` for navigation and pathfinding.
- **`src/campaign/include/campterr.h`**: Holds campaign-level terrain cell structures and shift/mask constants for extracting terrain features (cover type, roads, railways, relief).

---

## 3. Campaign Databases & Entity Collections

A campaign scenario is stored as a bundle of files containing all persistent world information (squadrons, airbases, strategic objectives, combat units, waypoints, and team configurations).

When loading a save game or starting a scenario, FreeFalcon parses multiple files sharing the same prefix base name (e.g., `Korea.cmp`, `Korea.cam`, `Korea.obd`, `Korea.uni`).

### Key Campaign Data Files (typically in `<campaigndir>`):

#### A. Campaign Header & Settings: `.cmp` (Campaign File)
- Holds general campaign variables (current game time, day, bullseye coordinates, theater size, active teams, scenario name, and overall stats).
- **Parsed by**: `src/campaign/campupd/cmpclass.cpp` via `CampaignClass::LoadCampaign()` and `CampaignClass::ReadData()`.

#### B. Objective Collections: `.obd` (Objective Delta / Base Objectives)
- Contains strategic locations, airbases, cities, defensive points, SAM radar sites, and harbors, along with their damage statuses, features (runways, buildings), supply levels, and links to neighboring objectives.
- **Parsed by**: `src/campaign/camplib/objectiv.cpp` via `LoadBaseObjectives()` and `ObjectiveClass::ObjectiveClass(VU_BYTE **stream)`.
  - Features statuses are read into `obj_data.fstatus`.
  - Links to neighboring objectives are parsed into `link_data` as arrays of `CampObjectiveLinkDataType`.

#### C. Unit Collections: `.uni` (Unit Database)
- Contains all military combat units (Air units like flights/squadrons, Ground units like battalions/brigades, and Naval units like task forces), their rosters, active strengths, current waypoint indexes, task/tactic modes, cargo IDs, and losses.
- **Parsed by**: `src/campaign/camplib/unit.cpp` via `LoadUnits()` and `UnitClass::UnitClass(VU_BYTE **stream)`.

#### D. Flight Waypoints: Attached inside `.uni` or `.cam` streams
- Holds flight plan waypoints including grid locations, speeds, altitudes, arrival/departure times, target IDs, target buildings, and actions (e.g., land, fly-by, orbit).
- **Parsed by**: `src/campaign/camplib/campwp.cpp` via `WayPointClass::WayPointClass(VU_BYTE **stream)`.

#### E. Team & Alliance Setup: Attached in save files or parsed from template files
- Manages alliances, colors, reinforcement schedules, and doctrine rules.
- **Parsed by**: `src/campaign/camplib/team.cpp` via `LoadTeams()`.

---

## 4. Class Tables and Database Assets

To decode the indices found in the binary campaign files (`.uni`, `.obd`) into real vehicle names, radar ranges, weapon details, and 3D visual models, the parser refers to the central Class Tables.

### Key Database Asset Files (typically in `<objectdir>`):
- **`Falcon4.ct` (Class Table)**: The master binary registry mapping every entity index to a type, domain, subclass, and class data pointer.
- **`FALCON4.idx` and `FALCON4.rsc`**: Asset bundles containing skins, UI art, 3D model vertices, and names.
- **`Strings.idx`**: Indexed strings mapping index numbers to text (e.g., airbase names, country names, military unit designation templates).

### Responsible Source Files:
- **`src/falclib/classtbl.cpp`**: Implements `LoadClassTable(char *name)`, which parses `Falcon4.ct` into `Falcon4ClassTable` structure pointers.
- **`src/falclib/entity.cpp`**: Sets up campaign-specific vehicle descriptors.
- **`src/campaign/camplib/aiinput.cpp`**: Implements `ReadCampAIInputs()`, which parses doctrines and rules of engagement (ROE).
- **`src/campaign/camplib/ptdata.cpp`**: Parses point databases (airbase runway runways, taxi points) so that deaggregated campaign airfields have physical structures.

---

## Summary Checklist for Parsing a Combat Theater

To build an external parser or visualizer for Falcon 4.0 / FreeFalcon theater data like Korea, you would need to parse files in this exact logical order:

1. **Locate theater path**: Read `theater.lst`, find `korea.tdf`, and extract paths for campaigns, terrain, and objects.
2. **Load class definitions**: Read `Falcon4.ct` so you can understand vehicle and objective class types.
3. **Load terrain grid**: Parse `<theater_name>.thr` to get the strategic cell maps (relief, cover type, road/rail overlay).
4. **Load scenario metadata**: Read `<scenario_name>.cmp` to establish the bullseye, game time, active teams, and theater grid dimensions.
5. **Load strategic objectives**: Parse `<scenario_name>.obd` to obtain all airbases, radar stations, factories, and SAM positions with their grid coordinates and links.
6. **Load military units**: Parse `<scenario_name>.uni` to locate ground divisions, squadron bases, and active combat units.
7. **Trace flight plans**: Decode waypoints embedded in unit records using the structures in `campwp.cpp`.
8. **Map visual names**: Decode the indices against strings extracted from `Strings.idx` (or other localization files).

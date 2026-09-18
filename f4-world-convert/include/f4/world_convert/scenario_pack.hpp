// f4-world-convert/include/f4/world_convert/scenario_pack.hpp
//
// The CAMP-INIT-1 scenario pack — the typed input of the
// CampaignInitializer (CAMP_HOST_PLAN.md §8). A pack is a complete,
// self-describing specification of a fresh campaign:
//
//   * the theater (name, grid extent, terrain reference),
//   * the OOB template (objectives with owners/priorities, squadrons
//     anchored to named airbases, battalions anchored to named sites),
//   * the force levels (per-team reserve aircraft, replacements,
//     experience),
//   * the date (the campaign's opening clock) and a weather seed
//     (metadata for hosts — the .wth sub-file carries no decoded
//     format, so the pack's weather block is carried, not encoded),
//   * and the SEED — the byte that lands in the .cmp's
//     CreationRand field and makes two builds of one pack
//     byte-identical by construction (same pack + seed → same .cam
//     bytes; a different seed → a different archive).
//
// Parsing is STRICT (the wire-protocol discipline): unknown keys and
// out-of-vocabulary values are named errors, never silently skipped —
// a typo'd pack must fail loudly at the parse, not mid-war.
//
// The vocabulary maps onto the wire's own enums:
//   objective kinds  → ObjectiveType (classtbl.h:65) — resolved to a
//                      class-table entity_type by the initializer
//   relation words   → RelType (data_source.hpp:76): 0 NoRelations,
//                      1 Allied, 2 Friendly, 3 Neutral, 4 Hostile, 5 War
//   battalion kinds  → STYPE_LAND_* (class_table.hpp)
//   squadron spec    → first (CLASS_UNIT, DOMAIN_AIR, VU_TYPE=3) entry
//                      unless the pack pins an explicit entity_type
//
// Dependencies: f4-json (the house Reader), standard library only.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace f4::world_convert {

/// RelType words the pack accepts (data_source.hpp:76).
/// The default row between two teams that declared nothing is
/// Neutral (3) — a war exists only where the pack declares one.
enum class PackRelation : int16_t {
    NoRelations = 0,
    Allied      = 1,
    Friendly    = 2,
    Neutral     = 3,
    Hostile     = 4,
    War         = 5,
};

/// Pack vocabulary → RelType. Throws std::runtime_error on an unknown
/// word (strict pack discipline).
[[nodiscard]] PackRelation relation_from_word(const std::string& word);

/// Objective kind words the pack accepts → the ObjectiveType enum value
/// (classtbl.h:65). The initializer resolves each to a class-table
/// entity_type (first (CLASS_OBJECTIVE, type) entry — deterministic).
/// Throws on an unknown kind.
[[nodiscard]] int objective_type_from_kind(const std::string& kind);

/// Battalion kind words the pack accepts → STYPE_LAND_* (class_table.hpp).
/// Throws on an unknown kind.
[[nodiscard]] int battalion_subtype_from_kind(const std::string& kind);

/// Squadron specialty words → the wire's ARO role byte
/// (SquadronState.specialty: 0 none, 1 AA, 2 AG). Throws on unknown.
[[nodiscard]] int squadron_specialty_from_word(const std::string& word);

/// One side of the war. Slots are the wire's own team vocabulary
/// (0..7); unnamed slots stay stock placeholders (the "XX" rows the
/// stock saves carry).
struct PackTeam {
    int slot = 0;                    // 0..7
    std::string name;                // required, unique, non-empty
    std::string motto;
    int colour = 0;                  // TeamEntry.colour byte
    int air_experience = 80;         // 0..100 (TeamRecord.air_experience)
    int ground_experience = 80;
    int naval_experience = 80;
    int air_defense_experience = 80;
    int reserve_aircraft = 0;        // te_number_aircraft[slot] — the
                                     // team-level pool (squadron rosters
                                     // keep their own counts ON TOP —
                                     // snapshot_squadron_force semantics)
    int replacements = 12;           // replacements_avail stock
    int supply = 1000;               // supply_avail
    int fuel = 1000;                 // fuel_avail
};

/// One declared relation row (applied in BOTH directions).
struct PackRelationDecl {
    int a = 0;                       // team slot
    int b = 0;                       // team slot
    PackRelation stance = PackRelation::Neutral;
};

/// One objective in the OOB template. `kind` names the ObjectiveType;
/// the initializer resolves the class-table entity_type (or takes the
/// pack's explicit override). Names are the template's handles —
/// squadrons/battalions/links reference objectives BY NAME.
struct PackObjective {
    std::string name;                // required, unique
    std::string kind = "city";       // objective_kind_from_kind vocabulary
    int x = 0;                       // grid column, [0, theater.size_x)
    int y = 0;                       // grid row,    [0, theater.size_y)
    int owner = 0;                   // team slot; 0 = neutral
    int priority = 30;               // 0..100 (ObjectiveClass.priority)
    int supply = 100;                // ObjectiveClass.supply
    int fuel = 100;                  // ObjectiveClass.fuel
    int entity_type = 0;             // 0 = resolve from the class table
};

/// One road/rail link between two named objectives (encoded in BOTH
/// directions, like the real net). Costs are the initializer's
/// documented default profile (road-capable: Foot 20 / Wheeled 30 /
/// Tracked 60 / LowAir 10 / Air 10; Naval/Rail 250).
struct PackLink {
    int a = 0;                       // objective index (declaration order)
    int b = 0;
};

/// One squadron in the OOB. `base` names a PackObjective (an airbase
/// is conventional but not enforced — the wire's own anchor rule).
struct PackSquadron {
    int team = 0;                    // team slot
    std::string base;                // objective name (required)
    std::string name;                // display name (UI preload list)
    int size = 12;                   // aircraft — packed into the roster
                                     // bitfield (2 bits/group, groups of 3)
    int skill = 5;                   // pilot skill nibble 0..9
    std::string specialty = "none";  // none | AA | AG
    int entity_type = 0;             // 0 = first (UNIT, AIR, SQUADRON)
};

/// One battalion in the OOB. `at` names a PackObjective.
struct PackBattalion {
    int team = 0;
    std::string at;                  // objective name (required)
    std::string kind = "armor";      // STYPE_LAND_* vocabulary
    int groups = 4;                  // 1..16 vehicle groups (3 each)
    int entity_type = 0;             // 0 = first (UNIT, LAND, BATTALION,
                                     //     stype=kind)
};

/// The parsed scenario pack. See the file header for the schema.
struct ScenarioPack {
    // --- identity ---------------------------------------------------
    int pack_version = 1;            // "pack" — must be 1
    std::string name;                // required (archive stem too)
    int camp_version = 71;           // 63 or 71 (the two live layouts)
    uint32_t seed = 0;               // → .cmp CreationRand

    // --- theater ----------------------------------------------------
    struct Theater {
        std::string name = "GENERATED WAR";   // TheaterName (char[40])
        std::string ui_name;                  // UIName (defaults to pack name)
        std::string scenario;                 // Scenario (defaults to pack name)
        std::string save_file;                // SaveFile (defaults to pack name)
        std::string id = "korea";             // the world JSON's "theater"
        std::string terrain_file = "korea.terrain.json";
        int size_x = 256;            // ownership-grid extent; sx*sy*2/8
        int size_y = 256;            // must fit the .cmp's i16 CampMapSize
    } theater;

    // --- date / weather ----------------------------------------------
    struct Date {
        int32_t current_time = 32400000;  // the opening clock (9 h in)
        int day = 1;                      // CurrentDay byte
    } date;
    struct Weather {
        uint32_t seed = 0;           // host metadata (see file header)
        std::string condition = "clear";
    } weather;

    // --- command anchors ---------------------------------------------
    int bullseye_x = 0;              // grid; name byte 0 = none
    int bullseye_y = 0;
    int bullseye_name = 0;
    int player_team = 0;             // TE_team; 0 = none
    int tempo = 255;                 // the stock saves' own value

    // --- the OOB template + force levels ------------------------------
    std::vector<PackTeam> teams;             // 2..8 named sides
    std::vector<PackRelationDecl> relations; // declared stance rows
    std::vector<PackObjective> objectives;   // 1..2000
    std::vector<PackLink> links;             // optional road/rail net
    std::vector<PackSquadron> squadrons;     // 0..128
    std::vector<PackBattalion> battalions;   // 0..512

    /// Parse a pack JSON document. STRICT: unknown top-level or
    /// nested keys are named errors; every cross-reference (relation
    /// slots, squadron bases, battalion sites, link indices) is
    /// validated here so the initializer can assume a consistent pack.
    /// Throws std::runtime_error with a `pack: ` prefix on any fault.
    [[nodiscard]] static ScenarioPack parse(const std::string& json);

    /// Load and parse a pack file. Throws on I/O or parse error.
    [[nodiscard]] static ScenarioPack load(const std::string& path);
};

} // namespace f4::world_convert

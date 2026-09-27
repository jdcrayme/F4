// f4-renderer/include/f4/renderer/symbols.hpp
//
// PUBLIC HEADER — the SymbolKind address space for F4 map symbols.
//
// Design
// ------
// An entity names its symbol in two hops: a component's objective_type or
// (unit_class, unit_subtype) maps to a SymbolKind (this header's pure
// functions), and the SymbolKind resolves to a data-driven definition via
// symbol_key_for_kind() — the seam the SymbolLibrary (symbol_library.hpp,
// backed by f4_symbols.json + symbols/*.svg overrides) is consumed
// through. There is no procedural draw path anymore: rendering goes
// through draw_library_symbol(), and a missing key falls back to a plain
// circle inside RenderEntityIcon so an icon can never go blank.

#pragma once

#include <f4/entities/types.hpp>     // for f4::entities::UnitClass

#include <cstdint>

namespace f4::renderer {

// POD mirror of Raylib's Color — avoids including raylib.h in this
// public header (symbol_library.hpp's draw API takes it too).
struct RlColor { unsigned char r, g, b, a; };

// ---------------------------------------------------------------------------
// SymbolKind — one entry per drawable symbol.
//
// Objective symbols (Obj*) are drawn as a single shape (no frame) — the
// shape itself encodes the objective type. Filled with team color.
//
// Unit symbols (Unit*) follow a frame+glyph convention borrowed from
// MIL-STD-2525 but simplified:
//   Battalion  -> rectangle frame
//   Brigade    -> diamond frame
//   Squadron   -> circle frame (or aircraft silhouette for air subtypes)
//   TaskForce  -> triangle frame (or ship silhouette for naval subtypes)
//   Flight     -> small circle outline (no fill)
//   Package    -> plus sign
// The frame is filled with team color; any inner glyph is drawn in a
// contrasting outline color (typically black or white) so it stays
// legible at small sizes.
// ---------------------------------------------------------------------------
enum class SymbolKind : uint16_t {
    // === Objectives (mapped from ObjectiveType 1..39) ===
    ObjAirbase = 0,    // 1  — runway
    ObjAirstrip,       // 2  — short runway
    ObjArmyBase,       // 3  — flag
    ObjBeach,          // 4  — wavy lines
    ObjBorder,         // 5  — dashed vertical
    ObjBridge,         // 6  — two bars + verticals
    ObjChemical,       // 7  — diamond + X
    ObjCity,           // 8  — cluster of 3 squares
    ObjComControl,     // 9  — square + antenna
    ObjDepot,          // 10 — square + X
    ObjFactory,        // 11 — box + 2 chimneys
    ObjFord,           // 12 — square + horizontal lines
    ObjFortification,  // 13 — chevron
    ObjHillTop,        // 14 — triangle + dot
    ObjIntersection,   // 15 — plus
    ObjNuclear,        // 17 — trefoil
    ObjPass,           // 18 — two triangles gap-up
    ObjPort,           // 19 — anchor
    ObjPowerPlant,     // 20 — lightning bolt
    ObjRadar,          // 21 — concentric arcs
    ObjRadioTower,     // 22 — triangle + dot on top
    ObjRailTerminal,   // 23 — train silhouette
    ObjRailroad,       // 24 — rail track
    ObjRefinery,       // 25 — triangle stack
    ObjRoad,           // 26 — single line
    ObjSamSite,        // 27 — triangle with notch (missile)
    ObjTown,           // 28 — 2 squares
    ObjVillage,        // 29 — 1 square
    ObjHarts,          // 30 — concentric circles
    ObjAirTerminal,    // 39 — airplane silhouette
    ObjUnknown,        // fallback — circle

    // === Units ===
    // Frames (used when no subtype glyph applies):
    UnitBattalion,     // rectangle
    UnitBrigade,       // diamond
    UnitSquadron,      // circle
    UnitTaskForce,     // triangle
    UnitFlight,        // small circle outline
    UnitPackage,       // plus
    // Ground subtype glyphs (rect frame for Battalion, diamond for Brigade):
    UnitArmor,         // rect + horizontal ellipse (tank turret)
    UnitAirDefense,    // rect + upward arc (air defense / SAM / AAA)
    UnitAirmobile,     // rect + helicopter silhouette
    UnitArmoredCav,    // rect + diagonal slash (cavalry cross-sabers)
    UnitArtillery,     // rect + dot (gun)
    UnitHQ,            // rect + star (headquarters)
    UnitInfantry,      // rect + X
    UnitEngineer,      // rect + E
    UnitMarine,        // rect + anchor cross
    UnitMechanized,    // rect + tracked undercarriage (two parallel bars)
    UnitRocket,        // rect + chevron (rocket/missile)
    UnitSAMissile,     // rect + diamond (surface-to-surface missile)
    UnitSupply,        // rect + box
    UnitFighter,       // circle + fighter
    UnitBomber,        // circle + bomber
    UnitTransport,     // circle + transport
    UnitHelicopter,    // circle + helo
    UnitCarrier,       // triangle + carrier
    UnitNavalSurface,  // triangle + ship
    UnitUnknown,       // fallback — circle

    SymbolCount,
};

// Map an ObjectiveType (1..39 from the class table) to a SymbolKind.
// Pure function — same input always yields the same symbol.
[[nodiscard]] SymbolKind symbol_for_objective_type(uint8_t obj_type) noexcept;

// Map a unit_class + unit_subtype to a SymbolKind. Pure function.
[[nodiscard]] SymbolKind symbol_for_unit(f4::entities::UnitClass cls,
                                          uint8_t subtype) noexcept;

// ---------------------------------------------------------------------------
// SymbolLibraryKey — the data-driven vocabulary's address for a SymbolKind.
//
// This is the SEAM the SymbolLibrary (symbol_library.hpp, backed by
// f4_symbols.json) is consumed through: every procedural SymbolKind names
// the library definition that replaces it. Objectives address their shape
// directly ("obj_airbase"); unit frames address their frame
// ("frame_battalion"); composed unit kinds carry BOTH the standalone
// composed symbol ("unit_armor" — frame and glyph baked) and the
// frame-agnostic glyph ("glyph_armor") for the compositional render path.
// `primary`/`glyph` are never null for valid kinds except a unit's glyph
// when the kind IS a frame; UnitUnknown maps at the circle frame
// (frame_squadron) — the procedural fallback's own shape — because the
// corpus deliberately has no unit_unknown definition.
// ---------------------------------------------------------------------------
struct SymbolLibraryKey {
    const char* primary;   // objective shape / unit frame / composed unit symbol
    const char* glyph;     // frame-agnostic glyph (composed unit kinds only)
};

// The canonical SymbolKind -> library-key mapping. Pure function over a
// static table; the coverage test (test_symbol_keys.cpp) pins every key
// against the committed f4_symbols.json corpus so the wiring can never
// silently fall back for a kind the library actually defines.
[[nodiscard]] SymbolLibraryKey symbol_key_for_kind(SymbolKind kind) noexcept;

} // namespace f4::renderer

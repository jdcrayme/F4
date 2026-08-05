// f4-world-viewer/src/symbols.hpp
//
// PRIVATE HEADER — internal to f4-world-viewer. Declares the procedural
// symbol vocabulary that replaces the old PNG-icon system.
//
// Design
// ------
// Every objective and unit on the canvas is drawn as a small procedural
// glyph composed from raylib primitives (triangles, rectangles, circles,
// lines). There are NO texture assets — symbols scale crisply at any zoom,
// pick up the team color automatically, and add zero startup cost.
//
// Vocabulary
// ----------
//   SymbolKind enum — one entry per drawable (objective_type or
//                     unit_class + subtype combination).
//   symbol_for_objective_type(uint8_t) -> SymbolKind
//       Replaces Impl::icon_for_objective_type. Pure function.
//   symbol_for_unit(UnitClass, uint8_t subtype) -> SymbolKind
//       Replaces Impl::icon_for_unit. Pure function.
//
// Two render paths share the same geometry:
//   Impl::draw_symbol(...)              — raylib direct (used by canvas.cpp).
//   draw_symbol_imgui(ImDrawList*, ...) — ImGui draw list (used by the
//                                         Legend panel and any other widget
//                                         that wants a live symbol preview).
//
// Both paths take an explicit fill color and outline color, so callers
// can render team-colored symbols on the canvas and white-on-dark symbols
// in the legend from the same vocabulary.

#pragma once

#include <f4/entities/types.hpp>     // for f4::entities::UnitClass
#include <f4/world/world_state.hpp>

#include <cstdint>

struct ImDrawList;
struct ImVec2;

namespace f4::viewer {

struct RlColor;  // defined in viewer_state.hpp

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
    // Subtype glyphs (frame is still class-based, glyph is subtype):
    UnitArmor,         // rect + tank
    UnitArtillery,     // rect + gun
    UnitInfantry,      // rect + X
    UnitEngineer,      // rect + E
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

// Free-function ImGui variant — renders the same symbol vocabulary into an
// ImGui draw list for use in panels/legends. Defined in symbols.cpp.
//   dl          — target draw list (e.g. ImGui::GetWindowDrawList())
//   kind        — which symbol to draw
//   center      — screen-space center of the symbol
//   size_px     — overall symbol extent (width = height = size_px)
//   fill_col    — ImGui-packed fill color (use IM_COL32(r,g,b,a))
//   outline_col — ImGui-packed outline color
//   filled      — if false, draws outline only (for hover/selection)
void draw_symbol_imgui(ImDrawList* dl, SymbolKind kind, ImVec2 center,
                       float size_px, unsigned int fill_col,
                       unsigned int outline_col, bool filled = true);

} // namespace f4::viewer

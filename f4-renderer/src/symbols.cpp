// f4-renderer/src/symbols.cpp
//
// Procedural symbol drawing — replaces the old PNG-icon system entirely.
// Every objective and unit on the canvas is drawn here from raylib
// primitives (triangles, rectangles, circles, lines). No texture assets,
// no PNG loading, no asset search path.
//
// Two render paths share the same SymbolKind vocabulary:
//   * Impl::draw_symbol()              — raylib direct (canvas.cpp).
//   * draw_symbol_imgui(ImDrawList*)   — ImGui draw list (imgui_panels.cpp).
//
// Geometry is duplicated across the two paths. This is deliberate: the
// shapes are small (<15 lines each), the indirection of a shared "primitive
// list" would add allocation overhead per call, and keeping each path
// self-contained makes it easy to verify visual parity by eye.

#include <f4/renderer/symbols.hpp>

#include <f4/math/constants.hpp>

#include <imgui.h>
#include <raylib.h>

#include <cmath>

namespace f4::renderer {

// ---------------------------------------------------------------------------
// Mapping tables (pure functions)
// ---------------------------------------------------------------------------

SymbolKind symbol_for_objective_type(uint8_t t) noexcept {
    switch (t) {
        case 1:  return SymbolKind::ObjAirbase;
        case 2:  return SymbolKind::ObjAirstrip;
        case 3:  return SymbolKind::ObjArmyBase;
        case 4:  return SymbolKind::ObjBeach;
        case 5:  return SymbolKind::ObjBorder;
        case 6:  return SymbolKind::ObjBridge;
        case 7:  return SymbolKind::ObjChemical;
        case 8:  return SymbolKind::ObjCity;
        case 9:  return SymbolKind::ObjComControl;
        case 10: return SymbolKind::ObjDepot;
        case 11: return SymbolKind::ObjFactory;
        case 12: return SymbolKind::ObjFord;
        case 13: return SymbolKind::ObjFortification;
        case 14: return SymbolKind::ObjHillTop;
        case 15: return SymbolKind::ObjIntersection;
        case 17: return SymbolKind::ObjNuclear;
        case 18: return SymbolKind::ObjPass;
        case 19: return SymbolKind::ObjPort;
        case 20: return SymbolKind::ObjPowerPlant;
        case 21: return SymbolKind::ObjRadar;
        case 22: return SymbolKind::ObjRadioTower;
        case 23: return SymbolKind::ObjRailTerminal;
        case 24: return SymbolKind::ObjRailroad;
        case 25: return SymbolKind::ObjRefinery;
        case 26: return SymbolKind::ObjRoad;
        case 27: return SymbolKind::ObjSamSite;
        case 28: return SymbolKind::ObjTown;
        case 29: return SymbolKind::ObjVillage;
        case 30: return SymbolKind::ObjHarts;
        case 39: return SymbolKind::ObjAirTerminal;
        default: return SymbolKind::ObjUnknown;
    }
}

SymbolKind symbol_for_unit(f4::entities::UnitClass cls, uint8_t subtype) noexcept {
    switch (cls) {
        case f4::entities::UnitClass::Battalion:
        case f4::entities::UnitClass::Brigade: {
            // Ground unit subtypes — same glyph vocabulary for both
            // battalion (rect frame) and brigade (diamond frame). The
            // caller (draw_symbol) picks the frame based on cls.
            switch (subtype) {
                case 1:  return SymbolKind::UnitAirDefense;  // STYPE_LAND_AIR_DEFENSE
                case 2:  return SymbolKind::UnitAirmobile;   // STYPE_LAND_AIRMOBILE
                case 3:  return SymbolKind::UnitArmor;       // STYPE_LAND_ARMOR
                case 4:  return SymbolKind::UnitArmoredCav;  // STYPE_LAND_ARMORED_CAV
                case 5:  return SymbolKind::UnitEngineer;    // STYPE_LAND_ENGINEER
                case 6:  return SymbolKind::UnitHQ;          // STYPE_LAND_HQ
                case 7:  return SymbolKind::UnitInfantry;    // STYPE_LAND_INFANTRY
                case 8:  return SymbolKind::UnitMarine;      // STYPE_LAND_MARINE
                case 9:  return SymbolKind::UnitMechanized;  // STYPE_LAND_MECHANIZED
                case 10: return SymbolKind::UnitRocket;      // STYPE_LAND_ROCKET
                case 11: return SymbolKind::UnitArtillery;   // STYPE_LAND_SP_ARTILLERY
                case 12: return SymbolKind::UnitSAMissile;   // STYPE_LAND_SS_MISSILE
                case 13: return SymbolKind::UnitSupply;      // STYPE_LAND_SUPPLY
                case 14: return SymbolKind::UnitArtillery;   // STYPE_LAND_TOWED_ARTILLERY
                default: break;
            }
            return (cls == f4::entities::UnitClass::Battalion)
                       ? SymbolKind::UnitBattalion
                       : SymbolKind::UnitBrigade;
        }
        case f4::entities::UnitClass::Squadron: {
            switch (subtype) {
                case 1:  return SymbolKind::UnitTransport;   // STYPE_AIR_AIR_TRANSPORT
                case 4:  return SymbolKind::UnitHelicopter;  // STYPE_AIR_ATTACK_HELO
                case 6:  return SymbolKind::UnitBomber;      // STYPE_AIR_BOMBER
                case 8:  return SymbolKind::UnitFighter;     // STYPE_AIR_FIGHTER
                case 9:  return SymbolKind::UnitFighter;     // STYPE_AIR_FIGHTER_BOMBER
                case 13: return SymbolKind::UnitTransport;   // STYPE_AIR_TANKER
                case 14: return SymbolKind::UnitHelicopter;  // STYPE_AIR_TRANSPORT_HELO
                default: break;
            }
            return SymbolKind::UnitSquadron;
        }
        case f4::entities::UnitClass::TaskForce: {
            switch (subtype) {
                case 3:  return SymbolKind::UnitCarrier;     // STYPE_SEA_CARRIER
                default: break;
            }
            return SymbolKind::UnitNavalSurface;
        }
        case f4::entities::UnitClass::Flight:   return SymbolKind::UnitFlight;
        case f4::entities::UnitClass::Package:  return SymbolKind::UnitPackage;
        default: return SymbolKind::UnitUnknown;
    }
}

// ---------------------------------------------------------------------------
// Helper: pick the unit frame shape from a Unit* SymbolKind.
// Returns one of: UnitBattalion, UnitBrigade, UnitSquadron, UnitTaskForce,
// UnitFlight, UnitPackage — the "bare frame" equivalent of the input.
// Used by draw_symbol to know which frame to draw before the glyph.
// ---------------------------------------------------------------------------
static SymbolKind unit_frame_for(SymbolKind k) {
    switch (k) {
        case SymbolKind::UnitArmor:
        case SymbolKind::UnitAirDefense:
        case SymbolKind::UnitAirmobile:
        case SymbolKind::UnitArmoredCav:
        case SymbolKind::UnitArtillery:
        case SymbolKind::UnitHQ:
        case SymbolKind::UnitInfantry:
        case SymbolKind::UnitEngineer:
        case SymbolKind::UnitMarine:
        case SymbolKind::UnitMechanized:
        case SymbolKind::UnitRocket:
        case SymbolKind::UnitSAMissile:
        case SymbolKind::UnitSupply:
        case SymbolKind::UnitBattalion:
            return SymbolKind::UnitBattalion;
        case SymbolKind::UnitBrigade:
            return SymbolKind::UnitBrigade;
        case SymbolKind::UnitFighter:
        case SymbolKind::UnitBomber:
        case SymbolKind::UnitTransport:
        case SymbolKind::UnitHelicopter:
        case SymbolKind::UnitSquadron:
            return SymbolKind::UnitSquadron;
        case SymbolKind::UnitCarrier:
        case SymbolKind::UnitNavalSurface:
        case SymbolKind::UnitTaskForce:
            return SymbolKind::UnitTaskForce;
        case SymbolKind::UnitFlight:  return SymbolKind::UnitFlight;
        case SymbolKind::UnitPackage: return SymbolKind::UnitPackage;
        default: return SymbolKind::UnitUnknown;
    }
}

#include "rlgl.h"       // OpenGL abstraction layer to OpenGL 1.1, 2.1, 3.3+ or ES2

// Draw filled upward-facing arc (Dome / Semi-ellipse)
void DrawAirArc(int centerX, int centerY, float radiusH, float radiusV, Color color)
{
    rlBegin(RL_TRIANGLES);
    for (int i = 180; i < 360; i += 10)
    {
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlVertex2f((float)centerX, (float)centerY);
        rlVertex2f((float)centerX + cosf(DEG2RAD * (i + 10)) * radiusH, (float)centerY + sinf(DEG2RAD * (i + 10)) * radiusV);
        rlVertex2f((float)centerX + cosf(DEG2RAD * i) * radiusH, (float)centerY + sinf(DEG2RAD * i) * radiusV);
    }
    rlEnd();
}

// Draw upward-facing arc outline (Open arch)
void DrawAirArcLines(int centerX, int centerY, float radiusH, float radiusV, Color color)
{
    rlBegin(RL_LINES);
    for (int i = 180; i < 360; i += 10)
    {
        rlColor4ub(color.r, color.g, color.b, color.a);
        rlVertex2f(centerX + cosf(DEG2RAD * (i + 10)) * radiusH, centerY + sinf(DEG2RAD * (i + 10)) * radiusV);
        rlVertex2f(centerX + cosf(DEG2RAD * i) * radiusH, centerY + sinf(DEG2RAD * i) * radiusV);
    }
    rlEnd();
}

// ===========================================================================
// RAYLIB RENDER PATH
// ===========================================================================
//
// Impl::draw_symbol — draws a symbol centered at (sx, sy) with extent
// size_px (width = height = size_px). The fill color is the team color;
// the outline is drawn on top in `outline` for crispness at small sizes.
//
// `filled = false` renders outline-only (used for hover/selection state).
// ===========================================================================

void draw_symbol(SymbolKind kind, float sx, float sy,
                                   float size_px, RlColor fill,
                                   RlColor outline, bool filled) {
    const float r = size_px * 0.5f;
    const Color fc = {fill.r, fill.g, fill.b, fill.a};
    const Color oc = {outline.r, outline.g, outline.b, outline.a};
    // Slightly transparent fill so overlapping symbols don't fully occlude.
    const Color fc_blend = {fill.r, fill.g, fill.b,
                            static_cast<unsigned char>(fill.a * 0.85f)};

    // --- Frame for unit symbols (drawn first, glyph on top) ---
    // We draw the frame then the glyph for Unit* kinds that have a glyph.
    if (kind >= SymbolKind::UnitBattalion) {
        const SymbolKind frame = unit_frame_for(kind);
        // Draw the frame.
        switch (frame) {
            case SymbolKind::UnitBattalion: {  // rectangle
                const float d = r * 0.75f;

                const Rectangle rec = {sx - r, sy - d, r * 2, d * 2};
                if (filled) DrawRectangleRec(rec, fc_blend);
                DrawRectangleLinesEx(rec, 1.0f, oc);

                                // battalion marker: vertical line above rectangle
                const Vector2 p0 = { sx, sy - r - r/2 };
                const Vector2 p1 = { sx, sy - r };
                DrawLineEx(p0, p1, 1.0f, oc);

                break;
            }
            case SymbolKind::UnitBrigade: {  // rectangle

                const float d = r * 0.75f;
                const Rectangle rec = { sx - r, sy - d, r * 2, d * 2 };
                if (filled) DrawRectangleRec(rec, fc_blend);
                DrawRectangleLinesEx(rec, 1.0f, oc);

                // brigade marker: X above rectangle                
                const Vector2 p0 = { sx - r / 4, sy - r - r / 2 };
                const Vector2 p1 = { sx + r / 4, sy - r };
                const Vector2 p2 = { sx + r / 4, sy - r - r / 2 };
                const Vector2 p3 = { sx - r / 4, sy - r };

                DrawLineEx(p0, p1, 1.0f, oc);
                DrawLineEx(p2, p3, 1.0f, oc);
                break;
            }
            case SymbolKind::UnitSquadron: {  // arc
                                const float w = r * 0.5f;
                                const float h = r;
                if (filled) DrawAirArc(static_cast<int>(sx), static_cast<int>(sy), w, h, fc_blend);
                DrawAirArcLines(static_cast<int>(sx), static_cast<int>(sy), w, h, oc);

                // battalion marker: vertical line above rectangle
                const Vector2 p0 = { sx, sy - r - r / 2 };
                const Vector2 p1 = { sx, sy - r };
                DrawLineEx(p0, p1, 1.0f, oc);

                break;
            }
            case SymbolKind::UnitTaskForce: {  // triangle (point up)
                const float d = r * 0.75f;
                if (filled) DrawCircle(static_cast<int>(sx), static_cast<int>(sy), d, fc_blend);
                DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy), d, oc);
                break;
            }
            case SymbolKind::UnitFlight: {  // small circle outline only
                DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy),
                                static_cast<int>(r * 0.55f), fc);
                break;
            }
            case SymbolKind::UnitPackage: {  // plus sign
                const float w = r * 0.25f;
                const float h = r * 0.7f;
                if (filled) {
                    DrawRectangleRec({sx - w, sy - h, w * 2, h * 2}, fc_blend);
                    DrawRectangleRec({sx - h, sy - w, h * 2, w * 2}, fc_blend);
                }
                DrawLineEx({sx - w, sy - h}, {sx - w, sy + h}, 1.0f, oc);
                DrawLineEx({sx + w, sy - h}, {sx + w, sy + h}, 1.0f, oc);
                DrawLineEx({sx - w, sy - h}, {sx + w, sy - h}, 1.0f, oc);
                DrawLineEx({sx - w, sy + h}, {sx + w, sy + h}, 1.0f, oc);
                DrawLineEx({sx - h, sy - w}, {sx - h, sy + w}, 1.0f, oc);
                DrawLineEx({sx + h, sy - w}, {sx + h, sy + w}, 1.0f, oc);
                DrawLineEx({sx - h, sy - w}, {sx + h, sy - w}, 1.0f, oc);
                DrawLineEx({sx - h, sy + w}, {sx + h, sy + w}, 1.0f, oc);
                break;
            }
            default: break;
        }
        // Draw the glyph on top of the frame for subtype-bearing kinds.
        // Glyph color is the outline color so it contrasts with the team fill.
        const Color gc = oc;
        switch (kind) {
            case SymbolKind::UnitArmor: {  // horizontal ellipse (tank turret barrel)
                const float radiusH = r * 0.6f;
                const float radiusV = r * 0.2f;
                const Rectangle rec = { sx - radiusH, sy - radiusV, radiusH * 2, radiusV * 2 };
                DrawRectangleRoundedLines(rec, radiusV, 18, 1.5f, gc);
                break;
            }
            case SymbolKind::UnitAirDefense: {  // upward arc (dome — SAM / AAA)
                DrawAirArc((int)sx, (int)sy, r * 0.55f, r * 0.45f, gc);
                // Small tick at top center (antenna / launcher)
                DrawLineEx({sx, sy - r * 0.45f}, {sx, sy - r * 0.7f}, 1.5f, gc);
                break;
            }
            case SymbolKind::UnitAirmobile: {  // helicopter silhouette
                DrawCircleV({sx, sy + r * 0.1f}, r * 0.2f, gc);
                DrawLineEx({sx - r * 0.45f, sy - r * 0.3f},
                           {sx + r * 0.45f, sy - r * 0.3f}, 1.5f, gc);
                DrawLineEx({sx, sy - r * 0.3f}, {sx, sy - r * 0.05f}, 1.0f, gc);
                // Tail boom
                DrawLineEx({sx, sy + r * 0.3f}, {sx + r * 0.3f, sy + r * 0.45f}, 1.0f, gc);
                break;
            }
            case SymbolKind::UnitArmoredCav: {  // diagonal slash (crossed sabers)
                const float d = r * 0.6f;
                DrawLineEx({sx - d, sy - d}, {sx + d, sy + d}, 1.5f, gc);
                DrawLineEx({sx + d, sy - d}, {sx - d, sy + d}, 1.5f, gc);
                // Small circle at center (unit dot)
                DrawCircleV({sx, sy}, r * 0.1f, gc);
                break;
            }
            case SymbolKind::UnitArtillery: {  // dot (gun)
                DrawCircleV({sx, sy}, r * 0.25f, gc);
                break;
            }
            case SymbolKind::UnitInfantry: {  // X
                const float d = r * 0.75f;
                DrawLineEx({ sx - r + 2, sy - d + 2 }, { sx + r - 2, sy + d - 2 }, 1.5f, gc);
                DrawLineEx({ sx + r - 2, sy - d + 2 }, { sx - r + 2, sy + d - 2 }, 1.5f, gc);
                break;
            }
            case SymbolKind::UnitEngineer: {  // E shape — 3 horizontal bars
                const float d = r * 0.35f;
                DrawLineEx({sx - d, sy - d}, {sx - d, sy + d}, 1.5f, gc);
                DrawLineEx({sx - d, sy - d}, {sx + d * 0.5f, sy - d}, 1.5f, gc);
                DrawLineEx({sx - d, sy},      {sx + d * 0.3f, sy},      1.5f, gc);
                DrawLineEx({sx - d, sy + d}, {sx + d * 0.5f, sy + d}, 1.5f, gc);
                break;
            }
            case SymbolKind::UnitHQ: {  // 6-pointed star (headquarters)
                const float d = r * 0.6f;
                // Two overlapping triangles form a Star of David
                DrawTriangle({sx, sy - d},
                             {sx - d * 0.87f, sy + d * 0.5f},
                             {sx + d * 0.87f, sy + d * 0.5f}, gc);
                DrawTriangle({sx, sy + d},
                             {sx - d * 0.87f, sy - d * 0.5f},
                             {sx + d * 0.87f, sy - d * 0.5f}, gc);
                break;
            }
            case SymbolKind::UnitMarine: {  // anchor cross — vertical + horizontal + bottom arc
                const float d = r * 0.55f;
                DrawLineEx({sx, sy - d}, {sx, sy + d * 0.5f}, 1.5f, gc);
                DrawLineEx({sx - d * 0.6f, sy - d * 0.3f}, {sx + d * 0.6f, sy - d * 0.3f}, 1.5f, gc);
                // Bottom hook (curved via short segments)
                DrawLineEx({sx - d * 0.35f, sy + d * 0.5f}, {sx + d * 0.35f, sy + d * 0.5f}, 1.5f, gc);
                DrawLineEx({sx + d * 0.35f, sy + d * 0.5f}, {sx + d * 0.35f, sy + d * 0.2f}, 1.0f, gc);
                DrawLineEx({sx - d * 0.35f, sy + d * 0.5f}, {sx - d * 0.35f, sy + d * 0.2f}, 1.0f, gc);
                break;
            }
            case SymbolKind::UnitMechanized: {  // two parallel horizontal bars (tracked undercarriage)
                const float d = r * 0.65f;
                const float gap = r * 0.25f;
                DrawLineEx({sx - d, sy - gap}, {sx + d, sy - gap}, 1.5f, gc);
                DrawLineEx({sx - d, sy + gap}, {sx + d, sy + gap}, 1.5f, gc);
                // Connecting bar (hull)
                DrawLineEx({sx - d * 0.3f, sy - gap}, {sx - d * 0.3f, sy + gap}, 1.0f, gc);
                DrawLineEx({sx + d * 0.3f, sy - gap}, {sx + d * 0.3f, sy + gap}, 1.0f, gc);
                break;
            }
            case SymbolKind::UnitRocket: {  // upward chevron (missile in flight)
                const float d = r * 0.6f;
                DrawLineEx({sx - d, sy + d * 0.4f}, {sx, sy - d * 0.6f}, 1.5f, gc);
                DrawLineEx({sx + d, sy + d * 0.4f}, {sx, sy - d * 0.6f}, 1.5f, gc);
                // Small dot at base (exhaust)
                DrawCircleV({sx, sy + d * 0.5f}, r * 0.08f, gc);
                break;
            }
            case SymbolKind::UnitSAMissile: {  // small diamond (surface-to-surface missile)
                const float d = r * 0.55f;
                const Vector2 p0 = {sx, sy - d};
                const Vector2 p1 = {sx + d * 0.65f, sy};
                const Vector2 p2 = {sx, sy + d};
                const Vector2 p3 = {sx - d * 0.65f, sy};
                DrawLineEx(p0, p1, 1.5f, gc);
                DrawLineEx(p1, p2, 1.5f, gc);
                DrawLineEx(p2, p3, 1.5f, gc);
                DrawLineEx(p3, p0, 1.5f, gc);
                break;
            }
            case SymbolKind::UnitSupply: {  // box with +
                const float d = r * 0.3f;
                DrawRectangleLines(static_cast<int>(sx - d),
                                   static_cast<int>(sy - d),
                                   static_cast<int>(d * 2),
                                   static_cast<int>(d * 2), gc);
                DrawLineEx({sx, sy - d * 0.7f}, {sx, sy + d * 0.7f}, 1.0f, gc);
                DrawLineEx({sx - d * 0.7f, sy}, {sx + d * 0.7f, sy}, 1.0f, gc);
                break;
            }
            case SymbolKind::UnitFighter: {  // fighter silhouette
                const float fw = r * 0.12f;
                const float fh = r * 0.85f;
                DrawRectangleRec({sx - fw, sy - fh * 0.5f, fw * 2, fh}, gc);
                DrawRectangleRec({sx - r * 0.6f, sy - fw, r * 1.2f, fw * 2}, gc);
                DrawTriangle({sx - r * 0.15f, sy + fh * 0.5f},
                             {sx + r * 0.15f, sy + fh * 0.5f},
                             {sx, sy + fh * 0.85f}, gc);
                break;
            }
            case SymbolKind::UnitBomber: {  // bomber — wider wings
                const float fw = r * 0.14f;
                const float fh = r * 0.7f;
                DrawRectangleRec({sx - fw, sy - fh * 0.5f, fw * 2, fh}, gc);
                DrawRectangleRec({sx - r * 0.75f, sy - fw, r * 1.5f, fw * 2}, gc);
                DrawTriangle({sx - r * 0.25f, sy + fh * 0.5f},
                             {sx + r * 0.25f, sy + fh * 0.5f},
                             {sx, sy + fh * 0.85f}, gc);
                break;
            }
            case SymbolKind::UnitTransport: {  // transport — long fuselage
                const float fw = r * 0.1f;
                const float fh = r * 0.9f;
                DrawRectangleRec({sx - fw, sy - fh * 0.5f, fw * 2, fh}, gc);
                DrawRectangleRec({sx - r * 0.5f, sy - fw, r * 1.0f, fw * 2}, gc);
                DrawTriangle({sx - r * 0.15f, sy + fh * 0.5f},
                             {sx + r * 0.15f, sy + fh * 0.5f},
                             {sx, sy + fh * 0.85f}, gc);
                break;
            }
            case SymbolKind::UnitHelicopter: {  // helo — body + rotor
                DrawCircleV({sx, sy + r * 0.1f}, r * 0.25f, gc);
                DrawLineEx({sx - r * 0.55f, sy - r * 0.35f},
                           {sx + r * 0.55f, sy - r * 0.35f}, 1.5f, gc);
                DrawLineEx({sx, sy - r * 0.35f}, {sx, sy - r * 0.1f}, 1.0f, gc);
                DrawLineEx({sx, sy + r * 0.35f}, {sx + r * 0.4f, sy + r * 0.5f},
                           1.0f, gc);
                break;
            }
            case SymbolKind::UnitCarrier: {  // carrier — long hull + island
                const float hw = r * 0.7f;
                const float hh = r * 0.18f;
                DrawRectangleRec({sx - hw, sy - hh, hw * 2, hh * 2}, gc);
                DrawRectangleRec({sx + hw * 0.4f, sy - hh * 1.6f,
                                  hw * 0.2f, hh * 0.8f}, gc);
                break;
            }
            case SymbolKind::UnitNavalSurface: {  // ship — hull + superstructure
                const float hw = r * 0.55f;
                const float hh = r * 0.16f;
                DrawRectangleRec({sx - hw, sy - hh, hw * 2, hh * 2}, gc);
                DrawRectangleRec({sx - hw * 0.15f, sy - hh * 1.8f,
                                  hw * 0.3f, hh * 0.9f}, gc);
                break;
            }
            default: break;
        }
        return;  // unit symbols done — no objective-shape fallthrough
    }

    // --- Objective symbols ---
    switch (kind) {
        case SymbolKind::ObjAirbase: {  // runway — long horizontal bar + centerline
            const float radius = r * 0.75f;

            // Draw the aerodrome circle boundary and optional fill
            if (filled) {
                DrawCircle((int)sx, (int)sy, radius, fc_blend);
            }
            DrawRing({ sx, sy }, radius - 1.0f, radius, 0.0f, 360.0f, 16, oc);

            // Draw intersecting runway lines pattern inside the circle
            // Runway 1 (Main diagonal/vertical-ish)
            const Vector2 r1_p1 = { sx - radius * 0.5f, sy - radius * 0.5f };
            const Vector2 r1_p2 = { sx + radius * 0.5f, sy + radius * 0.5f };

            // Runway 2 (Cross/horizontal-ish)
            const Vector2 r2_p1 = { sx - radius * 0.6f, sy + radius * 0.2f };
            const Vector2 r2_p2 = { sx + radius * 0.6f, sy - radius * 0.2f }; // balanced

            DrawLineEx(r1_p1, r1_p2, 2.0f, oc);
            break;
        }
        case SymbolKind::ObjAirstrip: {  // Circle with intersecting runways
            const float radius = r * 0.75f;

            // Draw the aerodrome circle boundary and optional fill
            if (filled) {
                DrawCircle((int)sx, (int)sy, radius, fc_blend);
            }
            DrawRing({ sx, sy }, radius - 1.0f, radius, 0.0f, 360.0f, 16, oc);

            // Draw intersecting runway lines pattern inside the circle
            // Runway 1 (Main diagonal/vertical-ish)
            const Vector2 r1_p1 = { sx - radius * 0.5f, sy - radius * 0.5f };
            const Vector2 r1_p2 = { sx + radius * 0.5f, sy + radius * 0.5f };

            // Runway 2 (Cross/horizontal-ish)
            const Vector2 r2_p1 = { sx - radius * 0.6f, sy + radius * 0.2f };
            const Vector2 r2_p2 = { sx + radius * 0.6f, sy - radius * 0.2f }; // balanced

            DrawLineEx(r1_p1, r1_p2, 2.0f, oc);
            DrawLineEx({ sx - radius * 0.5f, sy + radius * 0.4f },
                { sx + radius * 0.5f, sy - radius * 0.4f }, 2.0f, oc);
            break;
        }
        case SymbolKind::ObjArmyBase: {  // flag — vertical line + triangle
            const float d = r * 0.7f;
            DrawLineEx({sx - d * 0.5f, sy - d}, {sx - d * 0.5f, sy + d}, 2.0f, oc);
            if (filled) DrawTriangle({sx - d * 0.5f, sy - d},
                                     {sx + d,        sy - d * 0.6f},
                                     {sx - d * 0.5f, sy - d * 0.2f}, fc_blend);
            DrawLineEx({sx - d * 0.5f, sy - d}, {sx + d, sy - d * 0.6f}, 1.0f, oc);
            DrawLineEx({sx + d, sy - d * 0.6f}, {sx - d * 0.5f, sy - d * 0.2f}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjBeach: {  // 3 wavy horizontal lines
            const float dx = r * 0.75f;
            for (int i = 0; i < 3; ++i) {
                const float y = sy - r * 0.4f + i * r * 0.4f;
                // Three short segments to suggest a wave
                DrawLineEx({sx - dx, y}, {sx - dx * 0.3f, y - 1.5f}, 1.5f, fc);
                DrawLineEx({sx - dx * 0.3f, y - 1.5f}, {sx + dx * 0.3f, y + 1.5f}, 1.5f, fc);
                DrawLineEx({sx + dx * 0.3f, y + 1.5f}, {sx + dx, y}, 1.5f, fc);
            }
            break;
        }
        case SymbolKind::ObjBorder: {  // dashed vertical line
            const float dy = r * 0.9f;
            for (int i = 0; i < 4; ++i) {
                const float y0 = sy - dy + i * (dy * 0.5f);
                DrawLineEx({sx, y0}, {sx, y0 + dy * 0.3f}, 2.0f, fc);
            }
            break;
        }
        case SymbolKind::ObjBridge: {  // two horizontal bars + 2 verticals

            const float d = r * 0.75f / 4;

            const Vector2 t0 = { sx - d * 4, sy - d * 2 };
            const Vector2 t1 = { sx - d * 3, sy - d };
            const Vector2 t2 = { sx + d * 3, sy - d };
            const Vector2 t3 = { sx + d * 4, sy - d * 2 };

            const Vector2 b0 = { sx - d * 4, sy + d * 2 };
            const Vector2 b1 = { sx - d * 3, sy + d };
            const Vector2 b2 = { sx + d * 3, sy + d };
            const Vector2 b3 = { sx + d * 4, sy + d * 2 };
            if (filled) {
                DrawTriangle(t0, b0, b1, fc_blend);
                DrawTriangle(t0, b1, t1, fc_blend);
                DrawTriangle(t1, b1, b2, fc_blend);
                DrawTriangle(t1, b2, t2, fc_blend);
                DrawTriangle(t2, b2, b3, fc_blend);
                DrawTriangle(t2, b3, t3, fc_blend);
            }

            DrawLineEx(t0, t1, 1.5f, oc);
            DrawLineEx(t1, t2, 1.5f, oc);
            DrawLineEx(t2, t3, 1.5f, oc);

            DrawLineEx(b0, b1, 1.5f, oc);
            DrawLineEx(b1, b2, 1.5f, oc);
            DrawLineEx(b2, b3, 1.5f, oc);
            break;
        }
        case SymbolKind::ObjChemical: {  // diamond + X
            const float d = r * 0.75f;
            const Vector2 p0 = {sx, sy - d};
            const Vector2 p1 = {sx + d, sy};
            const Vector2 p2 = {sx, sy + d};
            const Vector2 p3 = {sx - d, sy};
            if (filled) {
                DrawTriangle(p0, p1, p2, fc_blend);
                DrawTriangle(p0, p2, p3, fc_blend);
            }
            DrawLineEx(p0, p1, 1.0f, oc);
            DrawLineEx(p1, p2, 1.0f, oc);
            DrawLineEx(p2, p3, 1.0f, oc);
            DrawLineEx(p3, p0, 1.0f, oc);
            // X
            const float e = d * 0.5f;
            DrawLineEx({sx - e, sy - e}, {sx + e, sy + e}, 1.0f, oc);
            DrawLineEx({sx + e, sy - e}, {sx - e, sy + e}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjCity: {  // 3 small squares

            const float vSize = r * 0.5f;
            for (auto i : { Vector3(-1.0f,0.1f, 1.4f), Vector3(1.1f,-0.2f,1.5f), Vector3(0.0f,-0.7f,2.3f) }) {
                const Rectangle vRec = { sx - vSize / 2.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize, vSize, vSize * i.z };
                if (filled) DrawRectangleRec(vRec, fc_blend);
                DrawRectangleLinesEx(vRec, 1.0f, oc);
            }
            break;
        }
        case SymbolKind::ObjComControl: {  // square + 2 antenna lines
            const float d = r * 0.5f;
            const Rectangle rec = {sx - d, sy - d, d * 2, d * 2};
            if (filled) DrawRectangleRec(rec, fc_blend);
            DrawRectangleLinesEx(rec, 1.0f, oc);
            DrawLineEx({sx - d * 0.5f, sy - d}, {sx - d * 0.5f, sy - r}, 1.0f, oc);
            DrawLineEx({sx + d * 0.5f, sy - d}, {sx + d * 0.5f, sy - r}, 1.0f, oc);
            DrawCircleV({sx - d * 0.5f, sy - r}, 1.2f, oc);
            DrawCircleV({sx + d * 0.5f, sy - r}, 1.2f, oc);
            break;
        }
        case SymbolKind::ObjDepot: {  // square + X
            const float d = r * 0.7f;
            const Rectangle rec = {sx - d, sy - d, d * 2, d * 2};
            if (filled) DrawRectangleRec(rec, fc_blend);
            DrawRectangleLinesEx(rec, 1.0f, oc);
            DrawLineEx({sx - d, sy - d}, {sx + d, sy + d}, 1.0f, oc);
            DrawLineEx({sx + d, sy - d}, {sx - d, sy + d}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjFactory: {  // box + 2 chimneys
            const float d = r * 0.6f;
            const Rectangle rec = {sx - d, sy - d * 0.5f, d * 2, d};
            if (filled) DrawRectangleRec(rec, fc_blend);
            DrawRectangleLinesEx(rec, 1.0f, oc);
            // Two chimneys on top
            const float cw = d * 0.2f;
            const float ch = d * 0.6f;
            const Rectangle c1 = {sx - d * 0.5f, sy - d * 0.5f - ch, cw, ch};
            const Rectangle c2 = {sx + d * 0.3f, sy - d * 0.5f - ch, cw, ch};
            if (filled) {
                DrawRectangleRec(c1, fc_blend);
                DrawRectangleRec(c2, fc_blend);
            }
            DrawRectangleLinesEx(c1, 1.0f, oc);
            DrawRectangleLinesEx(c2, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjFord: {  // square + horizontal lines
            const float d = r * 0.7f;
            const Rectangle rec = {sx - d, sy - d, d * 2, d * 2};
            if (filled) DrawRectangleRec(rec, fc_blend);
            DrawRectangleLinesEx(rec, 1.0f, oc);
            DrawLineEx({sx - d, sy - d * 0.3f}, {sx + d, sy - d * 0.3f}, 1.0f, oc);
            DrawLineEx({sx - d, sy + d * 0.3f}, {sx + d, sy + d * 0.3f}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjFortification: {  // chevron
            const float d = r * 0.75f;
            DrawLineEx({sx - d, sy + d * 0.5f}, {sx, sy - d * 0.6f}, 2.5f, fc);
            DrawLineEx({sx, sy - d * 0.6f}, {sx + d, sy + d * 0.5f}, 2.5f, fc);
            break;
        }
        case SymbolKind::ObjHillTop: {  // triangle + dot
            const float d = r * 0.75f;
            if (filled) DrawTriangle({sx, sy - d},
                                     {sx - d, sy + d * 0.7f},
                                     {sx + d, sy + d * 0.7f}, fc_blend);
            DrawLineEx({sx, sy - d}, {sx - d, sy + d * 0.7f}, 1.0f, oc);
            DrawLineEx({sx - d, sy + d * 0.7f}, {sx + d, sy + d * 0.7f}, 1.0f, oc);
            DrawLineEx({sx + d, sy + d * 0.7f}, {sx, sy - d}, 1.0f, oc);
            DrawCircleV({sx, sy - d * 0.15f}, 1.5f, oc);
            break;
        }
        case SymbolKind::ObjIntersection: {  // plus — road crossing
            const float w = r * 0.25f;
            const float h = r * 0.85f;
            if (filled) {
                DrawRectangleRec({sx - w, sy - h, w * 2, h * 2}, fc_blend);
                DrawRectangleRec({sx - h, sy - w, h * 2, w * 2}, fc_blend);
            }
            DrawRectangleLinesEx({sx - w, sy - h, w * 2, h * 2}, 1.0f, oc);
            DrawRectangleLinesEx({sx - h, sy - w, h * 2, w * 2}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjNuclear: {  // trefoil — center disc + 3 sectors
            DrawCircleV({sx, sy}, r * 0.25f, fc);
            DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy),
                            static_cast<int>(r * 0.7f), oc);
            for (int i = 0; i < 3; ++i) {
                const float angle = (-90.0f + i * 120.0f) * static_cast<float>(f4::math::DEG_TO_RAD);
                const float x1 = sx + std::cos(angle) * r * 0.3f;
                const float y1 = sy + std::sin(angle) * r * 0.3f;
                const float x2 = sx + std::cos(angle) * r * 0.65f;
                const float y2 = sy + std::sin(angle) * r * 0.65f;
                DrawLineEx({x1, y1}, {x2, y2}, 2.5f, fc);
            }
            break;
        }
        case SymbolKind::ObjPass: {  // two triangles gap-up
            const float d = r * 0.6f;
            if (filled) {
                DrawTriangle({sx - d * 1.3f, sy + d * 0.5f},
                             {sx - d * 0.3f, sy - d},
                             {sx + d * 0.2f, sy + d * 0.5f}, fc_blend);
                DrawTriangle({sx - d * 0.2f, sy + d * 0.5f},
                             {sx + d * 0.3f, sy - d},
                             {sx + d * 1.3f, sy + d * 0.5f}, fc_blend);
            }
            DrawLineEx({sx - d * 1.3f, sy + d * 0.5f}, {sx - d * 0.3f, sy - d}, 1.0f, oc);
            DrawLineEx({sx - d * 0.3f, sy - d}, {sx + d * 0.2f, sy + d * 0.5f}, 1.0f, oc);
            DrawLineEx({sx - d * 0.2f, sy + d * 0.5f}, {sx + d * 0.3f, sy - d}, 1.0f, oc);
            DrawLineEx({sx + d * 0.3f, sy - d}, {sx + d * 1.3f, sy + d * 0.5f}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjPort: {  // anchor — ring + cross + arc
            DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy - r * 0.55f),
                            static_cast<int>(r * 0.18f), fc);
            DrawLineEx({sx, sy - r * 0.35f}, {sx, sy + r * 0.55f}, 2.0f, fc);
            DrawLineEx({sx - r * 0.4f, sy + r * 0.35f}, {sx + r * 0.4f, sy + r * 0.35f},
                       2.0f, fc);
            break;
        }
        case SymbolKind::ObjPowerPlant: {  // lightning bolt
            const float d = r * 0.7f;
            const Vector2 p0 = {sx + d * 0.3f, sy - d};
            const Vector2 p1 = {sx - d * 0.3f, sy - d * 0.1f};
            const Vector2 p2 = {sx + d * 0.1f, sy + d * 0.1f};
            const Vector2 p3 = {sx - d * 0.3f, sy + d};
            const Vector2 p4 = {sx + d * 0.3f, sy + d * 0.1f};
            const Vector2 p5 = {sx - d * 0.1f, sy - d * 0.1f};
            if (filled) DrawTriangle(p0, p1, p2, fc);
            if (filled) DrawTriangle(p3, p4, p5, fc);
            DrawLineEx(p0, p1, 1.0f, oc);
            DrawLineEx(p1, p2, 1.0f, oc);
            DrawLineEx(p2, p5, 1.0f, oc);
            DrawLineEx(p5, p4, 1.0f, oc);
            DrawLineEx(p4, p3, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjRadar: {  // 3 concentric arcs (fan)
            for (int i = 0; i < 3; ++i) {
                const float rad = r * (0.35f + i * 0.25f);
                // Arc from -90 to 0 degrees (upper-right quadrant)
                const int segments = 12;
                for (int s = 0; s < segments; ++s) {
                    const float a0 = (-90.0f + s * (90.0f / segments)) * static_cast<float>(f4::math::DEG_TO_RAD);
                    const float a1 = (-90.0f + (s + 1) * (90.0f / segments)) * static_cast<float>(f4::math::DEG_TO_RAD);
                    DrawLineEx({sx + std::cos(a0) * rad, sy + std::sin(a0) * rad},
                               {sx + std::cos(a1) * rad, sy + std::sin(a1) * rad},
                               1.5f, fc);
                }
            }
            // Dot at center
            DrawCircleV({sx, sy}, 1.5f, fc);
            break;
        }
        case SymbolKind::ObjRadioTower: {  // tall thin triangle + dot on top
            const float d = r * 0.45f;
            if (filled) DrawTriangle({sx - d, sy + r * 0.9f},
                                     {sx + d, sy + r * 0.9f},
                                     {sx, sy - d * 0.5f}, fc_blend);
            DrawLineEx({sx - d, sy + r * 0.9f}, {sx + d, sy + r * 0.9f}, 1.0f, oc);
            DrawLineEx({sx + d, sy + r * 0.9f}, {sx, sy - d * 0.5f}, 1.0f, oc);
            DrawLineEx({sx, sy - d * 0.5f}, {sx - d, sy + r * 0.9f}, 1.0f, oc);
            DrawCircleV({sx, sy - r * 0.7f}, 2.0f, fc);
            // Two emission arcs
            DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy - r * 0.7f),
                            static_cast<int>(r * 0.3f), oc);
            break;
        }
        case SymbolKind::ObjRailTerminal: {  // train silhouette — rect + 2 wheels
            const float bw = r * 0.7f;
            const float bh = r * 0.35f;
            const Rectangle body = {sx - bw, sy - bh * 0.5f, bw * 2, bh};
            if (filled) DrawRectangleRec(body, fc_blend);
            DrawRectangleLinesEx(body, 1.0f, oc);
            DrawCircleV({sx - bw * 0.55f, sy + bh * 0.5f + r * 0.1f}, r * 0.12f, fc);
            DrawCircleV({sx + bw * 0.55f, sy + bh * 0.5f + r * 0.1f}, r * 0.12f, fc);
            break;
        }
        case SymbolKind::ObjRailroad: {  // 2 horizontal lines + cross ties
            const float w = r * 0.85f;
            DrawLineEx({sx - w, sy - r * 0.12f}, {sx + w, sy - r * 0.12f}, 1.5f, fc);
            DrawLineEx({sx - w, sy + r * 0.12f}, {sx + w, sy + r * 0.12f}, 1.5f, fc);
            for (int i = -2; i <= 2; ++i) {
                const float x = sx + i * w * 0.4f;
                DrawLineEx({x, sy - r * 0.25f}, {x, sy + r * 0.25f}, 1.0f, fc);
            }
            break;
        }
        case SymbolKind::ObjRefinery: {  // 3 distillation towers
            const float d = r * 0.4f;
            if (filled) {
                DrawTriangle({sx - d * 1.5f, sy + d}, {sx - d * 0.5f, sy + d},
                             {sx - d, sy - d}, fc_blend);
                DrawTriangle({sx - d * 0.5f, sy + d}, {sx + d * 0.5f, sy + d},
                             {sx, sy - d * 1.5f}, fc_blend);
                DrawTriangle({sx + d * 0.5f, sy + d}, {sx + d * 1.5f, sy + d},
                             {sx + d, sy - d * 0.5f}, fc_blend);
            }
            DrawLineEx({sx - d * 1.5f, sy + d}, {sx - d, sy - d}, 1.0f, oc);
            DrawLineEx({sx - d, sy - d}, {sx - d * 0.5f, sy + d}, 1.0f, oc);
            DrawLineEx({sx - d * 0.5f, sy + d}, {sx, sy - d * 1.5f}, 1.0f, oc);
            DrawLineEx({sx, sy - d * 1.5f}, {sx + d * 0.5f, sy + d}, 1.0f, oc);
            DrawLineEx({sx + d * 0.5f, sy + d}, {sx + d, sy - d * 0.5f}, 1.0f, oc);
            DrawLineEx({sx + d, sy - d * 0.5f}, {sx + d * 1.5f, sy + d}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjRoad: {  // single horizontal line
            DrawLineEx({sx - r * 0.85f, sy}, {sx + r * 0.85f, sy}, 2.5f, fc);
            break;
        }
        case SymbolKind::ObjSamSite: {  // triangle with notch (missile silhouette)
            const float d = r * 0.75f;
            if (filled) DrawTriangle({sx, sy - d},
                                     {sx - d, sy + d * 0.7f},
                                     {sx + d, sy + d * 0.7f}, fc_blend);
            DrawLineEx({sx, sy - d}, {sx - d, sy + d * 0.7f}, 1.0f, oc);
            DrawLineEx({sx - d, sy + d * 0.7f}, {sx + d, sy + d * 0.7f}, 1.0f, oc);
            DrawLineEx({sx + d, sy + d * 0.7f}, {sx, sy - d}, 1.0f, oc);
            // Notch (missile silhouette inside)
            DrawLineEx({sx, sy - d * 0.4f}, {sx, sy + d * 0.5f}, 1.5f, oc);
            DrawLineEx({sx, sy - d * 0.4f}, {sx - r * 0.15f, sy - d * 0.15f}, 1.0f, oc);
            DrawLineEx({sx, sy - d * 0.4f}, {sx + r * 0.15f, sy - d * 0.15f}, 1.0f, oc);
            break;
        }
        case SymbolKind::ObjTown: {  // 2 squares

            const float vSize = r * 0.5f;
            for (auto i : { Vector2(-1.0f,0.1f), Vector2(1.0f,-0.2f), Vector2(0.0f,0.5f) }) {
                const Rectangle vRec = { sx - vSize / 2.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize, vSize, vSize * 0.75f };
                if (filled) DrawRectangleRec(vRec, fc_blend);
                DrawRectangleLinesEx(vRec, 1.0f, oc);

                // Roof (Triangle on top of the building)
                const Vector2 v1 = { sx - vSize / 2.0f - 1.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize };
                const Vector2 v2 = { sx + vSize / 2.0f + 1.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize };
                const Vector2 v3 = { sx + i.x * vSize / 2, sy - vSize / 4.0f - vSize * 0.4f + i.y * vSize };
                if (filled) DrawTriangle(v1, v2, v3, fc_blend);
                DrawTriangleLines(v1, v2, v3, oc);
            }

        }
        case SymbolKind::ObjVillage: {

            const float vSize = r * 0.5f;
            for (auto i : { Vector2(0.0f,0.5f) }) {
                const Rectangle vRec = { sx - vSize / 2.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize, vSize, vSize * 0.75f };
                if (filled) DrawRectangleRec(vRec, fc_blend);
                DrawRectangleLinesEx(vRec, 1.0f, oc);

                // Roof (Triangle on top of the building)
                const Vector2 v1 = { sx - vSize / 2.0f - 1.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize };
                const Vector2 v2 = { sx + vSize / 2.0f + 1.0f + i.x * vSize / 2, sy - vSize / 4.0f + i.y * vSize };
                const Vector2 v3 = { sx + i.x * vSize / 2, sy - vSize / 4.0f - vSize * 0.4f + i.y * vSize };
                if (filled) DrawTriangle(v1, v2, v3, fc_blend);
                DrawTriangleLines(v1, v2, v3, oc);
            }
            break;
        }
        case SymbolKind::ObjHarts: {  // concentric circles
            DrawCircleV({sx, sy}, r * 0.7f, fc_blend);
            DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy),
                            static_cast<int>(r * 0.7f), oc);
            DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy),
                            static_cast<int>(r * 0.4f), oc);
            DrawCircleV({sx, sy}, r * 0.12f, oc);
            break;
        }
        case SymbolKind::ObjAirTerminal: {  // airplane silhouette
            const float fw = r * 0.14f;
            const float fh = r * 1.2f;
            if (filled) DrawRectangleRec({sx - fw, sy - fh * 0.5f, fw * 2, fh}, fc);
            else DrawRectangleLinesEx({sx - fw, sy - fh * 0.5f, fw * 2, fh}, 1.0f, oc);
            const float ww = r * 0.8f;
            const float wh = r * 0.16f;
            if (filled) DrawRectangleRec({sx - ww, sy - wh * 0.5f, ww * 2, wh}, fc);
            else DrawRectangleLinesEx({sx - ww, sy - wh * 0.5f, ww * 2, wh}, 1.0f, oc);
            const float tw = r * 0.35f;
            if (filled) DrawRectangleRec({sx - tw, sy + fh * 0.35f, tw * 2, wh * 0.7f}, fc);
            else DrawRectangleLinesEx({sx - tw, sy + fh * 0.35f, tw * 2, wh * 0.7f}, 1.0f, oc);
            if (!filled) {
                DrawRectangleLinesEx({sx - fw, sy - fh * 0.5f, fw * 2, fh}, 1.0f, oc);
            }
            break;
        }
        case SymbolKind::ObjUnknown:
        default: {  // circle
            if (filled) DrawCircleV({sx, sy}, r * 0.6f, fc_blend);
            DrawCircleLines(static_cast<int>(sx), static_cast<int>(sy),
                            static_cast<int>(r * 0.6f), oc);
            break;
        }
    }
}

} // namespace f4::renderer

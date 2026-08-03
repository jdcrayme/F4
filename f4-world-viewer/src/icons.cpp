// f4-world-viewer/src/icons.cpp
//
// Icon-table management for ViewerApp::Impl. This file owns:
//   * The icon-name → IconIndex table (a parallel array to Impl::icons[]).
//   * Impl::load_icons() — loads every PNG from assets/icons/ into
//     Impl::icons[] (a Texture2D array indexed by IconIndex).
//   * Impl::draw_icon() — draws a tinted icon centered at a screen
//     position with a fallback drawn circle for missing icons.
//   * Impl::icon_for_objective_type() — static map from ObjectiveType
//     (1..39, from the class table) to IconIndex.
//   * Impl::icon_for_unit() — map from UnitClass + subtype to IconIndex,
//     with subtype-specific icons (armor/fighter/carrier/...) when
//     available and generic shape fallbacks (square/diamond/circle/
//     triangle) when not.
//
// Split out of the original 1920-LoC viewer_app.cpp god-file (item #5
// of the architecture review). No behavior change — same icon mappings,
// same fallback policy, same asset search path order.

#include "viewer_state.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace f4::viewer {

void ViewerApp::Impl::load_icons() {
    if (icons_loaded) return;
    const char* names[] = {
        "bridge", "village", "town", "city", "factory", "road_intersection",
        "armybase", "sam_site", "airbase", "airstrip", "port", "road",
        "harts", "armor", "artillery", "supply", "infantry", "engineering",
        "fighter", "bomber", "transport", "helicopter", "naval_surface", "carrier",
        "powerplant", "radar", "railroad",
        "square", "diamond", "circle", "triangle"
    };
    static_assert(sizeof(names)/sizeof(names[0]) == ICON_COUNT,
                  "icon name table size mismatch");
    const char* search_dirs[] = {
        "assets/icons",
        "../assets/icons",
        "../../assets/icons",
        "../../../f4-world-viewer/assets/icons",
    };
    for (const char* dir : search_dirs) {
        bool found_any = false;
        for (int i = 0; i < ICON_COUNT; ++i) {
            std::string path = std::string(dir) + "/" + names[i] + ".png";
            if (FileExists(path.c_str())) {
                icons[i] = LoadTexture(path.c_str());
                SetTextureFilter(icons[i], TEXTURE_FILTER_BILINEAR);
                found_any = true;
            }
        }
        if (found_any) break;
    }
    icons_loaded = true;
}

void ViewerApp::Impl::draw_icon(int icon_idx, float sx, float sy, float size_px,
                                 const RlColor& tint) {
    if (icon_idx < 0 || icon_idx >= ICON_COUNT || icons[icon_idx].id == 0) {
        // Fallback circle: fixed small radius so unknown objectives
        // stay readable when zoomed out, instead of giant discs.
        const float fallback_radius = std::min(size_px * 0.4f, 5.0f);
        DrawCircleV({sx, sy}, fallback_radius,
                    Color{tint.r, tint.g, tint.b, 220});
        return;
    }
    const Texture2D& tex = icons[icon_idx];
    const Rectangle src = {0, 0,
                           static_cast<float>(tex.width),
                           static_cast<float>(tex.height)};
    const Rectangle dst = {sx - size_px * 0.5f, sy - size_px * 0.5f,
                           size_px, size_px};
    const Vector2 origin = {0, 0};
    DrawTexturePro(tex, src, dst, origin, 0.0f,
                   Color{tint.r, tint.g, tint.b, 255});
}

int ViewerApp::Impl::icon_for_objective_type(uint8_t obj_type) {
    switch (obj_type) {
        case 1:  return ICON_AIRBASE;            // TYPE_AIRBASE
        case 2:  return ICON_AIRSTRIP;           // TYPE_AIRSTRIP
        case 3:  return ICON_ARMYBASE;           // TYPE_ARMYBASE
        case 6:  return ICON_BRIDGE;             // TYPE_BRIDGE
        case 8:  return ICON_CITY;               // TYPE_CITY
        case 11: return ICON_FACTORY;            // TYPE_FACTORY
        case 15: return ICON_ROAD_INTERSECTION;  // TYPE_INTERSECT
        case 19: return ICON_PORT;               // TYPE_PORT
        case 26: return ICON_ROAD;               // TYPE_ROAD
        case 28: return ICON_TOWN;               // TYPE_TOWN
        case 29: return ICON_VILLAGE;            // TYPE_VILLAGE
        case 30: return ICON_HARTS;              // TYPE_HARTS
        case 31: return ICON_SAM_SITE;           // TYPE_SAM_SITE
        // Legacy icons (kept for types without a dedicated new icon):
        case 20: return ICON_POWERPLANT;         // TYPE_POWERPLANT
        case 21: return ICON_RADAR;              // TYPE_RADAR
        case 24: return ICON_RAILROAD;           // TYPE_RAILROAD
        case 23: return ICON_RAILROAD;           // TYPE_RAIL_TERMINAL (reuse)
        // Types without icons — fall back to circle:
        // 4=BEACH, 5=BORDER, 7=CHEMICAL, 9=COM_CONTROL, 10=DEPOT,
        // 12=FORD, 13=FORTIFICATION, 14=HILL_TOP, 17=NUCLEAR, 18=PASS,
        // 22=RADIO_TOWER, 25=REFINERY, 39=AIR_TERMINAL
        default: return -1;
    }
}

int ViewerApp::Impl::icon_for_unit(f4::world::UnitClass cls, uint8_t subtype) const {
    // Land battalions/brigades: use subtype-specific ground icons.
    if (cls == f4::world::UnitClass::Battalion ||
        cls == f4::world::UnitClass::Brigade) {
        switch (subtype) {
            case 3:  return ICON_ARMOR;        // STYPE_LAND_ARMOR
            case 5:  return ICON_ENGINEERING;  // STYPE_LAND_ENGINEER
            case 7:  return ICON_INFANTRY;     // STYPE_LAND_INFANTRY
            case 11: return ICON_ARTILLERY;    // STYPE_LAND_SP_ARTILLERY
            case 13: return ICON_SUPPLY;       // STYPE_LAND_SUPPLY
            case 14: return ICON_ARTILLERY;    // STYPE_LAND_TOWED_ARTILLERY (reuse)
            // No dedicated icon for: 1=AIR_DEFENSE, 2=AIRMOBILE, 4=ARMORED_CAV,
            // 6=HQ, 8=MARINE, 9=MECHANIZED, 10=ROCKET, 12=SS_MISSILE
            default: break;
        }
        // Fall back to generic shape.
        return (cls == f4::world::UnitClass::Battalion) ? ICON_SQUARE : ICON_DIAMOND;
    }
    // Squadrons: use subtype-specific air icons.
    if (cls == f4::world::UnitClass::Squadron) {
        switch (subtype) {
            case 1:  return ICON_TRANSPORT;    // STYPE_AIR_AIR_TRANSPORT
            case 4:  return ICON_HELICOPTER;   // STYPE_AIR_ATTACK_HELO
            case 6:  return ICON_BOMBER;       // STYPE_AIR_BOMBER
            case 8:  return ICON_FIGHTER;      // STYPE_AIR_FIGHTER
            case 9:  return ICON_FIGHTER;      // STYPE_AIR_FIGHTER_BOMBER (reuse)
            case 13: return ICON_TRANSPORT;    // STYPE_AIR_TANKER (reuse transport)
            case 14: return ICON_HELICOPTER;   // STYPE_AIR_TRANSPORT_HELO
            // No dedicated icon for: 2=ASW, 3=ATTACK, 5=AWACS, 7=ECM,
            // 10=JSTAR, 11=RECON, 12=RECON_HELO
            default: break;
        }
        return ICON_CIRCLE;
    }
    // Task forces: use subtype-specific naval icons.
    if (cls == f4::world::UnitClass::TaskForce) {
        switch (subtype) {
            case 3:  return ICON_CARRIER;        // STYPE_SEA_CARRIER
            // No dedicated icon for: 1=AMPHIBIOUS, 2=BATTLESHIP, 4=CRUISER,
            // 5=DESTROYER, 6=FRIGATE, 7=PATROL, 8/9/10=supply/tanker/transport
            default: return ICON_TRIANGLE;
        }
    }
    return -1;
}

} // namespace f4::viewer

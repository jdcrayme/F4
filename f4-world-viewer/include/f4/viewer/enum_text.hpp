// f4-world-viewer/include/f4/viewer/enum_text.hpp
//
// Human-readable decoders for integer fields that the viewer displays but
// that don't have a dedicated decoder in f4-world / f4-world-convert yet.
//
// These are viewer-presentation concerns (turning a raw byte into a short
// label for an ImGui Text line or a hex annotation), so they live here
// rather than polluting the engine-agnostic libraries. Each is a pure
// function — no globals, no allocation, safe to call from any thread.
//
// Where an equivalent decoder already exists in f4-world-convert
// (point_type_name, point_list_type_name, movement_type_name,
// unit_subtype_name, objective_type_name), USE THAT — this header only
// fills the gaps.
//
// Conventions:
//   - All functions take the raw integer (uint8_t / uint16_t / uint32_t /
//     int32_t) exactly as it appears in the WorldState struct.
//   - Return value is always const char* (no std::string — these are
//     called hundreds of times per frame and must stay allocation-free).
//   - Unknown / out-of-range values return "Unknown" (or similar) so the
//     UI degrades gracefully instead of showing garbage.
//   - For bitmaps (flags), use the *_text() variants that write into a
//     caller-supplied buffer. The buffer must be large enough for the
//     worst-case string; each function documents its max size.

#pragma once

#include <f4/world_types/class_table.hpp>   // DataType enum (DTYPE_*)
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace f4::viewer {

// ---------------------------------------------------------------------------
// Control enum (campbase.h) — owner / first_owner byte on every entity.
// 0=neutral, 1=enemy, 2=friendly, 3..7=team slot index. The first three
// are the canonical FreeFalcon Control enum values; 3..7 are team slots
// (ROK / Japan / DPRK / PRC / ...) resolved separately via WorldState.teams.
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* control_name(uint8_t control) noexcept {
    switch (control) {
        case 0: return "Neutral";
        case 1: return "Enemy";
        case 2: return "Friendly";
        default: return "Team";
    }
}

// ---------------------------------------------------------------------------
// VU_DOMAIN (classtbl.h) — domain byte on ClassTableEntry and UnitState.
// 0/1=abstract/animal (rare), 2=Air, 3=Land, 4=Sea. Returns "Unknown" for
// anything else.
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* domain_name(uint8_t domain) noexcept {
    switch (domain) {
        case 2: return "Air";
        case 3: return "Land";
        case 4: return "Sea";
        case 0: return "Abstract";
        case 1: return "Animal";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// VU_CLASS (classtbl.h) — class byte on ClassTableEntry. Tells whether
// the entity is an objective / unit / vehicle / feature / weapon / ...
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* vu_class_name(uint8_t cls) noexcept {
    switch (cls) {
        case 0: return "Abstract";
        case 1: return "Animal";
        case 2: return "Feature";
        case 3: return "Manager";
        case 4: return "Objective";
        case 5: return "SFX";
        case 6: return "Unit";
        case 7: return "Vehicle";
        case 8: return "Weapon";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// DataType (classtbl.h) — tells which theater-data table a class-table
// entry's dataPtr points into. Critical for navigating from a class-table
// entry to its corresponding OCD/UCD/VCD/FCD record.
//
// Delegates to the verified DataType enum in class_table.hpp rather than
// duplicating the mapping here. The enum values were verified against real
// Falcon4.ct data (Phase 1 fix).
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* data_type_name(uint8_t dt) noexcept {
    using f4::world_types::DataType;
    switch (static_cast<DataType>(dt)) {
        case DataType::DTYPE_NOTHING:       return "Nothing";
        case DataType::DTYPE_FEATURE:       return "Feature";      // -> Falcon4.FCD
        case DataType::DTYPE_OBJECTIVE:     return "Objective";    // -> Falcon4.OCD
        case DataType::DTYPE_UNIT:          return "Unit";         // -> Falcon4.UCD
        case DataType::DTYPE_VEHICLE:       return "Vehicle";      // -> Falcon4.VCD
        case DataType::DTYPE_WEAPON:        return "Weapon";       // -> Falcon4.WCD
        case DataType::DTYPE_SQUAD_STORES:  return "SquadStores";  // -> Falcon4.SSD
        default:                            return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// PtDataFlags (ptdata.h) — bitmap on each PtData record. Three bits are
// documented: PT_FIRST (first point of a list), PT_LAST (last point),
// PT_OCCUPIED (slot is currently in use). Writes "FIRST|LAST|OCCUPIED"
// style into the caller's buffer. Buffer must be >= 32 bytes.
// ---------------------------------------------------------------------------
inline void point_flags_text(uint8_t flags, char* buf, std::size_t buf_size) noexcept {
    if (buf_size == 0) return;
    if (flags == 0) {
        std::snprintf(buf, buf_size, "-");
        return;
    }
    char local[32];
    char* p = local;
    std::size_t remaining = sizeof(local);
    bool add_sep = false;

    auto append = [&](const char* label) {
        if (remaining < std::strlen(label) + 2) return;
        int n = std::snprintf(p, remaining, "%s%s", add_sep ? "|" : "", label);
        if (n > 0) { p += n; remaining -= static_cast<std::size_t>(n); add_sep = true; }
    };

    if (flags & 0x01) append("FIRST");
    if (flags & 0x02) append("LAST");
    if (flags & 0x04) append("OCCUPIED");
    // Catch-all for any remaining bits
    const uint8_t decoded_mask = 0x07;
    const uint8_t leftover = flags & ~decoded_mask;
    if (leftover) {
        char tail[8];
        std::snprintf(tail, sizeof(tail), "0x%02x", leftover);
        append(tail);
    }
    if (local[0] == '\0') {
        std::snprintf(local, sizeof(local), "0x%02x", flags);
    }
    std::snprintf(buf, buf_size, "%s", local);
}

// ---------------------------------------------------------------------------
// ltrt (ptdata.h) — left/right offset flag on PtHeaderData. -1 = left,
// +1 = right, 0 = neither (centerline). Returns a short string.
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* ltrt_name(int8_t ltrt) noexcept {
    if (ltrt < 0) return "Left";
    if (ltrt > 0) return "Right";
    return "Center";
}

// ---------------------------------------------------------------------------
// WP_ACTION (campwp.h) — waypoint action byte, AUTHORITATIVE table ported
// from FreeFalcon's src/campaign/include/campwp.h (verified against the
// shallow clone during the B.3 tranche). The pre-B.3 table was a guess
// that swapped TAKEOFF/LAND (it read route action 1 at the departure
// airbase as "Land" and action 7 back home as "Escort") — the saved
// TestCamp routes only make sense with the real mapping.
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* wp_action_name(uint8_t action) noexcept {
    switch (action) {
        case 0:  return "Nothing";      // WP_NOTHING
        case 1:  return "Takeoff";      // WP_TAKEOFF
        case 2:  return "Assemble";     // WP_ASSEMBLE
        case 3:  return "PostAsm";      // WP_POSTASSEMBLE
        case 4:  return "Refuel";       // WP_REFUEL
        case 5:  return "Rearm";        // WP_REARM
        case 6:  return "Pickup";       // WP_PICKUP
        case 7:  return "Land";         // WP_LAND
        case 8:  return "Timing";       // WP_TIMING
        case 9:  return "CASCP";        // WP_CASCP
        case 10: return "Escort";       // WP_ESCORT
        case 11: return "CA";           // WP_CA
        case 12: return "CAP";          // WP_CAP
        case 13: return "Intercept";    // WP_INTERCEPT
        case 14: return "GndStrike";    // WP_GNDSTRIKE
        case 15: return "NavStrike";    // WP_NAVSTRIKE
        case 16: return "SAD";          // WP_SAD
        case 17: return "Strike";       // WP_STRIKE
        case 18: return "Bomb";         // WP_BOMB
        case 19: return "SEAD";         // WP_SEAD
        case 20: return "ELINT";        // WP_ELINT
        case 21: return "Recon";        // WP_RECON
        case 22: return "Rescue";       // WP_RESCUE
        case 23: return "ASW";          // WP_ASW
        case 24: return "Tanker";       // WP_TANKER
        case 25: return "Airdrop";      // WP_AIRDROP
        case 26: return "Jam";          // WP_JAM
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Pilot status (squadron roster). 0=available, 1=dead, 2=on leave,
// 3=in hospital. Mirrors the inline switch already in imgui_panels.cpp
// but exposed as a function so other call sites can use it.
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* pilot_status_name(uint8_t status) noexcept {
    switch (status) {
        case 0: return "OK";
        case 1: return "Dead";
        case 2: return "Leave";
        case 3: return "Hosp";
        default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Pilot skill / rating. Per the FreeFalcon reference (simbrain.h):
//   skillLevel: 0=Recruit, 1=Rookie, 2=Veteran, 3=Ace, 4=???
// The on-disk byte packs skill (low nibble) + rating (high nibble), so
// pass the nibble directly. For values > 4, returns "Unknown".
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* pilot_skill_name(uint8_t skill) noexcept {
    switch (skill) {
        case 0: return "Recruit";
        case 1: return "Rookie";
        case 2: return "Veteran";
        case 3: return "Ace";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// Squadron specialty (ARO_* air role). The full enum is in falcent.h
// which is not yet ported. Best-effort mapping based on common values
// seen in fixture data.
// ---------------------------------------------------------------------------
[[nodiscard]] inline const char* squadron_specialty_name(uint8_t spec) noexcept {
    switch (spec) {
        case 0:  return "None";
        case 1:  return "Air-to-Air";
        case 2:  return "Air-to-Ground";
        case 3:  return "SEAD";
        case 4:  return "Recon";
        case 5:  return "Air Refuel";
        case 6:  return "ASW";
        case 7:  return "Strat Bomber";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// obj_flags bitmap (ObjectiveClass.obj_flags, uint32). Only the well-known
// bits are decoded; the rest are shown as a hex tail. Writes into the
// caller's buffer. Buffer must be >= 128 bytes.
//
// Documented bits (from objectiv.h OF_* flags, partial):
//   bit 0  (0x00000001) — OF_TARGETED
//   bit 1  (0x00000002) — OF_EASY_TO_CAPTURE
//   bit 8  (0x00000100) — OF_RADAR_VISIBLE
//   bit 9  (0x00000200) — OF_TACTICAL_SHIFT
//   bit 10 (0x00000400) — OF_SUPPORT_PLANE
//   bit 16 (0x00010000) — OF_INVISIBLE_TO_AI (no enemy spawn)
// ---------------------------------------------------------------------------
inline void obj_flags_text(uint32_t flags, char* buf, std::size_t buf_size) noexcept {
    if (buf_size == 0) return;
    if (flags == 0) {
        std::snprintf(buf, buf_size, "0");
        return;
    }
    char local[128];
    char* p = local;
    std::size_t remaining = sizeof(local);
    bool add_sep = false;

    auto append = [&](const char* label) {
        if (remaining < std::strlen(label) + 2) return;
        int n = std::snprintf(p, remaining, "%s%s", add_sep ? "|" : "", label);
        if (n > 0) { p += n; remaining -= static_cast<std::size_t>(n); add_sep = true; }
    };

    if (flags & 0x00000001u) append("TARGETED");
    if (flags & 0x00000002u) append("EASY_CAP");
    if (flags & 0x00000100u) append("RADAR_VIS");
    if (flags & 0x00000200u) append("TAC_SHIFT");
    if (flags & 0x00000400u) append("SUPPORT");
    if (flags & 0x00010000u) append("AI_INVIS");
    // Catch-all for any remaining bits
    const uint32_t decoded_mask = 0x00010703u;
    const uint32_t leftover = flags & ~decoded_mask;
    if (leftover) {
        char tail[16];
        std::snprintf(tail, sizeof(tail), "0x%08x", leftover);
        append(tail);
    }
    std::snprintf(buf, buf_size, "%s", local);
}

// ---------------------------------------------------------------------------
// Campaign clock formatting (B.3 QC tranche) — CampaignTime is a second
// count. Absolute times in saves are large (TestCamp's current_time is
// 38,574,360 ≈ day 0, 10:42), so the canonical display is "D# HH:MM:SS".
// Negative values render as-is (relative negatives happen on MOT fields).
// ---------------------------------------------------------------------------
[[nodiscard]] inline void format_campaign_time(int32_t t,
                                               char* buf,
                                               std::size_t buf_size) noexcept {
    if (t < 0) {
        std::snprintf(buf, buf_size, "%d", t);
        return;
    }
    const int32_t days  = t / 86400;
    const int32_t hours = (t % 86400) / 3600;
    const int32_t mins  = (t % 3600) / 60;
    const int32_t secs  = t % 60;
    std::snprintf(buf, buf_size, "D%d %02d:%02d:%02d",
                  days, hours, mins, secs);
}

} // namespace f4::viewer

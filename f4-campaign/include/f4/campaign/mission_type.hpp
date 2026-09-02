// f4-campaign/include/f4/campaign/mission_type.hpp
//
// The MissionType wire table — campaign mission bytes ↔ FreeFalcon names.
//
// FreeFalcon's campaign stores a mission as a single byte on every Flight
// unit (UnitClass-derived mission field, decoded by f4-world-convert into
// WorldState's Flight.mission). The byte indexes the static MissionData[]
// table in FreeFalcon's mission.cpp; the canonical enum lives in
// mission.h. This header is F4's version of that enum: a fixed 41-entry
// wire-name table, the single place where mission bytes get names.
//
// Binding rule (NEXT_PHASE_PLAN.md B.1): profiles bind to the mission
// BYTE through this table — exactly like combat_event_kind_name() — so
// both the JSON on disk and the C++ side key off the same names, and no
// switch/if-else chain ever selects behavior on the raw byte.
//
// Byte order (FreeFalcon mission.h):
//   * 0        AMIS_NONE      — no mission assigned
//   * 1–35     the combat/support mission types (BARCAP first, ASHIP last)
//   * 36       AMIS_PATROL
//   * 37–40    AMIS_TRAINING, AMIS_OTHER, AMIS_TANK, AMIS_SEARCH
// Corroboration: all 36 combat/support names + PATROL appear in the
// FreeFalcon Core Systems Reference (counter-air / strike / GA / support
// tables + SEADESCORT + PATROL); the kunsan campaign fixture's real-save
// flights carry byte 1 (AMIS_BARCAP). The tail four (TRAINING/OTHER/
// TANK/SEARCH) are mission.h's utility types.
//
// Zero dependencies. C++20.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace f4::campaign {

/// Count of wire mission types including AMIS_NONE (bytes 0..40).
inline constexpr std::size_t kMissionTypeCount = 41;

/// Fixed table: mission byte -> canonical FreeFalcon name.
/// Index with any uint8_t — out-of-range (>= kMissionTypeCount) is
/// undefined at the wire level, so callers use mission_type_name()
/// (which handles the range check) instead of indexing directly.
inline constexpr std::string_view kMissionTypeNames[kMissionTypeCount] = {
    "AMIS_NONE",        // 0
    "AMIS_BARCAP",      // 1
    "AMIS_BARCAP2",     // 2
    "AMIS_HAVCAP",      // 3
    "AMIS_TARCAP",      // 4
    "AMIS_RESCAP",      // 5
    "AMIS_AMBUSHCAP",   // 6
    "AMIS_SWEEP",       // 7
    "AMIS_ALERT",       // 8
    "AMIS_INTERCEPT",   // 9
    "AMIS_ESCORT",      // 10
    "AMIS_SEADESCORT",  // 11
    "AMIS_OCASTRIKE",   // 12
    "AMIS_INTSTRIKE",   // 13
    "AMIS_STRIKE",      // 14
    "AMIS_DEEPSTRIKE",  // 15
    "AMIS_STSTRIKE",    // 16
    "AMIS_SEADSTRIKE",  // 17
    "AMIS_ONCALLCAS",   // 18
    "AMIS_PRPLANCAS",   // 19
    "AMIS_CAS",         // 20
    "AMIS_SAD",         // 21
    "AMIS_INT",         // 22
    "AMIS_BAI",         // 23
    "AMIS_STRATBOMB",   // 24
    "AMIS_AWACS",       // 25
    "AMIS_JSTAR",       // 26
    "AMIS_TANKER",      // 27
    "AMIS_ECM",         // 28
    "AMIS_RECON",       // 29
    "AMIS_BDA",         // 30
    "AMIS_FAC",         // 31
    "AMIS_SAR",         // 32
    "AMIS_AIRLIFT",     // 33
    "AMIS_ASW",         // 34
    "AMIS_ASHIP",       // 35
    "AMIS_PATROL",      // 36
    "AMIS_TRAINING",    // 37
    "AMIS_OTHER",       // 38
    "AMIS_TANK",        // 39
    "AMIS_SEARCH",      // 40
};

/// Mission byte -> name. Returns "AMIS_?" for bytes beyond the table
/// (keeps diagnostics rendering for corrupt campaign data instead of UB).
[[nodiscard]] std::string_view mission_type_name(std::uint8_t mission_byte);

/// Name -> mission byte. Case-sensitive exact match against the table.
/// nullopt when the name is not a FreeFalcon mission type.
[[nodiscard]] std::optional<std::uint8_t>
mission_type_byte(std::string_view name);

/// True when the byte carries a real mission (1..40 — AMIS_NONE(0) means
/// "not tasked", which has no profile).
[[nodiscard]] constexpr bool is_mission_tasked(std::uint8_t mission_byte) {
    return mission_byte != 0;
}

// ============================================================================
// Mission categories (B.3 tranche)
// ============================================================================
//
// The 41 wire mission types collapse into a small set of BEHAVIORAL
// categories — the granularity at which the world viewer renders flights
// (symbol glyph per category), at which QC filters operate, and at which
// the B.3 spawner picks a default route profile. The mapping is many-to-
// one and total: every byte 0..40 lands in exactly one category.
//
// Mapping (from FreeFalcon's own mission grouping in mission.h plus the
// Core Systems Reference counter-air / strike / GA / support tables):
//   None        0 (AMIS_NONE — untasked)
//   CAP         1-6 (BARCAP..AMBUSHCAP), 36 (PATROL)
//   Sweep       7 (SWEEP)
//   Intercept   8-9 (ALERT, INTERCEPT)
//   Escort      10-11 (ESCORT, SEADESCORT)
//   Strike      12-16 (OCASTRIKE..STSTRIKE), 24 (STRATBOMB)
//   SEAD        17 (SEADSTRIKE)
//   CAS         18-21 (ONCALLCAS..SAD), 23 (BAI), 31 (FAC)
//   Recon       22 (INT), 29 (RECON), 30 (BDA)
//   Support     25-28 (AWACS, JSTAR, TANKER, ECM), 32-35 (SAR..ASHIP),
//               39-40 (TANK, SEARCH)
//   Other       37-38 (TRAINING, OTHER)

enum class MissionCategory : std::uint8_t {
    None = 0,
    CAP,
    Sweep,
    Intercept,
    Escort,
    Strike,
    SEAD,
    CAS,
    Recon,
    Support,
    Other,
};

/// Mission byte -> behavioral category. Out-of-range bytes map to Other
/// (same "render something sane on corrupt data" rule as the name table).
[[nodiscard]] constexpr MissionCategory
mission_category(std::uint8_t mission_byte) noexcept {
    switch (mission_byte) {
        case 0:  return MissionCategory::None;
        case 1: case 2: case 3: case 4: case 5: case 6: case 36:
            return MissionCategory::CAP;
        case 7:  return MissionCategory::Sweep;
        case 8: case 9:
            return MissionCategory::Intercept;
        case 10: case 11:
            return MissionCategory::Escort;
        case 12: case 13: case 14: case 15: case 16: case 24:
            return MissionCategory::Strike;
        case 17: return MissionCategory::SEAD;
        case 18: case 19: case 20: case 21: case 23: case 31:
            return MissionCategory::CAS;
        case 22: case 29: case 30:
            return MissionCategory::Recon;
        case 25: case 26: case 27: case 28:
        case 32: case 33: case 34: case 35:
        case 39: case 40:
            return MissionCategory::Support;
        case 37: case 38:
            return MissionCategory::Other;
        default: return MissionCategory::Other;
    }
}

/// Category -> short display name ("CAP", "Strike", ...). The name is the
/// stable rendering key for the viewer's symbol table and QC filters.
[[nodiscard]] constexpr std::string_view
mission_category_name(MissionCategory cat) noexcept {
    switch (cat) {
        case MissionCategory::None:      return "None";
        case MissionCategory::CAP:       return "CAP";
        case MissionCategory::Sweep:     return "Sweep";
        case MissionCategory::Intercept: return "Intercept";
        case MissionCategory::Escort:    return "Escort";
        case MissionCategory::Strike:    return "Strike";
        case MissionCategory::SEAD:      return "SEAD";
        case MissionCategory::CAS:       return "CAS";
        case MissionCategory::Recon:     return "Recon";
        case MissionCategory::Support:   return "Support";
        case MissionCategory::Other:     return "Other";
    }
    return "Other";  // silence pedantic fall-through warnings
}

/// The Air-Role (ARO) wire table — squadron specialty bytes ↔ names.
///
/// Squadrons carry a `specialty` byte (their primary air role, used for
/// ATM squadron matching); profiles name their required role as a string
/// ("ARO_CA" ...). This is the binding between the two, same discipline
/// as the mission table above.
///
/// Order: Counter-Air first (ARO_CA = 0 — corroborated by the kunsan
/// fixture, whose fighter squadrons carry specialty 0 and whose profiles
/// must match them), then Strike / Ground-Attack / Strategic-Bombing /
/// Reconnaissance / Support — the reference's role grouping order.
/// Bytes beyond the table render "ARO_?" (same rule as AMIS_?).
inline constexpr std::size_t kAroCount = 6;
inline constexpr std::string_view kAroNames[kAroCount] = {
    "ARO_CA",       // 0 — counter-air (fighters)
    "ARO_S",        // 1 — strike
    "ARO_GA",       // 2 — ground attack
    "ARO_SB",       // 3 — strategic bombing
    "ARO_REC",      // 4 — reconnaissance
    "ARO_SUPPORT",  // 5 — support & special (AEW&C, tanker, SAR, ...)
};

/// Squadron specialty byte -> role name ("ARO_?" when out of range).
[[nodiscard]] std::string_view aro_name(std::uint8_t specialty_byte);

/// Role name -> specialty byte (nullopt when unknown).
[[nodiscard]] std::optional<std::uint8_t> aro_byte(std::string_view name);

} // namespace f4::campaign

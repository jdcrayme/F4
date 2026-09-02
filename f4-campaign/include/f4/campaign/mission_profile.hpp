// f4-campaign/include/f4/campaign/mission_profile.hpp
//
// MissionProfile — F4's version of FreeFalcon's MissionDataType row
// (mission.cpp's static MissionData[]), loaded from JSON instead of
// compiled in (NEXT_PHASE_PLAN.md B.1 / Architecture Proposal M4.1).
//
// FreeFalcon keys mission behavior off MissionTypeEnum straight into a
// C array — every subsystem reads the same row: the ATM picks package
// composition from str/escorttype, the route builder flies minalt/
// maxalt/missionalt, the scheduler uses min_time/max_time, and the sim
// ladder reads the behavior flags. F4 keeps the SHAPE of that table but
// makes it data: one record per mission type in MissionProfiles.json,
// validated once at load, selected per Flight by its mission BYTE
// through the wire-name table (mission_type.hpp). No switch anywhere.
//
// The loader contract (mirrors ClassTable's vis_type discipline):
//   * A mission byte with no profile record is a LOAD ERROR — the table
//     must cover every tasked byte 1..40 (AMIS_NONE carries no mission
//     and has no record; validate() enforces exactly that coverage).
//   * Unknown values (bad name/byte pairing, unknown vocabulary key,
//     duplicate records) throw with the offending value and the source
//     file in the message. Loud, never defaulted.
//   * A loaded-and-validated table is immutable; for_mission() is an
//     array index (O(1), no search, no allocation).
//
// Field semantics follow the FreeFalcon Core Systems Reference §Part I
// "The Mission Data Table": altitudes are HUNDREDS of feet at the
// target, loitertime is minutes, separation is seconds, min/max_time is
// the planning-advance window in minutes, str is the default aircraft
// count per flight, escorttype is the escorting mission's WIRE BYTE
// (0 = none), caps are required vehicle capabilities (VEH_STEALTH,
// VTOL, ...), and flags are the behavioral toggles the tasking/sim
// ladders consume (ADDAWACS, AVOIDTHREAT, IMMEDIATE, ...). Route and
// waypoint action keys (target_desc/routewp/targetwp/target_profile)
// are carried as validated-vocabulary-free strings for now — their
// consumers arrive with the route tranche (M4.3–M4.5), which owns that
// vocabulary.
//
// Dependencies: f4-json (Reader), f4-io (read_file). C++20.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace f4::campaign {

/// One mission type's behavior specification (one MissionData[] row).
struct MissionProfile {
    std::string name;              ///< canonical FreeFalcon name ("AMIS_BARCAP")
    std::uint8_t mission_byte{0};  ///< the wire byte this profile owns

    /// Target class: "AMIS_TAR_NONE" | "OBJECTIVE" | "UNIT" | "LOCATION".
    std::string target;
    /// Primary air-role rating key for squadron matching:
    /// "ARO_CA" | "ARO_S" | "ARO_GA" | "ARO_SB" | "ARO_SUPPORT" | "ARO_NONE".
    std::string aro;
    /// Altitude profile: "MPROF_LOW" | "MPROF_STANDARD" | "MPROF_HIGH".
    std::string altitude_profile;

    // Route/target shaping keys (consumed by the M4.3–M4.5 tranche;
    // free-form strings here — the route tranche owns the vocabulary).
    std::string target_profile;    ///< waypoint pattern at target ("TPROF_*")
    std::string target_desc;       ///< when route actions apply (TTL/ATA/TAO)
    std::string routewp;           ///< ingress/egress waypoint action
    std::string targetwp;          ///< target waypoint action ("WP_STRIKE" ...)

    /// Altitudes at the target, HUNDREDS of feet (FreeFalcon convention).
    int minalt{0};                 ///< minimum target altitude
    int maxalt{0};                 ///< maximum target altitude
    int missionalt{0};             ///< suggested cruise altitude

    int separation{0};             ///< time offset for escort flights (seconds)
    int loitertime{0};             ///< loiter/station time (minutes)
    int str{0};                    ///< default aircraft count per flight
    int min_time{0};               ///< min planning advance (minutes)
    int max_time{0};               ///< max planning advance (minutes)
    std::uint8_t escort_type{0};   ///< escort mission byte (0 = none)
    int mindistance{0};            ///< min distance between similar missions
    int mintime{0};                ///< min time between similar missions

    /// Required vehicle capabilities ("VEH_STEALTH", "VTOL", ...).
    std::vector<std::string> caps;
    /// Behavioral flags ("ADDAWACS", "AVOIDTHREAT", "IMMEDIATE", ...).
    std::vector<std::string> flags;

    /// Convenience: true when the record requires a capability.
    [[nodiscard]] bool requires_cap(std::string_view cap) const noexcept {
        for (const auto& c : caps) {
            if (c == cap) return true;
        }
        return false;
    }

    /// Convenience: true when the behavioral flag is set.
    [[nodiscard]] bool has_flag(std::string_view flag) const noexcept {
        for (const auto& f : flags) {
            if (f == flag) return true;
        }
        return false;
    }
};

/// The loaded, validated profile table — one record per tasked mission
/// byte, indexed by byte. Selection is a flat array read: the Flight's
/// mission byte goes in, the profile comes out — no switch, no chain.
class MissionProfileTable {
public:
    MissionProfileTable() = default;

    /// Load + validate from a MissionProfiles.json file. Throws
    /// std::runtime_error (prefix "mission_profile:") on any malformed
    /// record, duplicate byte/name, or coverage gap.
    static MissionProfileTable load(const std::filesystem::path& json_path);

    /// Load + validate from an in-memory JSON string (tests, embedded
    /// data). `source_name` labels error messages.
    static MissionProfileTable load_from_string(std::string_view json,
                                                std::string_view source_name);

    /// Re-run the coverage/consistency validation on an existing table
    /// (used by tests and by callers that assembled a table by hand).
    /// Throws with the missing/duplicated bytes in the message.
    void validate() const;

    /// Profile for a mission byte. AMIS_NONE (0) and bytes >= 41 throw —
    /// "not tasked" and corrupt bytes have no profile. The error carries
    /// the byte, its wire name (or AMIS_?), and the source file.
    [[nodiscard]] const MissionProfile&
    for_mission(std::uint8_t mission_byte) const;

    /// Profile by canonical name (AMIS_BARCAP). Throws when unknown.
    [[nodiscard]] const MissionProfile&
    for_name(std::string_view mission_name) const;

    /// All records, byte order (0 is never present — see validate()).
    [[nodiscard]] const std::vector<MissionProfile>& profiles() const noexcept {
        return profiles_;
    }

    /// Number of loaded records (40 when fully validated: bytes 1..40).
    [[nodiscard]] std::size_t size() const noexcept { return profiles_.size(); }

    /// The source label carried in error messages.
    [[nodiscard]] std::string_view source_name() const noexcept {
        return source_name_;
    }

private:
    /// Indexed by mission byte — index 0 (AMIS_NONE) is never occupied;
    /// profiles_ has exactly kMissionTypeCount slots after load, minus
    /// the unused 0 slot kept as a default-constructed sentinel.
    std::vector<MissionProfile> profiles_;
    std::string source_name_{"<unloaded>"};
};

} // namespace f4::campaign

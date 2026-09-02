// f4-campaign/src/mission_type.cpp
//
// Implementation of the MissionType wire table — see mission_type.hpp.

#include <f4/campaign/mission_type.hpp>

namespace f4::campaign {

std::string_view mission_type_name(std::uint8_t mission_byte) {
    if (mission_byte >= kMissionTypeCount) return "AMIS_?";
    return kMissionTypeNames[mission_byte];
}

std::optional<std::uint8_t> mission_type_byte(std::string_view name) {
    for (std::size_t i = 0; i < kMissionTypeCount; ++i) {
        if (kMissionTypeNames[i] == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

std::string_view aro_name(std::uint8_t specialty_byte) {
    if (specialty_byte >= kAroCount) return "ARO_?";
    return kAroNames[specialty_byte];
}

std::optional<std::uint8_t> aro_byte(std::string_view name) {
    for (std::size_t i = 0; i < kAroCount; ++i) {
        if (kAroNames[i] == name) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return std::nullopt;
}

} // namespace f4::campaign

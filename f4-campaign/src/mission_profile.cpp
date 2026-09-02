// f4-campaign/src/mission_profile.cpp
//
// Implementation of the mission profile table — see mission_profile.hpp
// for the loader contract and field semantics.

#include <f4/campaign/mission_profile.hpp>

#include <f4/campaign/mission_type.hpp>
#include <f4/io/read_file.hpp>
#include <f4/json/f4_json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace f4::campaign {

namespace {

// Fixed vocabularies validated at load. Everything else (route/waypoint
// action keys) is deliberately free-form — the M4.3–M4.5 route tranche
// owns that vocabulary and its consumers.
constexpr std::initializer_list<std::string_view> kTargetValues = {
    "AMIS_TAR_NONE", "OBJECTIVE", "UNIT", "LOCATION",
};
constexpr std::initializer_list<std::string_view> kAroValues = {
    "ARO_NONE", "ARO_CA", "ARO_S", "ARO_GA", "ARO_SB", "ARO_SUPPORT",
};
constexpr std::initializer_list<std::string_view> kAltitudeProfileValues = {
    "MPROF_LOW", "MPROF_STANDARD", "MPROF_HIGH",
};

[[nodiscard]] bool in_vocabulary(
    std::string_view value, std::initializer_list<std::string_view> vocab) {
    for (const auto v : vocab) {
        if (v == value) return true;
    }
    return false;
}

[[noreturn]] void fail(const std::string& source, const std::string& what) {
    throw std::runtime_error("mission_profile: " + source + ": " + what);
}

// Parse one profile object (the Reader sits on the '{').
MissionProfile parse_profile(f4::json::Reader& r, const std::string& source) {
    MissionProfile p;
    r.expect('{');
    bool first = true;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "name")                p.name = r.read_string();
        else if (key == "mission_byte")   p.mission_byte = static_cast<std::uint8_t>(r.read_int());
        else if (key == "target")         p.target = r.read_string();
        else if (key == "aro")            p.aro = r.read_string();
        else if (key == "altitude_profile") p.altitude_profile = r.read_string();
        else if (key == "target_profile") p.target_profile = r.read_string();
        else if (key == "target_desc")    p.target_desc = r.read_string();
        else if (key == "routewp")        p.routewp = r.read_string();
        else if (key == "targetwp")       p.targetwp = r.read_string();
        else if (key == "minalt")         p.minalt = static_cast<int>(r.read_int());
        else if (key == "maxalt")         p.maxalt = static_cast<int>(r.read_int());
        else if (key == "missionalt")     p.missionalt = static_cast<int>(r.read_int());
        else if (key == "separation")     p.separation = static_cast<int>(r.read_int());
        else if (key == "loitertime")     p.loitertime = static_cast<int>(r.read_int());
        else if (key == "str")            p.str = static_cast<int>(r.read_int());
        else if (key == "min_time")       p.min_time = static_cast<int>(r.read_int());
        else if (key == "max_time")       p.max_time = static_cast<int>(r.read_int());
        else if (key == "escort_type")    p.escort_type = static_cast<std::uint8_t>(r.read_int());
        else if (key == "mindistance")    p.mindistance = static_cast<int>(r.read_int());
        else if (key == "mintime")        p.mintime = static_cast<int>(r.read_int());
        else if (key == "caps" || key == "flags") {
            auto& list = (key == "caps") ? p.caps : p.flags;
            r.expect('[');
            bool lfirst = true;
            while (!r.consume(']')) {
                if (!lfirst) r.expect(',');
                lfirst = false;
                list.push_back(r.read_string());
            }
        } else {
            r.skip_value();
        }
    }
    (void)source; // per-record errors carry the source at the table level
    return p;
}

} // namespace

MissionProfileTable
MissionProfileTable::load_from_string(std::string_view json,
                                      std::string_view source_name) {
    MissionProfileTable table;
    table.source_name_ = std::string(source_name);
    // One record slot per wire byte, including the AMIS_NONE(0) sentinel
    // (kept unoccupied — validate() enforces that it stays that way).
    table.profiles_.assign(kMissionTypeCount, MissionProfile{});

    // Reader pins a const std::string& — materialize the view's bytes.
    const std::string text(json);
    f4::json::Reader r(text);
    r.skip_ws();
    r.expect('{');
    bool first = true;
    bool saw_header = false;
    std::size_t loaded = 0;
    while (!r.consume('}')) {
        if (!first) r.expect(',');
        first = false;
        const auto key = r.read_string();
        r.expect(':');
        if (key == "format") {
            const auto fmt = r.read_string();
            if (fmt != "f4-mission-profiles") {
                fail(table.source_name_,
                     "unknown format '" + fmt + "' (expected f4-mission-profiles)");
            }
            saw_header = true;
        } else if (key == "version") {
            const auto version = static_cast<int>(r.read_int());
            if (version != 1) {
                fail(table.source_name_,
                     "unsupported version " + std::to_string(version));
            }
        } else if (key == "profiles") {
            r.expect('[');
            bool pfirst = true;
            while (!r.consume(']')) {
                if (!pfirst) r.expect(',');
                pfirst = false;
                MissionProfile p = parse_profile(r, table.source_name_);

                // --- Per-record validation (loud, value + source) ---
                if (p.mission_byte >= kMissionTypeCount) {
                    fail(table.source_name_,
                         "profile '" + p.name + "' has out-of-range mission_byte " +
                             std::to_string(p.mission_byte));
                }
                if (p.mission_byte == 0) {
                    fail(table.source_name_,
                         "AMIS_NONE (byte 0) carries no mission and has no profile");
                }
                const auto wire_name = mission_type_name(p.mission_byte);
                if (p.name != wire_name) {
                    fail(table.source_name_,
                         "profile '" + p.name + "' claims mission_byte " +
                             std::to_string(p.mission_byte) + " but the wire table says '" +
                             std::string(wire_name) + "'");
                }
                if (!table.profiles_[p.mission_byte].name.empty()) {
                    fail(table.source_name_,
                         "duplicate profile for mission byte " +
                             std::to_string(p.mission_byte) + " (" + p.name + ")");
                }
                if (!in_vocabulary(p.target, kTargetValues)) {
                    fail(table.source_name_,
                         p.name + ": unknown target '" + p.target + "'");
                }
                if (!in_vocabulary(p.aro, kAroValues)) {
                    fail(table.source_name_, p.name + ": unknown aro '" + p.aro + "'");
                }
                if (!in_vocabulary(p.altitude_profile, kAltitudeProfileValues)) {
                    fail(table.source_name_,
                         p.name + ": unknown altitude_profile '" + p.altitude_profile + "'");
                }
                if (p.escort_type >= kMissionTypeCount) {
                    fail(table.source_name_,
                         p.name + ": escort_type out of range " +
                             std::to_string(p.escort_type));
                }
                if (p.minalt > p.maxalt) {
                    fail(table.source_name_,
                         p.name + ": minalt " + std::to_string(p.minalt) +
                             " exceeds maxalt " + std::to_string(p.maxalt));
                }
                if (p.min_time > p.max_time) {
                    fail(table.source_name_,
                         p.name + ": min_time " + std::to_string(p.min_time) +
                             " exceeds max_time " + std::to_string(p.max_time));
                }
                table.profiles_[p.mission_byte] = std::move(p);
                ++loaded;
            }
        } else {
            r.skip_value();
        }
    }
    if (!saw_header) {
        fail(table.source_name_, "missing 'format' header");
    }
    if (loaded == 0) {
        fail(table.source_name_, "no profile records");
    }

    table.validate();
    return table;
}

MissionProfileTable
MissionProfileTable::load(const std::filesystem::path& json_path) {
    // read_file throws with the path on open/short-read failure.
    const auto bytes = f4::io::read_file(json_path, "mission_profile");
    std::string_view text(bytes.empty()
                              ? std::string_view("")
                              : std::string_view(
                                    reinterpret_cast<const char*>(bytes.data()),
                                    bytes.size()));
    return load_from_string(text, json_path.string());
}

void MissionProfileTable::validate() const {
    // Coverage: every tasked byte 1..40 must have a record. This is the
    // ClassTable vis_type discipline — a mission byte with no profile is
    // a load error, not a runtime lookup surprise.
    std::string missing;
    for (std::size_t b = 1; b < kMissionTypeCount; ++b) {
        if (profiles_.size() <= b || profiles_[b].name.empty()) {
            if (!missing.empty()) missing += ", ";
            missing += std::to_string(b) + " (" +
                       std::string(mission_type_name(static_cast<std::uint8_t>(b))) + ")";
        }
    }
    if (!missing.empty()) {
        fail(source_name_, "missing profile records for mission bytes: " + missing);
    }
    // The sentinel stays empty.
    if (!profiles_.empty() && !profiles_[0].name.empty()) {
        fail(source_name_, "byte 0 (AMIS_NONE) must not carry a profile");
    }
}

const MissionProfile&
MissionProfileTable::for_mission(std::uint8_t mission_byte) const {
    if (mission_byte == 0) {
        fail(source_name_,
             "mission byte 0 (AMIS_NONE) is not tasked — no profile exists");
    }
    if (mission_byte >= profiles_.size() || profiles_[mission_byte].name.empty()) {
        fail(source_name_,
             "no profile for mission byte " + std::to_string(mission_byte) + " (" +
                 std::string(mission_type_name(mission_byte)) + ")");
    }
    return profiles_[mission_byte];
}

const MissionProfile&
MissionProfileTable::for_name(std::string_view mission_name) const {
    const auto byte = mission_type_byte(mission_name);
    if (!byte) {
        fail(source_name_,
             "'" + std::string(mission_name) + "' is not a FreeFalcon mission type");
    }
    return for_mission(*byte);
}

} // namespace f4::campaign

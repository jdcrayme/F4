// f4-campaign/tests/test_mission_profiles.cpp
//
// The MissionProfileTable loader: the generated 41-type fixture loads
// and validates; a Flight's mission byte selects its profile through a
// flat table read (no switch); every documented failure mode is loud
// with the offending value and the source in the message.

#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/mission_type.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace f4::campaign;

namespace {

// The build-time generated fixture (f4-campaign/CMakeLists owns it).
std::string_view generated_json_path() { return F4_MISSION_PROFILES_JSON; }

} // namespace

// ── The generated table (M4.1's "done when" gate) ───────────────────────────

TEST(MissionProfiles, GeneratedTableLoadsAll41) {
    // Loads through the public loader — file I/O, JSON, validation, all.
    const auto table = MissionProfileTable::load(generated_json_path());
    EXPECT_EQ(table.size(), std::size_t{41});    // 41 wire slots (0..40)
    // Slot 0 (AMIS_NONE) stays unoccupied; bytes 1..40 all carry records.
    EXPECT_TRUE(table.profiles()[0].name.empty());
    std::size_t loaded = 0;
    for (std::size_t b = 1; b < kMissionTypeCount; ++b) {
        if (!table.profiles()[b].name.empty()) ++loaded;
    }
    EXPECT_EQ(loaded, std::size_t{40});
}

TEST(MissionProfiles, GeneratedTableCoversEveryTaskedByte) {
    const auto table = MissionProfileTable::load(generated_json_path());
    for (std::uint8_t b = 1; b < kMissionTypeCount; ++b) {
        const auto& p = table.for_mission(b);
        EXPECT_EQ(p.mission_byte, b) << "byte " << int{b};
        EXPECT_EQ(p.name, mission_type_name(b)) << "byte " << int{b};
    }
}

TEST(MissionProfiles, MissionByteSelectsWithoutSwitch) {
    // The B.1 contract in one test: byte in → profile out, table read.
    const auto table = MissionProfileTable::load(generated_json_path());
    const auto& barcap = table.for_mission(1);
    EXPECT_EQ(barcap.name, "AMIS_BARCAP");

    // Doc-anchored BARCAP values (Core Systems Reference §2): altitude
    // 10,000–40,000 ft (hundreds: 10..40), 15-min loiter, 2 aircraft,
    // and the exact seven-flag list.
    EXPECT_EQ(barcap.minalt, 10);
    EXPECT_EQ(barcap.maxalt, 40);
    EXPECT_EQ(barcap.loitertime, 15);
    EXPECT_EQ(barcap.str, 2);
    EXPECT_TRUE(barcap.has_flag("ADDAWACS"));
    EXPECT_TRUE(barcap.has_flag("NOTHREAT"));
    EXPECT_TRUE(barcap.has_flag("ADDTANKER"));
    EXPECT_TRUE(barcap.has_flag("DONT_COORD"));
    EXPECT_TRUE(barcap.has_flag("EXPECT_DIVERT"));
    EXPECT_TRUE(barcap.has_flag("NO_BREAKPT"));
    EXPECT_TRUE(barcap.has_flag("FLYALWAYS"));
    EXPECT_FALSE(barcap.has_flag("ADDESCORT"));

    // A Flight from the kunsan fixture (mission byte 1) selects BARCAP.
    const std::uint8_t flight_mission = 1;
    EXPECT_EQ(&table.for_mission(flight_mission), &barcap);
}

TEST(MissionProfiles, DocAnchoredProfilesCarryDocValues) {
    const auto table = MissionProfileTable::load(generated_json_path());

    // OCA Strike: alt 500–12,000 ft (5..120), 4 aircraft, escort/SEAD/
    // BARCAP package flags + NO_TARGETABORT.
    const auto& oca = table.for_name("AMIS_OCASTRIKE");
    EXPECT_EQ(oca.minalt, 5);
    EXPECT_EQ(oca.maxalt, 120);
    EXPECT_EQ(oca.str, 4);
    EXPECT_TRUE(oca.has_flag("ADDBDA"));
    EXPECT_TRUE(oca.has_flag("AVOIDTHREAT"));
    EXPECT_TRUE(oca.has_flag("ADDSEAD"));
    EXPECT_TRUE(oca.has_flag("ADDESCORT"));
    EXPECT_TRUE(oca.has_flag("ADDBARCAP"));
    EXPECT_TRUE(oca.has_flag("ADDOCASTRIKE"));
    EXPECT_TRUE(oca.has_flag("MATCHSPEED"));
    EXPECT_TRUE(oca.has_flag("NO_TARGETABORT"));

    // Deep Strike: OCA + ADDAWACS ADDECM HIGHTHREAT, SEAD escort.
    const auto& deep = table.for_name("AMIS_DEEPSTRIKE");
    EXPECT_TRUE(deep.has_flag("ADDAWACS"));
    EXPECT_TRUE(deep.has_flag("ADDECM"));
    EXPECT_TRUE(deep.has_flag("HIGHTHREAT"));
    EXPECT_EQ(deep.escort_type, 17); // AMIS_SEADSTRIKE

    // Stealth Strike: requires VEH_STEALTH, flies alone.
    const auto& stealth = table.for_name("AMIS_STSTRIKE");
    EXPECT_TRUE(stealth.requires_cap("VEH_STEALTH"));
    EXPECT_FALSE(stealth.has_flag("ADDESCORT"));
    EXPECT_FALSE(stealth.has_flag("ADDSEAD"));

    // Intercept: IMMEDIATE ASSIGNED_TAR EXPECT_DIVERT FLYALWAYS.
    const auto& intercept = table.for_name("AMIS_INTERCEPT");
    EXPECT_TRUE(intercept.has_flag("IMMEDIATE"));
    EXPECT_TRUE(intercept.has_flag("ASSIGNED_TAR"));

    // AWACS/JSTAR/TANKER: the 300-min loiter orbits.
    EXPECT_EQ(table.for_name("AMIS_AWACS").loitertime, 300);
    EXPECT_EQ(table.for_name("AMIS_JSTAR").loitertime, 300);
    EXPECT_EQ(table.for_name("AMIS_TANKER").loitertime, 300);

    // AMBUSHCAP: very low altitude band, 2,000–10,000 ft.
    const auto& ambush = table.for_name("AMIS_AMBUSHCAP");
    EXPECT_EQ(ambush.minalt, 2);
    EXPECT_EQ(ambush.maxalt, 10);
}

TEST(MissionProfiles, ForNameAndForMissionAgree) {
    const auto table = MissionProfileTable::load(generated_json_path());
    for (std::uint8_t b = 1; b < kMissionTypeCount; ++b) {
        EXPECT_EQ(&table.for_mission(b),
                  &table.for_name(mission_type_name(b)))
            << "byte " << int{b};
    }
}

// ── Loud failures ────────────────────────────────────────────────────────────

TEST(MissionProfiles, UnknownByteThrowsWithValueAndSource) {
    const auto table = MissionProfileTable::load(generated_json_path());
    try {
        (void)table.for_mission(200);
        FAIL() << "expected for_mission(200) to throw";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("200"), std::string::npos) << what;
        EXPECT_NE(what.find("no profile"), std::string::npos) << what;
        // The source file is in the message (the ClassTable contract).
        EXPECT_NE(what.find(std::string(generated_json_path())),
                  std::string::npos)
            << what;
    }
}

TEST(MissionProfiles, NoneByteThrows) {
    const auto table = MissionProfileTable::load(generated_json_path());
    EXPECT_THROW((void)table.for_mission(0), std::runtime_error);
}

TEST(MissionProfiles, UnknownNameThrows) {
    const auto table = MissionProfileTable::load(generated_json_path());
    try {
        (void)table.for_name("AMIS_MAGIC");
        FAIL() << "expected for_name to throw";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("AMIS_MAGIC"), std::string::npos) << what;
    }
}

TEST(MissionProfiles, MissingRecordFailsValidation) {
    // A table missing one tasked byte never loads.
    const std::string_view json = R"({
        "format": "f4-mission-profiles",
        "version": 1,
        "profiles": [
            {"name": "AMIS_BARCAP", "mission_byte": 1,
             "target": "OBJECTIVE", "aro": "ARO_CA",
             "altitude_profile": "MPROF_STANDARD"},
            {"name": "AMIS_SWEEP", "mission_byte": 7,
             "target": "LOCATION", "aro": "ARO_CA",
             "altitude_profile": "MPROF_HIGH"}
        ]
    })";
    try {
        (void)MissionProfileTable::load_from_string(json, "partial.json");
        FAIL() << "expected load to throw";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("missing profile records"), std::string::npos)
            << what;
        EXPECT_NE(what.find("partial.json"), std::string::npos) << what;
        EXPECT_NE(what.find("2 (AMIS_BARCAP2)"), std::string::npos) << what;
    }
}

TEST(MissionProfiles, ByteNameMismatchThrows) {
    // The record's name must be the wire table's name for its byte.
    // (single-record table: the mismatch fires before coverage does)
    const std::string json = R"({"format": "f4-mission-profiles", "version": 1,
        "profiles": [
            {"name": "AMIS_BARCAP", "mission_byte": 7,
             "target": "LOCATION", "aro": "ARO_CA",
             "altitude_profile": "MPROF_HIGH"}
        ]})";
    try {
        (void)MissionProfileTable::load_from_string(json, "bad.json");
        FAIL() << "expected load to throw";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("AMIS_BARCAP"), std::string::npos) << what;
        EXPECT_NE(what.find("wire table says 'AMIS_SWEEP'"), std::string::npos)
            << what;
    }
}

TEST(MissionProfiles, DuplicateByteThrows) {
    std::string json = R"({"format": "f4-mission-profiles", "version": 1,
        "profiles": [)";
    for (int i = 0; i < 2; ++i) {
        json += R"(
            {"name": "AMIS_BARCAP", "mission_byte": 1,
             "target": "OBJECTIVE", "aro": "ARO_CA",
             "altitude_profile": "MPROF_STANDARD"},)";
    }
    json += R"(
            {"name": "AMIS_BARCAP2", "mission_byte": 2,
             "target": "OBJECTIVE", "aro": "ARO_CA",
             "altitude_profile": "MPROF_STANDARD"}
        ]})";
    try {
        (void)MissionProfileTable::load_from_string(json, "dup.json");
        FAIL() << "expected load to throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("duplicate"),
                  std::string::npos)
            << e.what();
    }
}

TEST(MissionProfiles, NoneRecordThrows) {
    const std::string_view json = R"({
        "format": "f4-mission-profiles", "version": 1,
        "profiles": [
            {"name": "AMIS_NONE", "mission_byte": 0,
             "target": "AMIS_TAR_NONE", "aro": "ARO_NONE",
             "altitude_profile": "MPROF_STANDARD"}
        ]
    })";
    EXPECT_THROW(
        (void)MissionProfileTable::load_from_string(json, "none.json"),
        std::runtime_error);
}

TEST(MissionProfiles, UnknownVocabularyThrows) {
    const std::string_view json = R"({
        "format": "f4-mission-profiles", "version": 1,
        "profiles": [
            {"name": "AMIS_BARCAP", "mission_byte": 1,
             "target": "OBJECTIVE", "aro": "ARO_WIZARDRY",
             "altitude_profile": "MPROF_STANDARD"}
        ]
    })";
    try {
        (void)MissionProfileTable::load_from_string(json, "vocab.json");
        FAIL() << "expected load to throw";
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("ARO_WIZARDRY"), std::string::npos) << what;
        EXPECT_NE(what.find("vocab.json"), std::string::npos) << what;
    }
}

TEST(MissionProfiles, BadFormatHeaderThrows) {
    const std::string_view json = R"({
        "format": "something-else", "version": 1, "profiles": []
    })";
    EXPECT_THROW(
        (void)MissionProfileTable::load_from_string(json, "fmt.json"),
        std::runtime_error);
}

TEST(MissionProfiles, AltitudeInversionThrows) {
    const std::string_view json = R"({
        "format": "f4-mission-profiles", "version": 1,
        "profiles": [
            {"name": "AMIS_BARCAP", "mission_byte": 1,
             "target": "OBJECTIVE", "aro": "ARO_CA",
             "altitude_profile": "MPROF_STANDARD",
             "minalt": 50, "maxalt": 10}
        ]
    })";
    try {
        (void)MissionProfileTable::load_from_string(json, "alt.json");
        FAIL() << "expected load to throw";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("minalt"), std::string::npos)
            << e.what();
    }
}

TEST(MissionProfiles, EmptyTableThrows) {
    const std::string_view json = R"({
        "format": "f4-mission-profiles", "version": 1, "profiles": []
    })";
    EXPECT_THROW(
        (void)MissionProfileTable::load_from_string(json, "empty.json"),
        std::runtime_error);
}

TEST(MissionProfiles, MissingFileThrowsFromLoad) {
    EXPECT_THROW(
        (void)MissionProfileTable::load(
            std::filesystem::temp_directory_path() /
            "f4_no_such_profiles_2v7r.json"),
        std::runtime_error);
}

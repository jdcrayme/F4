// f4-campaign/tools/profiles_gen.cpp
//
// MissionProfiles.json generator — F4's version of FreeFalcon's static
// MissionData[] table, emitted at build time (the same build-time
// generation discipline as f4-convert's golden fixtures: parser/data
// improvements propagate to every consumer, nothing stale can hide).
//
// Usage: mission_profiles_gen <output.json>
//
// Value sources (FreeFalcon Core Systems Reference, Part I §2/§3):
//   * DOC-ANCHORED — the reference states the value explicitly:
//       - BARCAP:      alt 10,000–40,000 ft, 15-min loiter, 2 aircraft,
//                      flags ADDAWACS NOTHREAT ADDTANKER DONT_COORD
//                      EXPECT_DIVERT NO_BREAKPT FLYALWAYS
//       - OCASTRIKE:   alt 500–12,000 ft, 4 aircraft, flags ADDBDA
//                      AVOIDTHREAT ADDSEAD ADDESCORT ADDBARCAP
//                      ADDOCASTRIKE MATCHSPEED NO_TARGETABORT
//       - DEEPSTRIKE:  same as OCA + ADDAWACS ADDECM HIGHTHREAT
//       - STSTRIKE:    requires VEH_STEALTH, flies alone (no escorts),
//                      higher threat tolerance
//       - INTERCEPT:   flags IMMEDIATE ASSIGNED_TAR EXPECT_DIVERT
//                      FLYALWAYS
//       - AMBUSHCAP:   very low altitude, 2,000–10,000 ft
//       - AWACS/JSTAR/TANKER: 300-min loiter each
//   * PROVISIONAL — sensible FreeFalcon-consistent cadence/geometry for
//     the remaining types (marked with a provisional_ comment). They are
//     load-bearing for the golden-summary test only; correcting a value
//     is a one-line generator change + regenerate, never a code change.
//
// The output is deterministic: same generator, same bytes — the B.2
// golden-summary test relies on it.

#include <f4/campaign/mission_profile.hpp>
#include <f4/campaign/mission_type.hpp>
#include <f4/json/f4_json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using f4::campaign::MissionProfile;
using f4::campaign::kMissionTypeCount;
using f4::campaign::mission_type_name;

namespace {

struct Row {
    std::uint8_t byte;
    std::string target;
    std::string aro;
    std::string alt_profile;
    std::string target_profile;
    std::string target_desc;
    std::string routewp;
    std::string targetwp;
    int minalt, maxalt, missionalt;   // hundreds of feet
    int separation;                   // seconds
    int loitertime;                   // minutes
    int str;                          // aircraft per flight
    int min_time, max_time;           // planning advance, minutes
    std::uint8_t escort_type;
    int mindistance;
    int mintime;
    std::vector<std::string> caps;
    std::vector<std::string> flags;
};

// The 40 tasked mission types, byte order. Values per the doc anchors
// above; PROVISIONAL rows carry the values the comment says.
const std::vector<Row>& table() {
    static const std::vector<Row> rows = {
        // ── Counter-air (ARO_CA) ────────────────────────────────────────
        {1, "OBJECTIVE", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         10, 40, 25, 0, 15, 2, 60, 180, 0, 0, 0, {},
         // DOC-ANCHORED (BARCAP): the exact flag list from the reference.
         {"ADDAWACS", "NOTHREAT", "ADDTANKER", "DONT_COORD", "EXPECT_DIVERT", "NO_BREAKPT", "FLYALWAYS"}},
        {2, "OBJECTIVE", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         10, 40, 25, 0, 15, 2, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: BARCAP2 = the second-barrier cadence of BARCAP.
         {"ADDAWACS", "ADDTANKER", "DONT_COORD", "EXPECT_DIVERT", "NO_BREAKPT", "FLYALWAYS"}},
        {3, "OBJECTIVE", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         10, 35, 20, 0, 30, 2, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: HAVCAP orbits the package's high-value asset.
         {"ADDAWACS", "ADDTANKER", "EXPECT_DIVERT", "FLYALWAYS"}},
        {4, "OBJECTIVE", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         5, 30, 15, 0, 30, 2, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: TARCAP parks over the target area.
         {"ADDAWACS", "EXPECT_DIVERT", "FLYALWAYS"}},
        {5, "UNIT", "ARO_CA", "MPROF_LOW", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         2, 20, 10, 0, 30, 2, 15, 90, 0, 0, 0, {},
         // PROVISIONAL: RESCAP covers a SAR site — low and soon.
         {"ADDAWACS", "ADDESCORT", "EXPECT_DIVERT", "FLYALWAYS"}},
        {6, "LOCATION", "ARO_CA", "MPROF_LOW", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         2, 10, 5, 0, 30, 2, 30, 120, 0, 0, 0, {},
         // DOC-ANCHORED altitude (AMBUSHCAP: 2,000–10,000 ft); cadence PROVISIONAL.
         {"ADDAWACS", "EXPECT_DIVERT"}},
        {7, "LOCATION", "ARO_CA", "MPROF_HIGH", "TPROF_ATTACK", "TTL", "WP_UNUSED", "WP_SWEEP",
         15, 45, 30, 0, 5, 4, 15, 90, 0, 0, 0, {},
         // PROVISIONAL: sweeps go fast, high, and strike-capable.
         {"ADDAWACS", "ADDTANKER", "EXPECT_DIVERT", "FLYALWAYS"}},
        {8, "AMIS_TAR_NONE", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_CAP",
         5, 30, 15, 0, 60, 2, 0, 0, 0, 0, 0, {},
         // PROVISIONAL: ground alert sits at the field until scrambled.
         {"FLYALWAYS"}},
        {9, "UNIT", "ARO_CA", "MPROF_STANDARD", "TPROF_ATTACK", "TAO", "WP_UNUSED", "WP_INTERCEPT",
         10, 40, 25, 0, 0, 2, 0, 15, 0, 0, 0, {},
         // DOC-ANCHORED flags (INTERCEPT: IMMEDIATE ASSIGNED_TAR
         // EXPECT_DIVERT FLYALWAYS).
         {"IMMEDIATE", "ASSIGNED_TAR", "EXPECT_DIVERT", "FLYALWAYS"}},
        {10, "UNIT", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_ESCORT",
         15, 35, 25, 60, 0, 2, 15, 90, 0, 0, 0, {},
         // PROVISIONAL: escort flights separate from the package by time.
         {"ADDAWACS", "ADDTANKER", "EXPECT_DIVERT"}},
        {11, "UNIT", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_ESCORT",
         10, 30, 20, 60, 0, 2, 15, 90, 0, 0, 0, {},
         // PROVISIONAL: sea escort mirrors escort over water.
         {"ADDTANKER", "EXPECT_DIVERT"}},

        // ── Strike (ARO_S) ─────────────────────────────────────────────
        {12, "OBJECTIVE", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 120, 20, 0, 0, 4, 60, 180, 10, 0, 0, {},
         // DOC-ANCHORED (OCA Strike): altitude band, aircraft count, flags.
         {"ADDBDA", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "ADDBARCAP", "ADDOCASTRIKE", "MATCHSPEED", "NO_TARGETABORT"}},
        {13, "OBJECTIVE", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 120, 20, 0, 0, 4, 90, 240, 0, 0, 0, {},
         // PROVISIONAL: interdiction runs deeper than OCA → longer window.
         {"ADDBDA", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "MATCHSPEED"}},
        {14, "OBJECTIVE", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 120, 20, 0, 0, 4, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: general strike = OCA-shaped, target-agnostic.
         {"ADDBDA", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "MATCHSPEED", "NO_TARGETABORT"}},
        {15, "OBJECTIVE", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 120, 20, 0, 0, 4, 120, 300, 17, 0, 0, {},
         // DOC-ANCHORED (Deep Strike): OCA values + ADDAWACS ADDECM
         // HIGHTHREAT; escort_type = SEAD strike (byte 17) per the
         // "always gets escort+SEAD" rule.
         {"ADDBDA", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "ADDBARCAP", "ADDOCASTRIKE", "MATCHSPEED", "NO_TARGETABORT", "ADDAWACS", "ADDECM", "HIGHTHREAT"}},
        {16, "OBJECTIVE", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 120, 20, 0, 0, 2, 90, 240, 0, 0, 0,
         // DOC-ANCHORED (Stealth Strike): VEH_STEALTH required, flies
         // alone — no escort flags at all, higher threat tolerance.
         {"VEH_STEALTH"},
         {"AVOIDTHREAT", "HIGHTHREAT"}},
        {17, "OBJECTIVE", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 120, 20, 0, 0, 4, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: SEAD leads the package it clears → short window.
         {"ADDBDA", "AVOIDTHREAT", "HIGHTHREAT", "MATCHSPEED"}},

        // ── Ground attack (ARO_GA) ─────────────────────────────────────
        {18, "LOCATION", "ARO_GA", "MPROF_LOW", "TPROF_LOITER", "TAO", "WP_INGRESS", "WP_CAS",
         2, 20, 10, 0, 60, 2, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: on-call CAS orbits near the front, waits (TAO).
         {"ADDFAC", "ADDTANKER", "EXPECT_DIVERT"}},
        {19, "OBJECTIVE", "ARO_GA", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_BOMB",
         2, 20, 10, 0, 0, 2, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: pre-planned CAS hits a scheduled TOT (TTL).
         {"ADDFAC", "ADDBDA", "MATCHSPEED"}},
        {20, "UNIT", "ARO_GA", "MPROF_LOW", "TPROF_ATTACK", "TAO", "WP_INGRESS", "WP_CAS",
         2, 15, 8, 0, 0, 2, 0, 30, 0, 0, 0, {},
         // PROVISIONAL: immediate CAS = short window, launches now.
         {"IMMEDIATE", "ADDFAC"}},
        {21, "OBJECTIVE", "ARO_GA", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         2, 20, 10, 0, 0, 2, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: SAD = search, then destroy.
         {"ADDBDA", "AVOIDTHREAT"}},
        {22, "OBJECTIVE", "ARO_GA", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_BOMB",
         5, 120, 20, 0, 0, 4, 90, 240, 0, 0, 0, {},
         // PROVISIONAL: INT matches INTSTRIKE's cadence against supply.
         {"ADDBDA", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "MATCHSPEED"}},
        {23, "OBJECTIVE", "ARO_GA", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_BOMB",
         2, 60, 15, 0, 0, 4, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: BAI shapes the battlefield area, medium band.
         {"ADDBDA", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "MATCHSPEED"}},

        // ── Support & special ──────────────────────────────────────────
        {24, "OBJECTIVE", "ARO_SB", "MPROF_HIGH", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         20, 45, 30, 0, 0, 4, 90, 240, 0, 0, 0, {},
         // PROVISIONAL: strategic bombing goes high and deep.
         {"ADDAWACS", "AVOIDTHREAT", "ADDSEAD", "ADDESCORT", "ADDBARCAP", "ADDECM"}},
        {25, "OBJECTIVE", "ARO_SUPPORT", "MPROF_HIGH", "TPROF_LOITER", "TTL", "WP_INGRESS", "WP_ORBIT",
         250, 350, 300, 0,
         // DOC-ANCHORED loiter (AWACS: 300-min orbit); the rest PROVISIONAL.
         300, 1, 180, 480, 0, 0, 0, {},
         {"ADDESCORT", "DONT_COORD", "FLYALWAYS"}},
        {26, "OBJECTIVE", "ARO_SUPPORT", "MPROF_HIGH", "TPROF_LOITER", "TTL", "WP_INGRESS", "WP_ORBIT",
         250, 350, 300, 0, 300, 1, 180, 480, 0, 0, 0, {},
         // DOC-ANCHORED loiter (JSTAR: 300-min orbit).
         {"ADDESCORT", "DONT_COORD", "FLYALWAYS"}},
        {27, "OBJECTIVE", "ARO_SUPPORT", "MPROF_HIGH", "TPROF_LOITER", "TTL", "WP_INGRESS", "WP_ORBIT",
         200, 300, 250, 0, 300, 1, 120, 360, 0, 0, 0, {},
         // DOC-ANCHORED loiter (Tanker: 300-min orbit).
         {"ADDESCORT", "DONT_COORD", "FLYALWAYS"}},
        {28, "OBJECTIVE", "ARO_SUPPORT", "MPROF_HIGH", "TPROF_LOITER", "TTL", "WP_INGRESS", "WP_ORBIT",
         150, 250, 200, 0, 120, 2, 120, 360, 0, 0, 0, {},
         // PROVISIONAL: ECM standoff jamming, shorter orbit.
         {"ADDECM", "AVOIDTHREAT", "DONT_COORD"}},
        {29, "OBJECTIVE", "ARO_SUPPORT", "MPROF_HIGH", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_RECON",
         20, 60, 40, 0, 15, 2, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: recon sneaks a look and comes home.
         {"AVOIDTHREAT", "NO_BREAKPT"}},
        {30, "OBJECTIVE", "ARO_SUPPORT", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_RECON",
         5, 60, 20, 0, 15, 2, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: BDA = post-strike damage assessment pass.
         {"AVOIDTHREAT", "ADDBDA"}},
        {31, "UNIT", "ARO_SUPPORT", "MPROF_LOW", "TPROF_LOITER", "TAO", "WP_INGRESS", "WP_CAS",
         2, 15, 8, 0, 120, 2, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: FAC loiters low over the fight, talks to CAS.
         {"ADDFAC", "EXPECT_DIVERT"}},
        {32, "UNIT", "ARO_SUPPORT", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_SAR",
         1, 15, 8, 0, 0, 2, 0, 60, 0, 0, 0, {},
         // PROVISIONAL: SAR goes exactly where the pilot is.
         {"IMMEDIATE", "ADDESCORT", "EXPECT_DIVERT"}},
        {33, "OBJECTIVE", "ARO_SUPPORT", "MPROF_STANDARD", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_MOVE",
         10, 30, 20, 0, 0, 4, 60, 240, 0, 0, 0, {},
         // PROVISIONAL: airlift shuttles between supply bases.
         {"DONT_COORD"}},
        {34, "UNIT", "ARO_SUPPORT", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         2, 20, 10, 0, 0, 2, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: ASW hunts submarines at wave-top height.
         {"AVOIDTHREAT"}},
        {35, "UNIT", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 60, 20, 0, 0, 4, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: anti-shipping = low strike against moving units.
         {"ADDBDA", "AVOIDTHREAT", "ADDESCORT", "MATCHSPEED"}},
        {36, "OBJECTIVE", "ARO_CA", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_INGRESS", "WP_CAP",
         10, 35, 20, 0, 30, 2, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: patrol = slow-cycle CAP over owned space.
         {"ADDAWACS", "FLYALWAYS"}},
        {37, "AMIS_TAR_NONE", "ARO_NONE", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_UNUSED",
         10, 30, 20, 0, 30, 2, 0, 0, 0, 0, 0, {},
         // PROVISIONAL: training flights stay over friendly terrain.
         {}},
        {38, "AMIS_TAR_NONE", "ARO_NONE", "MPROF_STANDARD", "TPROF_LOITER", "TTL", "WP_UNUSED", "WP_UNUSED",
         5, 40, 20, 0, 30, 2, 0, 0, 0, 0, 0, {},
         // PROVISIONAL: OTHER = the catch-all, deliberately neutral.
         {}},
        {39, "UNIT", "ARO_S", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_STRIKE",
         5, 60, 15, 0, 0, 4, 60, 180, 0, 0, 0, {},
         // PROVISIONAL: TANK = armor hunting (the unit-role strikes).
         {"ADDBDA", "AVOIDTHREAT", "ADDESCORT"}},
        {40, "OBJECTIVE", "ARO_SUPPORT", "MPROF_LOW", "TPROF_ATTACK", "TTL", "WP_INGRESS", "WP_RECON",
         2, 30, 15, 0, 30, 2, 30, 120, 0, 0, 0, {},
         // PROVISIONAL: SEARCH = low-and-slow area search.
         {"AVOIDTHREAT", "NO_BREAKPT"}},
    };
    return rows;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: mission_profiles_gen <output.json>\n";
        return 2;
    }
    const std::filesystem::path out_path = argv[1];

    const auto& rows = table();
    if (rows.size() != kMissionTypeCount - 1) {
        std::cerr << "profiles_gen: internal table must list " << kMissionTypeCount - 1
                  << " tasked types, has " << rows.size() << "\n";
        return 1;
    }

    // The generator self-validates through the library: emit, reload,
    // and only write the file when the library accepts the full table.
    f4::json::Writer w;
    w.put("{\n  \"format\": \"f4-mission-profiles\",\n  \"version\": 1,\n  \"profiles\": [\n");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.byte != i + 1) {
            std::cerr << "profiles_gen: row " << i << " is out of byte order\n";
            return 1;
        }
        w.put("    {\"name\": ");
        w.string(mission_type_name(r.byte));
        w.put(", \"mission_byte\": ");
        w.number(r.byte);
        auto field = [&w](const char* k, const std::string& v) {
            w.put(", \"");
            w.put(k);
            w.put("\": ");
            w.string(v);
        };
        auto num = [&w](const char* k, long long v) {
            w.put(", \"");
            w.put(k);
            w.put("\": ");
            w.number(v);
        };
        field("target", r.target);
        field("aro", r.aro);
        field("altitude_profile", r.alt_profile);
        field("target_profile", r.target_profile);
        field("target_desc", r.target_desc);
        field("routewp", r.routewp);
        field("targetwp", r.targetwp);
        num("minalt", r.minalt);
        num("maxalt", r.maxalt);
        num("missionalt", r.missionalt);
        num("separation", r.separation);
        num("loitertime", r.loitertime);
        num("str", r.str);
        num("min_time", r.min_time);
        num("max_time", r.max_time);
        num("escort_type", r.escort_type);
        num("mindistance", r.mindistance);
        num("mintime", r.mintime);
        auto list = [&w](const char* k, const std::vector<std::string>& v) {
            w.put(", \"");
            w.put(k);
            w.put("\": [");
            for (std::size_t j = 0; j < v.size(); ++j) {
                if (j) w.put(", ");
                w.string(v[j]);
            }
            w.put("]");
        };
        list("caps", r.caps);
        list("flags", r.flags);
        w.put(i + 1 == rows.size() ? "}\n" : "},\n");
    }
    w.put("  ]\n}\n");

    // Round-trip through the real loader before writing anything. A
    // table that fails the library's validation never reaches the build.
    auto t = f4::campaign::MissionProfileTable::load_from_string(
        w.str(), "<generated MissionProfiles.json>");
    if (t.size() != kMissionTypeCount) {
        std::cerr << "profiles_gen: generated table has " << t.size()
                  << " slots, expected " << kMissionTypeCount << "\n";
        return 1;
    }

    std::filesystem::create_directories(out_path.parent_path());
    {
        std::ofstream out(out_path, std::ios::binary);
        out << w.str();
        if (!out) {
            std::cerr << "profiles_gen: cannot write " << out_path << "\n";
            return 1;
        }
    }
    std::cout << "profiles_gen: wrote " << out_path << " ("
              << t.size() - 1 << " tasked mission profiles)\n";
    return 0;
}

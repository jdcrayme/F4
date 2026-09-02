// f4-world-convert/src/team_decoder.cpp
//
// Decodes the .tea sub-file (team records) from a .cam archive.
//
// Layout sources (FreeFalcon):
//   SaveTeams / LoadTeams    team.cpp:1720 / 1666
//   TeamClass(FILE*)         team.cpp:270   (Save at team.cpp:863)
//   CampManagerClass(FILE*)  manager.cpp:60 (Save at manager.cpp:175)
//   ATM(FILE*)               atm.cpp:238    (Save at atm.cpp:430)
//   GTM(FILE*)               gtm.cpp:137    (manager + flags short)
//   NTM(FILE*)               ntm.cpp:65     (manager + flags short)
//
// Parity: TestCamp.cam (v71, 8 teams, 11,466-byte .tea) decodes all 8
// teams + managers consuming the stream exactly (11,466/11,466 bytes).

#include <f4/world_convert/team_decoder.hpp>
#include <f4/io/cursor.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

using f4::io::Cursor;

// Format constants (team.h / campbase.h / atm.h / mission.h).
constexpr int NUM_TEAMS = 8;             // NUM_TEAMS
constexpr int NUM_COUNS = 8;             // NUM_COUNS
constexpr int MAX_TEAM_NAME = 20;        // MAX_TEAM_NAME_SIZE
constexpr int MAX_MOTTO = 200;           // MAX_MOTTO_SIZE
constexpr int MAX_BONUSES = 20;          // MAX_BONUSES
constexpr int MAX_TGTTYPE = 36;          // MAX_TGTTYPE
constexpr int MAX_UNITTYPE = 20;         // MAX_UNITTYPE
constexpr int AMIS_OTHER = 41;           // AMIS_OTHER (mission.h:66)
constexpr int ATM_MAX_CYCLES = 32;       // ATM_MAX_CYCLES
constexpr int MISSION_REQUEST_SIZE = 76; // sizeof(MissionRequestClass) @ Win32 x86

struct VuId { uint32_t num; uint32_t creator; };
VuId read_vu_id(Cursor& c) { VuId v; v.num = c.u32(); v.creator = c.u32(); return v; }

std::string fixed_string(Cursor& c, std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char ch = static_cast<char>(c.u8());
        if (ch == '\0') { c.skip(n - i - 1); break; }
        s.push_back(ch);
    }
    return s;
}

// ---------------------------------------------------------------------------
// TeamClass(FILE*) — team.cpp:270. 739 bytes at v63/v71.
// ---------------------------------------------------------------------------
void parse_team_class(Cursor& c, TeamRecord& t, int v) {
    VuId id = read_vu_id(c);
    t.id_num = id.num;
    t.id_creator = id.creator;
    t.entity_type = c.u16();
    t.who = c.u8();          // Team = uchar (cmpglobl.h:87)
    t.cteam = c.u8();
    t.flags = c.i16();

    t.member.resize(NUM_COUNS);
    for (int j = 0; j < NUM_COUNS; ++j) t.member[j] = c.u8();
    t.stance.resize(NUM_TEAMS);
    for (int j = 0; j < NUM_TEAMS; ++j) t.stance[j] = c.i16();

    t.first_colonel   = c.i16();
    t.first_commander = c.i16();
    t.first_wingman   = c.i16();
    t.last_wingman    = c.i16();

    t.air_experience         = c.u8();   // v > 11
    t.air_defense_experience = c.u8();
    t.ground_experience      = c.u8();
    t.naval_experience       = c.u8();

    t.initiative     = c.i16();
    t.supply_avail   = c.u16();
    t.fuel_avail     = c.u16();
    if (v > 53) {
        t.replacements_avail  = c.u16();
        t.player_rating       = c.f32();
        t.last_player_mission = c.i32();
    }

    // TeamStatusType (pack(1)): 7 ushort + 2 uchar = 16 bytes.
    t.current_air_defense_vehs = c.u16();
    t.current_aircraft        = c.u16();
    t.current_ground_vehs     = c.u16();
    t.current_ships           = c.u16();
    t.current_supply          = c.u16();
    t.current_fuel            = c.u16();
    t.current_airbases        = c.u16();
    t.current_supply_level    = c.u8();
    t.current_fuel_level      = c.u8();
    t.start_air_defense_vehs = c.u16();
    t.start_aircraft         = c.u16();
    t.start_ground_vehs      = c.u16();
    t.start_ships            = c.u16();
    t.start_supply           = c.u16();
    t.start_fuel             = c.u16();
    t.start_airbases         = c.u16();
    t.start_supply_level     = c.u8();
    t.start_fuel_level       = c.u8();

    t.reinforcement = c.i16();

    t.bonus_obj_nums.resize(MAX_BONUSES);
    t.bonus_times.resize(MAX_BONUSES);
    for (int j = 0; j < MAX_BONUSES; ++j) {
        VuId b = read_vu_id(c);
        t.bonus_obj_nums[j] = b.num;
    }
    for (int j = 0; j < MAX_BONUSES; ++j) t.bonus_times[j] = c.i32();

    t.objtype_priority.resize(MAX_TGTTYPE);
    for (int j = 0; j < MAX_TGTTYPE; ++j) t.objtype_priority[j] = c.u8();
    t.unittype_priority.resize(MAX_UNITTYPE);
    for (int j = 0; j < MAX_UNITTYPE; ++j) t.unittype_priority[j] = c.u8();
    const int mission_priority_count = (v < 30) ? 40 : AMIS_OTHER;
    t.mission_priority.resize(mission_priority_count);
    for (int j = 0; j < mission_priority_count; ++j) t.mission_priority[j] = c.u8();

    for (int j = 0; j < 4; ++j) t.max_vehicle[j] = c.u8();

    if (v > 4)  t.team_flag = c.u8();
    if (v > 32) t.team_color = c.u8();
    t.equipment = c.u8();
    t.name = fixed_string(c, MAX_TEAM_NAME);
    if (v > 32) t.motto = fixed_string(c, MAX_MOTTO);

    // TeamGndActionType (pack(1), 19 bytes) at v > 33 (sizeof at v > 50).
    if (v > 33) {
        t.gnd_action_time    = c.i32();
        t.gnd_action_timeout = c.i32();
        VuId o = read_vu_id(c);
        t.gnd_action_obj_num = o.num;
        t.gnd_action_obj_creator = o.creator;
        t.gnd_action_type   = c.u8();
        t.gnd_action_tempo  = c.u8();
        t.gnd_action_points = c.u8();
    }
    // TeamAirActionType × 2 (natural alignment, 28 bytes each) at v > 33.
    if (v > 33) {
        t.def_air_start_time = c.i32();
        t.def_air_stop_time  = c.i32();
        VuId d = read_vu_id(c);
        t.def_air_obj_num = d.num;
        t.def_air_obj_creator = d.creator;
        VuId dl = read_vu_id(c);
        t.def_air_last_obj_num = dl.num;
        t.def_air_last_obj_creator = dl.creator;
        t.def_air_action_type = c.u8();
        c.skip(3);   // MSVC pads the uchar to the 4-byte struct alignment

        t.off_air_start_time = c.i32();
        t.off_air_stop_time  = c.i32();
        VuId o2 = read_vu_id(c);
        t.off_air_obj_num = o2.num;
        t.off_air_obj_creator = o2.creator;
        VuId ol = read_vu_id(c);
        t.off_air_last_obj_num = ol.num;
        t.off_air_last_obj_creator = ol.creator;
        t.off_air_action_type = c.u8();
        c.skip(3);
    }
}

// ---------------------------------------------------------------------------
// CampManagerClass(FILE*) — manager.cpp:60. 13 bytes.
// ---------------------------------------------------------------------------
void parse_manager_header(Cursor& c, ATMRecord& m) {
    VuId id = read_vu_id(c);
    m.id_num = id.num;
    m.id_creator = id.creator;
    m.entity_type = c.u16();
    m.manager_flags = c.i16();
    m.owner = c.u8();
}

// ---------------------------------------------------------------------------
// MissionRequestClass bulk — 76 bytes at v>=35. Same layout as the
// Package big branch (see unit_decoder.cpp for the offset map).
// ---------------------------------------------------------------------------
void parse_mission_request(Cursor& c, ATMRequestRecord& m) {
    VuId req = read_vu_id(c);
    m.requester_id_num = req.num; m.requester_id_creator = req.creator;
    VuId tar = read_vu_id(c);
    m.target_id_num = tar.num; m.target_id_creator = tar.creator;
    VuId sec = read_vu_id(c);
    m.secondary_id_num = sec.num; m.secondary_id_creator = sec.creator;
    VuId pk = read_vu_id(c);
    m.pak_id_num = pk.num; m.pak_id_creator = pk.creator;
    m.who = c.u8();
    m.vs  = c.u8();
    c.skip(2);                    // alignment padding
    m.tot = c.i32();
    m.tx  = c.i16();
    m.ty  = c.i16();
    m.flags = c.u32();
    m.caps = c.i16();
    m.target_num = c.i16();
    m.speed = c.i16();
    m.match_strength = c.i16();
    m.priority = c.i16();
    m.tot_type = c.u8();
    m.action_type = c.u8();
    m.mission = c.u8();
    m.aircraft = c.u8();
    m.context = c.u8();
    m.roe_check = c.u8();
    m.delayed = c.u8();
    m.start_block = c.u8();
    m.final_block = c.u8();
    for (int i = 0; i < 4; ++i) m.slots[i] = c.u8();
    m.min_to = static_cast<int8_t>(c.u8());
    m.max_to = static_cast<int8_t>(c.u8());
    c.skip(3);                    // trailing padding → 76
}

// ---------------------------------------------------------------------------
// AirTaskingManagerClass(FILE*) — atm.cpp:238.
// ---------------------------------------------------------------------------
void parse_atm(Cursor& c, ATMRecord& m, int v) {
    parse_manager_header(c, m);
    m.flags = c.i16();
    if (v >= 28) {
        if (v >= 63) m.average_ca_strength = c.i16();
        m.average_ca_missions = c.i16();
        m.sample_cycles = c.u8();
    }
    uint8_t num = c.u8();
    m.airbases.clear();
    m.airbases.reserve(num);
    for (uint8_t i = 0; i < num; ++i) {
        ATMAirbaseRecord ab;
        VuId id = read_vu_id(c);
        ab.id_num = id.num;
        ab.id_creator = id.creator;
        for (int j = 0; j < ATM_MAX_CYCLES; ++j) ab.schedule[j] = c.u8();
        m.airbases.push_back(ab);
    }
    m.cycle = c.u8();
    int16_t nreq = c.i16();
    if (nreq < 0 || nreq > 512) {
        c.error = true;           // implausible request count — bail out
        return;
    }
    m.requests.clear();
    m.requests.reserve(static_cast<std::size_t>(nreq));
    for (int16_t i = 0; i < nreq; ++i) {
        ATMRequestRecord r;
        if (v >= 35) {
            parse_mission_request(c, r);
        } else {
            c.skip(64);           // legacy 64-byte MissionRequestClass
        }
        m.requests.push_back(r);
    }
}

// GTM / NTM: manager header (13) + flags (2). Not exposed.
void parse_gtm(Cursor& c) {
    c.skip(13 + 2);
}
void parse_ntm(Cursor& c) {
    c.skip(13 + 2);
}

} // namespace

DecodedTeams decode_tea(const uint8_t* data, std::size_t size, int camp_version) {
    DecodedTeams out;
    if (size < 2) throw std::runtime_error("tea: sub-file too small");

    Cursor c{data, data + size};
    out.count = c.i16();
    if (out.count < 0 || out.count > 16)
        throw std::runtime_error("tea: implausible team count");

    out.teams.reserve(static_cast<std::size_t>(out.count));

    for (int i = 0; i < out.count; ++i) {
        TeamRecord t;
        parse_team_class(c, t, camp_version);
        if (c.error) {
            // Truncated mid-team — return what we have.
            break;
        }
        parse_atm(c, t.atm, camp_version);
        if (c.error) break;
        parse_gtm(c);
        if (c.error) break;
        parse_ntm(c);
        if (c.error) break;
        out.teams.push_back(std::move(t));
    }

    out.bytes_consumed = static_cast<std::size_t>(c.p - data);
    return out;
}

} // namespace f4::world_convert

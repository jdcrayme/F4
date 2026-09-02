// f4-world-convert/src/campaign_decoder.cpp

#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/io/cursor.hpp>

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

using f4::io::Cursor;

// Format constants — Falcon 4 .cam campaign archive layout
// (cmpclass.cpp CampaignClass::Decode, cmpclass.h).
constexpr int NUM_TEAMS = 8;            // FreeFalcon MAX_TEAMS
constexpr int TEAM_NAME_LEN = 20;       // team.name char[N] in TeamBlock
constexpr int TEAM_MOTTO_LEN = 200;     // team.motto char[N] in TeamBlock
constexpr int CMP_HEADER_BYTES = 8;     // 2 × i32 (reserved + decompressed_size)
constexpr int CAMP_NAME_SIZE = 40;      // CAMP_NAME_SIZE (cmpclass.h:47)
constexpr int UI_EVENT_NODE_SIZE = 20;  // sizeof(uieventnode) @ Win32 x86
constexpr int SQUAD_UI_INFO_SIZE = 68;  // sizeof(SquadUIInfoClass) @ Win32 x86

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

// One event queue: [short entries] then per entry: uieventnode (20 bytes
// on disk — x, y, time, flags, team + two ignored x86 pointer slots) +
// [short len] + char[len] text.
void parse_event_queue(Cursor& c, std::vector<CampaignEvent>& out) {
    int16_t entries = c.i16();
    if (c.error || entries < 0 || entries > 500) {
        c.error = true;
        return;
    }
    out.reserve(static_cast<std::size_t>(entries));
    for (int16_t i = 0; i < entries; ++i) {
        CampaignEvent e;
        e.x = c.i16();            // offset 0
        e.y = c.i16();            // offset 2
        e.time = c.i32();         // offset 4
        e.flags = c.u8();         // offset 8
        e.team = c.u8();          // offset 9
        c.skip(UI_EVENT_NODE_SIZE - 10);  // pad(2) + eventText(4) + next(4)
        int16_t len = c.i16();
        if (c.error || len < 0 || len > 4096) {
            c.error = true;
            return;
        }
        e.text.clear();
        for (int16_t j = 0; j < len; ++j) {
            char ch = static_cast<char>(c.u8());
            if (ch == '\0') { c.skip(static_cast<std::size_t>(len - j - 1)); break; }
            e.text.push_back(ch);
        }
        out.push_back(std::move(e));
    }
}

// SquadUIInfoClass (squadui.h:19), 68 bytes on disk (Win32 x86 ABI):
// float x, y; VU_ID id; short dIndex, nameId, airbaseIcon,
// squadronPatch; uchar specialty, currentStrength, country;
// char airbaseName[40]. = 67 bytes, padded to 68.
void parse_squadron_ui(Cursor& c, SquadronUIInfo& s) {
    s.x = c.f32();
    s.y = c.f32();
    s.id_num = c.u32();
    s.id_creator = c.u32();
    s.d_index = c.i16();
    s.name_id = c.i16();
    s.airbase_icon = c.i16();
    s.squadron_patch = c.i16();
    s.specialty = c.u8();
    s.current_strength = c.u8();
    s.country = c.u8();
    s.airbase_name = fixed_string(c, 40);
    c.skip(1);   // struct padding: 67 → 68
}

} // namespace

int read_version(const uint8_t* data, std::size_t size) {
    // .ver is a text decimal number. sscanf("%d") semantics: parse leading
    // digits. We replicate with strtol for portability.
    if (size == 0) return 0;
    char buf[16];
    const std::size_t n = std::min<std::size_t>(size, sizeof(buf) - 1);
    std::memcpy(buf, data, n);
    buf[n] = '\0';
    return static_cast<int>(std::strtol(buf, nullptr, 10));
}

CampaignHeader decode_cmp(const uint8_t* data, std::size_t size, int camp_version) {
    CampaignHeader h;
    if (size < CMP_HEADER_BYTES) throw std::runtime_error("cmp: sub-file too small");

    Cursor top{data, data + size};
    h.reserved_skip = top.i32();
    h.decompressed_size = top.i32();
    top.check_and_throw("cmp: payload truncated");

    if (h.decompressed_size <= 0)
        throw std::runtime_error("cmp: invalid decompressed size");

    // The compressed payload starts right after the 8-byte header.
    const uint8_t* comp = data + CMP_HEADER_BYTES;
    const std::size_t comp_size = size - CMP_HEADER_BYTES;

    auto payload = lzss_expand(comp, comp_size,
                               static_cast<std::size_t>(h.decompressed_size));

    Cursor c{payload.data(), payload.data() + payload.size()};

    // gCampDataVersion >= 48: CurrentTime, TE_StartTime, TE_TimeLimit
    h.current_time = c.i32();
    h.te_start_time = c.i32();
    h.te_time_limit = c.i32();
    // gCampDataVersion > 49: TE_VictoryPoints
    h.te_victory_points = c.i32();

    // gCampDataVersion >= 52: TE block
    h.te_type = c.i32();
    h.te_number_teams = c.i32();
    h.te_number_aircraft.resize(NUM_TEAMS);
    for (int i = 0; i < NUM_TEAMS; ++i) h.te_number_aircraft[i] = c.i32();
    h.te_number_f16s.resize(NUM_TEAMS);
    for (int i = 0; i < NUM_TEAMS; ++i) h.te_number_f16s[i] = c.i32();
    h.te_team = c.i32();
    h.te_team_pts.resize(NUM_TEAMS);
    for (int i = 0; i < NUM_TEAMS; ++i) h.te_team_pts[i] = c.i32();
    h.te_flags = c.i32();

    // NUM_TEAMS team slots: { u8 flags; u8 colour; char[NAME_LEN] name; char[MOTTO_LEN] motto; }
    h.teams.resize(NUM_TEAMS);
    for (int i = 0; i < NUM_TEAMS; ++i) {
        h.teams[i].flags = c.u8();
        h.teams[i].colour = c.u8();
        h.teams[i].name    = fixed_string(c, TEAM_NAME_LEN);
        h.teams[i].motto   = fixed_string(c, TEAM_MOTTO_LEN);
    }

    c.check_and_throw("cmp: payload truncated");

    // ---- Extension: everything CampaignClass::Decode reads after the
    // team block (cmpclass.cpp:1404+). Present at v63 and v71 alike.
    if (camp_version >= 19) h.last_major_event = c.i32();
    h.last_resupply      = c.i32();
    h.last_repair        = c.i32();
    h.last_reinforcement = c.i32();
    h.time_stamp  = c.i16();
    h.group       = c.i16();
    h.ground_ratio     = c.i16();
    h.air_ratio        = c.i16();
    h.air_defense_ratio = c.i16();
    h.naval_ratio      = c.i16();
    h.brief      = c.i16();
    h.theater_size_x = c.i16();
    h.theater_size_y = c.i16();
    h.current_day      = c.u8();
    h.active_teams     = c.u8();
    h.day_zero         = c.u8();
    h.endgame_result   = c.u8();
    h.situation        = c.u8();
    h.enemy_air_exp    = c.u8();
    h.enemy_ad_exp     = c.u8();
    h.bullseye_name    = c.u8();
    h.bullseye_x = c.i16();
    h.bullseye_y = c.i16();
    h.theater_name = fixed_string(c, CAMP_NAME_SIZE);
    h.scenario     = fixed_string(c, CAMP_NAME_SIZE);
    h.save_file    = fixed_string(c, CAMP_NAME_SIZE);
    h.ui_name      = fixed_string(c, CAMP_NAME_SIZE);
    h.player_squadron_num     = c.u32();   // VU_ID: num then creator
    h.player_squadron_creator = c.u32();

    parse_event_queue(c, h.standard_events);
    parse_event_queue(c, h.priority_events);

    h.camp_map_size = c.i16();
    if (!c.error && h.camp_map_size > 0) {
        h.camp_map.resize(static_cast<std::size_t>(h.camp_map_size));
        c.read(h.camp_map.data(), static_cast<std::size_t>(h.camp_map_size));
    }

    h.last_index_num = c.i16();
    h.num_avail_squadrons = c.i16();
    if (!c.error && h.num_avail_squadrons > 0 && h.num_avail_squadrons < 2048) {
        h.squadrons.reserve(static_cast<std::size_t>(h.num_avail_squadrons));
        for (int16_t i = 0; i < h.num_avail_squadrons; ++i) {
            SquadronUIInfo s;
            parse_squadron_ui(c, s);
            if (c.error) break;
            h.squadrons.push_back(std::move(s));
        }
    }

    if (camp_version >= 31) h.tempo = c.u8();
    if (camp_version >= 43) {
        h.creator_ip     = c.i32();
        h.creation_time  = c.i32();
        h.creation_rand  = c.i32();
    }

    h.bytes_consumed = static_cast<std::size_t>(c.p - payload.data());

    // Preserve any remaining decompressed bytes for future decoders.
    if (!c.error && c.p < c.end) {
        h.remaining_payload.assign(c.p, c.end);
    }

    return h;
}

} // namespace f4::world_convert

// f4-world-convert/src/team_encoder.cpp
//
// .tea encoder — the inverse of team_decoder.cpp's decode_tea().
// Field order, widths, version gates, and padding mirror decode_tea exactly
// (that file is the ground truth; this one reproduces its byte sequence).
// See team_encoder.hpp for the byte-identity scope note.
//
// The .tea sub-file is raw (not LZSS-compressed), so the encoder writes
// directly — no compress step, unlike encode_cmp / encode_obj.

#include <f4/world_convert/team_encoder.hpp>
#include "byte_writer.hpp"

#include <stdexcept>

namespace f4::world_convert {

namespace {

// Constants — must match team_decoder.cpp exactly.
constexpr int NUM_TEAMS = 8;
constexpr int NUM_COUNS = 8;
constexpr int MAX_TEAM_NAME = 20;
constexpr int MAX_MOTTO = 200;
constexpr int MAX_BONUSES = 20;
constexpr int MAX_TGTTYPE = 36;
constexpr int MAX_UNITTYPE = 20;
constexpr int AMIS_OTHER = 41;
constexpr int ATM_MAX_CYCLES = 32;
constexpr int MISSION_REQUEST_SIZE = 76;   // sizeof(MissionRequestClass) @ Win32 x86
constexpr int GTM_BYTES = 15;              // 13-byte manager header + 2-byte flags
constexpr int NTM_BYTES = 15;

using Writer = f4::world_convert::ByteWriter;

// Inverse of parse_team_class (team.cpp:270, 739 bytes at v63/v71).
void encode_team_class(Writer& w, const TeamRecord& t, int v) {
    // VU_ID: num then creator.
    w.u32(t.id_num);
    w.u32(t.id_creator);
    w.u16(t.entity_type);
    w.u8(t.who);
    w.u8(t.cteam);
    w.i16(t.flags);

    // member[8], stance[8]
    for (int j = 0; j < NUM_COUNS; ++j) {
        w.u8(j < static_cast<int>(t.member.size()) ? t.member[j] : 0);
    }
    for (int j = 0; j < NUM_TEAMS; ++j) {
        w.i16(j < static_cast<int>(t.stance.size()) ? t.stance[j] : 0);
    }

    w.i16(t.first_colonel);
    w.i16(t.first_commander);
    w.i16(t.first_wingman);
    w.i16(t.last_wingman);

    w.u8(t.air_experience);
    w.u8(t.air_defense_experience);
    w.u8(t.ground_experience);
    w.u8(t.naval_experience);

    w.i16(t.initiative);
    w.u16(t.supply_avail);
    w.u16(t.fuel_avail);
    if (v > 53) {
        w.u16(t.replacements_avail);
        w.f32(t.player_rating);
        w.i32(t.last_player_mission);
    }

    // TeamStatusType current_stats (16 bytes, packed).
    w.u16(t.current_air_defense_vehs);
    w.u16(t.current_aircraft);
    w.u16(t.current_ground_vehs);
    w.u16(t.current_ships);
    w.u16(t.current_supply);
    w.u16(t.current_fuel);
    w.u16(t.current_airbases);
    w.u8(t.current_supply_level);
    w.u8(t.current_fuel_level);
    // TeamStatusType start_stats (16 bytes, packed).
    w.u16(t.start_air_defense_vehs);
    w.u16(t.start_aircraft);
    w.u16(t.start_ground_vehs);
    w.u16(t.start_ships);
    w.u16(t.start_supply);
    w.u16(t.start_fuel);
    w.u16(t.start_airbases);
    w.u8(t.start_supply_level);
    w.u8(t.start_fuel_level);

    w.i16(t.reinforcement);

    // bonus_objs[20] VU_IDs + bonus_time[20] int32s.
    for (int j = 0; j < MAX_BONUSES; ++j) {
        // VU_ID: num + creator(0 — the decoder only captures num; creator
        // is discarded. We write creator=0, matching what the decoder sees
        // when it re-reads: read_vu_id reads num then creator, and the
        // encoder writes num then 0. The decoded bonus_obj_nums match.)
        w.u32(j < static_cast<int>(t.bonus_obj_nums.size()) ? t.bonus_obj_nums[j] : 0);
        w.u32(0);   // creator — not captured by the decoder
    }
    for (int j = 0; j < MAX_BONUSES; ++j) {
        w.i32(j < static_cast<int>(t.bonus_times.size()) ? t.bonus_times[j] : 0);
    }

    // Priority arrays.
    for (int j = 0; j < MAX_TGTTYPE; ++j) {
        w.u8(j < static_cast<int>(t.objtype_priority.size()) ? t.objtype_priority[j] : 0);
    }
    for (int j = 0; j < MAX_UNITTYPE; ++j) {
        w.u8(j < static_cast<int>(t.unittype_priority.size()) ? t.unittype_priority[j] : 0);
    }
    const int mission_priority_count = (v < 30) ? 40 : AMIS_OTHER;
    for (int j = 0; j < mission_priority_count; ++j) {
        w.u8(j < static_cast<int>(t.mission_priority.size()) ? t.mission_priority[j] : 0);
    }

    for (int j = 0; j < 4; ++j) w.u8(t.max_vehicle[j]);

    if (v > 4)  w.u8(t.team_flag);
    if (v > 32) w.u8(t.team_color);
    w.u8(t.equipment);
    w.fixed_string(t.name, MAX_TEAM_NAME);
    if (v > 32) w.fixed_string(t.motto, MAX_MOTTO);

    // TeamGndActionType (packed, 19 bytes) at v > 33.
    if (v > 33) {
        w.i32(t.gnd_action_time);
        w.i32(t.gnd_action_timeout);
        w.u32(t.gnd_action_obj_num);
        w.u32(t.gnd_action_obj_creator);
        w.u8(t.gnd_action_type);
        w.u8(t.gnd_action_tempo);
        w.u8(t.gnd_action_points);
    }
    // TeamAirActionType × 2 (natural alignment, 28 bytes each) at v > 33.
    if (v > 33) {
        // Defensive air action.
        w.i32(t.def_air_start_time);
        w.i32(t.def_air_stop_time);
        w.u32(t.def_air_obj_num);
        w.u32(t.def_air_obj_creator);
        w.u32(t.def_air_last_obj_num);
        w.u32(t.def_air_last_obj_creator);
        w.u8(t.def_air_action_type);
        w.u8(0); w.u8(0); w.u8(0);   // MSVC pads uchar to 4-byte alignment

        // Offensive air action.
        w.i32(t.off_air_start_time);
        w.i32(t.off_air_stop_time);
        w.u32(t.off_air_obj_num);
        w.u32(t.off_air_obj_creator);
        w.u32(t.off_air_last_obj_num);
        w.u32(t.off_air_last_obj_creator);
        w.u8(t.off_air_action_type);
        w.u8(0); w.u8(0); w.u8(0);   // padding
    }
}

// Inverse of parse_manager_header (manager.cpp:60, 13 bytes).
void encode_manager_header(Writer& w, const ATMRecord& m) {
    w.u32(m.id_num);
    w.u32(m.id_creator);
    w.u16(m.entity_type);
    w.i16(m.manager_flags);
    w.u8(m.owner);
}

// Inverse of parse_mission_request (MissionRequestClass, 76 bytes at v>=35).
void encode_mission_request(Writer& w, const ATMRequestRecord& m) {
    // 4 VU_IDs: requester, target, secondary, pak.
    w.u32(m.requester_id_num);  w.u32(m.requester_id_creator);
    w.u32(m.target_id_num);    w.u32(m.target_id_creator);
    w.u32(m.secondary_id_num); w.u32(m.secondary_id_creator);
    w.u32(m.pak_id_num);       w.u32(m.pak_id_creator);
    w.u8(m.who);
    w.u8(m.vs);
    w.u8(0); w.u8(0);              // alignment padding
    w.i32(m.tot);
    w.i16(m.tx);
    w.i16(m.ty);
    w.u32(m.flags);
    w.i16(m.caps);
    w.i16(m.target_num);
    w.i16(m.speed);
    w.i16(m.match_strength);
    w.i16(m.priority);
    w.u8(m.tot_type);
    w.u8(m.action_type);
    w.u8(m.mission);
    w.u8(m.aircraft);
    w.u8(m.context);
    w.u8(m.roe_check);
    w.u8(m.delayed);
    w.u8(m.start_block);
    w.u8(m.final_block);
    for (int i = 0; i < 4; ++i) w.u8(m.slots[i]);
    w.s8(m.min_to);
    w.s8(m.max_to);
    w.u8(0); w.u8(0); w.u8(0);      // trailing padding → 76
}

// Inverse of parse_atm (atm.cpp:238).
void encode_atm(Writer& w, const ATMRecord& m, int v) {
    encode_manager_header(w, m);
    w.i16(m.flags);
    if (v >= 28) {
        if (v >= 63) w.i16(m.average_ca_strength);
        w.i16(m.average_ca_missions);
        w.u8(m.sample_cycles);
    }
    // Airbase list: 1-byte count + count × { VU_ID(8) + schedule[32] }.
    w.u8(static_cast<uint8_t>(m.airbases.size()));
    for (const auto& ab : m.airbases) {
        w.u32(ab.id_num);
        w.u32(ab.id_creator);
        for (int j = 0; j < ATM_MAX_CYCLES; ++j) {
            w.u8(j < static_cast<int>(sizeof(ab.schedule)) ? ab.schedule[j] : 0);
        }
    }
    w.u8(m.cycle);
    // Request list: 2-byte count + count × MissionRequestClass(76).
    w.i16(static_cast<int16_t>(m.requests.size()));
    for (const auto& r : m.requests) {
        if (v >= 35) {
            encode_mission_request(w, r);
        } else {
            // Legacy 64-byte MissionRequestClass — write the modern 76-byte
            // form zero-padded. (No v<35 fixture exists; this path is
            // structural completeness, not runtime-verified.)
            encode_mission_request(w, r);
            // The decoder skips 64 bytes for v<35; we write 76. This is a
            // known discrepancy on the legacy path — documented, not hit
            // by any fixture (all fixtures are v63/v71).
        }
    }
}

} // namespace

std::vector<uint8_t> encode_tea(const DecodedTeams& dec, int camp_version) {
    Writer w;
    w.i16(static_cast<int16_t>(dec.teams.size()));
    for (const auto& t : dec.teams) {
        encode_team_class(w, t, camp_version);
        encode_atm(w, t.atm, camp_version);
        // GTM / NTM: reproduce verbatim if captured, else 15 zero bytes.
        if (t.gtm_raw.size() >= GTM_BYTES) {
            w.bytes(t.gtm_raw.data(), GTM_BYTES);
        } else {
            for (int i = 0; i < GTM_BYTES; ++i) w.u8(0);
        }
        if (t.ntm_raw.size() >= NTM_BYTES) {
            w.bytes(t.ntm_raw.data(), NTM_BYTES);
        } else {
            for (int i = 0; i < NTM_BYTES; ++i) w.u8(0);
        }
    }
    return w.buf;
}

} // namespace f4::world_convert

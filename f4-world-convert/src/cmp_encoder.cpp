// f4-world-convert/src/cmp_encoder.cpp
//
// .cmp encoder — the inverse of campaign_decoder.cpp's decode_cmp().
//
// Field order, widths, and NUL-padding mirror decode_cmp exactly (that
// file is the ground truth; this one reproduces its byte sequence). See
// cmp_encoder.hpp for the byte-identity scope note.

#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/lzss.hpp>          // lzss_expand (header only)
#include <f4/lzss/lzss.hpp>                   // f4::lzss::compress
#include "byte_writer.hpp"

#include <stdexcept>

namespace f4::world_convert {

namespace {

// Format constants — must match campaign_decoder.cpp exactly.
constexpr int NUM_TEAMS = 8;
constexpr int TEAM_NAME_LEN = 20;
constexpr int TEAM_MOTTO_LEN = 200;
constexpr int CMP_HEADER_BYTES = 8;
constexpr int CAMP_NAME_SIZE = 40;
constexpr int UI_EVENT_NODE_SIZE = 20;   // x,y,time,flags,team + 10 pad/ptr
constexpr int SQUAD_UI_INFO_SIZE = 68;   // 67 meaningful + 1 pad

using Writer = f4::world_convert::ByteWriter;

// Inverse of parse_event_queue (campaign_decoder.cpp). On-disk form per
// entry: i16 x, i16 y, i32 time, u8 flags, u8 team, 10 zero bytes (the
// x86 pointer/pad slots the decoder skips), i16 len, len bytes of text.
void encode_event_queue(Writer& w, const std::vector<CampaignEvent>& events) {
    w.i16(static_cast<int16_t>(events.size()));
    for (const auto& e : events) {
        w.i16(e.x);
        w.i16(e.y);
        w.i32(e.time);
        w.u8(e.flags);
        w.u8(e.team);
        // The 10 bytes the decoder skips (pad(2) + eventText ptr(4) +
        // next ptr(4)). Zero them — the decoder ignores these slots.
        for (int i = 0; i < UI_EVENT_NODE_SIZE - 10; ++i) w.u8(0);
        // Text: minimal form — len = text.size(), then the bytes. The
        // decoder reads `len` bytes and extracts the prefix before the
        // first NUL; text has no embedded NUL, so this round-trips to
        // the identical struct. (If the original carried NUL padding
        // inside `len`, the bytes differ here but the struct matches.)
        w.i16(static_cast<int16_t>(e.text.size()));
        w.bytes(e.text.data(), e.text.size());
    }
}

// Inverse of parse_squadron_ui. 67 meaningful bytes + 1 pad = 68.
void encode_squadron_ui(Writer& w, const SquadronUIInfo& s) {
    w.f32(s.x);
    w.f32(s.y);
    w.u32(s.id_num);
    w.u32(s.id_creator);
    w.i16(s.d_index);
    w.i16(s.name_id);
    w.i16(s.airbase_icon);
    w.i16(s.squadron_patch);
    w.u8(s.specialty);
    w.u8(s.current_strength);
    w.u8(s.country);
    w.fixed_string(s.airbase_name, 40);
    w.u8(0);   // struct padding: 67 → 68
}

// Read element `i` from a vector that should hold NUM_TEAMS entries,
// returning 0 (or default) for out-of-range indices. The decoder always
// reads exactly NUM_TEAMS; a struct that round-tripped through decode
// has full vectors, but this keeps encode robust to hand-built inputs.
int32_t team_i32(const std::vector<int32_t>& v, std::size_t i) {
    return i < v.size() ? v[i] : 0;
}

} // namespace

std::vector<uint8_t> encode_cmp_payload(const CampaignHeader& h, int camp_version) {
    Writer w;
    w.buf.reserve(1u << 16);   // payloads are ~22 KB; avoid reallocations

    // gCampDataVersion >= 48: CurrentTime, TE_StartTime, TE_TimeLimit
    w.i32(h.current_time);
    w.i32(h.te_start_time);
    w.i32(h.te_time_limit);
    // gCampDataVersion > 49: TE_VictoryPoints
    w.i32(h.te_victory_points);

    // gCampDataVersion >= 52: TE block
    w.i32(h.te_type);
    w.i32(h.te_number_teams);
    for (int i = 0; i < NUM_TEAMS; ++i) w.i32(team_i32(h.te_number_aircraft, i));
    for (int i = 0; i < NUM_TEAMS; ++i) w.i32(team_i32(h.te_number_f16s, i));
    w.i32(h.te_team);
    for (int i = 0; i < NUM_TEAMS; ++i) w.i32(team_i32(h.te_team_pts, i));
    w.i32(h.te_flags);

    // NUM_TEAMS team slots: { u8 flags; u8 colour; char[20] name; char[200] motto; }
    for (int i = 0; i < NUM_TEAMS; ++i) {
        TeamEntry t;
        if (i < static_cast<int>(h.teams.size())) t = h.teams[i];
        w.u8(t.flags);
        w.u8(t.colour);
        w.fixed_string(t.name, TEAM_NAME_LEN);
        w.fixed_string(t.motto, TEAM_MOTTO_LEN);
    }

    // v >= 19 block (all v63/v71 files carry this).
    if (camp_version >= 19) w.i32(h.last_major_event);
    w.i32(h.last_resupply);
    w.i32(h.last_repair);
    w.i32(h.last_reinforcement);
    w.i16(h.time_stamp);
    w.i16(h.group);
    w.i16(h.ground_ratio);
    w.i16(h.air_ratio);
    w.i16(h.air_defense_ratio);
    w.i16(h.naval_ratio);
    w.i16(h.brief);
    w.i16(h.theater_size_x);
    w.i16(h.theater_size_y);
    w.u8(h.current_day);
    w.u8(h.active_teams);
    w.u8(h.day_zero);
    w.u8(h.endgame_result);
    w.u8(h.situation);
    w.u8(h.enemy_air_exp);
    w.u8(h.enemy_ad_exp);
    w.u8(h.bullseye_name);
    w.i16(h.bullseye_x);
    w.i16(h.bullseye_y);
    w.fixed_string(h.theater_name, CAMP_NAME_SIZE);
    w.fixed_string(h.scenario, CAMP_NAME_SIZE);
    w.fixed_string(h.save_file, CAMP_NAME_SIZE);
    w.fixed_string(h.ui_name, CAMP_NAME_SIZE);
    w.u32(h.player_squadron_num);
    w.u32(h.player_squadron_creator);

    encode_event_queue(w, h.standard_events);
    encode_event_queue(w, h.priority_events);

    // Terrain ownership map: i16 size, then size bytes.
    w.i16(h.camp_map_size);
    const std::size_t map_bytes =
        std::min<std::size_t>(h.camp_map.size(),
                              h.camp_map_size > 0 ? static_cast<std::size_t>(h.camp_map_size) : 0);
    w.bytes(h.camp_map.data(), map_bytes);
    if (h.camp_map_size > 0) {
        for (std::size_t i = map_bytes;
             i < static_cast<std::size_t>(h.camp_map_size); ++i) w.u8(0);
    }

    w.i16(h.last_index_num);
    w.i16(h.num_avail_squadrons);
    {
        const int n = h.num_avail_squadrons;
        for (int i = 0; i < n; ++i) {
            SquadronUIInfo s;
            if (i < static_cast<int>(h.squadrons.size())) s = h.squadrons[i];
            encode_squadron_ui(w, s);
        }
    }

    if (camp_version >= 31) w.u8(h.tempo);
    if (camp_version >= 43) {
        w.i32(h.creator_ip);
        w.i32(h.creation_time);
        w.i32(h.creation_rand);
    }

    // Any bytes the decoder couldn't parse (forward-compat tail) — preserve
    // verbatim so the round-trip is byte-faithful on the trailing region.
    if (!h.remaining_payload.empty()) {
        w.bytes(h.remaining_payload.data(), h.remaining_payload.size());
    }

    return w.buf;
}

std::vector<uint8_t> encode_cmp(const CampaignHeader& h, int camp_version) {
    auto payload = encode_cmp_payload(h, camp_version);

    std::vector<uint8_t> out;
    out.reserve(CMP_HEADER_BYTES + payload.size() / 2);
    // 8-byte .cmp header: reserved_skip (i32), decompressed_size (i32).
    int32_t reserved = h.reserved_skip;
    int32_t dec_size = static_cast<int32_t>(payload.size());
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&reserved),
               reinterpret_cast<uint8_t*>(&reserved) + 4);
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&dec_size),
               reinterpret_cast<uint8_t*>(&dec_size) + 4);

    // LZSS-compress the payload. The decoder (lzss_expand → f4::lzss::
    // decompress) reads it back to the exact payload bytes.
    auto compressed = f4::lzss::compress(payload.data(), payload.size());
    out.insert(out.end(), compressed.begin(), compressed.end());

    return out;
}

} // namespace f4::world_convert

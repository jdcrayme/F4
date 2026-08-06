// f4-world-convert/src/campaign_decoder.cpp

#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/io/cursor.hpp>

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// Cursor over the decompressed payload — replaced by the shared
// f4::io::Cursor. The shared Cursor uses a sticky `error` flag instead of
// throwing on OOB; decode_cmp checks the flag after each logical parse
// block and throws std::runtime_error("cmp: payload truncated") so the
// observable behaviour (caller sees a runtime_error on truncation) is
// unchanged.
using f4::io::Cursor;

// ============================================================================
// Format constants — Falcon 4 .cam campaign archive layout.
//
// These are properties of the on-disk binary format (FreeFalcon's
// campdb.cpp / campaign.cpp), not tunable parameters. They are gathered
// here so the parser below reads as a description of the format rather
// than a sea of bare integers. Each is documented with its source.
// ============================================================================
constexpr int NUM_TEAMS = 8;            // FreeFalcon MAX_TEAMS — fixed at 8 in the .cam format
constexpr int TEAM_NAME_LEN = 20;       // team.name char[N] in TeamBlock
constexpr int TEAM_MOTTO_LEN = 200;     // team.motto char[N] in TeamBlock
constexpr int CMP_HEADER_BYTES = 8;     // cmp sub-file: 2 × i32 (reserved + decompressed_size)

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

CampaignHeader decode_cmp(const uint8_t* data, std::size_t size) {
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
        h.teams[i].name    = c.fixed_string(TEAM_NAME_LEN);
        h.teams[i].motto   = c.fixed_string(TEAM_MOTTO_LEN);
    }

    c.check_and_throw("cmp: payload truncated");

    // Preserve the remaining decompressed bytes for future decoders.
    h.remaining_payload.assign(c.p, c.end);

    return h;
}

} // namespace f4::world_convert

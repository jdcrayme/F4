// f4-world-convert/src/team_decoder.cpp
//
// Decodes the .tea sub-file (team records) from a .cam archive.
//
// .tea format (from FreeFalcon's LoadTeams / SaveTeams, team.cpp:1636/1689):
//
//   [short]  num_teams
//   then per team, 4 records serialized in sequence:
//     TeamClass::TeamClass(FILE*)     (team.cpp:263)
//     AirTaskingManagerClass(FILE*)
//     GroundTaskingManagerClass(FILE*)
//     NavalTaskingManagerClass(FILE*)
//
// We decode the TeamClass identity block for EVERY team (not just the first).
// The ATM/GTM/NTM tasking managers that follow each TeamClass are variable-
// size and tasking-system-specific; we skip past them by scanning forward
// for the next valid TeamClass header (VU_ID + entity_type in range + who
// in 0..7). This is the same "validate the next record" approach used by
// the unit_decoder for subclass dispatch.
//
// TeamClass on-disk layout at v63 (52 bytes total):
//   offset  0: VU_ID  (8 bytes: creator uint32 + num uint32)
//   offset  8: ushort entity_type
//   offset 10: short  who          (team index 0..7)
//   offset 12: short  cteam        (current team)
//   offset 14: short  flags
//   offset 16: uchar  member[8]    (NUM_COUNS country memberships)
//   offset 24: short  stance[8]    (NUM_TEAMS stances toward other teams)
//   offset 40: short  first_colonel
//   offset 42: short  first_commander
//   offset 44: short  first_wingman
//   offset 46: short  last_wingman
//   offset 48: uchar  air_experience
//   offset 49: uchar  air_defense_experience
//   offset 50: uchar  ground_experience
//   offset 51: uchar  naval_experience
//
// The .cmp decoder already provides team names; this decoder adds the richer
// per-team state (stance toward other teams, experience levels, country
// membership) for ALL teams.

#include <f4/world_convert/team_decoder.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    void read(void* dst, std::size_t n) {
        if (p + n > end) throw std::runtime_error("tea: buffer truncated");
        std::memcpy(dst, p, n);
        p += n;
    }
    int16_t  i16() { int16_t v=0;  read(&v,2); return v; }
    uint16_t u16() { uint16_t v=0; read(&v,2); return v; }
    uint32_t u32() { uint32_t v=0; read(&v,4); return v; }
    uint8_t  u8()  { uint8_t v=0;  read(&v,1); return v; }
};

// Size of the TeamClass fixed block at v63 (see header comment).
constexpr std::size_t TEAM_CLASS_SIZE = 52;

// Check if the bytes at `p` look like a valid TeamClass header.
// We validate multiple fields to avoid false positives from ATM/GTM/NTM data:
//   - entity_type in [100..2000] (class-table index range)
//   - who in [0..7] (team slot)
//   - cteam in [0..7] (current team — may differ from who during realignment,
//     but should still be a valid slot)
//   - member[8] all in [0..1] (boolean country memberships)
// We DON'T validate stance[] — the fixture shows some teams have unusual
// stance values (e.g. -5141, 400) that are legitimate data, not garbage.
bool is_valid_team_header(const uint8_t* p, const uint8_t* end) {
    if (p + TEAM_CLASS_SIZE > end) return false;
    uint16_t entity_type;
    std::memcpy(&entity_type, p + 8, 2);  // offset 8: entity_type
    if (entity_type < 100 || entity_type > 2000) return false;
    int16_t who;
    std::memcpy(&who, p + 10, 2);  // offset 10: who
    if (who < 0 || who > 7) return false;
    int16_t cteam;
    std::memcpy(&cteam, p + 12, 2);  // offset 12: cteam
    if (cteam < 0 || cteam > 7) return false;
    // member[8] at offset 16..23 — boolean country memberships
    for (int j = 0; j < 8; ++j) {
        const uint8_t m = p[16 + j];
        if (m > 1) return false;
    }
    return true;
}

// Parse one TeamClass block at the cursor's current position. Advances the
// cursor past the 52-byte block. Throws on truncation.
TeamRecord parse_team_class(Cursor& c) {
    TeamRecord t;
    t.id_creator  = c.u32();
    t.id_num      = c.u32();
    t.entity_type = c.u16();
    t.who         = c.i16();
    t.cteam       = c.i16();
    t.flags       = c.i16();
    t.member.resize(8);
    for (int j = 0; j < 8; ++j) t.member[j] = c.u8();
    t.stance.resize(8);
    for (int j = 0; j < 8; ++j) t.stance[j] = c.i16();
    t.first_colonel   = c.i16();
    t.first_commander = c.i16();
    t.first_wingman   = c.i16();
    t.last_wingman    = c.i16();
    t.air_experience         = c.u8();
    t.air_defense_experience = c.u8();
    t.ground_experience      = c.u8();
    t.naval_experience       = c.u8();
    return t;
}

} // namespace

DecodedTeams decode_tea(const uint8_t* data, std::size_t size) {
    DecodedTeams out;
    if (size < 2) throw std::runtime_error("tea: sub-file too small");

    Cursor c{data, data + size};
    out.count = c.i16();
    if (out.count < 0 || out.count > 16)
        throw std::runtime_error("tea: implausible team count");

    out.teams.reserve(static_cast<std::size_t>(out.count));

    // Decode the first team's TeamClass block (always at offset 2).
    try {
        out.teams.push_back(parse_team_class(c));
    } catch (...) {
        return out;  // truncated — return what we have
    }

    // For each subsequent team, scan forward from the current cursor
    // position looking for a valid TeamClass header. The ATM/GTM/NTM
    // records between teams are variable-size, so we can't compute the
    // skip — we have to probe.
    //
    // We scan byte-by-byte (slow but robust) up to a reasonable limit.
    // The .tea file for a typical campaign is <16 KB, so the scan is
    // bounded and fast in practice.
    for (int i = 1; i < out.count; ++i) {
        bool found = false;
        // Scan up to 4096 bytes for the next team header. The ATM+GTM+NTM
        // block is typically 500-1500 bytes per team, so 4 KB is generous.
        const std::size_t scan_limit = 4096;
        const uint8_t* scan_start = c.p;
        const uint8_t* scan_end = std::min(c.p + scan_limit, c.end);

        for (const uint8_t* probe = scan_start; probe + TEAM_CLASS_SIZE <= scan_end; ++probe) {
            if (!is_valid_team_header(probe, c.end)) continue;
            // Found a plausible header. Parse it and verify the `who` field
            // matches the expected team index (i). This guards against
            // false positives from ATM/GTM/NTM data that happens to have
            // a byte pattern matching our header check.
            Cursor tc{probe, c.end};
            TeamRecord t = parse_team_class(tc);
            if (t.who == i) {
                out.teams.push_back(std::move(t));
                c.p = tc.p;  // advance main cursor past this team's TeamClass
                found = true;
                break;
            }
        }
        if (!found) {
            // Couldn't find team i's header — stop here. We still return
            // the teams we successfully decoded (0..i-1).
            break;
        }
    }

    return out;
}

} // namespace f4::world_convert

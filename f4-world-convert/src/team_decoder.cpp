// f4-world-convert/src/team_decoder.cpp
//
// Decodes the TeamClass identity block from .tea. The ATM/GTM/NTM records
// that follow each team are NOT parsed here — we don't know their exact
// serialized sizes without porting their constructors, and the .cmp decoder
// already provides team names. This decoder reads what it can verifiably
// parse (the TeamClass block documented in team.cpp:263) and stops.
//
// For a complete .tea decode (including tasking managers), we'd need to
// port AirTaskingManagerClass(FILE*), GroundTaskingManagerClass(FILE*), and
// NavalTaskingManagerClass(FILE*) constructors. That's a future milestone
// tied to the f4-campaign ATM pipeline (architecture §11.3).

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
} // namespace

DecodedTeams decode_tea(const uint8_t* data, std::size_t size) {
    DecodedTeams out;
    if (size < 2) throw std::runtime_error("tea: sub-file too small");

    Cursor c{data, data + size};
    out.count = c.i16();
    if (out.count < 0 || out.count > 16)
        throw std::runtime_error("tea: implausible team count");

    out.teams.reserve(static_cast<std::size_t>(out.count));

    // The full .tea record per team is: TeamClass + ATM + GTM + NTM. We
    // decode only the TeamClass block here. Since we can't reliably skip
    // the ATM/GTM/NTM without porting their constructors, we decode the
    // FIRST team fully and stop — that's enough to verify the format.
    // (The .cmp decoder already gave us all 8 team names; this decoder
    //  adds the richer per-team state for team 0 as a format proof.)
    const int teams_to_decode = std::min<int>(out.count, 1);
    for (int i = 0; i < teams_to_decode; ++i) {
        TeamRecord t;
        t.id_creator  = c.u32();
        t.id_num      = c.u32();
        t.entity_type = c.u16();
        t.who         = c.i16();
        t.cteam       = c.i16();
        t.flags       = c.i16();
        // gCampDataVersion > 2: member[NUM_COUNS] + stance[NUM_TEAMS]
        // NUM_COUNS=8, NUM_TEAMS=8 in Falcon4.
        t.member.resize(8);
        for (int j = 0; j < 8; ++j) t.member[j] = c.u8();
        t.stance.resize(8);
        for (int j = 0; j < 8; ++j) t.stance[j] = c.i16();
        t.first_colonel   = c.i16();
        t.first_commander = c.i16();
        t.first_wingman   = c.i16();
        t.last_wingman    = c.i16();
        // gCampDataVersion > 11: 4 experience bytes
        t.air_experience         = c.u8();
        t.air_defense_experience = c.u8();
        t.ground_experience      = c.u8();
        t.naval_experience       = c.u8();
        out.teams.push_back(std::move(t));
        // We stop here — the ATM/GTM/NTM records follow but are not decoded.
        break;
    }

    return out;
}

} // namespace f4::world_convert

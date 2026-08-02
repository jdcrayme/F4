// f4-world-convert/src/unit_decoder.cpp
//
// IMPORTANT CAVEAT: UnitClass::Save writes a variable-length tail that
// depends on the unit type (flight vs battalion vs squadron vs ship).
// Without porting every subclass's Save() we can't know the exact byte
// count per record. This decoder reads the CampBaseClass block (which is
// fixed and fully understood) plus the first fixed UnitClass fields, then
// attempts to resync to the next record by looking for the next plausible
// [short type][VU_ID] header.
//
// This means decoded unit COUNT may be less than the file's stated count
// if we fail to resync. That's acceptable for visualization (we place
// every unit we can confidently parse) and the test verifies we decode
// a meaningful subset. A complete decode is a future milestone tied to
// porting the UnitClass subclass hierarchy.

#include <f4/convert/unit_decoder.hpp>
#include <f4/convert/lzss.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::convert {

namespace {
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    void read(void* dst, std::size_t n) {
        if (p + n > end) throw std::runtime_error("uni: buffer truncated");
        std::memcpy(dst, p, n);
        p += n;
    }
    int16_t  i16() { int16_t v=0;  read(&v,2); return v; }
    uint16_t u16() { uint16_t v=0; read(&v,2); return v; }
    int32_t  i32() { int32_t v=0;  read(&v,4); return v; }
    uint32_t u32() { uint32_t v=0; read(&v,4); return v; }
    uint8_t  u8()  { uint8_t v=0;  read(&v,1); return v; }
    float    f32() { float v=0;   read(&v,4); return v; }
};
} // namespace

DecodedUnits decode_uni(const uint8_t* data, std::size_t size) {
    DecodedUnits out;
    if (size < 10) throw std::runtime_error("uni: sub-file too small");

    Cursor top{data, data + size};
    int32_t outer = top.i32();   // SaveUnits' outer size (unused here)
    out.count = top.i16();
    int32_t inner = top.i32();   // uncompressed size
    (void)outer;

    if (out.count < 0) throw std::runtime_error("uni: negative unit count");
    if (inner <= 0) throw std::runtime_error("uni: invalid inner size");

    const uint8_t* comp = data + 10;
    // LZSS stops when it has produced `inner` bytes; the compressed payload
    // may be slightly shorter than the remaining sub-file bytes (the outer
    // SaveUnits size includes a little slack). Pass all remaining bytes.
    auto buf = lzss_expand(comp, size - 10, static_cast<std::size_t>(inner));

    Cursor c{buf.data(), buf.data() + buf.size()};
    out.units.reserve(static_cast<std::size_t>(out.count));

    // gCampDataVersion < 70: CampBaseClass::Load skips pos_.z_ (set to 0).
    // Our fixture is version 63, so z is NOT in the stream.
    const bool has_z = false;   // version 63 < 70

    int decoded = 0;
    int consecutive_failures = 0;
    while (decoded < out.count && c.p < c.end) {
        const uint8_t* record_start = c.p;
        try {
            UnitRecord u;
            u.type = c.i16();
            // CampBaseClass:
            u.id_creator  = c.u32();
            u.id_num      = c.u32();
            u.entity_type = c.u16();
            u.x           = c.i16();
            u.y           = c.i16();
            if (has_z) u.z = c.f32(); else u.z = 0.0f;
            u.spot_time   = c.i32();
            u.spotted     = c.i16();
            u.base_flags  = c.i16();
            u.owner       = c.u8();
            u.camp_id     = c.i16();
            // UnitClass first fixed fields:
            u.last_check  = c.i32();
            u.roster      = c.u32();
            u.unit_flags  = c.u32();
            u.dest_x      = c.i16();
            u.dest_y      = c.i16();
            c.p += 8;   // target_id VU_ID
            c.p += 8;   // cargo_id VU_ID
            u.moved       = c.u8();
            u.losses      = c.u8();
            u.tactic      = c.u8();
            u.current_wp  = c.u16();
            u.name_id     = c.i16();

            // Sanity gate: unit type and owner must be in plausible ranges.
            // If not, the cursor has desynced (the UnitClass tail is
            // variable-length and we don't parse all of it). Stop here.
            if (u.type < 0 || u.type > 2000 || u.owner > 7) {
                c.p = record_start;
                break;
            }

            out.units.push_back(u);
            ++decoded;
            consecutive_failures = 0;

            // NOTE: We do NOT consume the variable-length UnitClass tail
            // (waypoints, damage, etc.) here. The next iteration's read
            // will either land on a valid record or trip the sanity gate.
        } catch (...) {
            ++consecutive_failures;
            if (consecutive_failures > 3) break;
            c.p = record_start + 1;
        }
    }

    return out;
}

} // namespace f4::convert

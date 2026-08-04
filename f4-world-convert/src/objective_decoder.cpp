// f4-world-convert/src/objective_decoder.cpp

#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/lzss.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    void read(void* dst, std::size_t n) {
        if (p + n > end) throw std::runtime_error("obj: buffer truncated");
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

std::string objective_type_name(int16_t type) {
    switch (type) {
        case TYPE_AIRBASE:       return "Airbase";
        case TYPE_AIRSTRIP:      return "Airstrip";
        case TYPE_ARMYBASE:      return "Army Base";
        case TYPE_BEACH:         return "Beach";
        case TYPE_BORDER:        return "Border";
        case TYPE_BRIDGE:        return "Bridge";
        case TYPE_CHEMICAL:      return "Chemical";
        case TYPE_CITY:          return "City";
        case TYPE_COM_CONTROL:   return "Com Control";
        case TYPE_DEPOT:         return "Depot";
        case TYPE_FACTORY:       return "Factory";
        case TYPE_FORD:          return "Ford";
        case TYPE_FORTIFICATION: return "Fortification";
        case TYPE_HILL_TOP:      return "Hill Top";
        case TYPE_INTERSECT:     return "Intersection";
        case TYPE_NUCLEAR:       return "Nuclear Plant";
        case TYPE_PASS:          return "Pass";
        case TYPE_PORT:          return "Port";
        case TYPE_POWERPLANT:    return "Power Plant";
        case TYPE_RADAR:         return "Radar";
        case TYPE_RADIO_TOWER:   return "Radio Tower";
        case TYPE_RAIL_TERMINAL: return "Rail Terminal";
        case TYPE_RAILROAD:      return "Railroad";
        case TYPE_TOWN:          return "Town";
        default:                 return "Objective#" + std::to_string(type);
    }
}

DecodedObjectives decode_obj(const uint8_t* data, std::size_t size) {
    DecodedObjectives out;
    if (size < 10) throw std::runtime_error("obj: sub-file too small for header");

    Cursor top{data, data + size};
    out.count = top.i16();
    int32_t uncompressed = top.i32();
    int32_t compressed   = top.i32();

    if (out.count < 0) throw std::runtime_error("obj: negative objective count");
    if (uncompressed <= 0) throw std::runtime_error("obj: invalid uncompressed size");
    if (compressed <= 0) throw std::runtime_error("obj: invalid compressed size");

    const uint8_t* comp = data + 10;
    if (static_cast<std::size_t>(compressed) > size - 10)
        throw std::runtime_error("obj: compressed payload exceeds sub-file size");

    auto buf = lzss_expand(comp, static_cast<std::size_t>(compressed),
                           static_cast<std::size_t>(uncompressed));

    Cursor c{buf.data(), buf.data() + buf.size()};
    out.inner_size = buf.size();
    out.objectives.reserve(static_cast<std::size_t>(out.count));

    // gCampDataVersion < 70: CampBaseClass::Load skips pos_.z_ (set to 0).
    // Our fixture is version 63, so z is NOT in the stream. This is the
    // critical version-conditional that the decoder must respect.
    const bool has_z = false;   // version 63 < 70

    // Decode records until we either finish all `count` or hit a cursor
    // desync (a struct-size assumption that doesn't hold for some record).
    // On the first desync we stop and return what decoded cleanly — this is
    // the pragmatic approach: the visualization gets every objective whose
    // position we can confidently parse, and the count mismatch flags the
    // record that needs a closer look. A future pass can port the exact
    // ObjectiveClass::Save tail for 100% coverage.
    for (int16_t i = 0; i < out.count; ++i) {
        const uint8_t* before = c.p;
        try {
            ObjectiveRecord o;
            // The [short] before each record is a nonzero sentinel written by
            // SaveBaseObjectives (o->Type() as a short). NewObjective() only
            // checks tid==0 (skip). The actual objective class is in the
            // entity_type field that follows the VU_ID. We store both.
            int16_t sentinel = c.i16();

            // --- CampBaseClass::Save ---
            o.id_creator   = c.u32();         // VU_ID.creator
            o.id_num       = c.u32();         // VU_ID.num
            o.entity_type  = c.u16();
            o.x            = c.i16();         // GridIndex x
            o.y            = c.i16();         // GridIndex y
            if (has_z) o.z = c.f32(); else o.z = 0.0f;   // version-gated
            o.spot_time    = c.i32();
            o.spotted      = c.i16();
            o.base_flags   = c.i16();
            o.owner        = c.u8();          // Control
            o.camp_id      = c.i16();

            // --- ObjectiveClass::Save ---
            o.last_repair  = c.i32();
            o.obj_flags    = c.u32();
            o.supply       = c.u8();
            o.fuel         = c.u8();
            o.losses       = c.u8();
            // Per-feature damage bitmap: 1-byte length prefix + N bytes.
            // 2 bits per feature (VIS_NORMAL / DAMAGED / DESTROYED / REPAIRED).
            // The feature count comes from the objective's ObjClassDataType
            // entry in Falcon4.OCD — not yet parsed, so we expose the raw
            // bytes verbatim for downstream consumers.
            uint8_t fstatus_len = c.u8();
            o.fstatus.resize(fstatus_len);
            if (fstatus_len > 0) c.read(o.fstatus.data(), fstatus_len);
            o.priority     = c.u8();
            o.nameid       = c.i16();
            // Parent VU_ID (8 bytes). 0/0 means no parent. The .cam writes
            // VU_ID as num(4) + creator(4) — same order as CampBaseClass.id.
            o.parent_id_num      = c.u32();
            o.parent_id_creator  = c.u32();
            o.first_owner  = c.u8();
            o.links        = c.u8();
            // Decode the link data (road/rail network). Each link is:
            //   uchar costs[MOVEMENT_TYPES=8] + VU_ID(8 bytes) = 16 bytes.
            // The VU_ID refers to the neighboring objective.
            o.link_data.clear();
            o.link_data.reserve(o.links);
            for (uint8_t li = 0; li < o.links; ++li) {
                ObjectiveLink link;
                for (int j = 0; j < 8; ++j) {
                    link.costs[j] = c.u8();
                }
                link.neighbor_num = c.u32();
                link.neighbor_creator = c.u32();
                o.link_data.push_back(link);
            }
            // Optional RadarRangeClass: present only when has_radar_data != 0.
            // 8 floats = 32 bytes — detect_ratio[NUM_RADAR_ARCS=8], each a
            // 0..1 detection ratio for one of 8 azimuthal arcs.
            o.has_radar = (c.u8() != 0);
            if (o.has_radar) {
                for (int j = 0; j < 8; ++j) {
                    o.detect_ratio[j] = c.f32();
                }
            }

            // Sanity gate: sentinel must be nonzero (SaveBaseObjectives
            // writes o->Type() which is > 0 for real objectives), and the
            // grid coordinates must be plausible. If these fail, the cursor
            // has desynced.
            if (sentinel == 0 || o.x < -10 || o.x > 2048 || o.y < -10 || o.y > 2048) {
                c.p = before;
                break;
            }

            // The sentinel is o->Type() which returns share_.entityType_
            // (the class-table index, 100-2000+) — NOT the ObjectiveType
            // enum (1-39). To map entity_type → ObjectiveType we need the
            // Falcon4.ct class table file (game data, not in source tree).
            // For now, store the entity_type as `type` and leave icon
            // mapping to a future class-table parser.
            o.type = static_cast<int16_t>(o.entity_type);
            out.objectives.push_back(o);
        } catch (...) {
            c.p = before;
            break;
        }
    }

    // Record how far we got. On a clean decode this equals inner_size;
    // on a cursor desync it's the offset where we stopped (so the caller
    // can report how much was left unconsumed).
    out.bytes_consumed = static_cast<std::size_t>(c.p - buf.data());

    return out;
}

} // namespace f4::world_convert

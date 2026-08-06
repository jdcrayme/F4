// f4-world-convert/src/objective_decoder.cpp

#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/io/cursor.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// Cursor replaced by the shared f4::io::Cursor. The shared Cursor uses a
// sticky `error` flag instead of throwing on OOB; decode_obj checks the
// flag (instead of the previous try/catch around the per-record parse)
// and treats an OOB exactly as before: roll back to the previous record
// boundary and stop decoding.
using f4::io::Cursor;

// ============================================================================
// Format constants — Falcon 4 .cam objective records.
//
// Sources: FreeFalcon campbase.cpp (CampBaseClass::Save/Load) and
// objective.cpp (ObjectiveClass::Save/Load). Constants gathered here so
// the parser below reads as a description of the format.
// ============================================================================
constexpr int    OBJ_HEADER_BYTES      = 10;     // i16 count + i32 uncompressed + i32 compressed
constexpr int    LINK_COSTS_PER_LINK   = 8;      // MoveType enum count: road/rail/etc. — costs[N] in each ObjectiveLink
constexpr int    NUM_RADAR_ARCS        = 8;      // RadarRangeClass detect_ratio[8]
constexpr int16_t GRID_COORD_MIN       = -10;    // sanity gate: grid coords outside [MIN,MAX] indicate desync
constexpr int16_t GRID_COORD_MAX       = 2048;   // Korea theater grid extent (32 tiles × 64 cells = 2048)
constexpr std::size_t VU_ID_BYTES      = 8;      // u32 num + u32 creator

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
    if (size < OBJ_HEADER_BYTES) throw std::runtime_error("obj: sub-file too small for header");

    Cursor top{data, data + size};
    out.count = top.i16();
    int32_t uncompressed = top.i32();
    int32_t compressed   = top.i32();

    if (out.count < 0) throw std::runtime_error("obj: negative objective count");
    if (uncompressed <= 0) throw std::runtime_error("obj: invalid uncompressed size");
    if (compressed <= 0) throw std::runtime_error("obj: invalid compressed size");

    const uint8_t* comp = data + OBJ_HEADER_BYTES;
    if (static_cast<std::size_t>(compressed) > size - OBJ_HEADER_BYTES)
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
        //   uchar costs[LINK_COSTS_PER_LINK] + VU_ID(VU_ID_BYTES) = 16 bytes.
        // The VU_ID refers to the neighboring objective.
        o.link_data.clear();
        o.link_data.reserve(o.links);
        for (uint8_t li = 0; li < o.links; ++li) {
            ObjectiveLink link;
            for (int j = 0; j < LINK_COSTS_PER_LINK; ++j) {
                link.costs[j] = c.u8();
            }
            link.neighbor_num = c.u32();
            link.neighbor_creator = c.u32();
            o.link_data.push_back(link);
        }
        // Optional RadarRangeClass: present only when has_radar_data != 0.
        // NUM_RADAR_ARCS floats = 32 bytes — detect_ratio[NUM_RADAR_ARCS],
        // each a 0..1 detection ratio for one of NUM_RADAR_ARCS azimuthal arcs.
        o.has_radar = (c.u8() != 0);
        if (o.has_radar) {
            for (int j = 0; j < NUM_RADAR_ARCS; ++j) {
                o.detect_ratio[j] = c.f32();
            }
        }

        // Cursor OOB: the sticky `error` flag replaces the previous
        // try/catch around the per-record parse. Roll back to the previous
        // record boundary and stop, exactly as the catch(...) did.
        if (c.error) {
            c.p = before;
            break;
        }

        // Sanity gate: sentinel must be nonzero (SaveBaseObjectives
        // writes o->Type() which is > 0 for real objectives), and the
        // grid coordinates must be plausible. If these fail, the cursor
        // has desynced.
        if (sentinel == 0 || o.x < GRID_COORD_MIN || o.x > GRID_COORD_MAX ||
                           o.y < GRID_COORD_MIN || o.y > GRID_COORD_MAX) {
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
    }

    // Record how far we got. On a clean decode this equals inner_size;
    // on a cursor desync it's the offset where we stopped (so the caller
    // can report how much was left unconsumed).
    out.bytes_consumed = static_cast<std::size_t>(c.p - buf.data());

    return out;
}

} // namespace f4::world_convert

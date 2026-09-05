// f4-world-convert/src/objective_encoder.cpp
//
// .obj encoder — the inverse of objective_decoder.cpp's decode_obj().
// Field order, widths, and the LZSS-compression step mirror decode_obj
// exactly (that file is the ground truth; this one reproduces its byte
// sequence). See objective_encoder.hpp for the byte-identity scope note.

#include <f4/world_convert/objective_encoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/lzss/lzss.hpp>
#include "byte_writer.hpp"

#include <stdexcept>

namespace f4::world_convert {

namespace {

// Constants — must match objective_decoder.cpp exactly.
constexpr int OBJ_HEADER_BYTES = 10;        // i16 count + i32 uncomp + i32 comp
constexpr int LINK_COSTS_PER_LINK = 8;      // MoveType enum count
constexpr int NUM_RADAR_ARCS = 8;

using Writer = f4::world_convert::ByteWriter;

// Inverse of the per-record parse in decode_obj (CampBaseClass::Save +
// ObjectiveClass::Save). Writes every field the decoder reads, in order.
void encode_objective_record(Writer& w, const ObjectiveRecord& o,
                              int camp_version) {
    const bool has_z = camp_version >= 70;

    // The [short] sentinel before each record — SaveBaseObjectives writes
    // o->Type() which is the entity_type. The decoder stores this in a
    // local `sentinel` and validates it's nonzero; o.type is set from
    // entity_type. We write entity_type as the sentinel (they're the same
    // value in a clean file).
    w.i16(static_cast<int16_t>(o.entity_type));

    // --- CampBaseClass::Save ---
    w.u32(o.id_num);            // VU_ID: num first, then creator
    w.u32(o.id_creator);
    w.u16(o.entity_type);
    w.i16(o.x);
    w.i16(o.y);
    if (has_z) w.f32(o.z);
    w.i32(o.spot_time);
    w.i16(o.spotted);
    w.i16(o.base_flags);
    w.u8(o.owner);
    w.i16(o.camp_id);

    // --- ObjectiveClass::Save ---
    w.i32(o.last_repair);
    w.u32(o.obj_flags);
    w.u8(o.supply);
    w.u8(o.fuel);
    w.u8(o.losses);
    // Per-feature damage bitmap: 1-byte length prefix + N bytes.
    w.u8(static_cast<uint8_t>(o.fstatus.size()));
    w.bytes(o.fstatus.data(), o.fstatus.size());
    w.u8(o.priority);
    w.i16(o.nameid);
    // Parent VU_ID.
    w.u32(o.parent_id_num);
    w.u32(o.parent_id_creator);
    w.u8(o.first_owner);
    w.u8(o.links);
    // Link data (road/rail network): links × { costs[8] + VU_ID(8) } = 16 bytes.
    for (uint8_t li = 0; li < o.links && li < o.link_data.size(); ++li) {
        const auto& link = o.link_data[li];
        for (int j = 0; j < LINK_COSTS_PER_LINK; ++j) {
            w.u8(link.costs[j]);
        }
        w.u32(link.neighbor_num);
        w.u32(link.neighbor_creator);
    }
    // Optional RadarRangeClass.
    w.u8(o.has_radar ? 1 : 0);
    if (o.has_radar) {
        for (int j = 0; j < NUM_RADAR_ARCS; ++j) {
            w.f32(o.detect_ratio[j]);
        }
    }
}

} // namespace

std::vector<uint8_t> encode_obj_payload(const DecodedObjectives& dec, int camp_version) {
    Writer w;
    for (const auto& o : dec.objectives) {
        encode_objective_record(w, o, camp_version);
    }
    return w.buf;
}

std::vector<uint8_t> encode_obj(const DecodedObjectives& dec, int camp_version) {
    // Build the decompressed record buffer.
    auto payload = encode_obj_payload(dec, camp_version);

    // LZSS-compress it.
    auto compressed = f4::lzss::compress(payload.data(), payload.size());

    // Assemble the 10-byte header + compressed payload.
    std::vector<uint8_t> out;
    out.reserve(OBJ_HEADER_BYTES + compressed.size());
    const int16_t count = static_cast<int16_t>(dec.objectives.size());
    const int32_t uncompressed = static_cast<int32_t>(payload.size());
    const int32_t compressed_sz = static_cast<int32_t>(compressed.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&count),
               reinterpret_cast<const uint8_t*>(&count) + 2);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&uncompressed),
               reinterpret_cast<const uint8_t*>(&uncompressed) + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&compressed_sz),
               reinterpret_cast<const uint8_t*>(&compressed_sz) + 4);
    out.insert(out.end(), compressed.begin(), compressed.end());

    return out;
}

} // namespace f4::world_convert

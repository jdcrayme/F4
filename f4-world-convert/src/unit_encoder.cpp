// f4-world-convert/src/unit_encoder.cpp
//
// .uni encoder — the inverse of unit_decoder.cpp's decode_uni().
// Field order, widths, version gates, and padding mirror decode_uni exactly
// (that file is the ground truth; this one reproduces its byte sequence).
// See unit_encoder.hpp for the byte-identity scope note.
//
// The encoder writes each record as: [i16 type] + CampBaseClass +
// UnitClass fixed + wp_count + waypoints + subclass tail. The subclass
// tail is chosen by UnitRecord::unit_class; the Package branch by
// subclass.package_branch (or the U_FINAL heuristic if None).

#include <f4/world_convert/unit_encoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/lzss/lzss.hpp>
#include "byte_writer.hpp"

#include <stdexcept>

namespace f4::world_convert {

namespace {

// Constants — must match unit_decoder.cpp exactly.
constexpr uint32_t U_FINAL = 0x100000;
constexpr int PILOTS_PER_SQUADRON = 48;
constexpr int SQUADRON_RATING_COUNT = 16;   // ARO_OTHER

using Writer = f4::world_convert::ByteWriter;

// Inverse of parse_camp_base (campbase.cpp:229, 25 bytes at v63 / 29 at v70+).
void encode_camp_base(Writer& w, const UnitRecord& u, int v) {
    w.u32(u.id_num);
    w.u32(u.id_creator);
    w.u16(u.entity_type);
    w.i16(u.x);
    w.i16(u.y);
    if (v >= 70) w.f32(u.z);
    w.i32(u.spot_time);
    w.i16(u.spotted);
    w.i16(u.base_flags);
    w.u8(u.owner);
    w.i16(u.camp_id);
}

// Inverse of parse_unit_class_fixed (unit.cpp:465, 40 bytes at v63 / 41 at v71+).
void encode_unit_class_fixed(Writer& w, const UnitRecord& u, int v) {
    w.i32(u.last_check);
    w.u32(u.roster);
    w.u32(u.unit_flags);
    w.i16(u.dest_x);
    w.i16(u.dest_y);
    w.u32(u.target_id_num);
    w.u32(u.target_id_creator);
    w.u32(u.cargo_id_num);
    w.u32(u.cargo_id_creator);
    w.u8(u.moved);
    w.u8(u.losses);
    w.u8(u.tactic);
    if (v >= 71) w.u16(u.current_wp);
    else         w.u8(static_cast<uint8_t>(u.current_wp));
    w.i16(u.name_id);
    w.i16(u.reinforcement);
}

// Inverse of parse_waypoint (campwp.cpp:220, 16..29 bytes).
void encode_waypoint(Writer& w, const WaypointRecord& wp, int v) {
    w.u8(wp.haves);
    w.i16(wp.grid_x);
    w.i16(wp.grid_y);
    w.i16(wp.grid_z);
    w.i32(wp.arrive);
    w.u8(wp.action);
    w.u8(wp.route_action);
    w.u8(wp.formation);
    if (v < 72) w.i16(wp.flags);
    else        w.u32(static_cast<uint32_t>(wp.flags));
    if (wp.haves & 0x02) {
        w.u32(wp.target_id_num);
        w.u32(wp.target_id_creator);
        w.u8(wp.target_building);
    }
    if (wp.haves & 0x01) {
        w.i32(wp.depart);
    }
}

// Inverse of parse_waypoints (1-byte count at v63, ushort at v71+).
void encode_waypoints(Writer& w, const UnitRecord& u, int v) {
    if (v >= 71) w.u16(u.wp_count);
    else         w.u8(static_cast<uint8_t>(u.wp_count));
    for (const auto& wp : u.waypoints) {
        encode_waypoint(w, wp, v);
    }
}

// Inverse of parse_ground_unit (gndunit.cpp:152, 11 bytes).
void encode_ground_unit(Writer& w, const UnitSubclassData& s) {
    w.u8(s.orders);
    w.i16(s.division);
    w.u32(s.aobj_num);
    w.u32(s.aobj_creator);
}

// Inverse of parse_battalion (battalio.cpp:386, 30 bytes).
void encode_battalion(Writer& w, const UnitSubclassData& s) {
    w.i32(s.last_move);
    w.i32(s.last_combat);
    w.u32(s.parent_id_num);
    w.u32(s.parent_id_creator);
    w.u32(s.last_obj_num);
    w.u32(s.last_obj_creator);
    w.u8(s.supply);
    w.u8(s.fatigue);
    w.u8(s.morale);
    w.u8(s.heading);
    w.u8(s.final_heading);
    w.u8(s.position);
}

// Inverse of parse_brigade (brigade.cpp:192, 1 + 8*elements bytes).
void encode_brigade(Writer& w, const UnitSubclassData& s) {
    w.u8(s.elements);
    for (uint8_t i = 0; i < s.elements; ++i) {
        // element_ids is a flattened (num, creator) pair list.
        const std::size_t base = static_cast<std::size_t>(i) * 2;
        if (base + 1 < s.element_ids.size()) {
            w.u32(s.element_ids[base]);      // num
            w.u32(s.element_ids[base + 1]);  // creator
        } else {
            w.u32(0); w.u32(0);
        }
    }
}

// Inverse of parse_squadron (squadron.cpp:316). The decoder skips stores[],
// schedule[], and rating[]; the encoder writes them as zero (struct-faithful
// — the decoder reads 0 on re-decode, matching the default struct).
void encode_squadron(Writer& w, const UnitSubclassData& s, int v) {
    w.i32(s.fuel);
    w.u8(s.specialty);
    // stores[] — weapon stockpile. Size depends on version.
    std::size_t stores_bytes = 200;
    if (v >= 69 && v < 72) stores_bytes = 220;
    else if (v >= 72)      stores_bytes = 600;
    for (std::size_t i = 0; i < stores_bytes; ++i) w.u8(0);
    // pilot_data: 48 pilots × 10 bytes.
    for (int i = 0; i < PILOTS_PER_SQUADRON; ++i) {
        PilotRecord p;
        if (i < static_cast<int>(s.pilots.size())) p = s.pilots[i];
        w.i16(p.pilot_id);
        // skill_rating: low nibble = skill, high nibble = rating.
        w.u8(static_cast<uint8_t>((p.skill & 0x0F) | ((p.rating & 0x0F) << 4)));
        w.u8(p.status);
        w.u8(p.aa_kills);
        w.u8(p.ag_kills);
        w.u8(p.as_kills);
        w.u8(p.an_kills);
        w.i16(p.missions_flown);
    }
    // schedule[16 × 4 = 64 bytes] — not exposed, zeroed.
    for (int i = 0; i < 64; ++i) w.u8(0);
    // airbase VU_ID + hot_spot VU_ID.
    w.u32(s.airbase_id_num);
    w.u32(s.airbase_id_creator);
    w.u32(s.hot_spot_num);
    w.u32(s.hot_spot_creator);
    // rating[ARO_OTHER = 16] — not exposed, zeroed.
    for (int i = 0; i < SQUADRON_RATING_COUNT; ++i) w.u8(0);
    w.i16(s.aa_kills);
    w.i16(s.ag_kills);
    w.i16(s.as_kills);
    w.i16(s.an_kills);
    w.i16(s.missions_flown);
    w.i16(s.mission_score);
    w.u8(s.total_losses);
    w.u8(s.pilot_losses);     // v >= 9
    w.u8(s.squadron_patch);   // v >= 45
}

// Inverse of parse_taskforce (navunit.cpp:157, 2 bytes).
void encode_taskforce(Writer& w, const UnitSubclassData& s) {
    w.u8(s.orders);
    w.u8(s.supply);
}

// Inverse of parse_flight (flight.cpp:518). The decoder skips duplicate
// loadout entries and several timing slots; the encoder writes them as zero.
void encode_flight(Writer& w, const UnitSubclassData& s, int v) {
    w.f32(s.altitude);                  // pos_.z_
    w.i32(s.fuel_burnt);
    w.i32(0);                           // last_move — skipped by decoder
    w.i32(0);                           // last_combat — skipped
    w.i32(s.time_on_target);
    w.i32(s.mission_over_time);
    w.i16(s.mission_target);
    w.u8(s.loadouts);
    const std::size_t loadout_bytes = (v <= 72) ? 32 : 48;
    for (uint8_t li = 0; li < s.loadouts; ++li) {
        if (li == 0) {
            // Entry 0: reconstruct the 16-station struct from loadout_stations.
            // The struct is two parallel arrays: WeaponID[16] + WeaponCount[16].
            uint16_t ids[16] = {0};
            uint16_t cnts[16] = {0};
            for (const auto& st : s.loadout_stations) {
                // loadout_stations only carries non-zero weapon_ids; the
                // station index is implied by order. We write them in order
                // into the first N slots. (The decoder reconstructs the same
                // way — non-zero ids only — so the round-trip is struct-
                // faithful even though the original station indices are lost.)
                for (int slot = 0; slot < 16; ++slot) {
                    if (ids[slot] == 0) {
                        ids[slot] = st.weapon_id;
                        cnts[slot] = st.count;
                        break;
                    }
                }
            }
            for (int st = 0; st < 16; ++st) {
                if (v <= 72) w.u8(static_cast<uint8_t>(ids[st]));
                else         w.u16(ids[st]);
            }
            for (int st = 0; st < 16; ++st) {
                if (v <= 72) w.u8(static_cast<uint8_t>(cnts[st]));
                else         w.u16(cnts[st]);
            }
        } else {
            // Duplicate entries — skipped by decoder, zeroed here.
            for (std::size_t b = 0; b < loadout_bytes; ++b) w.u8(0);
        }
    }
    w.u8(s.mission);
    if (v > 65) w.u8(s.old_mission);
    w.u8(0);                            // last_direction — skipped
    w.u8(s.priority);
    w.u8(s.mission_id);
    w.u8(s.eval_flags);
    if (v > 65) w.u8(s.mission_context);
    w.u32(s.package_num);
    w.u32(s.package_creator);
    w.u32(s.squadron_num);
    w.u32(s.squadron_creator);
    if (v > 65) {
        w.u32(s.requester_num);
        w.u32(s.requester_creator);
    }
    // slots[4] + pilots[4] + plane_stats[4] + player_slots[4] — skipped, zeroed.
    for (int i = 0; i < 16; ++i) w.u8(0);
    w.u8(0);                            // last_player_slot — skipped
    w.u8(s.callsign_id);
    w.u8(s.callsign_num);
    if (v >= 72) {
        w.u32(0);                       // refuel — skipped
    }
}

// Inverse of parse_package_common (package.cpp:507).
void encode_package_common(Writer& w, const UnitSubclassData& s) {
    w.u8(s.elements);
    for (uint8_t i = 0; i < s.elements; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * 2;
        if (base + 1 < s.element_ids.size()) {
            w.u32(s.element_ids[base]);
            w.u32(s.element_ids[base + 1]);
        } else {
            w.u32(0); w.u32(0);
        }
    }
    w.u32(s.interceptor_num);  w.u32(s.interceptor_creator);
    w.u32(s.awacs_num);        w.u32(s.awacs_creator);
    w.u32(s.jstar_num);        w.u32(s.jstar_creator);
    w.u32(s.ecm_num);          w.u32(s.ecm_creator);
    w.u32(s.tanker_num);       w.u32(s.tanker_creator);
    w.u8(s.wait_cycles);
}

// Inverse of parse_package_small (package.cpp:434 small branch).
void encode_package_small(Writer& w, const UnitSubclassData& s, int v) {
    w.i16(s.requests);
    w.i16(s.responses);
    // mis_request.mission and .context are streamed as sizeof(short) —
    // the high byte is always 0 (uchar values).
    w.u16(static_cast<uint16_t>(s.mis_request.mission));
    w.u16(static_cast<uint16_t>(s.mis_request.context));
    w.u32(s.mis_request.requester_id_num);
    w.u32(s.mis_request.requester_id_creator);
    w.u32(s.mis_request.target_id_num);
    w.u32(s.mis_request.target_id_creator);
    if (v >= 26) w.i32(s.mis_request.tot);
    if (v >= 35) w.u8(s.mis_request.action_type);
    if (v >= 41) w.i16(s.mis_request.priority);
}

// Inverse of parse_package_big (package.cpp:434 big branch).
void encode_package_big(Writer& w, const UnitSubclassData& s, int v) {
    w.u8(s.flights);
    w.i16(s.wait_for);
    w.i16(s.iax); w.i16(s.iay);
    w.i16(s.eax); w.i16(s.eay);
    w.i16(s.bpx); w.i16(s.bpy);
    w.i16(s.tpx); w.i16(s.tpy);
    w.i32(s.takeoff);
    w.i32(s.tp_time);
    w.u32(s.package_flags);
    w.i16(s.caps);
    w.i16(s.requests);
    if (v < 35) {
        w.u8(0); w.u8(0);   // threat_stats — v < 35 only, skipped by decoder
    }
    w.i16(s.responses);
    // Two waypoint routes (ingress, egress), each 1-byte count-prefixed.
    for (int route = 0; route < 2; ++route) {
        const auto& route_wps = (route == 0) ? s.ingress : s.egress;
        w.u8(static_cast<uint8_t>(route_wps.size()));
        for (const auto& wp : route_wps) {
            encode_waypoint(w, wp, v);
        }
    }
    // MissionRequestClass bulk (76 bytes at v>=35, 64 at v<35).
    const MissionRequestRecord& m = s.mis_request;
    if (v < 35) {
        // Legacy 64-byte struct: write IDs + 48 zero bytes.
        w.u32(m.requester_id_num);  w.u32(m.requester_id_creator);
        w.u32(m.target_id_num);    w.u32(m.target_id_creator);
        for (int i = 0; i < 64 - 16; ++i) w.u8(0);
        return;
    }
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
    w.u8(0); w.u8(0); w.u8(0);     // trailing padding → 76
}

// Encode the subclass tail for a unit, dispatching on unit_class.
void encode_subclass_tail(Writer& w, const UnitRecord& u, int v) {
    const auto& s = u.subclass;
    switch (u.unit_class) {
        case UnitClass::Battalion:
            encode_ground_unit(w, s);
            encode_battalion(w, s);
            break;
        case UnitClass::Brigade:
            encode_ground_unit(w, s);
            encode_brigade(w, s);
            break;
        case UnitClass::Squadron:
            encode_squadron(w, s, v);
            break;
        case UnitClass::TaskForce:
            encode_taskforce(w, s);
            break;
        case UnitClass::Flight:
            encode_flight(w, s, v);
            break;
        case UnitClass::Package: {
            encode_package_common(w, s);
            // Branch selection: use package_branch if set; else the heuristic.
            bool use_small = false;
            if (s.package_branch == PackageBranch::Small) use_small = true;
            else if (s.package_branch == PackageBranch::Big) use_small = false;
            else use_small = (u.unit_flags & U_FINAL) && s.wait_cycles == 0;
            if (use_small) encode_package_small(w, s, v);
            else           encode_package_big(w, s, v);
            break;
        }
        case UnitClass::Unknown:
            // No tail — the decoder stopped here on an unclassifiable record.
            break;
    }
}

} // namespace

std::vector<uint8_t> encode_uni_payload(const DecodedUnits& dec, int camp_version) {
    Writer w;
    for (const auto& u : dec.units) {
        w.i16(u.type);
        encode_camp_base(w, u, camp_version);
        encode_unit_class_fixed(w, u, camp_version);
        encode_waypoints(w, u, camp_version);
        encode_subclass_tail(w, u, camp_version);
    }
    return w.buf;
}

std::vector<uint8_t> encode_uni(const DecodedUnits& dec, int camp_version) {
    auto payload = encode_uni_payload(dec, camp_version);
    auto compressed = f4::lzss::compress(payload.data(), payload.size());

    std::vector<uint8_t> out;
    out.reserve(10 + compressed.size());
    const int32_t outer = static_cast<int32_t>(10 + compressed.size());
    const int16_t count = static_cast<int16_t>(dec.units.size());
    const int32_t inner = static_cast<int32_t>(payload.size());
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&outer),
               reinterpret_cast<const uint8_t*>(&outer) + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&count),
               reinterpret_cast<const uint8_t*>(&count) + 2);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&inner),
               reinterpret_cast<const uint8_t*>(&inner) + 4);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

} // namespace f4::world_convert

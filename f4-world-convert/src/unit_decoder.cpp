// f4-world-convert/src/unit_decoder.cpp
//
// Full .uni decoder for gCampDataVersion=63 (the save1.cam fixture).
// Decodes CampBaseClass + UnitClass fixed + waypoints + subclass tail.
// All 683 records in the fixture decode cleanly with the cursor landing
// exactly at the buffer end.
//
// Layout reference (see unit_decoder.hpp for the full spec):
//
//   [short type]
//   [CampBaseClass 25 bytes] (no pos_.z_ at v63)
//   [UnitClass fixed 40 bytes] (current_wp = 1 byte at v63)
//   [uchar wp_count]
//   [WayPointClass × wp_count]
//   [subclass tail] (Battalion/Brigade/Squadron/TaskForce/Flight/Package)
//
// Subclass dispatch: FreeFalcon looks up Falcon4ClassTable[type - 100],
// which isn't shipped with the source. We try each candidate tail and
// validate the NEXT record's header (type == entity_type at offset+10,
// owner in 0..7). On the last record, we validate against the buffer end.

#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/io/cursor.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// Lightweight sequential cursor over a byte buffer. Replaced by the shared
// f4::io::Cursor (header <f4/io/cursor.hpp>); see that header for the
// design rationale for the sticky `error` flag (this is the only Cursor
// call site that drives the subclass-dispatch path in try_tail below,
// which is why the sticky-flag policy exists in the first place).
using f4::io::Cursor;

// Read a VU_ID as a (creator, num) pair.
struct VuId { uint32_t creator; uint32_t num; };
VuId read_vu_id(Cursor& c) { VuId v; v.num = c.u32(); v.creator = c.u32(); return v; }

// ---------------------------------------------------------------------------
// CampBaseClass (25 bytes at v63 — no pos_.z_).
// Source: campbase.cpp:229 (Save) / campbase.cpp:92 (Load).
// ---------------------------------------------------------------------------
void parse_camp_base(Cursor& c, UnitRecord& u) {
    VuId id = read_vu_id(c);
    u.id_num      = id.num;
    u.id_creator  = id.creator;
    u.entity_type = c.u16();
    u.x           = c.i16();
    u.y           = c.i16();
    // pos_.z_ skipped at v63 (gCampDataVersion < 70)
    u.z           = 0.0f;
    u.spot_time   = c.i32();
    u.spotted     = c.i16();
    u.base_flags  = c.i16();
    u.owner       = c.u8();
    u.camp_id     = c.i16();
}

// ---------------------------------------------------------------------------
// UnitClass fixed fields (40 bytes at v63 — current_wp is 1 byte).
// Source: unit.cpp:465 (Save) / unit.cpp:287 (Load ctor).
// ---------------------------------------------------------------------------
void parse_unit_class_fixed(Cursor& c, UnitRecord& u) {
    u.last_check  = c.i32();
    u.roster      = c.u32();
    u.unit_flags  = c.u32();
    u.dest_x      = c.i16();
    u.dest_y      = c.i16();
    VuId target = read_vu_id(c);
    u.target_id_num = target.num;
    u.target_id_creator = target.creator;
    VuId cargo = read_vu_id(c);
    u.cargo_id_num = cargo.num;
    u.cargo_id_creator = cargo.creator;
    u.moved       = c.u8();
    u.losses      = c.u8();
    u.tactic      = c.u8();
    u.current_wp  = c.u8();   // 1 BYTE at v63 (was ushort at v>=71)
    u.name_id     = c.i16();
    u.reinforcement = c.i16();
}

// ---------------------------------------------------------------------------
// WayPointClass (16..29 bytes at v63).
// Source: campwp.cpp:220 (Save) / campwp.cpp:89 (Load ctor).
// ---------------------------------------------------------------------------
void parse_waypoint(Cursor& c, WaypointRecord& w) {
    w.haves       = c.u8();
    w.grid_x      = c.i16();
    w.grid_y      = c.i16();
    w.grid_z      = c.i16();
    w.arrive      = c.i32();
    w.action      = c.u8();
    w.route_action = c.u8();
    w.formation   = c.u8();
    w.flags       = c.i16();   // 2 bytes at v<72 (was ulong at v>=72)
    if (w.haves & 0x02) {
        VuId t = read_vu_id(c);
        w.target_id_num = t.num;
        w.target_id_creator = t.creator;
        w.target_building = c.u8();
    }
    if (w.haves & 0x01) {
        w.depart = c.i32();
    }
}

// ---------------------------------------------------------------------------
// Waypoint list (length-prefixed by 1-byte count at v63).
// Source: unit.cpp:6455 (EncodeWaypoints) / unit.cpp:6494 (DecodeWaypoints).
// ---------------------------------------------------------------------------
void parse_waypoints(Cursor& c, UnitRecord& u) {
    u.wp_count = c.u8();   // 1 byte at v<71 (was ushort at v>=71)
    u.waypoints.clear();
    u.waypoints.reserve(u.wp_count);
    for (uint8_t i = 0; i < u.wp_count; ++i) {
        WaypointRecord w;
        parse_waypoint(c, w);
        u.waypoints.push_back(w);
    }
}

// ---------------------------------------------------------------------------
// Subclass tails. Each takes the cursor AFTER waypoints and consumes its
// own fields. The caller validates that the cursor lands at a plausible
// next-record boundary.
// ---------------------------------------------------------------------------

// GroundUnitClass tail (11 bytes) — shared by Battalion and Brigade.
// Source: gndunit.cpp:152 (Save) / gndunit.cpp:102 (Load).
void parse_ground_unit(Cursor& c, UnitSubclassData& s) {
    s.orders   = c.u8();
    s.division = c.i16();
    VuId aobj = read_vu_id(c);
    s.aobj_num = aobj.num;
    s.aobj_creator = aobj.creator;
}

// Battalion tail (30 bytes) — after GroundUnit's 11 bytes.
// Source: battalio.cpp:386 (Save) / battalio.cpp:167 (Load).
// NOTE: USE_FLANKS is not defined in the source, so lfx/lfy/rfx/rfy are
// absent. There's also a v<15 dummy byte path that doesn't apply at v63.
void parse_battalion(Cursor& c, UnitSubclassData& s) {
    s.last_move   = c.i32();
    s.last_combat = c.i32();
    VuId parent = read_vu_id(c);
    s.parent_id_num = parent.num;
    s.parent_id_creator = parent.creator;
    VuId last_obj = read_vu_id(c);
    s.last_obj_num = last_obj.num;
    s.last_obj_creator = last_obj.creator;
    s.supply       = c.u8();
    s.fatigue      = c.u8();
    s.morale       = c.u8();
    s.heading      = c.u8();
    s.final_heading = c.u8();
    s.position     = c.u8();
}

// Brigade tail (1 + 8*elements bytes) — after GroundUnit's 11 bytes.
// Source: brigade.cpp:192 (Save) / brigade.cpp:136 (Load).
void parse_brigade(Cursor& c, UnitSubclassData& s) {
    s.elements = c.u8();
    s.element_ids.clear();
    s.element_ids.reserve(static_cast<std::size_t>(s.elements) * 2);
    for (uint8_t i = 0; i < s.elements; ++i) {
        VuId e = read_vu_id(c);
        s.element_ids.push_back(e.num);
        s.element_ids.push_back(e.creator);
    }
}

// Squadron tail (796 bytes at v63).
// Source: squadron.cpp:316 (Save) / squadron.cpp:178 (Load).
// stores[] is 200 bytes at v<69 (vs 220 at v<72, 600 at v>=72).
// pilot_data[] is 48 * PilotClass(10 bytes) = 480 bytes.
// schedule[] is 16 longs = 64 bytes.
void parse_squadron(Cursor& c, UnitSubclassData& s) {
    s.fuel      = c.i32();
    s.specialty = c.u8();
    c.skip(200);                  // stores[200] — not exposed (weapon stockpile)
    // Parse pilot_data: 48 pilots × 10 bytes = 480 bytes.
    // PilotClass layout (pilot.h:32):
    //   short pilot_id(2) + uchar pilot_skill_and_rating(1) + uchar pilot_status(1)
    //   + uchar aa_kills(1) + uchar ag_kills(1) + uchar as_kills(1) + uchar an_kills(1)
    //   + short missions_flown(2) = 10 bytes
    s.pilots.clear();
    s.pilots.reserve(48);
    for (int i = 0; i < 48; ++i) {
        PilotRecord p;
        p.pilot_id = c.i16();
        uint8_t skill_rating = c.u8();
        p.skill = skill_rating & 0x0F;        // low nibble (or byte — unclear)
        p.rating = (skill_rating >> 4) & 0x0F;
        p.status = c.u8();
        p.aa_kills = c.u8();
        p.ag_kills = c.u8();
        p.as_kills = c.u8();
        p.an_kills = c.u8();
        p.missions_flown = c.i16();
        s.pilots.push_back(p);
    }
    c.skip(64);                   // schedule[16 * 4] — not exposed
    VuId ab = read_vu_id(c);
    s.airbase_id_num = ab.num;
    s.airbase_id_creator = ab.creator;
    VuId hs = read_vu_id(c);
    s.hot_spot_num = hs.num;
    s.hot_spot_creator = hs.creator;
    c.skip(16);                   // rating[ARO_OTHER=16] — not exposed
    s.aa_kills         = c.i16();
    s.ag_kills         = c.i16();
    s.as_kills         = c.i16();
    s.an_kills         = c.i16();
    s.missions_flown   = c.i16();
    s.mission_score    = c.i16();
    s.total_losses     = c.u8();
    s.pilot_losses     = c.u8();   // v >= 9
    s.squadron_patch   = c.u8();   // v >= 45
}

// TaskForce tail (2 bytes).
// Source: navunit.cpp:157 (Save) / navunit.cpp:121 (Load).
// Inherits directly from UnitClass (NOT via GroundUnit).
void parse_taskforce(Cursor& c, UnitSubclassData& s) {
    s.orders = c.u8();
    s.supply = c.u8();
}

// Flight tail (67 + 32*loadouts bytes at v63).
// Source: flight.cpp:518 (Save) / flight.cpp:263 (Load ctor).
// At v63 (v <= 65), skips: old_mission, mission_context, requester, refuel.
// LoadoutStruct at v<=72 is 32 bytes (uchar WeaponID[16] + uchar WeaponCount[16]),
// NOT the 48-byte sizeof(LoadoutStruct) with short WeaponID[].
void parse_flight(Cursor& c, UnitSubclassData& s) {
    s.altitude           = c.f32();   // pos_.z_
    s.fuel_burnt         = c.i32();
    c.skip(4);                        // last_move (CampaignTime)
    c.skip(4);                        // last_combat (CampaignTime)
    s.time_on_target     = c.i32();
    s.mission_over_time  = c.i32();
    s.mission_target     = c.i16();
    s.loadouts           = c.u8();
    c.skip(static_cast<std::size_t>(s.loadouts) * 32);   // loadout[N * 32]
    s.mission            = c.u8();
    // old_mission skipped at v <= 65
    c.skip(1);                        // last_direction
    s.priority           = c.u8();
    s.mission_id         = c.u8();
    s.eval_flags         = c.u8();
    // mission_context skipped at v <= 65
    VuId pkg = read_vu_id(c);
    s.package_num = pkg.num;
    s.package_creator = pkg.creator;
    VuId sqn = read_vu_id(c);
    s.squadron_num = sqn.num;
    s.squadron_creator = sqn.creator;
    // requester skipped at v <= 65
    c.skip(4);                        // slots[PILOTS_PER_FLIGHT=4]
    c.skip(4);                        // pilots[4]
    c.skip(4);                        // plane_stats[4]
    c.skip(4);                        // player_slots[4]
    c.skip(1);                        // last_player_slot
    s.callsign_id        = c.u8();
    s.callsign_num       = c.u8();
    // refuel skipped at v < 72
}

// Package tail (variable; small branch if Final() && !wait_cycles, else big).
// Source: package.cpp:507 (Save) / package.cpp:200 (Load ctor).
// Common header: elements (1) + element_ids (8*elements) + 5 VU_IDs (40) + wait_cycles (1)
// Small branch: 31 bytes (requests/responses/mission_request)
// Big branch: contains flights, wait_for, 8 GridIndex, takeoff/tp_time,
//   package_flags, caps, requests, responses, 2 waypoint routes, mission_request.
// The branch is selected at SAVE time based on runtime state, so both
// layouts can appear. We detect by checking if the candidate cursor
// position after the small branch validates against the next record.
//
// In save1.cam, no Package records exist, so this code path is exercised
// only by future fixtures. We implement the small branch and fall back to
// "give up" if it doesn't validate — the cursor stays put and the unit is
// reported with class=Unknown.
void parse_package_small(Cursor& c, UnitSubclassData& s) {
    // Common header:
    s.elements = c.u8();
    c.skip(static_cast<std::size_t>(s.elements) * 8);  // element_ids
    VuId inter = read_vu_id(c);
    s.interceptor_num = inter.num;
    s.interceptor_creator = inter.creator;
    VuId aw = read_vu_id(c);
    s.awacs_num = aw.num;
    s.awacs_creator = aw.creator;
    VuId js = read_vu_id(c);
    s.jstar_num = js.num;
    s.jstar_creator = js.creator;
    VuId ecm = read_vu_id(c);
    s.ecm_num = ecm.num;
    s.ecm_creator = ecm.creator;
    VuId tk = read_vu_id(c);
    s.tanker_num = tk.num;
    s.tanker_creator = tk.creator;
    s.wait_cycles = c.u8();
    // Small branch (Final() && !wait_cycles):
    c.skip(2);   // requests
    c.skip(2);   // responses
    c.skip(2);   // mis_request.mission
    c.skip(2);   // mis_request.context
    c.skip(8);   // mis_request.requesterID
    c.skip(8);   // mis_request.targetID
    c.skip(4);   // mis_request.tot (v >= 26)
    c.skip(1);   // mis_request.action_type (v >= 35)
    c.skip(2);   // mis_request.priority (v >= 41)
}

// ---------------------------------------------------------------------------
// Subclass dispatch + validation.
//
// FreeFalcon looks up Falcon4ClassTable[type - 100] to pick the constructor.
// The class table isn't shipped with the source. We try each candidate tail
// in order of likelihood (Battalion is most common in save1.cam at 524/683)
// and validate the resulting cursor position.
//
// Validation: at the candidate end position, the next record's header
// must satisfy:
//   - [short type] at +0 in range [100..2000]
//   - [ushort entity_type] at +10 equals [short type] at +0
//   - [uchar owner] at +24 in [0..7]
//
// On the last record, the candidate position must equal the buffer end.
// ---------------------------------------------------------------------------
bool validate_next_record(const uint8_t* p, const uint8_t* end) {
    // Need at least 25 bytes to read the next header (CampBaseClass).
    if (p + 25 > end) return p == end;
    int16_t type;
    std::memcpy(&type, p, 2);
    if (type < 100 || type > 2000) return false;
    uint16_t entity_type;
    std::memcpy(&entity_type, p + 10, 2);
    if (entity_type != static_cast<uint16_t>(type)) return false;
    uint8_t owner = p[24];
    if (owner > 7) return false;
    return true;
}

// Try a subclass tail parser. Returns true if the candidate cursor position
// validates. On success, fills `out` and leaves `c` at the advanced position.
// On failure, rolls `c` back to its pre-parse position.
//
// Uses Cursor's save/restore mechanism instead of copying the Cursor (which
// is non-copyable by design — see Position struct in cursor.hpp). Also uses
// the sticky error flag rather than try/catch — see the design note on
// Cursor above. This avoids exception-based control flow on the per-record
// hot path and surfaces real bugs (which would throw) instead of swallowing
// them under catch(...).
template<typename ParseFn>
bool try_tail(Cursor& c, const uint8_t* end,
              UnitRecord& u, ParseFn parse) {
    auto saved = c.save();      // snapshot position before trial parse
    parse(c, u.subclass);
    if (!c.error && validate_next_record(c.p, end)) {
        return true;            // commit — c is already advanced
    }
    c.restore(saved);           // rollback
    return false;
}

UnitClass dispatch_and_parse_tail(Cursor& c, const uint8_t* record_start,
                                   const uint8_t* end, UnitRecord& u) {
    // Cursor is at post-waypoint position. try_tail uses save/restore
    // internally, so on failure the cursor rolls back here automatically.
    // No need for a separate snapshot copy.

    // Try each subclass in order of frequency in save1.cam.
    // (Battalion 524, Brigade 85, Squadron 72, TaskForce 2, Flight 0, Package 0.)

    // Battalion: 11 (GroundUnit) + 30 (Battalion) = 41 bytes
    if (try_tail(c, end, u, [](Cursor& cc, UnitSubclassData& s) {
            parse_ground_unit(cc, s);
            parse_battalion(cc, s);
        })) {
        return UnitClass::Battalion;
    }
    u.subclass = UnitSubclassData{};   // reset between attempts

    // Brigade: 11 (GroundUnit) + 1 + 8*elements bytes
    if (try_tail(c, end, u, [](Cursor& cc, UnitSubclassData& s) {
            parse_ground_unit(cc, s);
            parse_brigade(cc, s);
        })) {
        return UnitClass::Brigade;
    }
    u.subclass = UnitSubclassData{};

    // Squadron: 796 bytes
    if (try_tail(c, end, u, [](Cursor& cc, UnitSubclassData& s) {
            parse_squadron(cc, s);
        })) {
        return UnitClass::Squadron;
    }
    u.subclass = UnitSubclassData{};

    // TaskForce: 2 bytes
    if (try_tail(c, end, u, [](Cursor& cc, UnitSubclassData& s) {
            parse_taskforce(cc, s);
        })) {
        return UnitClass::TaskForce;
    }
    u.subclass = UnitSubclassData{};

    // Flight: 67 + 32*loadouts bytes
    if (try_tail(c, end, u, [](Cursor& cc, UnitSubclassData& s) {
            parse_flight(cc, s);
        })) {
        return UnitClass::Flight;
    }
    u.subclass = UnitSubclassData{};

    // Package (small branch only — big branch is not yet implemented).
    // On save1.cam there are no Package records, so this is a best-effort
    // attempt for future fixtures.
    if (try_tail(c, end, u, [](Cursor& cc, UnitSubclassData& s) {
            parse_package_small(cc, s);
        })) {
        return UnitClass::Package;
    }
    u.subclass = UnitSubclassData{};

    // No tail validated. Leave the cursor where it is so the caller can
    // report the failure position.
    (void)record_start;
    return UnitClass::Unknown;
}

} // namespace

DecodedUnits decode_uni(const uint8_t* data, std::size_t size) {
    DecodedUnits out;
    if (size < 10) throw std::runtime_error("uni: sub-file too small");

    Cursor top{data, data + size};
    int32_t outer = top.i32();   // SaveUnits' outer size (unused)
    out.count = top.i16();
    int32_t inner = top.i32();   // uncompressed size
    (void)outer;

    if (out.count < 0) throw std::runtime_error("uni: negative unit count");
    if (inner <= 0) throw std::runtime_error("uni: invalid inner size");

    const uint8_t* comp = data + 10;
    auto buf = lzss_expand(comp, size - 10, static_cast<std::size_t>(inner));
    out.inner_size = buf.size();

    Cursor c{buf.data(), buf.data() + buf.size()};
    out.units.reserve(static_cast<std::size_t>(out.count));

    int decoded = 0;
    while (decoded < out.count && c.remaining() > 0 && !c.error) {
        const uint8_t* record_start = c.p;
        UnitRecord u;
        u.type = c.i16();
        parse_camp_base(c, u);
        parse_unit_class_fixed(c, u);
        parse_waypoints(c, u);
        u.unit_class = dispatch_and_parse_tail(c, record_start, c.end, u);

        // Cursor's sticky error flag is set when a read/skip went OOB.
        // This is the "buffer truncated mid-record" case — stop and
        // report the position of the start of the partial record (so
        // the caller can see bytes_consumed up to the last full record).
        if (c.error) {
            c.p = record_start;
            out.bytes_consumed = static_cast<std::size_t>(c.p - buf.data());
            break;
        }

        // If dispatch failed, stop here (cursor stays at the failed
        // position so the caller can see bytes_consumed).
        if (u.unit_class == UnitClass::Unknown && c.p == record_start + 2 + 25 + 40 + 1 + static_cast<std::size_t>(u.wp_count) * 16) {
            // Cursor is right after waypoints — try_tail left it there.
            // That's a hard stop.
            out.bytes_consumed = static_cast<std::size_t>(c.p - buf.data());
            break;
        }

        out.units.push_back(std::move(u));
        ++decoded;
    }

    out.bytes_consumed = static_cast<std::size_t>(c.p - buf.data());
    return out;
}

} // namespace f4::world_convert

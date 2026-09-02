// f4-world-convert/src/unit_decoder.cpp
//
// Full .uni decoder for gCampDataVersion 63 (the save1.cam fixture) and
// 71 (FreeFalcon 7.x campaign saves, e.g. TestCamp.cam). Intermediate
// versions 64-70 follow the same gates; 72+ layout rules are stubbed
// where they differ (stores[600], 48-byte loadouts, 32-bit waypoint
// flags, refuel) but no fixture exercises them yet.
//
// Layout reference (see unit_decoder.hpp for the full spec):
//
//   [short type]
//   [CampBaseClass 25 bytes @v63 / 29 @v70+ (pos_.z_ float)] (unit.cpp:92)
//   [UnitClass fixed 40 bytes @v63 / 41 @v71+ (current_wp ushort)] (unit.cpp:287)
//   [wp_count 1 byte @v63 / ushort @v71+]        (unit.cpp:6494 DecodeWaypoints)
//   [WayPointClass × wp_count]                   (campwp.cpp:89)
//   [subclass tail] (Battalion/Brigade/Squadron/TaskForce/Flight/Package)
//
// Subclass dispatch: with a class table (Falcon4.ct) we replicate
// FreeFalcon's NewUnit exactly — (domain, vu_type) from classInfo_ picks
// the constructor (unit.cpp:5890). Without one we try each candidate
// tail and validate the NEXT record's header. On the last record, we
// validate against the buffer end.
//
// Record-count parity: v63 fixture save1.cam decodes 683/683 records,
// v71 TestCamp.cam decodes 1715/1715 — both landing exactly at the
// buffer end (423,065 bytes for TestCamp's .uni payload).

#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/io/cursor.hpp>

#include <cstring>
#include <stdexcept>

namespace f4::world_convert {

namespace {

using f4::io::Cursor;

// U_FINAL (unit.h:65) — "Package elements finalized and sent, or flight
// contains actual a/c". PackageClass::Save writes the small branch iff
// Final() && !wait_cycles (package.cpp:434).
constexpr uint32_t U_FINAL = 0x100000;

// Read a VU_ID as a (num, creator) pair — vutypes.h: class VU_ID lays out
// num_ first, creator_ second.
struct VuId { uint32_t creator; uint32_t num; };
VuId read_vu_id(Cursor& c) { VuId v; v.num = c.u32(); v.creator = c.u32(); return v; }

// ---------------------------------------------------------------------------
// CampBaseClass (25 bytes at v63 — no pos_.z_; 29 at v70+).
// Source: campbase.cpp:229 (Save) / campbase.cpp:92 (Load).
// ---------------------------------------------------------------------------
void parse_camp_base(Cursor& c, UnitRecord& u, int v) {
    VuId id = read_vu_id(c);
    u.id_num      = id.num;
    u.id_creator  = id.creator;
    u.entity_type = c.u16();
    u.x           = c.i16();
    u.y           = c.i16();
    if (v >= 70) {
        u.z = c.f32();   // pos_.z_ — present at gCampDataVersion >= 70
    } else {
        u.z = 0.0f;
    }
    u.spot_time   = c.i32();
    u.spotted     = c.i16();
    u.base_flags  = c.i16();
    u.owner       = c.u8();
    u.camp_id     = c.i16();
}

// ---------------------------------------------------------------------------
// UnitClass fixed fields (40 bytes at v63; 41 at v71 — current_wp ushort).
// Source: unit.cpp:465 (Save) / unit.cpp:287 (Load ctor).
// ---------------------------------------------------------------------------
void parse_unit_class_fixed(Cursor& c, UnitRecord& u, int v) {
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
    if (v >= 71) {
        u.current_wp = c.u16();   // ushort at v>=71
    } else {
        u.current_wp = c.u8();    // 1 byte at v63
    }
    u.name_id     = c.i16();
    u.reinforcement = c.i16();
}

// ---------------------------------------------------------------------------
// WayPointClass (16..29 bytes; flags 2 bytes at v<72).
// Source: campwp.cpp:220 (Save) / campwp.cpp:89 (Load ctor).
// ---------------------------------------------------------------------------
void parse_waypoint(Cursor& c, WaypointRecord& w, int v) {
    w.haves       = c.u8();
    w.grid_x      = c.i16();
    w.grid_y      = c.i16();
    w.grid_z      = c.i16();
    w.arrive      = c.i32();
    w.action      = c.u8();
    w.route_action = c.u8();
    w.formation   = c.u8();
    if (v < 72) {
        w.flags   = c.i16();      // 2 bytes at v<72 (ulong at v>=72 — not exercised)
    } else {
        w.flags   = static_cast<int16_t>(c.u32());
    }
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

// Waypoint list (count 1 byte at v63, ushort at v71).
// Source: unit.cpp:6455 (EncodeWaypoints) / unit.cpp:6494 (DecodeWaypoints).
void parse_waypoints(Cursor& c, UnitRecord& u, int v) {
    uint16_t count;
    if (v >= 71) {
        count = c.u16();          // ushort at v>=71
    } else {
        count = c.u8();           // 1 byte at v<71
    }
    u.wp_count = count;
    u.waypoints.clear();
    u.waypoints.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        WaypointRecord w;
        parse_waypoint(c, w, v);
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
// absent. There's also a v<15 dummy byte path that doesn't apply at v63+.
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

// Squadron tail. 794 bytes at v63 (stores[200]); 814 at 69<=v<72
// (stores[220]); stores[600] at v>=72 (MAXIMUM_WEAPTYPES).
// Source: squadron.cpp:316 (Save) / squadron.cpp:178 (Load).
// pilot_data[] is 48 * PilotClass(10 bytes) = 480 bytes.
// schedule[] is 16 on-disk 32-bit longs = 64 bytes.
void parse_squadron(Cursor& c, UnitSubclassData& s, int v) {
    s.fuel      = c.i32();
    s.specialty = c.u8();
    std::size_t stores_bytes = 200;                 // v < 69
    if (v >= 69 && v < 72) stores_bytes = 220;      // 69 <= v < 72
    else if (v >= 72)      stores_bytes = 600;      // MAXIMUM_WEAPTYPES
    c.skip(stores_bytes);                           // stores[] — weapon stockpile
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
        p.skill = skill_rating & 0x0F;
        p.rating = (skill_rating >> 4) & 0x0F;
        p.status = c.u8();
        p.aa_kills = c.u8();
        p.ag_kills = c.u8();
        p.as_kills = c.u8();
        p.an_kills = c.u8();
        p.missions_flown = c.i16();
        s.pilots.push_back(p);
    }
    c.skip(64);                   // schedule[16 × 4] — not exposed
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

// Flight tail. 67 + 32*loadouts bytes at v63; +10 at v>65
// (old_mission 1, mission_context 1, requester VU_ID 8); +4 at v>=72
// (refuel uint). LoadoutStruct at v<=72 is 32 bytes (uchar WeaponID[16]
// + uchar WeaponCount[16]), 48 at v>=72 (short WeaponID[]).
// Source: flight.cpp:518 (Save) / flight.cpp:263 (Load ctor).
void parse_flight(Cursor& c, UnitSubclassData& s, int v) {
    s.altitude           = c.f32();   // pos_.z_
    s.fuel_burnt         = c.i32();
    c.skip(4);                        // last_move (CampaignTime)
    c.skip(4);                        // last_combat (CampaignTime)
    s.time_on_target     = c.i32();
    s.mission_over_time  = c.i32();
    s.mission_target     = c.i16();
    s.loadouts           = c.u8();
    const std::size_t loadout_bytes = (v <= 72) ? 32 : 48;
    c.skip(static_cast<std::size_t>(s.loadouts) * loadout_bytes);
    s.mission            = c.u8();
    if (v > 65) {
        s.old_mission    = c.u8();    // old_mission — written at v > 65
    }
    c.skip(1);                        // last_direction
    s.priority           = c.u8();
    s.mission_id         = c.u8();
    s.eval_flags         = c.u8();
    if (v > 65) {
        s.mission_context = c.u8();   // mission_context — written at v > 65
    }
    VuId pkg = read_vu_id(c);
    s.package_num = pkg.num;
    s.package_creator = pkg.creator;
    VuId sqn = read_vu_id(c);
    s.squadron_num = sqn.num;
    s.squadron_creator = sqn.creator;
    if (v > 65) {
        VuId req = read_vu_id(c);
        s.requester_num = req.num;    // requester — written at v > 65
        s.requester_creator = req.creator;
    }
    c.skip(4);                        // slots[PILOTS_PER_FLIGHT=4]
    c.skip(4);                        // pilots[4]
    c.skip(4);                        // plane_stats[4]
    c.skip(4);                        // player_slots[4]
    c.skip(1);                        // last_player_slot
    s.callsign_id        = c.u8();
    s.callsign_num       = c.u8();
    if (v >= 72) {
        c.skip(4);                    // refuel (unsigned int) — v >= 72
    }
}

// Package common header: elements (1) + element_ids (8*elements) +
// interceptor/awacs/jstar/ecm/tanker VU_IDs (5 × 8) + wait_cycles (1).
// Source: package.cpp:507 (Save) / package.cpp:200 (Load ctor).
void parse_package_common(Cursor& c, UnitSubclassData& s) {
    s.elements = c.u8();
    s.element_ids.clear();
    s.element_ids.reserve(static_cast<std::size_t>(s.elements) * 2);
    for (uint8_t i = 0; i < s.elements; ++i) {
        VuId e = read_vu_id(c);
        s.element_ids.push_back(e.num);
        s.element_ids.push_back(e.creator);
    }
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
}

// Package small branch (Final() && !wait_cycles): requests/responses +
// compact mis_request. 31 bytes at v63+.
// Source: package.cpp:434 (Save small branch) / package.cpp:245 (Load).
void parse_package_small(Cursor& c, UnitSubclassData& s, int v) {
    s.requests  = c.i16();
    s.responses = c.i16();
    // mis_request.mission and .context are streamed as sizeof(short) — a
    // FreeFalcon quirk (the uchar fields are written/read as shorts in
    // both directions; package.cpp:245/451). The source values are uchar,
    // so the high byte is always 0 — take the low byte.
    s.mis_request.mission  = static_cast<uint8_t>(c.u16() & 0xFF);
    s.mis_request.context  = static_cast<uint8_t>(c.u16() & 0xFF);
    VuId req = read_vu_id(c);
    s.mis_request.requester_id_num = req.num;
    s.mis_request.requester_id_creator = req.creator;
    VuId tar = read_vu_id(c);
    s.mis_request.target_id_num = tar.num;
    s.mis_request.target_id_creator = tar.creator;
    if (v >= 26) {
        s.mis_request.tot = c.i32();  // tot (CampaignTime) at v >= 26
    }
    if (v >= 35) {
        s.mis_request.action_type = c.u8();  // action_type at v >= 35
    }
    if (v >= 41) {
        s.mis_request.priority = c.i16();    // priority at v >= 41
    }
    s.package_branch = PackageBranch::Small;
}

// Package big branch: flights, wait_for, route corners, takeoff/tp_time,
// package_flags, caps, requests, responses, ingress + egress routes, and
// the full 76-byte MissionRequestClass bulk struct.
// Source: package.cpp:434+ (Save big branch) / package.cpp:298+ (Load).
void parse_package_big(Cursor& c, UnitSubclassData& s, int v) {
    s.flights  = c.u8();
    s.wait_for = c.i16();
    s.iax = c.i16(); s.iay = c.i16();     // ingress x/y
    s.eax = c.i16(); s.eay = c.i16();     // egress x/y
    s.bpx = c.i16(); s.bpy = c.i16();     // bypass x/y
    s.tpx = c.i16(); s.tpy = c.i16();     // target point x/y
    s.takeoff    = c.i32();
    s.tp_time    = c.i32();
    s.package_flags = c.u32();            // on-disk 32-bit ulong (#104)
    s.caps       = c.i16();
    s.requests   = c.i16();
    if (v < 35) {
        c.skip(2);                        // threat_stats — v < 35 only
    }
    s.responses  = c.i16();
    // Two waypoint routes (ingress, egress), each 1-byte count-prefixed.
    for (int route = 0; route < 2; ++route) {
        uint8_t wps = c.u8();
        std::vector<WaypointRecord>& out =
            (route == 0) ? s.ingress : s.egress;
        out.clear();
        out.reserve(wps);
        for (uint8_t i = 0; i < wps; ++i) {
            WaypointRecord w;
            parse_waypoint(c, w, v);
            out.push_back(w);
        }
    }
    // MissionRequestClass bulk struct. sizeof on the Win32 x86 ABI (the
    // reference target that wrote these files) = 76 bytes at v >= 35
    // (64 at v < 35). Field order per mission.h:326, offsets accounting
    // for MSVC natural alignment:
    //   0:  requesterID (VU_ID 8)
    //   8:  targetID (VU_ID 8)
    //   16: secondaryID (VU_ID 8)
    //   24: pakID (VU_ID 8)
    //   32: who (uchar 1), 33: vs (uchar 1)
    //   36: tot (CampaignTime 4 — 2 bytes pad to 4-align)
    //   40: tx (short), 42: ty (short)
    //   44: flags (uint)
    //   48: caps, 50: target_num, 52: speed, 54: match_strength, 56: priority (shorts)
    //   58: tot_type, 59: action_type, 60: mission, 61: aircraft,
    //   62: context, 63: roe_check, 64: delayed, 65: start_block, 66: final_block
    //   67: slots[4]
    //   71: min_to, 72: max_to
    //   → 73, padded to 76 (4-byte struct alignment)
    const bool full_mr = (v >= 35);
    MissionRequestRecord& m = s.mis_request;
    if (!full_mr) {
        // v < 35: 64-byte legacy struct — decode the leading IDs, skip rest.
        VuId req = read_vu_id(c);
        m.requester_id_num = req.num; m.requester_id_creator = req.creator;
        VuId tar = read_vu_id(c);
        m.target_id_num = tar.num; m.target_id_creator = tar.creator;
        c.skip(64 - 16);
        s.package_branch = PackageBranch::Big;
        return;
    }
    VuId req = read_vu_id(c);
    m.requester_id_num = req.num; m.requester_id_creator = req.creator;
    VuId tar = read_vu_id(c);
    m.target_id_num = tar.num; m.target_id_creator = tar.creator;
    VuId sec = read_vu_id(c);
    m.secondary_id_num = sec.num; m.secondary_id_creator = sec.creator;
    VuId pk = read_vu_id(c);
    m.pak_id_num = pk.num; m.pak_id_creator = pk.creator;
    m.who = c.u8();
    m.vs  = c.u8();
    c.skip(2);                     // alignment padding (34..36)
    m.tot = c.i32();               // 36
    m.tx  = c.i16();               // 40
    m.ty  = c.i16();               // 42
    m.flags = c.u32();             // 44 (on-disk 32-bit ulong)
    m.caps = c.i16();              // 48
    m.target_num = c.i16();        // 50
    m.speed = c.i16();             // 52
    m.match_strength = c.i16();    // 54
    m.priority = c.i16();          // 56
    m.tot_type = c.u8();           // 58
    m.action_type = c.u8();        // 59
    m.mission = c.u8();            // 60
    m.aircraft = c.u8();           // 61
    m.context = c.u8();            // 62
    m.roe_check = c.u8();          // 63
    m.delayed = c.u8();            // 64
    m.start_block = c.u8();        // 65
    m.final_block = c.u8();        // 66
    for (int i = 0; i < 4; ++i) m.slots[i] = c.u8();   // 67..70
    m.min_to = static_cast<int8_t>(c.u8());            // 71
    m.max_to = static_cast<int8_t>(c.u8());            // 72
    c.skip(3);                     // trailing padding → sizeof 76
    s.package_branch = PackageBranch::Big;
}

// ---------------------------------------------------------------------------
// Next-record validation. At the candidate end position, the next record's
// header must satisfy (all offsets relative to the record start, which
// begins with the 2-byte type short — NOT part of CampBaseClass):
//   - [short type] at +0 in the valid entity-type range
//   - [ushort entity_type] at +10 equals [short type] at +0
//     (type(2) + VU_ID(8) put entityType_ at +10 — both versions)
//   - [uchar owner] at +24 (v63: 25-byte CampBase) / +28 (v70+: 29-byte)
//     in [0..7]
// On the last record, the candidate position must equal the buffer end.
// ---------------------------------------------------------------------------
struct ValidateCtx {
    int v;
    int max_type;      // exclusive upper bound (100 + class-table entries)
    bool use_ct_range;
};

bool validate_next_record(const uint8_t* p, const uint8_t* end, const ValidateCtx& vc) {
    // Minimum bytes to read the next header: 2-byte type + CampBaseClass.
    const std::size_t min_hdr = (vc.v >= 70) ? 31 : 25;
    if (p + min_hdr > end) return p == end;
    int16_t type;
    std::memcpy(&type, p, 2);
    // Legacy heuristic range (no class table): wide enough to cover the
    // full Falcon4.ct span (100..2235) plus headroom — entity types past
    // 2000 are real (TestCamp.cam carries battalions at type 2022). The
    // type == entity_type equality below is the primary false-positive
    // filter; the range just rejects absurd shorts.
    const bool type_ok = vc.use_ct_range
        ? (type >= 100 && type < vc.max_type)
        : (type >= 100 && type <= 3000);
    if (!type_ok) return false;
    uint16_t entity_type;
    std::memcpy(&entity_type, p + 10, 2);
    if (entity_type != static_cast<uint16_t>(type)) return false;
    const std::size_t owner_off = (vc.v >= 70) ? 28 : 24;
    uint8_t owner = p[owner_off];
    if (owner > 7) return false;
    // Grid-coordinate plausibility (same trick as objective_decoder's
    // GRID_COORD gate): x/y sit right after entity_type at +12/+14 in
    // BOTH layouts (pos_.z_ comes after y). Real records carry grid
    // coords within the map extent; interior bytes from a mis-sized
    // candidate tail almost never satisfy this ON TOP of the checks
    // above. This is what disambiguates "brigade before squadron" vs
    // "squadron before brigade" trial ordering: without it, one order
    // false-positives on v63 saves, the other on v71 saves.
    int16_t gx, gy;
    std::memcpy(&gx, p + 12, 2);
    std::memcpy(&gy, p + 14, 2);
    if (gx < -64 || gx > 2048 || gy < -64 || gy > 2048) return false;
    return true;
}

// Try a subclass tail parser. Returns true if the candidate cursor position
// validates. On success, leaves `c` at the advanced position; on failure,
// rolls `c` back to its pre-parse position.
template<typename ParseFn>
bool try_tail(Cursor& c, const uint8_t* end, const ValidateCtx& vc,
              UnitRecord& u, ParseFn parse) {
    auto saved = c.save();
    parse(c, u.subclass);
    if (!c.error && validate_next_record(c.p, end, vc)) {
        return true;
    }
    c.restore(saved);
    return false;
}

// Parse the full tail for a predicted class; returns true if it validates.
bool parse_tail_for(Cursor& c, const uint8_t* end, const ValidateCtx& vc,
                    UnitRecord& u, UnitClass predicted, int v) {
    switch (predicted) {
        case UnitClass::Battalion:
            return try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
                (void)v;
                parse_ground_unit(cc, s);
                parse_battalion(cc, s);
            });
        case UnitClass::Brigade:
            return try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
                (void)v;
                parse_ground_unit(cc, s);
                parse_brigade(cc, s);
            });
        case UnitClass::Squadron:
            return try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
                parse_squadron(cc, s, v);
            });
        case UnitClass::TaskForce:
            return try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
                (void)v;
                parse_taskforce(cc, s);
            });
        case UnitClass::Flight:
            return try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
                parse_flight(cc, s, v);
            });
        case UnitClass::Package: {
            // Deterministic branch: small iff (unit_flags & U_FINAL) and
            // wait_cycles == 0 — mirrors PackageClass::Save (package.cpp:434).
            // wait_cycles isn't known until the common header is parsed, so
            // the lambda below decides after parse_package_common.
            return try_tail(c, end, vc, u, [v, &u](Cursor& cc, UnitSubclassData& s) {
                parse_package_common(cc, s);
                const bool use_small = (u.unit_flags & U_FINAL) && s.wait_cycles == 0;
                if (use_small) {
                    parse_package_small(cc, s, v);
                } else {
                    parse_package_big(cc, s, v);
                }
            });
        }
        case UnitClass::Unknown:
            break;
    }
    return false;
}

// Predict the unit class from the class table (FreeFalcon NewUnit logic).
UnitClass predict_from_class_table(const ClassTable& ct, uint16_t entity_type) {
    const ClassTableEntry* e = ct.lookup(entity_type);
    if (!e) return UnitClass::Unknown;
    switch (e->domain) {
        case DOMAIN_AIR:   // 2
            if (e->type == 1) return UnitClass::Flight;      // TYPE_FLIGHT
            if (e->type == 2) return UnitClass::Package;     // TYPE_PACKAGE
            if (e->type == 3) return UnitClass::Squadron;    // TYPE_SQUADRON
            break;
        case DOMAIN_LAND:  // 3
            if (e->type == 1) return UnitClass::Battalion;   // TYPE_BATTALION
            if (e->type == 2) return UnitClass::Brigade;     // TYPE_BRIGADE
            break;
        case DOMAIN_SEA:   // 4
            if (e->type == 1) return UnitClass::TaskForce;   // TYPE_TASKFORCE
            break;
        default:
            break;
    }
    return UnitClass::Unknown;
}

// Trial-and-error fallback: candidates in the original v63 order
// (battalion, brigade, squadron, taskforce, flight, package), proven
// on the save1.cam fixture. This path is HISTORICALLY best-effort —
// without the class table, a wrong-length candidate can land exactly
// on a real record boundary (v63's brigade-vs-squadron and v71's
// squadron-vs-brigade ambiguities are mirror images; the grid-
// coordinate gate in validate_next_record rejects interior-bytes
// false positives but cannot reject boundary-exact ones). Callers
// that want guaranteed parity should pass the class table — the repo
// bundles FALCON4.ct and every entry point auto-loads it. The Package
// candidate branches deterministically on (unit_flags & U_FINAL) —
// same as the class-table path — with a both-branches retry if the
// first choice fails.
UnitClass dispatch_and_parse_tail(Cursor& c, const uint8_t* end,
                                  const ValidateCtx& vc, UnitRecord& u, int v) {
    // Cursor is at post-waypoint position. try_tail uses save/restore
    // internally, so on failure the cursor rolls back automatically.

    if (try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
            parse_ground_unit(cc, s);
            parse_battalion(cc, s);
        })) {
        return UnitClass::Battalion;
    }
    u.subclass = UnitSubclassData{};

    if (try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
            parse_ground_unit(cc, s);
            parse_brigade(cc, s);
        })) {
        return UnitClass::Brigade;
    }
    u.subclass = UnitSubclassData{};

    if (try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
            parse_squadron(cc, s, v);
        })) {
        return UnitClass::Squadron;
    }
    u.subclass = UnitSubclassData{};

    if (try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
            (void)v;
            parse_taskforce(cc, s);
        })) {
        return UnitClass::TaskForce;
    }
    u.subclass = UnitSubclassData{};

    if (try_tail(c, end, vc, u, [v](Cursor& cc, UnitSubclassData& s) {
            parse_flight(cc, s, v);
        })) {
        return UnitClass::Flight;
    }
    u.subclass = UnitSubclassData{};

    // Package: deterministic branch first; on failure retry with the
    // other branch (protects against U_FINAL quirks in foreign saves).
    {
        auto pkg = [&u, v](Cursor& cc, UnitSubclassData& s, bool force_small) {
            parse_package_common(cc, s);
            const bool use_small = force_small ||
                ((u.unit_flags & U_FINAL) && s.wait_cycles == 0);
            if (use_small) parse_package_small(cc, s, v);
            else           parse_package_big(cc, s, v);
        };
        if (try_tail(c, end, vc, u, [&pkg](Cursor& cc, UnitSubclassData& s) {
                pkg(cc, s, false);
            })) {
            return UnitClass::Package;
        }
        u.subclass = UnitSubclassData{};
        if (try_tail(c, end, vc, u, [&pkg](Cursor& cc, UnitSubclassData& s) {
                pkg(cc, s, true);
            })) {
            return UnitClass::Package;
        }
        u.subclass = UnitSubclassData{};
    }

    // No tail validated. Leave the cursor where it is so the caller can
    // report the failure position.
    return UnitClass::Unknown;
}

} // namespace

DecodedUnits decode_uni(const uint8_t* data, std::size_t size,
                        const UnitDecodeOptions& opts) {
    DecodedUnits out;
    if (size < 10) throw std::runtime_error("uni: sub-file too small");

    const int v = opts.camp_version;

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

    // Validation context: entity-type range from the class table when
    // available (types can exceed 2000 — TestCamp has battalions at
    // 2022), else the legacy heuristic range.
    ValidateCtx vc;
    vc.v = v;
    vc.max_type = 2001;
    vc.use_ct_range = false;
    if (opts.class_table) {
        vc.max_type = VU_LAST_ENTITY_TYPE + static_cast<int>(opts.class_table->size());
        vc.use_ct_range = true;
    }

    Cursor c{buf.data(), buf.data() + buf.size()};
    out.units.reserve(static_cast<std::size_t>(out.count));

    int decoded = 0;
    while (decoded < out.count && c.remaining() > 0 && !c.error) {
        const uint8_t* record_start = c.p;
        UnitRecord u;
        u.type = c.i16();
        parse_camp_base(c, u, v);
        parse_unit_class_fixed(c, u, v);
        parse_waypoints(c, u, v);

        // Deterministic class-table dispatch (FreeFalcon's NewUnit), with
        // trial-and-error fallback for unknown/unclassifiable types.
        UnitClass predicted = UnitClass::Unknown;
        if (opts.class_table) {
            predicted = predict_from_class_table(*opts.class_table, u.entity_type);
        }
        if (predicted != UnitClass::Unknown) {
            if (parse_tail_for(c, c.end, vc, u, predicted, v)) {
                u.unit_class = predicted;
            } else {
                u.subclass = UnitSubclassData{};
                u.unit_class = dispatch_and_parse_tail(c, c.end, vc, u, v);
            }
        } else {
            u.unit_class = dispatch_and_parse_tail(c, c.end, vc, u, v);
        }

        // Cursor's sticky error flag is set when a read/skip went OOB.
        // This is the "buffer truncated mid-record" case — stop and
        // report the position of the start of the partial record.
        if (c.error) {
            c.p = record_start;
            out.bytes_consumed = static_cast<std::size_t>(c.p - buf.data());
            break;
        }

        // If dispatch failed, stop here (cursor stays at the failed
        // position so the caller can see bytes_consumed). The post-
        // waypoint position is 2 (type) + CampBase + UnitClass fixed +
        // wp_count + waypoints.
        if (u.unit_class == UnitClass::Unknown) {
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

// f4-world/src/world_state.cpp — JSON loader for WorldState.
//
// Uses f4-json's dependency-free Reader to walk the world-state schema
// emitted by f4-world-convert. The reader is shape-compatible with the
// hand-rolled JsonReader that lived here previously — the field parsers
// below are unchanged from the original implementation; only the local
// class definition has been replaced with #include <f4/json/reader.hpp>.

#include <f4/world/detail/world_state.hpp>

#include <f4/json/reader.hpp>
#include <f4/assets/asset_root.hpp>  // for load_terrain_via_assets()

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace f4::world {

namespace {

using f4::json::Reader;

// Parse a team slot object starting at the '{'.
TeamState parse_team(Reader& r) {
    TeamState t;
    r.skip_ws(); r.expect('{');
    if (r.consume('}')) return t;
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "slot")    t.slot    = static_cast<int>(r.read_int());
        else if (k == "flags")   t.flags   = static_cast<uint8_t>(r.read_int());
        else if (k == "colour")  t.colour  = static_cast<uint8_t>(r.read_int());
        else if (k == "name")    t.name    = r.read_string();
        else if (k == "motto")   t.motto   = r.read_string();
        // C2: the .cmp team block's replacement stock (aircraft the
        // team can hand to squadrons as reinforcements).
        else if (k == "replacements_avail")
            t.replacements_avail = static_cast<uint16_t>(r.read_int());
        // --- .tea enrichment fields (emitted by world_json when .tea
        // was successfully decoded). Each team slot may or may not have
        // these — the .tea decoder finds them by `who` field match.
        else if (k == "cteam")              { t.cteam              = static_cast<int16_t>(r.read_int()); t.tea_loaded = true; }
        else if (k == "team_flags")         { t.team_flags         = static_cast<int16_t>(r.read_int()); }
        else if (k == "first_colonel")      { t.first_colonel      = static_cast<int16_t>(r.read_int()); }
        else if (k == "first_commander")    { t.first_commander    = static_cast<int16_t>(r.read_int()); }
        else if (k == "first_wingman")      { t.first_wingman      = static_cast<int16_t>(r.read_int()); }
        else if (k == "last_wingman")       { t.last_wingman       = static_cast<int16_t>(r.read_int()); }
        else if (k == "air_experience")         { t.air_experience         = static_cast<uint8_t>(r.read_int()); }
        else if (k == "air_defense_experience") { t.air_defense_experience = static_cast<uint8_t>(r.read_int()); }
        else if (k == "ground_experience")      { t.ground_experience      = static_cast<uint8_t>(r.read_int()); }
        else if (k == "naval_experience")       { t.naval_experience       = static_cast<uint8_t>(r.read_int()); }
        else if (k == "member") {
            // Array of NUM_COUNS country membership bytes (0 or 1).
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                t.member.push_back(static_cast<uint8_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
            t.tea_loaded = true;
        }
        else if (k == "stance") {
            // Array of NUM_TEAMS stance int16s (stance toward each team).
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                t.stance.push_back(static_cast<int16_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
            t.tea_loaded = true;
        }
        // --- C4 (ATM pipeline): the team's tasking priorities + ATM
        // state (all emitted only when .tea enrichment decoded — the
        // skip_value default keeps non-enriched saves unchanged).
        else if (k == "mission_priority") {
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                t.mission_priority.push_back(static_cast<uint8_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
            t.tea_loaded = true;
        }
        else if (k == "objtype_priority") {
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                t.objtype_priority.push_back(static_cast<uint8_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
            t.tea_loaded = true;
        }
        else if (k == "atm_schedules") {
            // Array of {"id": N, "schedule": [32 block bitmasks]}.
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                AtmAirbaseState ab;
                r.skip_ws(); r.expect('{');
                for (;;) {
                    std::string ak = r.read_string();
                    r.expect(':');
                    if (ak == "id") {
                        ab.id_num = static_cast<uint32_t>(r.read_int());
                    } else if (ak == "schedule") {
                        // Up to 32 block bitmasks (the emission writes
                        // exactly 32; shorter arrays zero-fill — a
                        // hand-written world is a legal input).
                        r.skip_ws(); r.expect('[');
                        if (r.consume(']')) { /* empty */ }
                        else for (int bi = 0; bi < 32; ++bi) {
                            ab.schedule[static_cast<std::size_t>(bi)] =
                                static_cast<uint8_t>(r.read_int());
                            if (r.consume(']')) break;
                            r.expect(',');
                        }
                    } else {
                        r.skip_value();
                    }
                    if (r.consume('}')) break;
                    r.expect(',');
                }
                t.atm_airbases.push_back(std::move(ab));
                if (r.consume(']')) break;
                r.expect(',');
            }
            t.tea_loaded = true;
        }
        else if (k == "atm_requests") {
            // Array of the ATO backlog records (the emission's field set).
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                AtmRequestState rq;
                r.skip_ws(); r.expect('{');
                for (;;) {
                    std::string rk = r.read_string();
                    r.expect(':');
                    if      (rk == "mission")      rq.mission      = static_cast<uint8_t>(r.read_int());
                    else if (rk == "who")          rq.who          = static_cast<uint8_t>(r.read_int());
                    else if (rk == "vs")           r.skip_value();
                    else if (rk == "tot")          rq.tot          = static_cast<int32_t>(r.read_int());
                    else if (rk == "priority")     rq.priority     = static_cast<int32_t>(r.read_int());
                    else if (rk == "action_type")  r.skip_value();
                    else if (rk == "context")      r.skip_value();
                    else if (rk == "aircraft")     rq.aircraft     = static_cast<uint8_t>(r.read_int());
                    else if (rk == "target_num")   rq.target_num   = static_cast<uint32_t>(r.read_int());
                    else if (rk == "requester_num") rq.requester_num = static_cast<uint32_t>(r.read_int());
                    else                           r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                }
                t.atm_requests.push_back(std::move(rq));
                if (r.consume(']')) break;
                r.expect(',');
            }
            t.tea_loaded = true;
        }
        else                     r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return t;
}

ObjectiveState parse_objective(Reader& r) {
    ObjectiveState o;
    r.skip_ws(); r.expect('{');
    if (r.consume('}')) return o;
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "type")            o.type            = static_cast<int16_t>(r.read_int());
        else if (k == "type_name")       r.skip_value();  // display only
        else if (k == "objective_type")  o.objective_type  = static_cast<uint8_t>(r.read_int());
        // B.3 fix: id_num/id_creator were never parsed — objective_id_map
        // came back EMPTY for every JSON-loaded world, silently disabling
        // every VU_ID cross-reference (squadron→airbase fell back to the
        // positional heuristic; flight targets and ATM requests resolved
        // to nothing). In-memory WorldStates (the cam2json in-process
        // pipeline) always carried them, which is why only JSON reloads
        // were broken.
        else if (k == "id_num")          o.id_num          = static_cast<uint32_t>(r.read_int());
        else if (k == "id_creator")      o.id_creator      = static_cast<uint32_t>(r.read_int());
        else if (k == "x")               o.x               = static_cast<int16_t>(r.read_int());
        else if (k == "y")               o.y               = static_cast<int16_t>(r.read_int());
        else if (k == "z")               o.z               = static_cast<float>(r.read_number());
        else if (k == "owner")           o.owner           = static_cast<uint8_t>(r.read_int());
        else if (k == "priority")        o.priority        = static_cast<uint8_t>(r.read_int());
        else if (k == "nameid")          o.nameid          = static_cast<int16_t>(r.read_int());
        else if (k == "camp_id")         o.camp_id         = static_cast<int16_t>(r.read_int());
        else if (k == "entity_type")     o.entity_type     = static_cast<uint16_t>(r.read_int());
        else if (k == "obj_flags")       o.obj_flags       = static_cast<uint32_t>(r.read_int());
        else if (k == "supply")          o.supply          = static_cast<uint8_t>(r.read_int());
        else if (k == "fuel")            o.fuel            = static_cast<uint8_t>(r.read_int());
        else if (k == "losses")          o.losses          = static_cast<uint8_t>(r.read_int());
        else if (k == "last_repair")     o.last_repair     = static_cast<int32_t>(r.read_int());
        else if (k == "first_owner")     o.first_owner     = static_cast<uint8_t>(r.read_int());
        else if (k == "parent_id")       o.parent_id       = static_cast<uint32_t>(r.read_int());
        else if (k == "fstatus") {
            // Array of uchar values (per-feature damage bitmap).
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                o.fstatus.push_back(static_cast<uint8_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        else if (k == "has_radar") {
            o.has_radar = r.read_bool();
        }
        else if (k == "detect_ratio") {
            r.skip_ws(); r.expect('[');
            for (int i = 0; i < 8; ++i) {
                if (i) r.expect(',');
                o.detect_ratio[i] = static_cast<float>(r.read_number());
            }
            r.expect(']');
        }
        // Phase 3: real radar range from Falcon4.RCD.
        else if (k == "radar_range_km")   o.radar_range_km   = static_cast<float>(r.read_number());
        else if (k == "radar_name")       o.radar_name       = r.read_string();
        else if (k == "radar_type_idx")   o.radar_type_idx   = static_cast<int16_t>(r.read_int());
        else if (k == "links") {
            // Array of {n, c, road, rail, costs[8]} objects.
            // Phase 1 fix A.4: costs[8] is the per-movement-type traversal
            // cost array (Foot/Wheeled/Tracked/LowAir/Air/Naval/Rail).
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                ObjectiveLink link;
                if (!r.peek('}')) for (;;) {
                    std::string lk = r.read_string();
                    r.expect(':');
                    if      (lk == "n")    link.neighbor_num    = static_cast<uint32_t>(r.read_int());
                    else if (lk == "c")    link.neighbor_creator = static_cast<uint32_t>(r.read_int());
                    else if (lk == "road") link.is_road = r.read_bool();
                    else if (lk == "rail") link.is_rail = r.read_bool();
                    else if (lk == "costs") {
                        // Per-movement-type traversal cost array (8 uchar).
                        r.skip_ws(); r.expect('[');
                        for (int ci = 0; ci < 8; ++ci) {
                            if (ci) r.expect(',');
                            link.costs[ci] = static_cast<uint8_t>(r.read_int());
                        }
                        r.expect(']');
                    }
                    else r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                o.links.push_back(link);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        // --- Theater static-data enrichment fields (from Falcon4.OCD/PHD/PD) ---
        else if (k == "class_name")      o.class_name      = r.read_string();
        else if (k == "features_count")  o.features_count  = static_cast<uint8_t>(r.read_int());
        else if (k == "radar_feature")   o.radar_feature   = static_cast<uint8_t>(r.read_int());
        else if (k == "deag_distance")   o.deag_distance   = static_cast<uint8_t>(r.read_int());
        else if (k == "pt_data_index")   o.pt_data_index   = static_cast<uint16_t>(r.read_int());
        // Phase 1 fix A.7: OCD detection[8] — per-movement-type electronic
        // detection ranges. Previously emitted by world_json.cpp only when
        // theater_db was loaded but dropped here.
        else if (k == "detection") {
            r.skip_ws(); r.expect('[');
            for (int di = 0; di < 8; ++di) {
                if (di) r.expect(',');
                o.objective_detection[di] = static_cast<uint8_t>(r.read_int());
            }
            r.expect(']');
        }
        else if (k == "features") {
            // Array of feature placement objects (from Falcon4.FED + FCD).
            // Each feature has: index (entity_type), flags, value, offset_x/y/z,
            // facing, and optional name/hit_points/repair_time/priority/feat_flags
            // resolved from FCD.
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                FeatureEntryState fes;
                if (!r.peek('}')) for (;;) {
                    std::string fk = r.read_string();
                    r.expect(':');
                    if      (fk == "index")        fes.index        = static_cast<int16_t>(r.read_int());
                    else if (fk == "flags")        fes.flags        = static_cast<uint16_t>(r.read_int());
                    else if (fk == "value")        fes.value        = static_cast<uint8_t>(r.read_int());
                    else if (fk == "offset_x")     fes.offset_x     = static_cast<float>(r.read_number());
                    else if (fk == "offset_y")     fes.offset_y     = static_cast<float>(r.read_number());
                    else if (fk == "offset_z")     fes.offset_z     = static_cast<float>(r.read_number());
                    else if (fk == "facing")       fes.facing       = static_cast<int16_t>(r.read_int());
                    else if (fk == "name")         fes.name         = r.read_string();
                    else if (fk == "hit_points")   fes.hit_points   = static_cast<int16_t>(r.read_int());
                    // Phase 1 fix A.5: previously emitted by world_json.cpp
                    // but dropped here because FeatureEntryState had no slots.
                    else if (fk == "repair_time")  fes.repair_time  = static_cast<int16_t>(r.read_int());
                    else if (fk == "priority")     fes.priority     = static_cast<uint8_t>(r.read_int());
                    else if (fk == "feat_flags")   fes.feat_flags   = static_cast<uint16_t>(r.read_int());
                    else if (fk == "radar_type")   fes.radar_type   = static_cast<int16_t>(r.read_int());
                    else                           r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                // Resolve damage_state from the parent objective's fstatus
                // bitmap (2 bits per feature, indexed by feature position
                // within the objective's feature list). FreeFalcon's f4vu.h
                // VIS states: 0=normal, 1=repaired, 2=damaged, 3=destroyed.
                const std::size_t fidx = o.features.size();
                const std::size_t byte_idx = fidx / 4;
                const std::size_t bit_shift = (fidx % 4) * 2;
                if (byte_idx < o.fstatus.size()) {
                    fes.damage_state = (o.fstatus[byte_idx] >> bit_shift) & 0x03;
                }
                o.features.push_back(fes);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        else if (k == "ground_layout") {
            // Array of ground-layout list objects, each with type, count,
            // runway_num, ltrt, heading_deg, and a points array.
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                GroundLayoutList gll;
                if (!r.peek('}')) for (;;) {
                    std::string gk = r.read_string();
                    r.expect(':');
                    if      (gk == "type")         gll.type         = static_cast<uint8_t>(r.read_int());
                    else if (gk == "count")        gll.count        = static_cast<uint8_t>(r.read_int());
                    else if (gk == "runway_num")   gll.runway_num   = static_cast<uint8_t>(r.read_int());
                    else if (gk == "ltrt")         gll.ltrt         = static_cast<int8_t>(r.read_int());
                    else if (gk == "heading_deg" || gk == "heading")
                        gll.heading_deg = static_cast<float>(r.read_number());
                    else if (gk == "points") {
                        r.skip_ws(); r.expect('[');
                        if (r.consume(']')) { /* empty */ }
                        else for (;;) {
                            r.skip_ws(); r.expect('{');
                            GroundLayoutPoint pt;
                            if (!r.peek('}')) for (;;) {
                                std::string pk = r.read_string();
                                r.expect(':');
                                if      (pk == "x")     pt.x     = static_cast<float>(r.read_number());
                                else if (pk == "y")     pt.y     = static_cast<float>(r.read_number());
                                else if (pk == "type")  pt.type  = static_cast<uint8_t>(r.read_int());
                                else if (pk == "flags") pt.flags = static_cast<uint8_t>(r.read_int());
                                else                    r.skip_value();
                                if (r.consume('}')) break;
                                r.expect(',');
                            } else r.consume('}');
                            gll.points.push_back(pt);
                            if (r.consume(']')) break;
                            r.expect(',');
                        }
                    }
                    else r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                o.ground_layout.push_back(gll);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        else                             r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    // A-G tranche: nominal feature placements when the FED list is
    // missing. The OCD class data says how MANY features an objective has
    // (features_count — real data); the fixture FED covers only a subset
    // of the game's classes, so objectives like TestCamp's Army Bases
    // carry the count but no placements. Without placements there is no
    // damage ledger, and bombs on those objectives do nothing — so we
    // synthesize nominal entries (4-wide grid, 250 ft spacing, centered
    // on the objective). Same discipline as the synthetic airfields: the
    // real FED data takes precedence the moment it is present, and the
    // QC summary says which world it ran against.
    // Feature count: the OCD class row OR the save's own fstatus bitmap,
    // whichever is larger — the fixture OCD covers only 12 of the game's
    // objective classes, and TestCamp's Airbases decode with
    // features_count 0 while their fstatus bitmaps carry 64-100 real
    // feature slots (the bitmap is save data, not class data).
    const int n_features = std::max(
        static_cast<int>(o.features_count),
        static_cast<int>(o.fstatus.size() * 4));
    if (n_features > 0 && o.features.empty()) {
        constexpr float kSpacingFt = 250.0f;
        constexpr int kCols = 4;
        o.features.reserve(n_features);
        for (int i = 0; i < n_features; ++i) {
            FeatureEntryState f;
            const int row = i / kCols;
            const int col = i % kCols;
            f.offset_x = (static_cast<float>(col) - 1.5f) * kSpacingFt;
            f.offset_y = (static_cast<float>(row) -
                          (o.features_count - 1) / (2.0f * kCols)) * kSpacingFt;
            // Inherit the save's own damage state for this feature index
            // (the .obd deltas carry fstatus even when the FED placements
            // are missing — a mid-campaign objective that already lost
            // features stays damaged in the ledger).
            const std::size_t fidx = static_cast<std::size_t>(i);
            const std::size_t byte_idx = fidx / 4;
            const std::size_t bit_shift = (fidx % 4) * 2;
            if (byte_idx < o.fstatus.size()) {
                f.damage_state = static_cast<uint8_t>(
                    (o.fstatus[byte_idx] >> bit_shift) & 0x03);
            }
            o.features.push_back(f);
        }
    }
    return o;
}

UnitState parse_unit(Reader& r) {
    UnitState u;
    r.skip_ws(); r.expect('{');
    if (r.consume('}')) return u;
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "type")           u.type           = static_cast<int16_t>(r.read_int());
        else if (k == "unit_class") {
            std::string s = r.read_string();
            if      (s == "battalion") u.unit_class = UnitClass::Battalion;
            else if (s == "brigade")   u.unit_class = UnitClass::Brigade;
            else if (s == "squadron")  u.unit_class = UnitClass::Squadron;
            else if (s == "taskforce") u.unit_class = UnitClass::TaskForce;
            else if (s == "flight")    u.unit_class = UnitClass::Flight;
            else if (s == "package")   u.unit_class = UnitClass::Package;
            else                        u.unit_class = UnitClass::Unknown;
        }
        else if (k == "unit_subtype")   u.unit_subtype   = static_cast<uint8_t>(r.read_int());
        else if (k == "domain")         u.domain         = static_cast<uint8_t>(r.read_int());
        // B.3 fix: id_num/id_creator were never parsed — unit_id_map came
        // back EMPTY for every JSON-loaded world, so Flight→Package /
        // Flight→Squadron / Battalion→Brigade / Package→element resolution
        // silently no-oped (see the objective-side comment above).
        else if (k == "id_num")         u.id_num         = static_cast<uint32_t>(r.read_int());
        else if (k == "id_creator")     u.id_creator     = static_cast<uint32_t>(r.read_int());
        else if (k == "roster")         u.roster         = static_cast<uint32_t>(r.read_int());
        else if (k == "x")              u.x              = static_cast<int16_t>(r.read_int());
        else if (k == "y")              u.y              = static_cast<int16_t>(r.read_int());
        else if (k == "z")              u.z              = static_cast<float>(r.read_number());
        else if (k == "owner")          u.owner          = static_cast<uint8_t>(r.read_int());
        else if (k == "dest_x")         u.dest_x         = static_cast<int16_t>(r.read_int());
        else if (k == "dest_y")         u.dest_y         = static_cast<int16_t>(r.read_int());
        else if (k == "name_id")        u.name_id        = static_cast<int16_t>(r.read_int());
        else if (k == "camp_id")        u.camp_id        = static_cast<int16_t>(r.read_int());
        else if (k == "entity_type")    u.entity_type    = static_cast<uint16_t>(r.read_int());
        else if (k == "reinforcement")  u.reinforcement  = static_cast<int16_t>(r.read_int());
        else if (k == "wp_count")       u.wp_count       = static_cast<uint8_t>(r.read_int());
        else if (k == "waypoints") {
            // Array of waypoint objects. Each waypoint has x/y/z + action +
            // route_action + formation + flags + optional target_num/creator/
            // building + optional depart.
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                WaypointState w;
                if (!r.peek('}')) for (;;) {
                    std::string wk = r.read_string();
                    r.expect(':');
                    if      (wk == "x")                w.x                = static_cast<int16_t>(r.read_int());
                    else if (wk == "y")                w.y                = static_cast<int16_t>(r.read_int());
                    else if (wk == "z")                w.z                = static_cast<int16_t>(r.read_int());
                    else if (wk == "arrive")           w.arrive           = static_cast<int32_t>(r.read_int());
                    else if (wk == "action")           w.action           = static_cast<uint8_t>(r.read_int());
                    else if (wk == "route_action")     w.route_action     = static_cast<uint8_t>(r.read_int());
                    else if (wk == "formation")        w.formation        = static_cast<uint8_t>(r.read_int());
                    else if (wk == "flags")            w.flags            = static_cast<int16_t>(r.read_int());
                    else if (wk == "target_num")       w.target_num       = static_cast<uint32_t>(r.read_int());
                    else if (wk == "target_creator")   w.target_creator   = static_cast<uint32_t>(r.read_int());
                    else if (wk == "target_building")  w.target_building  = static_cast<uint8_t>(r.read_int());
                    else if (wk == "depart")           w.depart           = static_cast<int32_t>(r.read_int());
                    else                               r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                u.waypoints.push_back(w);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        else if (k == "losses")         u.losses         = static_cast<uint8_t>(r.read_int());
        else if (k == "supply")         u.supply         = static_cast<uint8_t>(r.read_int());
        else if (k == "morale")         u.morale         = static_cast<uint8_t>(r.read_int());
        else if (k == "fatigue")        u.fatigue        = static_cast<uint8_t>(r.read_int());
        else if (k == "elements")       u.elements       = static_cast<uint8_t>(r.read_int());
        else if (k == "fuel")           u.fuel           = static_cast<int32_t>(r.read_int());
        else if (k == "parent_id")      u.parent_id      = static_cast<uint32_t>(r.read_int());
        else if (k == "last_move")      u.last_move      = static_cast<int32_t>(r.read_int());
        else if (k == "last_combat")    u.last_combat    = static_cast<int32_t>(r.read_int());
        else if (k == "heading")        u.heading        = static_cast<uint8_t>(r.read_int());
        else if (k == "final_heading")  u.final_heading  = static_cast<uint8_t>(r.read_int());
        else if (k == "position")       u.position       = static_cast<uint8_t>(r.read_int());
        else if (k == "airbase_id")     u.airbase_id     = static_cast<uint32_t>(r.read_int());
        else if (k == "specialty")      u.specialty      = static_cast<uint8_t>(r.read_int());
        else if (k == "aa_kills")       u.aa_kills       = static_cast<int16_t>(r.read_int());
        else if (k == "ag_kills")       u.ag_kills       = static_cast<int16_t>(r.read_int());
        else if (k == "as_kills")       u.as_kills       = static_cast<int16_t>(r.read_int());
        else if (k == "an_kills")       u.an_kills       = static_cast<int16_t>(r.read_int());
        else if (k == "missions_flown") u.missions_flown = static_cast<int16_t>(r.read_int());
        else if (k == "mission_score")  u.mission_score  = static_cast<int16_t>(r.read_int());
        else if (k == "total_losses")   u.total_losses   = static_cast<uint8_t>(r.read_int());
        else if (k == "pilot_losses")   u.pilot_losses   = static_cast<uint8_t>(r.read_int());
        else if (k == "squadron_patch") u.squadron_patch = static_cast<uint8_t>(r.read_int());
        else if (k == "element_ids") {
            // Array of integers (VU_ID.num values).
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                u.element_ids.push_back(static_cast<uint32_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        else if (k == "pilots") {
            // Array of pilot objects.
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                PilotState p;
                if (!r.peek('}')) for (;;) {
                    std::string pk = r.read_string();
                    r.expect(':');
                    if      (pk == "id")       p.pilot_id       = static_cast<int16_t>(r.read_int());
                    else if (pk == "skill")    p.skill          = static_cast<uint8_t>(r.read_int());
                    else if (pk == "rating")   p.rating         = static_cast<uint8_t>(r.read_int());
                    else if (pk == "status")   p.status         = static_cast<uint8_t>(r.read_int());
                    else if (pk == "aa")       p.aa_kills       = static_cast<uint8_t>(r.read_int());
                    else if (pk == "ag")       p.ag_kills       = static_cast<uint8_t>(r.read_int());
                    // Phase 1 fix A.6: as/an kills were decoded by
                    // unit_decoder.cpp but dropped by PilotState.
                    else if (pk == "as")       p.as_kills       = static_cast<uint8_t>(r.read_int());
                    else if (pk == "an")       p.an_kills       = static_cast<uint8_t>(r.read_int());
                    else if (pk == "missions") p.missions_flown = static_cast<int16_t>(r.read_int());
                    else                       r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                u.pilots.push_back(p);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        // --- Theater static-data enrichment fields (from Falcon4.UCD/VCD) ---
        else if (k == "class_name")          u.class_name          = r.read_string();
        else if (k == "movement_type")       u.movement_type       = static_cast<int32_t>(r.read_int());
        else if (k == "movement_type_name")  u.movement_type_name  = r.read_string();
        else if (k == "movement_speed")      u.movement_speed      = static_cast<int16_t>(r.read_int());
        else if (k == "max_range")           u.max_range           = static_cast<int16_t>(r.read_int());
        else if (k == "vehicle_groups") {
            // Array of vehicle group objects with vehicle_type, count,
            // live_count, vehicle_name, vehicle_nctr, hit_points, max_speed.
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                VehicleGroup vg;
                if (!r.peek('}')) for (;;) {
                    std::string gk = r.read_string();
                    r.expect(':');
                    if      (gk == "group")         vg.group         = static_cast<uint8_t>(r.read_int());
                    else if (gk == "vehicle_type")  vg.vehicle_type  = static_cast<int16_t>(r.read_int());
                    else if (gk == "count")         vg.count         = static_cast<int32_t>(r.read_int());
                    else if (gk == "live_count")    vg.live_count    = static_cast<int32_t>(r.read_int());
                    else if (gk == "vehicle_name")  vg.vehicle_name  = r.read_string();
                    else if (gk == "vehicle_nctr")  vg.vehicle_nctr  = r.read_string();
                    else if (gk == "hit_points")    vg.hit_points    = static_cast<int16_t>(r.read_int());
                    else if (gk == "max_speed")     vg.max_speed     = static_cast<int16_t>(r.read_int());
                    else                            r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                u.vehicle_groups.push_back(vg);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        // Phase 1 fix A.8: UCD scores[16] — per-mission-role scoring.
        // Previously emitted by world_json.cpp but dropped here.
        else if (k == "scores") {
            r.skip_ws(); r.expect('[');
            for (int si = 0; si < 16; ++si) {
                if (si) r.expect(',');
                u.unit_class_scores[si] = static_cast<uint8_t>(r.read_int());
            }
            r.expect(']');
        }
        // C3 (war-loop routing): UCD hit_chance[8]/weapon_range[8], the
        // threat-model fields indexed by MoveType. Same emission contract
        // as "scores" — arrays of 8 ints.
        else if (k == "hit_chance") {
            r.skip_ws(); r.expect('[');
            for (int mi = 0; mi < 8; ++mi) {
                if (mi) r.expect(',');
                u.unit_hit_chance[mi] = static_cast<uint8_t>(r.read_int());
            }
            r.expect(']');
        }
        else if (k == "weapon_range") {
            r.skip_ws(); r.expect('[');
            for (int mi = 0; mi < 8; ++mi) {
                if (mi) r.expect(',');
                u.unit_weapon_range[mi] = static_cast<uint8_t>(r.read_int());
            }
            r.expect(']');
        }
        // Phase 1 fix A.1: Flight subclass fields. Previously decoded by
        // unit_decoder.cpp but never emitted/consumed.
        else if (k == "flight_altitude")    u.flight_altitude    = static_cast<float>(r.read_number());
        else if (k == "fuel_burnt")         u.fuel_burnt         = static_cast<int32_t>(r.read_int());
        else if (k == "time_on_target")     u.time_on_target     = static_cast<int32_t>(r.read_int());
        else if (k == "mission_over_time")  u.mission_over_time  = static_cast<int32_t>(r.read_int());
        else if (k == "mission_target")     u.mission_target     = static_cast<int16_t>(r.read_int());
        else if (k == "loadouts")           u.loadouts           = static_cast<uint8_t>(r.read_int());
        // A-G tranche: decoded loadout stations (wire weapon ids + counts).
        else if (k == "loadout_stations") {
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                LoadoutStationState st;
                if (!r.peek('}')) for (;;) {
                    std::string sk = r.read_string();
                    r.expect(':');
                    if      (sk == "id")    st.weapon_id = static_cast<uint16_t>(r.read_int());
                    else if (sk == "count") st.count     = static_cast<uint16_t>(r.read_int());
                    else                    r.skip_value();   // "name" enrichment
                    if (r.consume('}')) break;
                    r.expect(',');
                } else r.consume('}');
                u.loadout_stations.push_back(st);
                if (r.consume(']')) break;
                r.expect(',');
            }
        }
        else if (k == "mission")            u.mission            = static_cast<uint8_t>(r.read_int());
        else if (k == "flight_priority")    u.flight_priority    = static_cast<uint8_t>(r.read_int());
        else if (k == "mission_id")         u.mission_id         = static_cast<uint8_t>(r.read_int());
        else if (k == "eval_flags")         u.eval_flags         = static_cast<uint8_t>(r.read_int());
        else if (k == "package_id")         u.package_id         = static_cast<uint32_t>(r.read_int());
        else if (k == "squadron_id")        u.squadron_id        = static_cast<uint32_t>(r.read_int());
        else if (k == "callsign_id")        u.callsign_id        = static_cast<uint8_t>(r.read_int());
        else if (k == "callsign_num")       u.callsign_num       = static_cast<uint8_t>(r.read_int());
        // Phase 1 fix A.1: Package subclass fields.
        else if (k == "wait_cycles")        u.wait_cycles        = static_cast<uint8_t>(r.read_int());
        else if (k == "interceptor_id")     u.interceptor_id     = static_cast<uint32_t>(r.read_int());
        else if (k == "awacs_id")           u.awacs_id           = static_cast<uint32_t>(r.read_int());
        else if (k == "jstar_id")           u.jstar_id           = static_cast<uint32_t>(r.read_int());
        else if (k == "ecm_id")             u.ecm_id             = static_cast<uint32_t>(r.read_int());
        else if (k == "tanker_id")          u.tanker_id          = static_cast<uint32_t>(r.read_int());
        // B.3 tranche: the ATM mission request that produced this package
        // (v71 saves carry it as a nested mis_request object). Older /
        // synthetic world JSON without the block simply leaves
        // request_present false.
        else if (k == "mis_request") {
            u.request_present = true;
            r.skip_ws(); r.expect('{');
            if (!r.peek('}')) for (;;) {
                std::string mk = r.read_string();
                r.expect(':');
                if      (mk == "mission")        u.request_mission       = static_cast<uint8_t>(r.read_int());
                else if (mk == "tot")            u.request_tot           = static_cast<int32_t>(r.read_int());
                else if (mk == "priority")       u.request_priority      = static_cast<uint8_t>(r.read_int());
                else if (mk == "action_type")    u.request_action_type   = static_cast<uint16_t>(r.read_int());
                else if (mk == "target_num")     u.request_target_num    = static_cast<uint32_t>(r.read_int());
                else if (mk == "target_creator") u.request_target_creator= static_cast<uint32_t>(r.read_int());
                else if (mk == "requester_num")  u.request_requester_num = static_cast<uint32_t>(r.read_int());
                else                             r.skip_value();
                if (r.consume('}')) break;
                r.expect(',');
            } else r.consume('}');
        }
        else                            r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return u;
}

void parse_campaign_field(Reader& r, const std::string& key, CampaignState& c) {
    if      (key == "current_time")         c.current_time = static_cast<int32_t>(r.read_int());
    else if (key == "te_start_time")        c.te_start_time = static_cast<int32_t>(r.read_int());
    else if (key == "te_time_limit")        c.te_time_limit = static_cast<int32_t>(r.read_int());
    else if (key == "te_victory_points")    c.te_victory_points = static_cast<int32_t>(r.read_int());
    else if (key == "te_type")              c.te_type = static_cast<int32_t>(r.read_int());
    else if (key == "te_number_teams")      c.te_number_teams = static_cast<int32_t>(r.read_int());
    else if (key == "te_team")              c.te_team = static_cast<int32_t>(r.read_int());
    else if (key == "te_flags")             c.te_flags = static_cast<int32_t>(r.read_int());
    // B.3 QC tranche: the campaign bullseye (shared reference point).
    else if (key == "bullseye_x")          c.bullseye_x = static_cast<int32_t>(r.read_int());
    else if (key == "bullseye_y")          c.bullseye_y = static_cast<int32_t>(r.read_int());
    else if (key == "bullseye_name")       c.bullseye_name = static_cast<int32_t>(r.read_int());
    // C2 (war-loop tasking): the maintenance timers that anchor the
    // resupply/repair/reinforcement cadences (absolute campaign times,
    // v>=19 block). Emitted by world_json since the v71 tranche.
    else if (key == "last_resupply")       c.last_resupply = static_cast<int32_t>(r.read_int());
    else if (key == "last_repair")         c.last_repair = static_cast<int32_t>(r.read_int());
    else if (key == "last_reinforcement")  c.last_reinforcement = static_cast<int32_t>(r.read_int());
    else if (key == "te_number_aircraft" || key == "te_team_pts") {
        r.skip_ws(); r.expect('[');
        std::vector<int32_t> arr;
        if (!r.peek(']')) for (;;) {
            arr.push_back(static_cast<int32_t>(r.read_int()));
            if (r.consume(']')) break;
            r.expect(',');
        }
        if (key == "te_number_aircraft") c.te_number_aircraft = std::move(arr);
        else                              c.te_team_pts = std::move(arr);
    } else {
        r.skip_value();
    }
}

} // namespace

void WorldState::load_from_string(const std::string& json) {
    Reader r(json);

    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return;

    for (;;) {
        std::string key = r.read_string();
        r.expect(':');

        if (key == "version") {
            version = static_cast<int>(r.read_int());
        } else if (key == "theater") {
            theater = r.read_string();
        } else if (key == "terrain_file") {
            terrain_file = r.read_string();
        } else if (key == "campaign") {
            // Parse campaign object — may contain a teams array we capture
            // into the WorldState (not CampaignState).
            r.skip_ws(); r.expect('{');
            if (r.consume('}')) continue;
            for (;;) {
                std::string ck = r.read_string();
                r.expect(':');
                if (ck == "teams") {
                    r.skip_ws(); r.expect('[');
                    if (r.consume(']')) { /* empty */ }
                    else for (;;) {
                        teams.push_back(parse_team(r));
                        if (r.consume(']')) break;
                        r.expect(',');
                    }
                } else {
                    parse_campaign_field(r, ck, campaign);
                }
                if (r.consume('}')) break;
                r.expect(',');
            }
        } else if (key == "objectives") {
            // The objective decoder emits: { "count": N, "decoded": M, "items": [...] }
            r.skip_ws(); r.expect('{');
            if (r.consume('}')) continue;
            for (;;) {
                std::string ok = r.read_string();
                r.expect(':');
                if (ok == "items") {
                    r.skip_ws(); r.expect('[');
                    if (r.consume(']')) { /* empty */ }
                    else for (;;) {
                        objectives.push_back(parse_objective(r));
                        if (r.consume(']')) break;
                        r.expect(',');
                    }
                } else {
                    r.skip_value();
                }
                if (r.consume('}')) break;
                r.expect(',');
            }
        } else if (key == "units") {
            r.skip_ws(); r.expect('{');
            if (r.consume('}')) continue;
            for (;;) {
                std::string uk = r.read_string();
                r.expect(':');
                if (uk == "items") {
                    r.skip_ws(); r.expect('[');
                    if (r.consume(']')) { /* empty */ }
                    else for (;;) {
                        units.push_back(parse_unit(r));
                        if (r.consume(']')) break;
                        r.expect(',');
                    }
                } else {
                    r.skip_value();
                }
                if (r.consume('}')) break;
                r.expect(',');
            }
        } else {
            r.skip_value();
        }

        if (r.consume('}')) break;
        r.expect(',');
    }
}

void WorldState::load(const std::filesystem::path& json_path) {
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("WorldState: cannot open " + json_path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    load_from_string(ss.str());

    // Remember where we loaded from so load_terrain() can resolve the
    // terrain_file path relative to the world JSON's directory.
    world_json_dir_ = json_path.parent_path().string();
}

void WorldState::load_terrain(const std::filesystem::path& base_dir) {
    if (terrain_file.empty())
        throw std::runtime_error("WorldState: no terrain_file referenced");

    std::filesystem::path resolved = terrain_file;
    if (!base_dir.empty()) {
        resolved = std::filesystem::path(base_dir) / terrain_file;
    } else if (!world_json_dir_.empty()) {
        resolved = std::filesystem::path(world_json_dir_) / terrain_file;
    }

    terrain.load_terrain_json(resolved);
    terrain_loaded = true;
}

void WorldState::load_terrain_via_assets(const f4::assets::AssetRoot& root) {
    if (terrain_file.empty())
        throw std::runtime_error("WorldState: no terrain_file referenced");
    if (!f4::assets::is_asset_ref(terrain_file)) {
        throw std::runtime_error(
            "WorldState::load_terrain_via_assets: terrain_file is not in "
            "the @asset: form (call load_terrain() for legacy bare "
            "filenames): '" + terrain_file + "'");
    }
    f4::assets::AssetId id = f4::assets::parse_asset_ref(terrain_file);
    auto path = root.resolve_existing(id);
    if (path.empty()) {
        const auto raw = root.resolve_asset_path(id);
        if (raw.empty()) {
            throw std::runtime_error(
                "WorldState::load_terrain_via_assets: asset " + id.to_string() +
                " is not in the manifest");
        }
        throw std::runtime_error(
            "WorldState::load_terrain_via_assets: asset " + id.to_string() +
            " is in the manifest but file missing on disk: " + raw.string());
    }
    terrain.load_terrain_json(path);
    terrain_loaded = true;
}

} // namespace f4::world

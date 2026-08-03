// f4-world/src/world_state.cpp — JSON loader for WorldState.
//
// Uses f4-json's dependency-free Reader to walk the world-state schema
// emitted by f4-world-convert. The reader is shape-compatible with the
// hand-rolled JsonReader that lived here previously — the field parsers
// below are unchanged from the original implementation; only the local
// class definition has been replaced with #include <f4/json/reader.hpp>.

#include <f4/world/world_state.hpp>

#include <f4/json/reader.hpp>

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
        else if (k == "objective_type")  o.objective_type  = static_cast<uint8_t>(r.read_int());
        else if (k == "x")               o.x               = static_cast<int16_t>(r.read_int());
        else if (k == "y")               o.y               = static_cast<int16_t>(r.read_int());
        else if (k == "z")               o.z               = static_cast<float>(r.read_number());
        else if (k == "owner")           o.owner           = static_cast<uint8_t>(r.read_int());
        else if (k == "priority")        o.priority        = static_cast<uint8_t>(r.read_int());
        else if (k == "nameid")          o.nameid          = static_cast<int16_t>(r.read_int());
        else if (k == "camp_id")         o.camp_id         = static_cast<int16_t>(r.read_int());
        else if (k == "entity_type")     o.entity_type     = static_cast<uint16_t>(r.read_int());
        else if (k == "links") {
            // Array of {n, c, road, rail} objects.
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
                    else if (lk == "road") {
                        r.skip_ws();
                        if (r.consume('t')) { link.is_road = true;  r.skip_value(); }
                        else                { link.is_road = false; r.skip_value(); }
                    }
                    else if (lk == "rail") {
                        r.skip_ws();
                        if (r.consume('t')) { link.is_rail = true;  r.skip_value(); }
                        else                { link.is_rail = false; r.skip_value(); }
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
        else                             r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
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
        else if (k == "losses")         u.losses         = static_cast<uint8_t>(r.read_int());
        else if (k == "supply")         u.supply         = static_cast<uint8_t>(r.read_int());
        else if (k == "morale")         u.morale         = static_cast<uint8_t>(r.read_int());
        else if (k == "fatigue")        u.fatigue        = static_cast<uint8_t>(r.read_int());
        else if (k == "elements")       u.elements       = static_cast<uint8_t>(r.read_int());
        else if (k == "fuel")           u.fuel           = static_cast<int32_t>(r.read_int());
        else if (k == "parent_id")      u.parent_id      = static_cast<uint32_t>(r.read_int());
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
                    else if (pk == "status")   p.status         = static_cast<uint8_t>(r.read_int());
                    else if (pk == "aa")       p.aa_kills       = static_cast<uint8_t>(r.read_int());
                    else if (pk == "ag")       p.ag_kills       = static_cast<uint8_t>(r.read_int());
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

} // namespace f4::world

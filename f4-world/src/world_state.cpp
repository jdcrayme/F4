// f4-world/src/world_state.cpp — JSON loader for WorldState.
//
// A minimal, dependency-free JSON reader sufficient for the world-state
// schema emitted by f4-world-convert. We parse the fields we need and skip
// everything else. This keeps f4-world free of external JSON dependencies;
// if richer querying is ever needed, nlohmann/json is available in the
// build tree and can be linked instead.

#include <f4/world/world_state.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace f4::world {

namespace {

// Tiny recursive-descent JSON reader. Handles objects, arrays, strings,
// numbers (integers), booleans, null. Sufficient for the world JSON schema.
class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s), pos_(0) {}

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool peek(char ch) {
        skip_ws();
        return pos_ < s_.size() && s_[pos_] == ch;
    }

    void expect(char ch) {
        skip_ws();
        if (pos_ >= s_.size() || s_[pos_] != ch)
            throw std::runtime_error(std::string("JSON: expected '") + ch + "'");
        ++pos_;
    }

    bool consume(char ch) {
        if (peek(ch)) { ++pos_; return true; }
        return false;
    }

    std::string read_string() {
        skip_ws();
        expect('"');
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
                char esc = s_[pos_ + 1];
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    default: out += esc; break;
                }
                pos_ += 2;
            } else {
                out += s_[pos_++];
            }
        }
        expect('"');
        return out;
    }

    long read_int() {
        skip_ws();
        std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        if (start == pos_) throw std::runtime_error("JSON: expected number");
        return std::strtol(s_.c_str() + start, nullptr, 10);
    }

    double read_number() {
        skip_ws();
        std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        if (start == pos_) throw std::runtime_error("JSON: expected number");
        return std::strtod(s_.c_str() + start, nullptr);
    }

    // Skip the value at the current position (for fields we don't need).
    void skip_value() {
        skip_ws();
        if (pos_ >= s_.size()) return;
        char c = s_[pos_];
        if (c == '"') { (void)read_string(); }
        else if (c == '{') {
            ++pos_;
            skip_ws();
            if (consume('}')) return;
            for (;;) {
                (void)read_string();
                expect(':');
                skip_value();
                if (consume('}')) break;
                expect(',');
            }
        } else if (c == '[') {
            ++pos_;
            skip_ws();
            if (consume(']')) return;
            for (;;) {
                skip_value();
                if (consume(']')) break;
                expect(',');
            }
        } else {
            while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' &&
                   s_[pos_] != ']' && !std::isspace(static_cast<unsigned char>(s_[pos_])))
                ++pos_;
        }
    }

private:
    const std::string& s_;
    std::size_t pos_;
};

// Parse a team slot object starting at the '{'.
TeamState parse_team(JsonReader& r) {
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

ObjectiveState parse_objective(JsonReader& r) {
    ObjectiveState o;
    r.skip_ws(); r.expect('{');
    if (r.consume('}')) return o;
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "type")         o.type         = static_cast<int16_t>(r.read_int());
        else if (k == "x")            o.x            = static_cast<int16_t>(r.read_int());
        else if (k == "y")            o.y            = static_cast<int16_t>(r.read_int());
        else if (k == "z")            o.z            = static_cast<float>(r.read_number());
        else if (k == "owner")        o.owner        = static_cast<uint8_t>(r.read_int());
        else if (k == "priority")     o.priority     = static_cast<uint8_t>(r.read_int());
        else if (k == "nameid")       o.nameid       = static_cast<int16_t>(r.read_int());
        else if (k == "camp_id")      o.camp_id      = static_cast<int16_t>(r.read_int());
        else if (k == "entity_type")  o.entity_type  = static_cast<uint16_t>(r.read_int());
        else                          r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return o;
}

UnitState parse_unit(JsonReader& r) {
    UnitState u;
    r.skip_ws(); r.expect('{');
    if (r.consume('}')) return u;
    for (;;) {
        std::string k = r.read_string();
        r.expect(':');
        if      (k == "type")           u.type           = static_cast<int16_t>(r.read_int());
        else if (k == "unit_class") {
            // String → enum. Unknown if not recognized.
            std::string s = r.read_string();
            if      (s == "battalion") u.unit_class = UnitClass::Battalion;
            else if (s == "brigade")   u.unit_class = UnitClass::Brigade;
            else if (s == "squadron")  u.unit_class = UnitClass::Squadron;
            else if (s == "taskforce") u.unit_class = UnitClass::TaskForce;
            else if (s == "flight")    u.unit_class = UnitClass::Flight;
            else if (s == "package")   u.unit_class = UnitClass::Package;
            else                        u.unit_class = UnitClass::Unknown;
        }
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
        else                            r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
    return u;
}

void parse_campaign_field(JsonReader& r, const std::string& key, CampaignState& c) {
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
    JsonReader r(json);

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

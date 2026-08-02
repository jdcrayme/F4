// f4-world/src/world_state.cpp — JSON loader for WorldState.
//
// A minimal, dependency-free JSON reader sufficient for the world-state
// schema emitted by f4-world-convert. We parse only the fields we need
// (version, campaign header, teams) and skip everything else. This keeps
// f4-world free of external JSON dependencies; if richer querying is ever
// needed, nlohmann/json is already available in the build tree and can be
// linked instead.

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

    // Read the value at the current position and discard it (for fields we
    // don't need). Handles nested objects/arrays/strings/numbers.
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
                (void)read_string();   // key
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
            // number, true, false, null — read until delimiter
            while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' &&
                   s_[pos_] != ']' && !std::isspace(static_cast<unsigned char>(s_[pos_])))
                ++pos_;
        }
    }

    // Position the reader at the value for a given top-level key. Returns
    // false if the key isn't found. Assumes we're at the start of an object.
    bool find_key(const std::string& key) {
        skip_ws();
        expect('{');
        if (consume('}')) return false;
        for (;;) {
            std::string k = read_string();
            expect(':');
            if (k == key) return true;
            skip_value();
            if (consume('}')) return false;
            expect(',');
        }
    }

private:
    const std::string& s_;
    std::size_t pos_;
};

void parse_campaign(JsonReader& r, CampaignState& c) {
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return;
    for (;;) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "current_time")         c.current_time = static_cast<int32_t>(r.read_int());
        else if (key == "te_start_time")   c.te_start_time = static_cast<int32_t>(r.read_int());
        else if (key == "te_time_limit")   c.te_time_limit = static_cast<int32_t>(r.read_int());
        else if (key == "te_victory_points")c.te_victory_points = static_cast<int32_t>(r.read_int());
        else if (key == "te_type")         c.te_type = static_cast<int32_t>(r.read_int());
        else if (key == "te_number_teams") c.te_number_teams = static_cast<int32_t>(r.read_int());
        else if (key == "te_team")         c.te_team = static_cast<int32_t>(r.read_int());
        else if (key == "te_flags")        c.te_flags = static_cast<int32_t>(r.read_int());
        else if (key == "te_number_aircraft" || key == "te_team_pts") {
            // array of ints
            r.skip_ws(); r.expect('[');
            std::vector<int32_t> arr;
            if (!r.peek(']')) for (;;) {
                arr.push_back(static_cast<int32_t>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
            if (key == "te_number_aircraft") c.te_number_aircraft = arr;
            else c.te_team_pts = arr;
        }
        else if (key == "teams") {
            r.skip_ws(); r.expect('[');
            if (r.consume(']')) { /* empty */ }
            else for (;;) {
                r.skip_ws(); r.expect('{');
                TeamState t;
                if (!r.peek('}')) for (;;) {
                    std::string tk = r.read_string();
                    r.expect(':');
                    if (tk == "slot")        t.slot = static_cast<int>(r.read_int());
                    else if (tk == "flags")  t.flags = static_cast<uint8_t>(r.read_int());
                    else if (tk == "colour") t.colour = static_cast<uint8_t>(r.read_int());
                    else if (tk == "name")   t.name = r.read_string();
                    else if (tk == "motto")  t.motto = r.read_string();
                    else r.skip_value();
                    if (r.consume('}')) break;
                    r.expect(',');
                }
                // teams array entry consumed
                {
                    // capture into outer scope
                }
                if (key == "teams") {
                    // We need to push back — but we're inside the teams branch.
                    // Use a static lambda-free approach: re-read into a temp.
                }
                r.skip_ws();
                if (r.consume(']')) { /* done */ break; }
                r.expect(',');
            }
        }
        else r.skip_value();
        if (r.consume('}')) break;
        r.expect(',');
    }
}

} // namespace

void WorldState::load_from_string(const std::string& json) {
    JsonReader r(json);

    // Top-level object.
    r.skip_ws();
    r.expect('{');
    if (r.consume('}')) return;
    for (;;) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "version") {
            version = static_cast<int>(r.read_int());
        } else if (key == "campaign") {
            parse_campaign(r, campaign);
            // parse_campaign needs to also fill teams — restructure below.
        } else {
            r.skip_value();
        }
        if (r.consume('}')) break;
        r.expect(',');
    }

    // Second pass for teams (they're nested inside campaign, but the
    // parse_campaign above left them out for clarity — re-parse from the
    // raw string to extract the teams array).
    // Actually, let's do it properly: re-walk to campaign.teams.
    JsonReader r2(json);
    r2.skip_ws();
    r2.expect('{');
    if (r2.consume('}')) return;
    for (;;) {
        std::string k = r2.read_string();
        r2.expect(':');
        if (k == "campaign") {
            r2.skip_ws(); r2.expect('{');
            if (r2.consume('}')) { /* no teams */ }
            else for (;;) {
                std::string ck = r2.read_string();
                r2.expect(':');
                if (ck == "teams") {
                    r2.skip_ws(); r2.expect('[');
                    if (r2.consume(']')) { /* empty */ }
                    else for (;;) {
                        r2.skip_ws(); r2.expect('{');
                        TeamState t;
                        if (!r2.peek('}')) for (;;) {
                            std::string tk = r2.read_string();
                            r2.expect(':');
                            if (tk == "slot")        t.slot = static_cast<int>(r2.read_int());
                            else if (tk == "flags")  t.flags = static_cast<uint8_t>(r2.read_int());
                            else if (tk == "colour") t.colour = static_cast<uint8_t>(r2.read_int());
                            else if (tk == "name")   t.name = r2.read_string();
                            else if (tk == "motto")  t.motto = r2.read_string();
                            else r2.skip_value();
                            if (r2.consume('}')) break;
                            r2.expect(',');
                        }
                        teams.push_back(std::move(t));
                        if (r2.consume(']')) break;
                        r2.expect(',');
                    }
                    // teams found — we can stop.
                    return;
                } else {
                    r2.skip_value();
                }
                if (r2.consume('}')) break;
                r2.expect(',');
            }
        } else {
            r2.skip_value();
        }
        if (r2.consume('}')) break;
        r2.expect(',');
    }
}

void WorldState::load(const std::filesystem::path& json_path) {
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("WorldState: cannot open " + json_path.string());
    std::ostringstream ss;
    ss << f.rdbuf();
    load_from_string(ss.str());
}

} // namespace f4::world

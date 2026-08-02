// f4-world-vis/cli/world2map.cpp
//
// CLI: read a world JSON (from cam2json) and emit an HTML map (default) or
// raw SVG. Mirrors f4-world-convert's cam2json pattern: thin main() that
// calls into the library.
//
//   world2map save1.json            -> save1.html
//   world2map save1.json out.svg    -> out.svg (raw SVG, no HTML wrapper)
//   world2map save1.json out.html   -> out.html (standalone, browser-openable)

#include <f4/vis/svg_map.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Minimal JSON reader for the world-map subset (objectives + units arrays).
// We only need x, y, owner, type, dest_x, dest_y from each item. This is a
// purpose-built pull parser, not a general JSON library.
namespace {

struct SimpleJson {
    std::string s;
    std::size_t i = 0;

    void skip_ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool peek(char c) { skip_ws(); return i < s.size() && s[i] == c; }
    void expect(char c) { skip_ws(); if (i >= s.size() || s[i] != c) { std::fprintf(stderr, "JSON parse error at %zu: expected '%c'\n", i, c); std::exit(1); } ++i; }

    std::string read_string() {
        skip_ws(); expect('"');
        std::string out;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) { out += s[i+1]; i += 2; }
            else out += s[i++];
        }
        expect('"');
        return out;
    }

    long read_int() {
        skip_ws();
        std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        return std::strtol(s.c_str() + start, nullptr, 10);
    }

    double read_num() {
        skip_ws();
        std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        return std::strtod(s.c_str() + start, nullptr);
    }

    // Skip an arbitrary value (object, array, string, number, bool, null).
    void skip_value() {
        skip_ws();
        if (i >= s.size()) return;
        char c = s[i];
        if (c == '"') { (void)read_string(); }
        else if (c == '{') {
            ++i;
            if (peek('}')) { ++i; return; }
            for (;;) { (void)read_string(); expect(':'); skip_value(); if (peek('}')) { ++i; return; } expect(','); }
        }
        else if (c == '[') {
            ++i;
            if (peek(']')) { ++i; return; }
            for (;;) { skip_value(); if (peek(']')) { ++i; return; } expect(','); }
        }
        else { while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    }

    // Find a top-level key in the current object and position i at its value.
    bool find_key(const std::string& key) {
        skip_ws(); expect('{');
        if (peek('}')) { return false; }
        for (;;) {
            std::string k = read_string();
            expect(':');
            if (k == key) return true;
            skip_value();
            if (peek('}')) return false;
            expect(',');
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "usage: world2map <world.json> [output.html|output.svg]\n";
        return 2;
    }
    const std::string in_path = argv[1];
    std::string out_path = (argc == 3) ? argv[2] : in_path;
    if (out_path.find('.') == std::string::npos) out_path += ".html";
    const bool want_svg = out_path.size() >= 4 && out_path.substr(out_path.size()-4) == ".svg";

    // Read the JSON file.
    std::ifstream f(in_path);
    if (!f) { std::cerr << "world2map: cannot open " << in_path << "\n"; return 1; }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    SimpleJson r{json};
    std::vector<f4::vis::ObjectivePoint> objectives;
    std::vector<f4::vis::UnitPoint> units;

    // Top-level object.
    r.skip_ws(); r.expect('{');
    while (!r.peek('}')) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "objectives") {
            // objectives: { "count":N, "decoded":N, "items":[ {...}, ... ] }
            r.skip_ws(); r.expect('{');
            while (!r.peek('}')) {
                std::string ok = r.read_string();
                r.expect(':');
                if (ok == "items") {
                    r.skip_ws(); r.expect('[');
                    if (!r.peek(']')) for (;;) {
                        f4::vis::ObjectivePoint ob;
                        r.skip_ws(); r.expect('{');
                        while (!r.peek('}')) {
                            std::string fk = r.read_string();
                            r.expect(':');
                            if (fk == "x") ob.x = static_cast<int16_t>(r.read_int());
                            else if (fk == "y") ob.y = static_cast<int16_t>(r.read_int());
                            else if (fk == "owner") ob.owner = static_cast<uint8_t>(r.read_int());
                            else if (fk == "type") ob.type = static_cast<int16_t>(r.read_int());
                            else if (fk == "nameid") ob.nameid = static_cast<int16_t>(r.read_int());
                            else if (fk == "priority") ob.priority = static_cast<uint8_t>(r.read_int());
                            else r.skip_value();
                            if (r.peek('}')) break;
                            r.expect(',');
                        }
                        r.expect('}');
                        objectives.push_back(ob);
                        if (r.peek(']')) break;
                        r.expect(',');
                    }
                    r.expect(']');
                } else {
                    r.skip_value();
                }
                if (r.peek('}')) break;
                r.expect(',');
            }
            r.expect('}');
        } else if (key == "units") {
            r.skip_ws(); r.expect('{');
            while (!r.peek('}')) {
                std::string uk = r.read_string();
                r.expect(':');
                if (uk == "items") {
                    r.skip_ws(); r.expect('[');
                    if (!r.peek(']')) for (;;) {
                        f4::vis::UnitPoint u;
                        r.skip_ws(); r.expect('{');
                        while (!r.peek('}')) {
                            std::string fk = r.read_string();
                            r.expect(':');
                            if (fk == "x") u.x = static_cast<int16_t>(r.read_int());
                            else if (fk == "y") u.y = static_cast<int16_t>(r.read_int());
                            else if (fk == "owner") u.owner = static_cast<uint8_t>(r.read_int());
                            else if (fk == "type") u.type = static_cast<int16_t>(r.read_int());
                            else if (fk == "dest_x") u.dest_x = static_cast<int16_t>(r.read_int());
                            else if (fk == "dest_y") u.dest_y = static_cast<int16_t>(r.read_int());
                            else r.skip_value();
                            if (r.peek('}')) break;
                            r.expect(',');
                        }
                        r.expect('}');
                        units.push_back(u);
                        if (r.peek(']')) break;
                        r.expect(',');
                    }
                    r.expect(']');
                } else {
                    r.skip_value();
                }
                if (r.peek('}')) break;
                r.expect(',');
            }
            r.expect('}');
        } else {
            r.skip_value();
        }
        if (r.peek('}')) break;
        r.expect(',');
    }

    auto colors = f4::vis::default_team_colors();
    std::string output;
    if (want_svg) {
        output = f4::vis::render_svg(objectives, units, colors);
    } else {
        output = f4::vis::render_html(objectives, units, colors);
    }

    std::ofstream out(out_path);
    if (!out) { std::cerr << "world2map: cannot write " << out_path << "\n"; return 1; }
    out << output;
    std::cout << "wrote " << out_path << " (" << output.size() << " bytes): "
              << objectives.size() << " objectives, " << units.size() << " units\n";
    return 0;
}

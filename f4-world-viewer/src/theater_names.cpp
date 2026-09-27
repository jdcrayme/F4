// f4-world-viewer/src/theater_names.cpp
//
// The theater name table's loader — see theater_names.hpp. Parsing via
// f4-json (the settings.cpp pattern: the project's own writer output,
// skipped unknown fields, throw-on-malformed).

#include "theater_names.hpp"

#include <f4/json/reader.hpp>

#include <stdexcept>

namespace f4::viewer {

std::string theater_name_for_id(const std::vector<std::string>& names,
                                int nameid) {
    if (nameid < 0 || nameid >= static_cast<int>(names.size())) {
        return {};
    }
    return names[static_cast<std::size_t>(nameid)];
}

std::vector<std::string> load_theater_names(const std::string& json) {
    f4::json::Reader r(json);
    r.skip_ws();
    r.expect('{');

    std::vector<std::string> names;
    bool saw_format = false;
    r.skip_ws();
    if (r.peek('}')) {
        r.consume('}');
        throw std::runtime_error(
            "theater names: empty document (no format tag)");
    }
    for (;;) {
        const std::string key = r.read_string();
        r.expect(':');
        if (key == "format") {
            if (r.read_string() != "f4.theater.names/1") {
                throw std::runtime_error(
                    "theater names: unknown format tag");
            }
            saw_format = true;
        } else if (key == "names") {
            r.skip_ws();
            r.expect('[');
            r.skip_ws();
            if (!r.peek(']')) {
                for (;;) {
                    names.push_back(r.read_string());
                    r.skip_ws();
                    if (r.consume(',')) continue;
                    r.expect(']');
                    break;
                }
            } else {
                r.consume(']');
            }
        } else {
            r.skip_value();
        }
        r.skip_ws();
        if (r.consume(',')) continue;
        r.expect('}');
        break;
    }
    if (!saw_format) {
        throw std::runtime_error(
            "theater names: no format tag (not f4.theater.names/1)");
    }
    return names;
}

} // namespace f4::viewer

// f4-json/include/f4/json/reader.hpp
//
// Minimal, dependency-free JSON reader sufficient for the schemas emitted
// by f4-convert / f4-world-convert / f4-terrain-convert.
//
// This is NOT a general-purpose JSON library. It is a small recursive-descent
// parser tuned for the project's own JSON output: objects, arrays, strings,
// numbers (integers and doubles), booleans, and null. It supports two
// consumption styles:
//
//   1. Structural: peek(), expect(), consume() — drive the parser by hand
//      for the fields you care about, skip_value() the rest. This is what
//      f4-world and f4-terrain do today.
//   2. Token: read_string(), read_int(), read_number() — pull typed values
//      at known positions.
//
// Why not nlohmann/json? Two reasons:
//   - f4-geo, f4-entities, f4-messaging, and f4-state-machine all keep a
//     "zero deps beyond stdlib" stance so they can be lifted into any host
//     project. f4-world and f4-terrain inherit that stance.
//   - We control both ends of the wire format. A 200-line reader is
//     sufficient, simpler to audit, and faster to compile than a header
//     that ships 25k lines of template metaprogramming.
//
// Where nlohmann/json is already in use (f4-convert, f4-data) it stays —
// those libraries need rich random-access JSON for the aircraft config
// schema (nested arrays of aero tables, optional fields, type coercion).
// f4-json is for the simpler "walk a known schema" pattern.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace f4::json {

// ============================================================================
// Reader — recursive-descent JSON parser.
//
// Construction:
//   f4::json::Reader r(some_string_view_or_string);
//   r.skip_ws();
//   r.expect('{');
//   ...
//
// Error handling:
//   All structural mismatches throw std::runtime_error with the position
//   and the expected character. Callers wrap parse entry points in
//   try/catch and translate to library-specific exceptions if desired.
//
// String lifetime:
//   The Reader holds a const reference to the source string. The caller
//   must keep the source alive for the Reader's lifetime. This matches
//   the existing JsonReader pattern in f4-world / f4-terrain and avoids
//   a string copy on every parse.
// ============================================================================
class Reader {
public:
    explicit Reader(const std::string& s) : s_(s), pos_(0) {}

    // ------------------- position / diagnostics ---------------------------

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] const std::string& source() const noexcept { return s_; }

    // ------------------- whitespace / structural --------------------------

    void skip_ws() {
        while (pos_ < s_.size() &&
               std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    bool peek(char ch) {
        skip_ws();
        return pos_ < s_.size() && s_[pos_] == ch;
    }

    void expect(char ch) {
        skip_ws();
        if (pos_ >= s_.size() || s_[pos_] != ch) {
            throw std::runtime_error(
                std::string("f4::json: expected '") + ch + "' at position " +
                std::to_string(pos_));
        }
        ++pos_;
    }

    bool consume(char ch) {
        if (peek(ch)) { ++pos_; return true; }
        return false;
    }

    // ------------------- typed readers ------------------------------------

    // Read a JSON string (with escape decoding). Throws on malformed.
    std::string read_string() {
        skip_ws();
        expect('"');
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
                char esc = s_[pos_ + 1];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        // \uXXXX — basic ASCII range only (code <= 0x7F).
                        // Higher code points would require surrogate-pair
                        // handling; the project's own emitters never
                        // produce non-ASCII \u escapes (paths and team
                        // names are passed through as raw UTF-8 bytes).
                        if (pos_ + 5 >= s_.size()) {
                            throw std::runtime_error(
                                "f4::json: truncated \\u escape");
                        }
                        char buf[5] = {
                            s_[pos_+2], s_[pos_+3], s_[pos_+4], s_[pos_+5], 0
                        };
                        char* end = nullptr;
                        long code = std::strtol(buf, &end, 16);
                        if (end != buf + 4) {
                            throw std::runtime_error(
                                "f4::json: malformed \\u escape");
                        }
                        out += static_cast<char>(code);
                        pos_ += 6;
                        continue;  // we already advanced past the escape
                    }
                    default:
                        out += esc;
                        break;
                }
                pos_ += 2;
            } else {
                out += s_[pos_++];
            }
        }
        expect('"');
        return out;
    }

    // Read a JSON integer (may be negative). Throws if no digits present.
    long read_int() {
        skip_ws();
        std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < s_.size() &&
               std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
        if (start == pos_ ||
            // Reject a bare sign with no digits.
            (pos_ == start + 1 &&
             (s_[start] == '-' || s_[start] == '+'))) {
            throw std::runtime_error(
                "f4::json: expected integer at position " +
                std::to_string(start));
        }
        return std::strtol(s_.c_str() + start, nullptr, 10);
    }

    // Read a JSON number (integer or float, possibly in scientific notation).
    // Throws if no number is present.
    double read_number() {
        skip_ws();
        std::size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                s_[pos_] == '+' || s_[pos_] == '-')) {
            ++pos_;
        }
        if (start == pos_) {
            throw std::runtime_error(
                "f4::json: expected number at position " +
                std::to_string(start));
        }
        return std::strtod(s_.c_str() + start, nullptr);
    }

    // Skip the value at the current position: any of string / number /
    // object / array / true / false / null. Used to ignore fields we don't
    // care about without parsing them.
    void skip_value() {
        skip_ws();
        if (pos_ >= s_.size()) return;
        char c = s_[pos_];
        if (c == '"') {
            (void)read_string();
        } else if (c == '{') {
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
            // Bare token: true, false, null, or a number. Advance until
            // we hit a structural delimiter or whitespace.
            while (pos_ < s_.size() &&
                   s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ']' &&
                   !std::isspace(static_cast<unsigned char>(s_[pos_]))) {
                ++pos_;
            }
        }
    }

private:
    const std::string& s_;
    std::size_t pos_;
};

} // namespace f4::json

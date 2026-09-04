// f4-json/include/f4/json/writer.hpp
//
// Minimal JSON writer — emits compact, human-readable documents. Mirrors
// the JsonWriter pattern that lived in f4-terrain/src/terrain_data.cpp.
//
// Use:
//   f4::json::Writer w;
//   w.raw("{\n");
//   w.string_key("theater", "Korea"); w.raw(",\n");
//   w.number_key("width",  128);      w.raw(",\n");
//   w.raw("}\n");
//   std::string out = w.str();
//
// The Writer does NOT enforce structure — it's a string builder that
// knows how to escape strings and format numbers. Callers are responsible
// for commas, braces, and indentation. This keeps it tiny (~80 lines) and
// lets each emitter pick its own pretty-print style.
//
// Why not nlohmann/json? Same reasoning as Reader (see reader.hpp):
// the consumers control both ends of the wire format and want zero deps.
// nlohmann/json stays in f4-convert / f4-data where the richer random-
// access API is needed.

#pragma once

#include <concepts>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace f4::json {

class Writer {
public:
    void put(char c) { buf_.push_back(c); }
    void put(const char* s) { while (*s) buf_.push_back(*s++); }
    void put(const std::string& s) { buf_.append(s); }
    void put(std::string_view s) { buf_.append(s); }

    // Emit a raw string verbatim — no escaping. Use for structural
    // punctuation: {, }, [, ], ,, :, whitespace, indentation.
    void raw(const char* s) { put(s); }
    void raw(std::string_view s) { put(s); }

    // Emit a properly-escaped JSON string literal (including the
    // surrounding quotes). Use for any value that came from user input
    // or external data — paths, team names, theater names, etc.
    void string(std::string_view s) {
        put('"');
        for (char ch : s) {
            switch (ch) {
                case '"':  put("\\\""); break;
                case '\\': put("\\\\"); break;
                case '\b': put("\\b");  break;
                case '\f': put("\\f");  break;
                case '\n': put("\\n");  break;
                case '\r': put("\\r");  break;
                case '\t': put("\\t");  break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char tmp[8];
                        std::snprintf(tmp, sizeof(tmp), "\\u%04x",
                                      static_cast<unsigned char>(ch));
                        put(tmp);
                    } else {
                        put(ch);
                    }
            }
        }
        put('"');
    }

    // Emit an integer (no quotes). Templated to accept any integral type
    // (int, long, uint32_t, uint8_t, size_t, ...) without ambiguity.
    // Promote to (unsigned) long long before formatting: `long` is
    // 32-bit on Windows, so a naive cast wraps every 64-bit value
    // (entity ids are generation<<32 | index — 2^32 became 0) and the
    // divergence is silent. Values within 32 bits emit identical bytes,
    // so existing goldens are untouched. Mirrors read_int()'s own
    // long long contract — the pair round-trips 64-bit everywhere.
    template <typename T>
        requires std::is_integral_v<T>
    void number(T v) {
        char tmp[32];
        if constexpr (std::is_signed_v<T>) {
            std::snprintf(tmp, sizeof(tmp), "%lld",
                          static_cast<long long>(v));
        } else {
            std::snprintf(tmp, sizeof(tmp), "%llu",
                          static_cast<unsigned long long>(v));
        }
        put(tmp);
    }

    // Emit a double (no quotes). Uses %g which picks the shortest
    // representation that round-trips — sufficient for terrain elevations
    // and aircraft aero coefficients.
    void number(double v) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%.17g", v);
        put(tmp);
    }

    // Convenience: "key": value (assumes the caller manages commas).
    void string_key(std::string_view key, std::string_view value) {
        string(key);
        put(':');
        string(value);
    }
    template <typename T>
        requires std::is_integral_v<T>
    void number_key(std::string_view key, T value) {
        string(key);
        put(':');
        number(value);
    }
    void number_key(std::string_view key, double value) {
        string(key);
        put(':');
        number(value);
    }

    [[nodiscard]] const std::string& str() const& noexcept { return buf_; }
    [[nodiscard]] std::string str() && noexcept { return std::move(buf_); }

    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

private:
    std::string buf_;
};

// Free-function form of the JSON string escaping used by Writer::string().
//
// Returns the escaped BODY of the string literal — WITHOUT the surrounding
// double quotes. This matches how the existing emitters (e.g.
// f4-world-convert/src/world_json.cpp) call it:
//
//     o << "  \"theater\": \"" << escape_string(opts.theater) << "\",\n";
//                              ^^^ literal opening quote      ^^^ literal closing quote
//
// Callers that want the full quoted literal (body + quotes) should use
// Writer::string() instead, which delegates to this function and adds the
// quotes. The two are kept consistent by sharing the same escape table.
[[nodiscard]] inline std::string escape_string(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        switch (ch) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char tmp[8];
                    std::snprintf(tmp, sizeof(tmp), "\\u%04x",
                                  static_cast<unsigned char>(ch));
                    out.append(tmp);
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

} // namespace f4::json

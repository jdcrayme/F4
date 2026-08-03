// f4-json/src/f4_json.cpp
//
// Today f4-json is header-only — Reader and Writer are entirely templated
// or inline. This .cpp exists so the library has a targettable translation
// unit for future ABI surfaces (e.g. a shared diagnostics hook for parse
// errors), and so `add_library(f4-json STATIC src/f4_json.cpp)` follows
// the same pattern as f4-messaging rather than being an INTERFACE library.

namespace f4::json {
// (intentionally empty — see file header)
} // namespace f4::json

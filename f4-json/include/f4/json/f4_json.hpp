// f4-json/include/f4/json/f4_json.hpp
//
// Umbrella header for f4-json — the minimal dependency-free JSON
// reader/writer used by f4-world, f4-terrain, f4-world-viewer/settings,
// and (future) f4-campaign's mission profile loader.
//
// Components:
//   f4::json::Reader — recursive-descent parser for the project's own
//                      JSON output (objects/arrays/strings/numbers/bool/null)
//   f4::json::Writer — compact string builder with proper string escaping
//
// Zero dependencies. C++20. Header-only.

#pragma once

#include <f4/json/reader.hpp>
#include <f4/json/writer.hpp>

// f4-world-viewer/src/theater_names.hpp
//
// The theater name table — the strings ObjectivePriorityComponent::nameid
// (and SquadronUIInfo::name_id) index. The F4 original resolves them
// through a <theater>.idx (short offsets) + <theater>.wch (string stream)
// pair (FreeFalcon CAMPLIB/Name.cpp); scripts/export_names.py rewrites
// that pair as Data/Theater/<theater>/names.json for the no-binary
// runtime, and this module loads that document.
//
// Pure + ImGui-free: unit-tested (test_theater_names.cpp).

#pragma once

#include <string>
#include <vector>

namespace f4::viewer {

// The resolved name for a nameid: bounds-checked (an id past the table —
// saves made under a name table larger than this install's — returns ""),
// so callers fall back to their own id rendering.
std::string theater_name_for_id(const std::vector<std::string>& names,
                                int nameid);

// Parse an f4.theater.names/1 document (see scripts/export_names.py).
// Throws std::runtime_error on a malformed document (wrong format tag,
// missing names array) — the loader contract matches the symbol
// library's: the caller owns the fallback.
std::vector<std::string> load_theater_names(const std::string& json);

} // namespace f4::viewer

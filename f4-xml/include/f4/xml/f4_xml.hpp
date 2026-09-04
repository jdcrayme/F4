// f4-xml/include/f4/xml/f4_xml.hpp
//
// PUBLIC HEADER — umbrella for f4-xml, the project's shared XML parser.
//
// f4-xml vendors pugixml v1.15 (MIT — see third_party/pugixml/LICENSE.md)
// as a single translation unit and exposes it project-wide under the
// f4::xml namespace alias. Two consumers drove this library:
//
//   f4-renderer  — SVG symbol import for the data-driven SymbolLibrary
//                  (f4_symbols.json authoring round-trip; see
//                  include/f4/renderer/svg_import.hpp)
//   f4-world-convert (future) — BMS installs ship every binary Falcon4.*
//                  data table as XML (FALCON4_CT.XML 5.9 MB class table,
//                  OCD/UCD/VCD/... and per-instance SSD/UCD files). The
//                  worklog tracks this as tasks BMS-CT-1 / BMS-DATA-1;
//                  Docs/FALCON4_FILE_LAYOUT.md §"TerrData" documents the
//                  file set.
//
// Vendoring (not FetchContent) matches the repo's convention for small
// single-file deps (see f4-world-viewer/third_party/tinyfiledialogs): the
// sources are committed, reviewable, and build with no network access.
//
// Usage:
//     #include <f4/xml/f4_xml.hpp>
//     f4::xml::xml_document doc;
//     doc.load_string(svg_source);            // pugi::parse_default
//     for (auto node : doc.child("svg").children()) { ... }
//
// The alias is exactly `pugi` — see pugixml.hpp for the full API.

#pragma once

#include "pugixml.hpp"

namespace f4 {
namespace xml = pugi;
} // namespace f4

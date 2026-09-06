// f4-world-types/include/f4/world_types/world_types.hpp
//
// Runtime-safe world type constants + the JSON class table loader.
//
// Tranche 0d (NO_BINARY_RUNTIME_PLAN.md): the runtime subset of
// f4-world-convert's theater/class-table definitions, extracted into a
// neutral library that the runtime (f4-simulation, f4-renderer) can link
// WITHOUT pulling in the legacy binary parsers.
//
// What lives here (runtime-safe — no binary parsing):
//   - ObjectiveType / PointType / PointListType enum constants
//   - ClassTableEntry + ClassTable (JSON loader only: load_json / load_auto)
//   - unit_subtype_name() helper
//
// What stays in f4-world-convert (importer-only — binary parsing):
//   - ClassTable::load() (the FALCON4.ct binary decoder)
//   - find_class_table() (install-layout search)
//   - The theater-object struct parsers (ObjectiveClassData, PtHeaderData, …)
//
// Dependencies: f4-json (for the JSON reader), standard library only.
// C++20.

#pragma once

#include <f4/world_types/aii_config.hpp>
#include <f4/world_types/class_table.hpp>
#include <f4/world_types/layout_types.hpp>

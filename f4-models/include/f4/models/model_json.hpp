// f4-models/include/f4/models/model_json.hpp
//
// JSON export functions for model data. Uses f4-json Writer internally.
// These produce JSON strings suitable for CLI output, debugging, or
// interchange with Python tools.

#pragma once

#include <f4/models/model_database.hpp>

#include <string>

namespace f4::models {

/// Export the full model list as JSON.
[[nodiscard]] std::string model_list_json(const ModelDatabase& db);

/// Export a single model record as JSON.
[[nodiscard]] std::string model_record_json(const ModelRecord& m);

/// Export a BSP tree structure as JSON (for debugging).
/// @param max_nodes  Maximum number of nodes to include (0 = all).
[[nodiscard]] std::string bsp_tree_json(const BspTree& tree, int max_nodes = 0);

} // namespace f4::models

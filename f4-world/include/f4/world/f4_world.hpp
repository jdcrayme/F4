// f4-world/include/f4/world/f4_world.hpp — umbrella include.
//
// Public API for f4-world:
//   - load() / load_from_string() — convenience functions that hide WorldState
//   - populate_world() / populate_campaign() / populate_teams() /
//     populate_objectives() / populate_units() — bridge functions
//   - IDataSource interfaces — for custom data source adapters
//   - WorldStateAdapters — convenience adapter bundle
//
// WorldState itself is an implementation detail and lives in
// <f4/world/detail/world_state.hpp>. Include that header explicitly
// only if you need direct WorldState access (e.g., for terrain loading
// or the JSON loader). New code should prefer EntityWorld + the bridge.

#pragma once

#include "data_source.hpp"
#include "world_loader.hpp"

// f4-data — Aircraft configuration loading, validation, and table access.
//
// Master include. Pulls in every f4-data header.
//
// Dependencies: f4-math (for Table2D table accessors), nlohmann/json (for
// config loading). f4-units is NOT yet a dependency — typed Quantity
// accessors will be added when f4-flight-model lands and we know what
// units the consumers actually need.

#pragma once

#include "f4/data/aircraft_config.hpp"
#include "f4/data/config_loader.hpp"
#include "f4/data/engine_rpm_schedule.hpp"
#include "f4/data/table_accessors.hpp"

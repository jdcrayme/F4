cmake_minimum_required(VERSION 3.20)
project(f4-data VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Aircraft configuration loading, validation, and table access for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-data depends on:
#   - f4-math       (for Table2D table accessors)
#   - f4-units     (for typed Quantity accessors on geometry fields)
#   - nlohmann/json (for config loading from JSON)
# ---------------------------------------------------------------------------
add_library(f4-data STATIC
    src/aircraft_config.cpp
    src/brain_data.cpp
    src/config_loader.cpp
    src/engine_rpm_schedule.cpp
    src/formation_data.cpp
    src/maneuver_data.cpp
    src/sensor_data.cpp
    src/signature_data.cpp
    src/vehicle_def_data.cpp
)
target_link_libraries(f4-data PUBLIC f4_warnings)

target_include_directories(f4-data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-data PUBLIC
    f4-math
    f4-units
PRIVATE
    nlohmann_json::nlohmann_json
)

# Tests
option(F4_DATA_BUILD_TESTS "Build f4-data unit tests" ON)
if(F4_DATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

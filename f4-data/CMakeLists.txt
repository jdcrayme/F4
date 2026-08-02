cmake_minimum_required(VERSION 3.20)
project(f4-data VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Aircraft configuration loading, validation, and table access for the F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# f4-data depends on:
#   - f4-math       (for Table2D table accessors)
#   - nlohmann/json (for config loading from JSON)
#
# f4-units is NOT yet a dependency. Typed Quantity accessors will be added
# when f4-flight-model lands and we know what units the consumers need.
# ---------------------------------------------------------------------------
add_library(f4-data STATIC
    src/aircraft_config.cpp
    src/config_loader.cpp
)
target_include_directories(f4-data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(f4-data PUBLIC
    f4-math
    nlohmann_json::nlohmann_json
)

# Tests
option(F4_DATA_BUILD_TESTS "Build f4-data unit tests" ON)
if(F4_DATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

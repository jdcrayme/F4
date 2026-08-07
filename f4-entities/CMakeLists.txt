cmake_minimum_required(VERSION 3.20)
project(f4-entities VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Component-based entity system with spatial indexing")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: the non-template EntityWorld / EntityHandle / SpatialIndex
# methods live in src/entity.cpp. Templates (get<T>, add<T>, with_component<T>)
# remain in the header for inlining.
add_library(f4-entities STATIC
    src/entity.cpp
)
target_link_libraries(f4-entities PUBLIC f4_warnings)

target_include_directories(f4-entities
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# TransformComponent carries f4::geo::WorldPosition, so f4-geo is a PUBLIC dep.
target_link_libraries(f4-entities PUBLIC f4-geo)

# Tests
option(F4_ENTITIES_BUILD_TESTS "Build unit tests" ON)
if(F4_ENTITIES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

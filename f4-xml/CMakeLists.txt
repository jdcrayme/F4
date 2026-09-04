cmake_minimum_required(VERSION 3.20)
project(f4-xml VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "Vendored pugixml XML parser shared by F4 libraries")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static library: vendored pugixml v1.15 (MIT) + an f4::xml namespace alias.
#
# Consumers (SVG symbol import in f4-renderer; future BMS XML data-table
# parsers in f4-world-convert — worklog tasks BMS-CT-1 / BMS-DATA-1) link
# one parser instead of each vendoring their own. f4-xml sits BELOW
# f4-renderer and f4-world-convert in the dependency graph and must not
# pull in any domain library.
add_library(f4-xml STATIC
    third_party/pugixml/pugixml.cpp
    src/f4_xml.cpp
)
target_link_libraries(f4-xml PUBLIC f4_warnings)

target_include_directories(f4-xml
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml>
        $<INSTALL_INTERFACE:include>
)

# Tests
option(F4_XML_BUILD_TESTS "Build unit tests" ON)
if(F4_XML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# f4-world-types/tests/CMakeLists.txt
#
# Smoke tests: the JSON class table loader round-trips against the
# committed Data/Classes/falcon4.ct.json, and the enum constants match
# the values f4-simulation::campaign_bridge depends on.

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-world-types)
    target_compile_definitions(${name} PRIVATE
        F4_CT_JSON="${CMAKE_SOURCE_DIR}/Data/Classes/falcon4.ct.json"
    )
    gtest_discover_tests(${name} PROPERTIES LABELS "f4-world-types" TIMEOUT 60)
endfunction()

f4_add_test(test_class_table_json test_class_table_json.cpp)
f4_add_test(test_layout_types test_layout_types.cpp)
f4_add_test(test_day_night test_day_night.cpp)
f4_add_test(test_weather test_weather.cpp)

# The Falcon4.AII reader tests drive a committed fixture next to this
# test dir (ported here when the f4-world-convert duplicate of AiiConfig
# was retired in favor of this one).
add_executable(test_aii_config test_aii_config.cpp)
target_link_libraries(test_aii_config PRIVATE GTest::gtest_main f4-world-types)
target_compile_definitions(test_aii_config PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
)
gtest_discover_tests(test_aii_config PROPERTIES LABELS "f4-world-types" TIMEOUT 60)

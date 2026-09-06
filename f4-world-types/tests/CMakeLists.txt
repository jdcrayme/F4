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
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_class_table_json test_class_table_json.cpp)
f4_add_test(test_layout_types test_layout_types.cpp)

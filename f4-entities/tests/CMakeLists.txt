# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().
#
# f4-messaging is linked PRIVATELY because the behavioral-component tests
# pass a real MessageBus to update_all(); the library itself only needs a
# forward declaration of MessageBus (declared in entity.hpp).

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE
        GTest::gtest_main
        f4-entities
        f4-messaging
    )
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_entities_entity              test_entity.cpp)
f4_add_test(test_entities_spatial             test_spatial_index.cpp)
f4_add_test(test_entities_behavioral          test_behavioral_component.cpp)

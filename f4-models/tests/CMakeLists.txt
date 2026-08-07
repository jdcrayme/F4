cmake_minimum_required(VERSION 3.20)

# GoogleTest is fetched and GoogleTest module included from the root
# CMakeLists.txt (via cmake/f4_deps.cmake). This file only registers
# test executables via gtest_discover_tests().

function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-models)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    target_compile_definitions(${name} PRIVATE
        MODELS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
    )
    gtest_discover_tests(${name})
endfunction()

f4_add_test(test_models test_models.cpp)

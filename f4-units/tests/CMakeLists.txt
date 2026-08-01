include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# Helper function
function(f4_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain f4-units)
    catch_discover_tests(${name})
endfunction()

f4_add_test(test_dimension    test_dimension.cpp)
f4_add_test(test_conversions test_conversions.cpp)
f4_add_test(test_arithmetic  test_arithmetic.cpp)
f4_add_test(test_temperature test_temperature.cpp)
f4_add_test(test_aviation    test_aviation.cpp)
f4_add_test(test_derived     test_derived.cpp)
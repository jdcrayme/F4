# GoogleTest is fetched and the module included from the root CMakeLists.txt
# (via cmake/f4_deps.cmake). This file only registers test executables via
# gtest_discover_tests().
#
# Each test target links ONLY against f4-sensors (plus gtest_main). If a test
# ever needs f4-flight-model, f4-ai or f4-weapons, that is a coupling bug:
# f4-sensors is a leaf library of the combat chain and its tests must prove
# that. f4-data is the one sanctioned addition (test_simdata_sensors
# constructs f4::data::SignatureGrid values — the DATA side of the SimData
# pipeline, mirroring f4-sensors' own PRIVATE f4-data link).

function(f4_sensors_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GTest::gtest_main f4-sensors)
    gtest_discover_tests(${name})
endfunction()

f4_sensors_add_test(test_detection     test_detection.cpp)
f4_sensors_add_test(test_track_store   test_track_store.cpp)
f4_sensors_add_test(test_rwr           test_rwr.cpp)
f4_sensors_add_test(test_radar_component test_radar_component.cpp)
f4_sensors_add_test(test_simdata_sensors test_simdata_sensors.cpp)
target_link_libraries(test_simdata_sensors PRIVATE f4-data)

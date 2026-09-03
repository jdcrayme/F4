# f4-convert/cli/CMakeLists.txt
#
# CLI tool wrappers around the f4-convert library. Each is a thin main()
# so that the parsing/serialization logic stays in the library where it's
# unit-testable.

add_executable(dat2json     dat2json.cpp)
target_link_libraries(dat2json PRIVATE f4-convert)

add_executable(mnvr2json    mnvr2json.cpp)
target_link_libraries(mnvr2json PRIVATE f4-convert)

add_executable(brain2json   brain2json.cpp)
target_link_libraries(brain2json PRIVATE f4-convert)

add_executable(form2json    form2json.cpp)
target_link_libraries(form2json PRIVATE f4-convert)

add_executable(veh2json     veh2json.cpp)
target_link_libraries(veh2json PRIVATE f4-convert)

add_executable(sens2json    sens2json.cpp)
target_link_libraries(sens2json PRIVATE f4-convert)

add_executable(sig2json     sig2json.cpp)
target_link_libraries(sig2json PRIVATE f4-convert)

add_executable(dat_validate dat_validate.cpp)
target_link_libraries(dat_validate PRIVATE f4-convert)

add_executable(json_diff    json_diff.cpp)
target_link_libraries(json_diff PRIVATE f4-convert)

# Install the CLI tools so they can be called from the build tree or after install.
install(TARGETS dat2json mnvr2json brain2json form2json veh2json sens2json sig2json dat_validate json_diff
        RUNTIME DESTINATION bin)

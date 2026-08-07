# f4_deps.cmake
#
# Shared dependency fetching. Included once from the root CMakeLists.txt so
# that GoogleTest (and any future shared deps) are fetched/configured exactly
# once per build configuration — previously each module's
# tests/CMakeLists.txt repeated the same FetchContent block, pinning the
# GoogleTest version in 17 places and forcing CMake to dedupe the fetch on
# every configure pass.

include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

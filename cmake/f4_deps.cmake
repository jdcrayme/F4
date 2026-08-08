# f4_deps.cmake
#
# Shared dependency fetching. Included once from the root CMakeLists.txt so
# that GoogleTest (and any future shared deps) are fetched/configured exactly
# once per build configuration — previously each module's
# tests/CMakeLists.txt repeated the same FetchContent block, pinning the
# GoogleTest version in 17 places and forcing CMake to dedupe the fetch on
# every configure pass.

include(FetchContent)

# ── GoogleTest ────────────────────────────────────────────────────────────────
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
# Single-header JSON library used by f4-data and f4-convert.
# Fetched once at the root so that f4-data (configured before f4-convert)
# can link it without depending on f4-convert's FetchContent ordering.
# Previously this lived in f4-convert/CMakeLists.txt, which created a latent
# configure failure on clean builds: f4-data was processed first but the
# nlohmann_json target did not yet exist.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

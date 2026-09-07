# PHUG-PLAN Phase 1: flight-model system-identification harness
# (Docs/LONGITUDINAL_STABILITY_PLAN.md §5).
#
# Standalone executable — not a GoogleTest. Links f4-flight-model (plant)
# and f4-ai (outer-loop bisection configs C/D/E). Emits CSV for offline
# analysis (period/damping extraction, static maps, margin measurement).

add_executable(fm_sysid fm_sysid.cpp)
# CMAKE_CXX_STANDARD 20 is set directory-scoped inside f4-flight-model/ and
# does not propagate to sibling directories added from the root — set it here.
set_target_properties(fm_sysid PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(fm_sysid PRIVATE f4-flight-model f4-ai)
target_compile_definitions(fm_sysid PRIVATE
    F4_GENERATED_FIXTURES_DIR="${F4_GENERATED_FIXTURES_DIR}")
add_dependencies(fm_sysid convert_golden_fixtures)

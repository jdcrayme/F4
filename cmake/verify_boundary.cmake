# verify_boundary.cmake
#
# Tranche 0b — CMake boundary enforcement (NO_BINARY_RUNTIME_PLAN.md §0b).
#
# Implements ASSET_PIPELINE_SPEC.md §10 principle P2 (link-time isolation):
# runtime targets must never link the legacy binary parser libraries. The
# parser libraries are physically absent from a runtime target's transitive
# link closure — enforced here, not by convention.
#
# The forbidden parser libraries (the legacy-binary decoders):
#   f4-models         — KoreaObj.HDR/LOD/TEX 3D model database
#   f4-world-convert  — .cam campaign archive + FALCON4.ct class table
#   f4-terrain-convert — THEATER.* binary → JSON (thin wrapper over f4-terrain)
#   f4-lzss           — LZSS decompression (TEX blobs, CAM archives)
#
# Each f4-* target declares its side of the boundary via the F4_SIDE property:
#   F4_SIDE=importer  — may link parsers (the importer/converter side)
#   F4_SIDE=runtime   — must NOT link parsers (the runtime side)
#   (unset)           — treated as runtime-safe (checked, but typically neutral)
#
# Usage (root CMakeLists.txt, after all add_subdirectory calls):
#
#   include(verify_boundary)
#   f4_mark_side(importer f4-models f4-world-convert ...)
#   f4_mark_side(runtime  f4-renderer f4-simulation ...)
#   f4_verify_boundary()
#
# Enforcement mode:
#   -DF4_ENFORCE_BOUNDARY=OFF (default) — violations print as WARNING (build proceeds)
#   -DF4_ENFORCE_BOUNDARY=ON            — violations are FATAL_ERROR (configure fails)
#
# Status (Tranche 0d landed, Task 59): the verifier PASSES with every
# target enabled — the runtime link closure contains zero legacy binary
# parsers. The boundary is a contract now; any new runtime target that
# links a parser fails configure ( fatally with F4_ENFORCE_BOUNDARY=ON).

option(F4_ENFORCE_BOUNDARY "Fail configure if a runtime target links a legacy binary parser" OFF)

# ────────────────────────────────────────────────────────────────────────────
# f4_mark_side(side target...)
#
# Sets F4_SIDE=<side> on each named target that exists. Silently skips
# targets that don't exist (conditionally-built apps, platform-specific
# tools). This makes the marking block a single declarative list that works
# regardless of which optional targets are enabled.
# ────────────────────────────────────────────────────────────────────────────
function(f4_mark_side side)
    foreach(_t ${ARGN})
        if(TARGET ${_t})
            set_target_properties(${_t} PROPERTIES F4_SIDE ${side})
            # Register for the verifier (single source of truth for the
            # target set the boundary check iterates).
            set_property(GLOBAL APPEND PROPERTY F4_REGISTERED_TARGETS "${_t}")
        endif()
    endforeach()
endfunction()

# ────────────────────────────────────────────────────────────────────────────
# _f4_collect_registered_targets(out_var)
#
# Returns the list of targets registered via f4_mark_side(). This is the
# authoritative target set the verifier iterates — every f4-* library and
# executable is registered exactly once, at the root CMakeLists marking block.
#
# Unregistered targets (test executables, dev tools that link only non-forbidden
# libraries) are not checked. If a new runtime target is added, it MUST be
# registered via f4_mark_side(runtime ...) — the marking is the declaration of
# which side of the boundary it lives on.
# ────────────────────────────────────────────────────────────────────────────
function(_f4_collect_registered_targets out_var)
    get_property(_targets GLOBAL PROPERTY F4_REGISTERED_TARGETS)
    if(NOT _targets)
        set(_targets "")
    endif()
    # De-duplicate (a target may appear in both the importer and runtime
    # lists if misconfigured — keep the first occurrence).
    set(_unique "")
    foreach(_t ${_targets})
        if(NOT "${_t}" IN_LIST _unique)
            list(APPEND _unique "${_t}")
        endif()
    endforeach()
    set(${out_var} "${_unique}" PARENT_SCOPE)
endfunction()

# ────────────────────────────────────────────────────────────────────────────
# _f4_link_closure(out_var target)
#
# Computes the transitive link closure of <target> — every target that
# appears in its link graph, directly or transitively, through both
# LINK_LIBRARIES (direct links, PUBLIC + PRIVATE) and INTERFACE_LINK_LIBRARIES
# (propagated interface links). Generator expressions like
# $<LINK_ONLY:f4-models> and $<BUILD_INTERFACE:f4-models> are stripped to
# recover the bare target name.
#
# The closure includes PRIVATE links: for static libraries (the entire f4
# tree), CMake propagates a target's private deps to consumers' final link
# lines, so a PRIVATE link to a parser still puts parser object code in the
# binary. This is the correct semantic for "does parser code end up here".
# ────────────────────────────────────────────────────────────────────────────
function(_f4_link_closure out_var target)
    set(_result "")
    set(_worklist "${target}")
    set(_visited "")

    while(_worklist)
        list(POP_FRONT _worklist _current)

        # Cycle guard
        if("${_current}" IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_current}")

        # Only walk real targets (skip flags, files, imported libs)
        if(NOT TARGET "${_current}")
            continue()
        endif()

        list(APPEND _result "${_current}")

        # Gather direct links (LINK_LIBRARIES: PUBLIC + PRIVATE) and
        # interface-propagated links (INTERFACE_LINK_LIBRARIES).
        get_target_property(_ll "${_current}" LINK_LIBRARIES)
        if(NOT _ll OR _ll MATCHES "NOTFOUND")
            set(_ll "")
        endif()

        get_target_property(_ill "${_current}" INTERFACE_LINK_LIBRARIES)
        if(NOT _ill OR _ill MATCHES "NOTFOUND")
            set(_ill "")
        endif()

        foreach(_lib ${_ll} ${_ill})
            # Skip linker flags (-lpthread, -lm, etc.)
            if(_lib MATCHES "^-")
                continue()
            endif()

            # Strip generator expressions: $<LINK_ONLY:name>,
            # $<BUILD_INTERFACE:name>, $<INSTALL_INTERFACE:name>, etc.
            if(_lib MATCHES "^\\$<[^:]*:(.+)>$")
                set(_lib_name "${CMAKE_MATCH_1}")
            else()
                set(_lib_name "${_lib}")
            endif()

            if(TARGET "${_lib_name}" AND NOT "${_lib_name}" IN_LIST _visited)
                list(APPEND _worklist "${_lib_name}")
            endif()
        endforeach()
    endwhile()

    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

# ────────────────────────────────────────────────────────────────────────────
# f4_verify_boundary()
#
# The P2 check. For every target that is NOT on the importer side:
#   - walk its transitive link closure
#   - fail (or warn) if any forbidden parser library is in the closure
#
# Exemptions:
#   - F4_SIDE=importer targets — they're the parser's legitimate consumers
#   - Test executables (TYPE=EXECUTABLE whose SOURCE_DIR contains /tests) —
#     they test what they link; they're build-time artifacts, not shipped
#     runtime
#   - UTILITY targets (custom targets) — they don't link anything
#   - IMPORTED targets — third-party deps (GTest, nlohmann_json, raylib)
# ────────────────────────────────────────────────────────────────────────────
function(f4_verify_boundary)
    set(_forbidden f4-models f4-world-convert f4-terrain-convert f4-lzss)

    _f4_collect_registered_targets(_all_targets)

    set(_violations "")
    set(_violation_count 0)

    foreach(_target ${_all_targets})
        # Skip custom targets (UTILITY) — they have no link closure
        get_target_property(_type "${_target}" TYPE)
        if(_type STREQUAL "UTILITY")
            continue()
        endif()

        # Skip imported third-party targets (GTest::gtest_main, raylib, etc.)
        get_target_property(_imported "${_target}" IMPORTED)
        if(_imported)
            continue()
        endif()

        # Skip importer-side targets — they're allowed to link parsers
        get_target_property(_side "${_target}" F4_SIDE)
        if(_side STREQUAL "importer")
            continue()
        endif()

        # Skip test executables — they test what they link, they don't ship
        get_target_property(_src_dir "${_target}" SOURCE_DIR)
        if(_type STREQUAL "EXECUTABLE" AND _src_dir MATCHES "/tests")
            continue()
        endif()

        # Walk the transitive link closure
        _f4_link_closure(_closure "${_target}")

        # Check for forbidden parsers in the closure
        set(_found_parsers "")
        foreach(_parser ${_forbidden})
            if("${_parser}" IN_LIST _closure)
                list(APPEND _found_parsers "${_parser}")
            endif()
        endforeach()

        if(_found_parsers)
            math(EXPR _violation_count "${_violation_count} + 1")

            # Classify each finding as direct (in the target's own
            # LINK_LIBRARIES) or transitive (reached through a dependency).
            get_target_property(_ll "${_target}" LINK_LIBRARIES)
            if(NOT _ll OR _ll MATCHES "NOTFOUND")
                set(_ll "")
            endif()

            set(_direct "")
            set(_transitive "")
            foreach(_parser ${_found_parsers})
                if("${_parser}" IN_LIST _ll)
                    list(APPEND _direct "${_parser}")
                else()
                    list(APPEND _transitive "${_parser}")
                endif()
            endforeach()

            set(_detail "")
            if(_direct)
                string(JOIN ", " _direct_str ${_direct})
                string(APPEND _detail "direct: ${_direct_str}")
            endif()
            if(_transitive)
                string(JOIN ", " _trans_str ${_transitive})
                if(_detail)
                    string(APPEND _detail " | ")
                endif()
                string(APPEND _detail "transitive: ${_trans_str}")
            endif()

            if(NOT _side)
                set(_side "(unset)")
            endif()
            list(APPEND _violations "  ${_target} [F4_SIDE=${_side}] -- ${_detail}")
        endif()
    endforeach()

    if(_violations)
        string(JOIN "\n" _violations_str ${_violations})
        set(_msg "")
        string(APPEND _msg "F4 P2 boundary violations (NO_BINARY_RUNTIME_PLAN.md Tranche 0b):\n")
        string(APPEND _msg "The following runtime/non-importer targets link legacy binary parsers:\n")
        string(APPEND _msg "${_violations_str}\n")
        string(APPEND _msg "\n")
        string(APPEND _msg "These violations are EXPECTED until Tranche 0d (runtime glTF rewire)\n")
        string(APPEND _msg "decouples the runtime from f4-models / f4-world-convert.\n")
        string(APPEND _msg "\n")
        string(APPEND _msg "  -DF4_ENFORCE_BOUNDARY=ON   -> fatal (CI gate)\n")
        string(APPEND _msg "  -DF4_ENFORCE_BOUNDARY=OFF  -> warning only (default, build proceeds)")

        if(F4_ENFORCE_BOUNDARY)
            message(FATAL_ERROR "${_msg}")
        else()
            message(WARNING "${_msg}")
        endif()
    else()
        message(STATUS "F4 boundary check: PASS (no runtime target links a legacy binary parser)")
    endif()
endfunction()

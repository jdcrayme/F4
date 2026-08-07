# f4_warnings.cmake
#
# Shared warning-flag INTERFACE library. Linked into every f4-* target so the
# same baseline diagnostics apply across the project. The flags are NOT
# promoted to errors (-Werror) yet; that turn-on is reserved for a follow-up
# once the existing warning count is driven to zero on all compilers.
#
# Flag selection rationale:
#   -Wall -Wextra -Wpedantic   : the GCC/Clang baseline; catches most real bugs
#   -Wmissing-field-initializers: SUPPRESSED. Tests legitimately use `{}`
#                                 aggregate init for structs with many fields;
#                                 the warning is stylistic noise here.
#   -Wshadow -Wconversion       : NOT YET. They catch real issues but produce
#                                 hundreds of warnings on legacy-shaped code
#                                 (narrowing in binary parsers, shadowed loop
#                                 vars in viewer code). Add in a follow-up
#                                 after the codebase has been cleaned.
#
# Compiler portability:
#   - GCC / Clang : native flags
#   - MSVC        : /W4 + /permissive- for standards-conformant mode

add_library(f4_warnings INTERFACE)
add_library(F4::Warnings ALIAS f4_warnings)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(f4_warnings INTERFACE
        /W4
        /permissive-
        /wd4820  # padding added after data members — noise on legacy structs
        /wd4514  # unreferenced inline function removed — noise
        /wd4668  # macro not defined — noise from third-party headers
    )
else()
    target_compile_options(f4_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers  # see comment above
        # Helpful extras that don't produce much noise on this codebase:
        -Wno-unused-parameter            # interface headers define params that not all impls use
    )
endif()

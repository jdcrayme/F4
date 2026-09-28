#!/usr/bin/env bash
# scripts/test.sh — the one-command verification: build + the full ctest suite.
#
# The repo registers every module's tests with CTest (per-module labels +
# the `slow` label on the long harness binaries). On a single-config
# generator (Ninja/make, what CI uses) plain `ctest` sees all 3,000+ tests;
# on a multi-config generator (Visual Studio, the local default) the
# gtest_discover_tests registrations only exist per configuration, so the
# `-C <config>` flag is REQUIRED — without it ctest silently sees only the
# hand-written add_test rows (the renderer's 15). This script always passes
# -C, which is harmless on single-config generators.
#
# Usage:
#   scripts/test.sh                     # build Debug + run EVERYTHING (CI parity)
#   scripts/test.sh --fast              # skip the `slow`-labeled harness tier
#   scripts/test.sh --release           # build + test Release
#   scripts/test.sh --filter ThreatMap  # ctest -R regex (implies --fast? no — full)
#   scripts/test.sh --label f4-ai       # one module's suite
#   scripts/test.sh --list              # what would run (ctest -N)
#   scripts/test.sh --no-build          # run the suite over an existing build
#
# Env:
#   F4_BUILD=<dir>    build directory (default: Build/ if it exists, else build/)
#   F4_TEST_JOBS=<n>  ctest parallelism (default: all cores)
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SRC"

# --- arguments ---------------------------------------------------------------
CONFIG="Debug"
RUN_SLOW=1
BUILD=1
LIST=0
FILTER=""
LABEL=""
EXTRA_CTEST=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)  CONFIG="Release"; shift ;;
        --debug)    CONFIG="Debug"; shift ;;
        --fast)     RUN_SLOW=0; shift ;;
        --no-build) BUILD=0; shift ;;
        --list)     LIST=1; shift ;;
        --filter)   FILTER="$2"; shift 2 ;;
        --label)    LABEL="$2"; shift 2 ;;
        -j)         EXTRA_CTEST+=("-j" "$2"); shift 2 ;;
        -h|--help)  sed -n '2,26p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)          echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
done

# --- build directory ---------------------------------------------------------
if [[ -n "${F4_BUILD:-}" ]]; then
    BUILD_DIR="$F4_BUILD"
elif [[ -d "$SRC/Build" ]]; then
    BUILD_DIR="Build"       # the Visual Studio-generator checkout default
else
    BUILD_DIR="build"
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "error: $BUILD_DIR is not configured." >&2
    echo "  Linux:   ./scripts/configure-dev.sh && scripts/test.sh" >&2
    echo "  Windows: cmake -B Build -DCMAKE_BUILD_TYPE=Debug (or open CMake in your IDE)" >&2
    exit 1
fi

# --- parallelism -------------------------------------------------------------
if [[ ${#EXTRA_CTEST[@]} -eq 0 ]]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        JOBS="$NUMBER_OF_PROCESSORS"
    else
        JOBS=4
    fi
    EXTRA_CTEST+=("-j" "$JOBS")
fi

# --- build -------------------------------------------------------------------
if [[ "$BUILD" -eq 1 ]]; then
    echo "== building ($BUILD_DIR, $CONFIG) =="
    cmake --build "$BUILD_DIR" --config "$CONFIG"
fi

# --- test --------------------------------------------------------------------
CTEST_ARGS=(--test-dir "$BUILD_DIR" -C "$CONFIG" --output-on-failure)
[[ "$RUN_SLOW" -eq 0 ]] && CTEST_ARGS+=(-LE slow)
[[ -n "$FILTER" ]]      && CTEST_ARGS+=(-R "$FILTER")
[[ -n "$LABEL" ]]       && CTEST_ARGS+=(-L "$LABEL")
[[ "$LIST" -eq 1 ]]     && CTEST_ARGS+=(-N)
CTEST_ARGS+=("${EXTRA_CTEST[@]}")

echo "== testing (${CTEST_ARGS[*]}) =="
ctest "${CTEST_ARGS[@]}"

#!/usr/bin/env bash
# scripts/configure-dev.sh — the recommended dev configure invocation.
#
# Tier 1 speedup: Ninja (faster dep analysis than Make) + ccache (turns the
# ~10-min cold build into <30 s warm) + the repo-local FetchContent cache
# (skips the ~30-s GoogleTest/nlohmann_json re-fetch on every clean configure).
#
# Requirements (one-time per machine):
#   apt install ninja-build ccache          # or: brew install ninja ccache
#   # optional but recommended (10-50x link speedup over default ld):
#   apt install mold                         # then uncomment the MOLD line below
#
# Usage:
#   ./scripts/configure-dev.sh                # Debug, default flags
#   ./scripts/configure-dev.sh Release        # Release
#   BUILD_DIR=build-rel ./scripts/configure-dev.sh Release
#
# After configure:
#   cmake --build build -j                    # Ninja parallelism is automatic
#   ctest --test-dir build -LE slow -j4       # the fast iteration tier (~30 s)
#   ctest --test-dir build -L f4-ai -j4      # one library
#   ctest --test-dir build -L slow -j1       # the nightly harness tier (serial)
set -euo pipefail

BUILD_TYPE="${1:-Debug}"
BUILD_DIR="${BUILD_DIR:-build}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Probe for the fastest available toolchain.
GEN_FLAG="-G Ninja"
LINKER_FLAGS=""
if command -v ninja >/dev/null 2>&1; then
    : # Ninja available
else
    echo "WARNING: ninja not found on PATH; falling back to Unix Makefiles generator." >&2
    echo "  Install it for the 2-3x incremental-build speedup: apt install ninja-build" >&2
    GEN_FLAG=""
fi

if command -v ccache >/dev/null 2>&1; then
    CCACHE_FLAG="-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache"
else
    echo "WARNING: ccache not found; warm rebuilds will be slow." >&2
    echo "  Install it for the biggest single iteration win: apt install ccache" >&2
    CCACHE_FLAG=""
fi

# Optional: mold linker (10-50x faster than gold/bfd on large link jobs).
# Uncomment if you have it installed.
# if command -v mold >/dev/null 2>&1; then
#     LINKER_FLAGS="-DCMAKE_CXX_LINKER=mold -DCMAKE_C_LINKER=mold"
# fi

# Headless by default (no X11/OpenGL needed for the core libs + tests).
# Pass VIEWER=1 to enable the Raylib viewers (needs libxrandr-dev etc.).
VIEWER_FLAGS=""
if [ "${VIEWER:-0}" = "1" ]; then
    VIEWER_FLAGS="-DF4_BUILD_RENDERER=ON -DF4_BUILD_VIEWER=ON -DF4_BUILD_SCENARIO_PLAYER=ON"
else
    VIEWER_FLAGS="-DF4_BUILD_RENDERER=OFF -DF4_BUILD_MODEL_VIEWER=OFF -DF4_BUILD_VIEWER=OFF -DF4_BUILD_SCENARIO_PLAYER=OFF"
fi

echo "=== Configuring $BUILD_TYPE into $BUILD_DIR ==="
echo "  generator: ${GEN_FLAG:-(default Make)}"
echo "  ccache:    ${CCACHE_FLAG:+enabled}${CCACHE_FLAG:-disabled}"
echo "  viewers:   ${VIEWER_FLAGS#*F4_BUILD_RENDERER=}"

cmake -S "$SRC" -B "$BUILD_DIR" \
    $GEN_FLAG \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    $CCACHE_FLAG \
    $LINKER_FLAGS \
    $VIEWER_FLAGS \
    -DF4_ENFORCE_BOUNDARY=ON \
    "$@"

echo
echo "=== Configure complete. Next: ==="
echo "  cmake --build $BUILD_DIR -j"
echo "  ctest --test-dir $BUILD_DIR -LE slow -j4    # fast tier (~30 s)"
echo "  ctest --test-dir $BUILD_DIR -L slow -j1     # nightly tier (serial)"

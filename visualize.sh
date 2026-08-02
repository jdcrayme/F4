#!/usr/bin/env bash
#
# visualize.sh — one-click theater visualization for F4.
#
# Builds the project (if needed), converts the campaign .cam to JSON,
# generates an HTML map with real Korea terrain tiles, and opens it in
# your default browser.
#
# Usage:
#   ./visualize.sh                      # use temp/save1.cam (default fixture)
#   ./visualize.sh path/to/save1.cam    # use a custom .cam file
#   ./visualize.sh --no-open            # generate but don't auto-open
#
# Prerequisites:
#   - CMake 3.20+ (install: pip install cmake)
#   - A C++20 compiler (g++ 10+, clang 12+, MSVC 19.28+)
#   - The terrain files in temp/ (THEATER.MAP, .MEA, .O2)
#
# The script is idempotent — re-running it just regenerates the map.

set -euo pipefail

# Resolve the repo root (directory containing this script).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Add common cmake install locations to PATH (pip installs to ~/.local/bin).
export PATH="$HOME/.local/bin:$HOME/.cargo/bin:/usr/local/bin:$PATH"

# Colors for output.
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
die()   { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# --- Parse args ---
# Default: look for save1.cam in the test fixtures (bundled with the repo).
# If not there, check temp/.
DEFAULT_CAM="f4-world-convert/tests/fixtures/save1.cam"
[ -f "$DEFAULT_CAM" ] || DEFAULT_CAM="temp/save1.cam"

CAM_FILE="$DEFAULT_CAM"
OPEN_BROWSER=true
for arg in "$@"; do
    case "$arg" in
        --no-open) OPEN_BROWSER=false ;;
        -h|--help)
            head -22 "$0" | tail -20
            exit 0
            ;;
        *)
            [ -f "$arg" ] && CAM_FILE="$arg" || die "File not found: $arg"
            ;;
    esac
done

# --- Verify prerequisites ---
command -v cmake >/dev/null 2>&1 || die "cmake not found. Install: pip install cmake"
command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || die "C++ compiler not found"

[ -f "$CAM_FILE" ] || die "Campaign file not found: $CAM_FILE
  Place a .cam file at temp/save1.cam or pass it as an argument: ./visualize.sh path/to/file.cam"

# Find terrain files: check temp/ first, then f4-terrain/tests/fixtures/.
if [ -f "temp/THEATER.MEA" ]; then
    TERRAIN_DIR="temp"
elif [ -f "f4-terrain/tests/fixtures/THEATER.MEA" ]; then
    TERRAIN_DIR="f4-terrain/tests/fixtures"
else
    die "Terrain files not found. Place THEATER.MAP/.MEA/.O2 in temp/
  or run from a repo with f4-terrain fixtures."
fi

# --- Step 1: Build (if needed) ---
BUILD_DIR="build"
if [ ! -d "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/f4-world-vis/world2map" ]; then
    info "Configuring build (first time only)..."
    cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
fi

if [ ! -f "$BUILD_DIR/f4-world-convert/cam2json" ] || [ ! -f "$BUILD_DIR/f4-world-vis/world2map" ]; then
    info "Building project (this takes a few minutes the first time)..."
    cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 2)" 2>&1 | tail -5
fi

[ -f "$BUILD_DIR/f4-world-convert/cam2json" ] || die "Build failed: cam2json not found"
[ -f "$BUILD_DIR/f4-world-vis/world2map" ]   || die "Build failed: world2map not found"
ok "Build ready"

# --- Step 2: .cam -> JSON ---
CAM_STEM=$(basename "$CAM_FILE" .cam)
JSON_FILE="$BUILD_DIR/${CAM_STEM}.json"

info "Converting $CAM_FILE -> JSON..."
"$BUILD_DIR/f4-world-convert/cam2json" "$CAM_FILE" "$JSON_FILE" 2>&1 | grep -v "^  " || true
ok "JSON: $JSON_FILE"

# --- Step 3: JSON -> HTML map (with terrain) ---
MAP_FILE="$BUILD_DIR/${CAM_STEM}_map.html"

info "Generating terrain map..."
"$BUILD_DIR/f4-world-vis/world2map" "$JSON_FILE" "$MAP_FILE" "$TERRAIN_DIR" 2>&1 | grep -v "^  " || true
ok "Map: $MAP_FILE"

# Also generate a land-mask-only version (no terrain, for comparison)
MAP_FILE_NOMAP="$BUILD_DIR/${CAM_STEM}_map_noterrain.html"
"$BUILD_DIR/f4-world-vis/world2map" "$JSON_FILE" "$MAP_FILE_NOMAP" 2>/dev/null | grep -v "^  " || true

# --- Step 4: Open in browser ---
if $OPEN_BROWSER; then
    info "Opening map in browser..."
    # Copy to a stable location for easy access.
    cp "$MAP_FILE" "${CAM_STEM}_map.html"
    MAP_PATH="${CAM_STEM}_map.html"

    # Try multiple browser-open methods.
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$MAP_PATH" >/dev/null 2>&1 &
    elif command -v open >/dev/null 2>&1; then
        open "$MAP_PATH"
    elif command -v start >/dev/null 2>&1; then
        start "$MAP_PATH"
    else
        warn "Could not auto-open browser. Open manually: $MAP_PATH"
    fi
    ok "Map opened: $(pwd)/$MAP_PATH"
else
    ok "Map generated at: $(pwd)/$MAP_FILE"
    echo "    Open in browser: file://$(pwd)/$MAP_FILE"
fi

echo ""
echo -e "${GREEN}=== Done ===${NC}"
echo "  Campaign:  $CAM_FILE"
echo "  Terrain:   $TERRAIN_DIR/THEATER.*"
echo "  Map:       $(pwd)/${CAM_STEM}_map.html"
echo ""
echo "  Other maps:"
echo "    Land-mask only (no terrain): $BUILD_DIR/${CAM_STEM}_map_noterrain.html"
echo "    Raw SVG (embed in docs):     run: world2map $JSON_FILE out.svg $TERRAIN_DIR"

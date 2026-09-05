#!/usr/bin/env bash
# scripts/export-game-data.sh
#
# Crawl a Falcon 4.0 install and convert every supported asset into the
# engine-agnostic Data/ tree (NO_BINARY_RUNTIME_PLAN.md Tranche 0a.2).
# No binary files are written to Data/ — the KoreaObj binary (HDR/LOD/TEX)
# becomes glTF+PNG in Tranche 0c/0d, not here.
#
# Everything is DISCOVERED, not hardcoded: the script crawls the install
# for FALCON4.ct, the theater terrain files, campaign saves, and the
# SimData.ZIP archive (whose payload — aircraft .dat, BRAINDAT.brn,
# FORMDAT.FIL, VehDef, SENSDATA, SIGDATA — only exists inside the ZIP).
# Converter executables are located regardless of generator/layout
# (Ninja, MSVC Debug/Release, cli/ subdirs, .exe suffix or not).
#
# Usage:
#   scripts/export-game-data.sh --install <path> [options]
#
# Options:
#   --install <path>   Falcon 4.0 install dir (or set F4_INSTALL env)
#   --output <dir>     Output Data/ dir (default: <repo>/Data)
#   --build <dir>      Build dir to find converters (default: <repo>/build,
#                      or F4_BUILD env)
#   --theater <name>   Theater key (default: korea)
#   --save <name>      Campaign save stem (default: save1; e.g. "Auto Save")
#   --f16-only         Export only f16.json to Aircraft/ (default: every
#                      aircraft .dat in the install's ACDATA)
#   -h, --help         Show this help
#
# Output layout (consumed by f4-data / f4-simulation; names match the
# build-tree generated_fixtures the loaders already use):
#
#   Data/
#   ├── manifest.json                    # provenance + SHA-256 per asset
#   ├── World/<theater>.world.json       # cam2json (save + real theater DB)
#   ├── Theater/<theater>/terrain.json   # terrain2json
#   ├── Aircraft/<name>.json             # dat2json (f16.json + the rest)
#   ├── SimData/                         # the SimData.ZIP payload
#   │   ├── mnvrdata.json  braindata.json  formdat.json
#   │   ├── vehdef.json    sigdata.json
#   │   └── irstdata.json  rwrdata.json   visualdata.json
#   ├── Classes/falcon4.ct.json          # ct2json (kills the .ct binary)
#   └── Models/koreaobj/                 # f4import (LOCAL ONLY — gitignored
#       ├── *.gltf|*.bin                 #  until Tranche 0d rewires the
#       └── textures/*.png               #  runtime; PNGs feed the glTF materials)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

INSTALL="${F4_INSTALL:-}"
OUTPUT="$REPO_ROOT/Data"
BUILD="${F4_BUILD:-$REPO_ROOT/build}"
THEATER="korea"
SAVE="save1"
F16_ONLY=0

usage() { sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install)  INSTALL="$2"; shift 2 ;;
        --output)   OUTPUT="$2"; shift 2 ;;
        --build)    BUILD="$2"; shift 2 ;;
        --theater)  THEATER="$2"; shift 2 ;;
        --save)     SAVE="$2"; shift 2 ;;
        --f16-only) F16_ONLY=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "unknown arg: $1"; usage; exit 1 ;;
    esac
done

# Normalize Windows-style paths (D:\SteamLibrary\...) for the MSYS/Git Bash
# toolchain. cygpath -u passes through unchanged when unavailable.
norm_path() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1" 2>/dev/null || echo "$1"; else echo "$1"; fi
}

fail() { echo "ERROR: $*" >&2; exit 1; }

[[ -n "$INSTALL" ]] || { echo "ERROR: --install is required (or set F4_INSTALL)" >&2; usage; exit 1; }
INSTALL="$(norm_path "$INSTALL")"
OUTPUT="$(norm_path "$OUTPUT")"
BUILD="$(norm_path "$BUILD")"
[[ -d "$INSTALL" ]] || fail "install dir not found: $INSTALL"
[[ -d "$BUILD" ]] || fail "build dir not found: $BUILD (pass --build or set F4_BUILD)"

# find -first helpers. Every crawl is guarded: a missing dir or an empty
# result yields "" instead of aborting the script (set -e is deliberately
# NOT global — required-step failures fail explicitly below).
find_first() { find "$@" 2>/dev/null | head -1 || true; }

# ---------------------------------------------------------------------------
# Converter discovery: generators differ (Ninja puts binaries next to the
# CMakeLists dir; MSVC adds Debug/Release/... configs and .exe; some CLIs
# live under a cli/ subdir). Try them all; also fall back to a find sweep.
# ---------------------------------------------------------------------------
find_tool() {
    local tool="$1" cfg dir cand
    local configs=("" Release RelWithDebInfo MinSizeRel Debug)
    local exts=("" ".exe")
    local subdirs=("")
    case "$tool" in
        cam2json|ct2json)              subdirs=("f4-world-convert" "f4-world-convert/cli") ;;
        terrain2json)                  subdirs=("f4-terrain-convert" "f4-terrain-convert/cli") ;;
        f4import)                      subdirs=("f4-import" "f4-import/cli") ;;
        *)                             subdirs=("f4-convert/cli" "f4-convert") ;;
    esac
    for dir in "${subdirs[@]}"; do
        for cfg in "${configs[@]}"; do
            for ext in "${exts[@]}"; do
                cand="$BUILD/$dir/${cfg:+$cfg/}$tool$ext"
                [[ -x "$cand" ]] && { echo "$cand"; return 0; }
            done
        done
    done
    cand="$( { find "$BUILD" -name "$tool.exe" -o -name "$tool" 2>/dev/null || true; } \
              | grep -iv '\.dir\|CMakeFiles' | head -1 )"
    [[ -n "$cand" && -x "$cand" ]] && { echo "$cand"; return 0; }
    return 1
}

TOOL_C2J="$(find_tool cam2json)"      || fail "cam2json not built — configure+build first (cmake -B $BUILD)"
TOOL_T2J="$(find_tool terrain2json)"  || fail "terrain2json not built"
TOOL_CT2J="$(find_tool ct2json)"      || fail "ct2json not built (Tranche 0a.1)"
TOOL_D2J="$(find_tool dat2json)"      || fail "dat2json not built"
TOOL_MNVR="$(find_tool mnvr2json)"    || fail "mnvr2json not built"
TOOL_BRAIN="$(find_tool brain2json)"  || fail "brain2json not built"
TOOL_FORM="$(find_tool form2json)"    || fail "form2json not built"
TOOL_VEH="$(find_tool veh2json)"      || fail "veh2json not built"
TOOL_SENS="$(find_tool sens2json)"    || fail "sens2json not built"
TOOL_SIG="$(find_tool sig2json)"      || fail "sig2json not built"
TOOL_F4IMPORT="$(find_tool f4import)" || true   # optional: glTF models (0c)

# ---------------------------------------------------------------------------
# Crawl the install.
# ---------------------------------------------------------------------------
# FALCON4.ct — fast path first, then a case-insensitive crawl (covers
# repackaged installs that hoist files out of terrdata).
CT="$INSTALL/terrdata/objects/FALCON4.ct"
[[ -f "$CT" ]] || CT="$(find_first "$INSTALL" -iname 'falcon4.ct' -type f)"
[[ -n "$CT" ]] || fail "FALCON4.ct not found under $INSTALL"

# Theater object DB (FALCON4.OCD/.PHD/.PD/... for cam2json --theater-data):
# the directory FALCON4.ct lives in (vanilla layout), else crawl for it.
THEATER_DATA="$(dirname "$CT")"
if ! find "$THEATER_DATA" -maxdepth 1 \( -iname 'FALCON4.PHD' -o -iname 'FALCON4.OCD' \) 2>/dev/null | grep -q .; then
    phd="$(find_first "$INSTALL" -iname 'FALCON4.PHD' -type f)"
    [[ -n "$phd" ]] || phd="$(find_first "$INSTALL" -iname 'FALCON4.OCD' -type f)"
    [[ -n "$phd" ]] || fail "theater object DB (FALCON4.PHD/OCD) not found under $INSTALL"
    THEATER_DATA="$(dirname "$phd")"
fi

# Theater terrain: the directory holding THEATER.MEA etc. NOTE: that is
# terrdata/<theater>/terrain in a vanilla install — not terrdata/<theater>.
TERRAIN_DIR="$INSTALL/terrdata/$THEATER/terrain"
if [[ ! -f "$TERRAIN_DIR/THEATER.MEA" ]]; then
    meas="$(find "$INSTALL/terrdata/$THEATER" "$INSTALL" -iname 'THEATER.MEA' -type f 2>/dev/null | head -1 || true)"
    [[ -n "$meas" ]] || fail "THEATER.MEA (theater terrain) not found under $INSTALL"
    TERRAIN_DIR="$(dirname "$meas")"
fi

# Campaign save. Spaces are legal ("Auto Save.cam") — find handles them.
CAM="$(find_first "$INSTALL/campaign" -iname "$SAVE.cam" -type f)"
[[ -z "$CAM" ]] && CAM="$(find_first "$INSTALL" -iname "$SAVE.cam" -type f)"
[[ -n "$CAM" ]] || fail "campaign save '$SAVE.cam' not found under $INSTALL"
SAVE_DIR="$(dirname "$CAM")"

# Base objectives for in-campaign saves (they carry only .obd deltas);
# applied on top when present next to the save. Optional but normal.
OBJECTIVES="$(find "$SAVE_DIR" -maxdepth 1 -iname 'OBJECTIV.*' -type f 2>/dev/null | head -1 || true)"

# SimData.ZIP — the ONLY place the sim-side source files exist (aircraft
# .dat, BRAINDAT.brn, FORMDAT.FIL, VehDef, SENSDATA, SIGDATA). Prefer the
# Zips/ copy; some installs also have a stray root-level copy (ignore it).
SIMZIP="$INSTALL/Zips/Simdata.ZIP"
[[ -f "$SIMZIP" ]] || SIMZIP="$(find "$INSTALL" -iname 'simdata.zip' -type f 2>/dev/null | grep -i 'zips' | head -1 || true)"
[[ -f "$SIMZIP" ]] || SIMZIP="$(find_first "$INSTALL" -iname 'simdata.zip' -type f)"
[[ -n "$SIMZIP" ]] || fail "Simdata.ZIP not found under $INSTALL"

# ---------------------------------------------------------------------------
# Extract SimData.ZIP (the converters read individual files) and locate
# its payload case-insensitively (ZIP member casing varies by install).
# ---------------------------------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
if command -v unzip >/dev/null 2>&1; then
    unzip -oq "$SIMZIP" -d "$TMP" || fail "cannot extract $SIMZIP (unzip failed)"
elif command -v python >/dev/null 2>&1 || command -v python3 >/dev/null 2>&1; then
    PYTHON="$(command -v python3 || command -v python)"
    "$PYTHON" - "$SIMZIP" "$TMP" <<'PY' || fail "cannot extract $SIMZIP (python zipfile failed)"
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
else
    fail "need unzip or python to extract $SIMZIP"
fi

ACDATA="$(find_first "$TMP" -type d -iname 'ACDATA')"
MNUVR_DAT="$(find_first "$TMP" -iname 'mnvrdata.dat')"
BRAIN_BRN="$(find_first "$TMP" -iname 'BRAINDAT.brn')"
FORM_FIL="$(find_first "$TMP" -iname 'FORMDAT.FIL')"
VEH_LST="$(find_first "$TMP" -iname 'Vehicle.lst')"
SENS_IRST="$(find_first "$TMP" -ipath '*SENSDATA*' -iname 'IRST.LST')"
SENS_RWR="$(find_first "$TMP" -ipath '*SENSDATA*' -iname 'RWR.LST')"
SENS_VIS="$(find_first "$TMP" -ipath '*SENSDATA*' -iname 'VISUAL.LST')"
SIG_DIR="$(find_first "$TMP" -type d -iname 'SIGDATA')"
[[ -n "$ACDATA"    ]] || fail "sim/ACDATA not found in $SIMZIP"
[[ -n "$MNUVR_DAT" ]] || fail "ACDATA/BRAIN/mnvrdata.dat not found in $SIMZIP"
[[ -n "$BRAIN_BRN" ]] || fail "ACDATA/BRAIN/BRAINDAT.brn not found in $SIMZIP"
[[ -n "$FORM_FIL"  ]] || fail "ACDATA/FORMDATA/FORMDAT.FIL not found in $SIMZIP"
[[ -n "$VEH_LST"   ]] || fail "VehDef/Vehicle.lst not found in $SIMZIP"
[[ -n "$SENS_IRST" && -n "$SENS_RWR" && -n "$SENS_VIS" ]] || fail "SENSDATA lists not found in $SIMZIP"
[[ -n "$SIG_DIR"   ]] || fail "SIGDATA dir not found in $SIMZIP"

# ---------------------------------------------------------------------------
# Convert.
# ---------------------------------------------------------------------------
mkdir -p "$OUTPUT/World" "$OUTPUT/Theater/$THEATER" "$OUTPUT/Aircraft" \
         "$OUTPUT/SimData" "$OUTPUT/Classes"

echo "=== Exporting game data to $OUTPUT/ ==="
echo "    install:     $INSTALL"
echo "    theater:     $THEATER  (terrain: $TERRAIN_DIR)"
echo "    save:        $CAM"
echo "    simdata.zip: $SIMZIP"
echo "    class table: $CT"
echo ""

echo "[1/8] World JSON (cam2json --theater-data)..."
CAM2JSON_ARGS=("$CAM" "$OUTPUT/World/$THEATER.world.json"
               --theater "$THEATER"
               --theater-data "$THEATER_DATA"
               --class-table "$CT")
[[ -n "$OBJECTIVES" ]] && CAM2JSON_ARGS+=(--objectives "$OBJECTIVES")
if ! "$TOOL_C2J" "${CAM2JSON_ARGS[@]}" >/dev/null 2>"$TMP/cam2json.log"; then
    sed 's/^/    /' "$TMP/cam2json.log" >&2
    fail "cam2json failed (log above)"
fi
sed 's/^/    /' "$TMP/cam2json.log"

echo "[2/8] Terrain JSON (terrain2json)..."
"$TOOL_T2J" "$TERRAIN_DIR" "$OUTPUT/Theater/$THEATER/terrain.json" --name "$THEATER" | sed 's/^/    /' \
    || fail "terrain2json failed"

echo "[3/8] Class table JSON (ct2json)..."
"$TOOL_CT2J" "$CT" "$OUTPUT/Classes/falcon4.ct.json" | sed 's/^/    /' || fail "ct2json failed"

echo "[4/8] Aircraft JSON (dat2json over ACDATA/*.dat)..."
if [[ "$F16_ONLY" == 1 ]]; then
    DAT_FILES="$(find "$ACDATA" -maxdepth 1 -iname 'f16.dat' 2>/dev/null | sort || true)"
else
    DAT_FILES="$(find "$ACDATA" -maxdepth 1 -iname '*.dat' 2>/dev/null | sort || true)"
fi
[[ -n "$DAT_FILES" ]] || fail "no aircraft .dat files in $ACDATA"
aircraft_ok=0; aircraft_total=0
while IFS= read -r dat; do
    base="$(basename "$dat")"; base="${base%.*}"
    aircraft_total=$((aircraft_total + 1))
    if "$TOOL_D2J" "$dat" "$OUTPUT/Aircraft/$base.json" >/dev/null 2>&1; then
        aircraft_ok=$((aircraft_ok + 1))
    else
        echo "    WARNING: dat2json failed on $(basename "$dat") — skipped"
    fi
done <<< "$DAT_FILES"
echo "    $aircraft_ok/$aircraft_total aircraft converted -> Aircraft/"

echo "[5/8] SimData JSON (mnvr/brain/form/veh/sens/sig)..."
"$TOOL_MNVR"  "$MNUVR_DAT" "$OUTPUT/SimData/mnvrdata.json"   | sed 's/^/    /' || fail "mnvr2json failed"
"$TOOL_BRAIN" "$BRAIN_BRN" "$OUTPUT/SimData/braindata.json"  | sed 's/^/    /' || fail "brain2json failed"
"$TOOL_FORM"  "$FORM_FIL"  "$OUTPUT/SimData/formdat.json"    | sed 's/^/    /' || fail "form2json failed"
"$TOOL_VEH"   "$VEH_LST"   "$OUTPUT/SimData/vehdef.json"     | sed 's/^/    /' || fail "veh2json failed"
"$TOOL_SENS"  irst   "$SENS_IRST" "$OUTPUT/SimData/irstdata.json"   | sed 's/^/    /' || fail "sens2json irst failed"
"$TOOL_SENS"  rwr    "$SENS_RWR"  "$OUTPUT/SimData/rwrdata.json"    | sed 's/^/    /' || fail "sens2json rwr failed"
"$TOOL_SENS"  visual "$SENS_VIS"  "$OUTPUT/SimData/visualdata.json" | sed 's/^/    /' || fail "sens2json visual failed"
"$TOOL_SIG"   "$SIG_DIR" "$OUTPUT/SimData/sigdata.json"           | sed 's/^/    /' || fail "sig2json failed"

echo "[6/8] Textures -> PNG (f4import textures --all; writes Data/Models/koreaobj/textures/)..."
if [[ -n "$TOOL_F4IMPORT" ]]; then
    # Same local-only policy as the models: Data/Models stays gitignored
    # until Tranche 0d. These PNGs are what the emitted glTF materials
    # reference ("textures/NNNNN.png").
    TEX_LOG="$TMP/f4import_textures.log"
    if "$TOOL_F4IMPORT" textures --install "$INSTALL" --data "$OUTPUT" --all >"$TEX_LOG" 2>&1; then
        tail -1 "$TEX_LOG" | sed 's/^/    /'
    else
        tail -3 "$TEX_LOG" | sed 's/^/    /'
        echo "    WARN: some textures failed to decode (see $TEX_LOG)"
    fi
else
    echo "    SKIP: f4import not built — no texture PNGs (build f4-import)"
fi

echo "[7/8] Models -> glTF (f4import models --all; writes Data/Models/koreaobj/)..."
if [[ -n "$TOOL_F4IMPORT" ]]; then
    # Local artifacts only: Data/Models stays gitignored until the runtime
    # glTF rewire (Tranche 0d). f4import exits non-zero when some models are
    # empty placeholder records (no extractable geometry) — that is expected
    # (~40 of 1342), hence the soft warning below.
    F4IMPORT_LOG="$TMP/f4import.log"
    if "$TOOL_F4IMPORT" models --install "$INSTALL" --data "$OUTPUT" --all >"$F4IMPORT_LOG" 2>&1; then
        tail -1 "$F4IMPORT_LOG" | sed 's/^/    /'
    else
        tail -1 "$F4IMPORT_LOG" | sed 's/^/    /'
        echo "    note: unconverted models above are empty KoreaObj records (expected)"
    fi
else
    echo "    SKIP: f4import not built — no glTF models (build f4-import)"
fi

echo "[8/8] Manifest (provenance + SHA-256 over the committed JSON subset)..."
MANIFEST_PY="$SCRIPT_DIR/generate_manifest.py"
PYTHON_BIN="$(command -v python3 || command -v python || true)"
[[ -n "$PYTHON_BIN" ]] || fail "no python available for manifest generation"
# Models is excluded while it stays gitignored (Tranche 0c/0d) so the
# committed manifest always matches the committed tree. This step runs
# LAST: f4import rewrites the manifest in its own schema.
"$PYTHON_BIN" "$MANIFEST_PY" --output "$OUTPUT" --install "$INSTALL" \
    --theater "$THEATER" --save "$SAVE" --exclude Models | sed 's/^/    /' || fail "manifest generation failed"

echo ""
echo "=== Done. $OUTPUT/ contains: ==="
find "$OUTPUT" -type f | sort | sed 's/^/    /'
echo ""
echo "Next: commit $OUTPUT/ (the .gitignore whitelist already tracks this"
echo "JSON subset). The runtime loads it instead of the install's binaries."

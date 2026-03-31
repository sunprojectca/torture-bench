#!/usr/bin/env bash
# ----------------------------------------------------------------------------
# torture-bench one-liner
#
# Usage:
#   curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | bash
#   curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | bash -s -- --gb-single 2800 --gb-multi 18000
#
# Flags (all optional):
#   --gb-single <score>   Geekbench 6 single-core score
#   --gb-multi  <score>   Geekbench 6 multi-core score
#
# What it does:
#   1. Detects OS & architecture, installs missing build tools
#   2. Clones the repo (or pulls if already cloned)
#   3. Builds torture-bench + tune-probe via CMake
#   4. Runs torture-bench (30s per module)
#   5. Saves a formatted report to ~/Desktop (or ~/)
# ----------------------------------------------------------------------------
set -e

REPO="https://github.com/sunprojectca/torture-bench.git"
DIR="${HOME}/torture-bench"
DURATION=30
GB_SINGLE=0
GB_MULTI=0

# ── Parse flags ────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --gb-single)  GB_SINGLE="$2"; shift 2 ;;
        --gb-multi)   GB_MULTI="$2";  shift 2 ;;
        *)            shift ;;
    esac
done

# ── Colors ─────────────────────────────────────────────────────────────────
R='\033[0;31m'; G='\033[0;32m'; C='\033[0;36m'; Y='\033[0;33m'; B='\033[1m'; N='\033[0m'
step() { echo -e "${G}[$1/5] $2${N}"; }
fail() { echo -e "${R}ERROR: $1${N}" >&2; exit 1; }

echo ""
echo -e "${C}  +-------------------------------------------------------+${N}"
echo -e "${C}  |${B}   torture-bench — one-command benchmark pipeline   ${C}|${N}"
echo -e "${C}  +-------------------------------------------------------+${N}"
echo ""

# ── Detect platform ───────────────────────────────────────────────────────
OS="$(uname -s)"
ARCH="$(uname -m)"
IS_WSL=0

case "$OS" in
    MINGW*|MSYS*|CYGWIN*) OS="Windows" ;;
    Darwin)               OS="macOS"   ;;
    Linux)                OS="Linux"   ;;
esac

if [ "$OS" = "Linux" ] && grep -qi microsoft /proc/version 2>/dev/null; then
    OS="WSL"; IS_WSL=1
fi

echo -e "  Platform: ${B}${OS} ${ARCH}${N}  |  Duration: ${DURATION}s/module"
[ "$GB_SINGLE" -gt 0 ] 2>/dev/null && echo -e "  Geekbench: single=${GB_SINGLE} multi=${GB_MULTI}"
echo ""

# ═══════════════════════════════════════════════════════════════════════════
# STEP 1: Install & check dependencies
# ═══════════════════════════════════════════════════════════════════════════
step 1 "Checking & installing dependencies..."

need_install=0
for cmd in git cmake; do
    command -v "$cmd" >/dev/null 2>&1 || need_install=1
done
command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 || need_install=1
command -v ninja >/dev/null 2>&1 || command -v make >/dev/null 2>&1 || need_install=1
command -v curl >/dev/null 2>&1 || need_install=1

if [ "$need_install" -eq 1 ]; then
    echo -e "  ${Y}Some tools are missing — installing...${N}"
    if [ "$OS" = "macOS" ]; then
        xcode-select --install 2>/dev/null || true
        if command -v brew >/dev/null 2>&1; then
            brew install cmake ninja git curl 2>/dev/null || true
        else
            echo -e "  ${Y}Homebrew not found — installing...${N}"
            /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
            eval "$(/opt/homebrew/bin/brew shellenv 2>/dev/null || /usr/local/bin/brew shellenv 2>/dev/null)" || true
            brew install cmake ninja git curl 2>/dev/null || true
        fi
    else
        if command -v apt-get >/dev/null 2>&1; then
            sudo apt-get update -qq
            sudo apt-get install -y build-essential cmake ninja-build git curl
        elif command -v dnf >/dev/null 2>&1; then
            sudo dnf install -y gcc gcc-c++ cmake ninja-build git curl
        elif command -v pacman >/dev/null 2>&1; then
            sudo pacman -Sy --noconfirm base-devel cmake ninja git curl
        elif command -v zypper >/dev/null 2>&1; then
            sudo zypper install -y gcc cmake ninja git curl
        elif command -v apk >/dev/null 2>&1; then
            sudo apk add build-base cmake ninja git curl
        else
            fail "No supported package manager found (apt/dnf/pacman/zypper/apk). Install gcc, cmake, ninja, git, curl manually."
        fi
    fi
    echo -e "  ${G}[ok]${N} Packages installed"
fi

# Verify essentials
command -v git   >/dev/null 2>&1 || fail "git still not found after install."
command -v cmake >/dev/null 2>&1 || fail "cmake still not found after install."
command -v curl  >/dev/null 2>&1 || fail "curl still not found after install."

CC_FOUND=""
command -v gcc   >/dev/null 2>&1 && CC_FOUND="gcc"
command -v clang >/dev/null 2>&1 && CC_FOUND="clang"
[ -n "$CC_FOUND" ] || fail "No C compiler found after install."

GENERATOR=""
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
elif command -v make >/dev/null 2>&1; then
    GENERATOR="Unix Makefiles"
else
    fail "No build tool (ninja/make) found after install."
fi

echo -e "  Compiler: ${B}${CC_FOUND}${N}  Generator: ${B}${GENERATOR}${N}"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 2: Clone or update
# ═══════════════════════════════════════════════════════════════════════════
step 2 "Getting source..."

if [ -d "$DIR/.git" ]; then
    echo "  Repo exists at $DIR — pulling latest..."
    cd "$DIR"
    git pull --rebase 2>/dev/null || git pull 2>/dev/null || {
        DIR="${DIR}-fresh-$(date +%s)"
        echo "  Pull failed — cloning fresh to $DIR"
        git clone "$REPO" "$DIR"
        cd "$DIR"
    }
else
    echo "  Cloning $REPO → $DIR"
    git clone "$REPO" "$DIR"
    cd "$DIR"
fi

# ═══════════════════════════════════════════════════════════════════════════
# STEP 3: Configure + Build
# ═══════════════════════════════════════════════════════════════════════════
step 3 "Building..."

BUILD_DIR="$DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC_FOUND" -G "$GENERATOR" 2>&1 | tail -3

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build . --config Release --parallel "$NPROC"

# Find binaries
BENCH=""; PROBE=""
for p in "$BUILD_DIR/torture-bench" "$BUILD_DIR/Release/torture-bench"; do
    [ -f "$p" ] && BENCH="$p" && break
done
for p in "$BUILD_DIR/tune-probe" "$BUILD_DIR/Release/tune-probe"; do
    [ -f "$p" ] && PROBE="$p" && break
done
[ -n "$BENCH" ] || fail "torture-bench binary not found after build."
echo -e "  ${G}[ok]${N} Built successfully"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 4: Run benchmark
# ═══════════════════════════════════════════════════════════════════════════
step 4 "Running torture-bench (${DURATION}s x 20 modules)..."

cd "$DIR"
mkdir -p results

# Run tune-probe first if available
[ -n "$PROBE" ] && [ -f "$PROBE" ] && { "$PROBE" >/dev/null 2>&1 || true; }

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
HOSTNAME_CLEAN=$(hostname 2>/dev/null | tr '[:upper:]' '[:lower:]' | tr ' .' '_' || echo "unknown")
OS_CLEAN=$(echo "$OS" | tr '[:upper:]' '[:lower:]')
ARCH_CLEAN=$(echo "$ARCH" | tr '[:upper:]' '[:lower:]')
JSON_FILE="results/bench_${OS_CLEAN}_${ARCH_CLEAN}_${HOSTNAME_CLEAN}_${TIMESTAMP}.json"
TXT_FILE="results/bench_${OS_CLEAN}_${ARCH_CLEAN}_${HOSTNAME_CLEAN}_${TIMESTAMP}.txt"

echo ""
"$BENCH" --tune -d "$DURATION" -o "$JSON_FILE" --json
echo ""
echo -e "  ${G}[ok]${N} Results saved"

# ═══════════════════════════════════════════════════════════════════════════
# STEP 5: Generate report & finish
# ═══════════════════════════════════════════════════════════════════════════
step 5 "Generating report..."

# Determine report destination — prefer Desktop, fall back to home
REPORT_DIR="$HOME/Desktop"
[ -d "$REPORT_DIR" ] || REPORT_DIR="$HOME"
REPORT_DEST="$REPORT_DIR/torture-bench-report_${TIMESTAMP}.txt"

# The C binary creates a .txt alongside the .json via -o flag
if [ -f "$DIR/$TXT_FILE" ]; then
    cp "$DIR/$TXT_FILE" "$REPORT_DEST"
elif [ -f "$DIR/$JSON_FILE" ]; then
    # Generate report from JSON using awk (no python needed)
    awk '
    BEGIN {
        FS=":"
        print "  +====================================================================="
        print "  |              TORTURE-BENCH RESULTS REPORT"
        print "  +====================================================================="
        print ""
    }' /dev/null > "$REPORT_DEST"

    # Use a small inline parser
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "
import json, sys
d = json.load(open(sys.argv[1]))
p = d.get('platform', {})
print(f'  Generated     : {d.get(\"timestamp\",\"N/A\")}')
print(f'  Composite     : {d.get(\"composite_score\",0)}')
print(f'  Verdict       : {d.get(\"verdict\",\"N/A\")}')
print(f'  Chain Hash    : {d.get(\"chain_proof_hash\",\"N/A\")}')
print()
print('  Platform')
print('  ---------------------------------------------------------------------')
print(f'  OS            : {p.get(\"os\",\"N/A\")}')
print(f'  Arch          : {p.get(\"arch\",\"N/A\")}')
print(f'  CPU           : {p.get(\"cpu\",\"N/A\")}')
print(f'  Cores         : {p.get(\"logical_cores\",\"N/A\")}')
print(f'  RAM           : {p.get(\"ram_gb\", round(p.get(\"ram_bytes\",0)/1073741824,1))} GB')
print()
print('  Module Results')
print('  ---------------------------------------------------------------------')
print(f'  {\"Module\":<24s} {\"Score\":>12s} {\"Ops/sec\":>14s} {\"Time(s)\":>10s}')
print(f'  {\"------------------------\":<24s} {\"------------\":>12s} {\"--------------\":>14s} {\"----------\":>10s}')
for m in d.get('modules', []):
    print(f'  {m[\"name\"]:<24s} {m[\"score\"]:>12.2f} {m.get(\"ops_per_sec\",0):>14.0f} {m.get(\"wall_time_sec\",0):>10.2f}')
print()
print('  =====================================================================')
print(f'  COMPOSITE SCORE : {d.get(\"composite_score\",0)}')
print(f'  VERDICT         : {d.get(\"verdict\",\"N/A\")}')
print('  =====================================================================')
" "$DIR/$JSON_FILE" >> "$REPORT_DEST"
    else
        echo "  (Install python3 for a detailed report. Raw JSON saved at $DIR/$JSON_FILE)" >> "$REPORT_DEST"
    fi
fi

# ── Show scores in terminal ───────────────────────────────────────────────
echo ""
echo -e "${C}  +-----------------------------------------------------------+${N}"
echo -e "${C}  |${B}   BENCHMARK RESULTS                                    ${C}|${N}"
echo -e "${C}  +-----------------------------------------------------------+${N}"

if [ -f "$DIR/$JSON_FILE" ] && command -v python3 >/dev/null 2>&1; then
    python3 -c "
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    for m in d.get('modules', []):
        print(f'  |  {m[\"name\"]:<28s} {m[\"score\"]:>12.2f}       |')
    print('  +-----------------------------------------------------------+')
    print(f'  |  {\"TOTAL SCORE\":<28s} {d.get(\"composite_score\",0):>12.4f}       |')
    print(f'  |  {\"Verdict\":<28s} {d.get(\"verdict\",\"N/A\"):>18s} |')
except Exception as e:
    print(f'  |  (Could not parse results: {e})')
" "$DIR/$JSON_FILE"
fi

echo -e "${C}  +-----------------------------------------------------------+${N}"
echo ""
echo -e "  Report      : ${B}$REPORT_DEST${N}"
echo -e "  Raw JSON    : ${B}$DIR/$JSON_FILE${N}"
echo ""

#!/usr/bin/env bash
# run_and_submit.sh — Build, run both executables, and save results
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"

usage() {
    cat <<'EOF'
Usage: ./run_and_submit.sh [-h]

Build the project, run tune-probe and torture-bench, and save results locally.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# ── Colors ─────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'

echo -e "${CYAN}"
echo "  ╔═══════════════════════════════════════════════════╗"
echo "  ║     TORTURE-BENCH  Run & Submit Pipeline          ║"
echo "  ╚═══════════════════════════════════════════════════╝"
echo -e "${NC}"

# ── Step 1: Build ──────────────────────────────────────────────────────────
echo -e "${GREEN}[1/3] Building...${NC}"
bash "$SCRIPT_DIR/build.sh"

# ── Step 2: Find binaries ─────────────────────────────────────────────────
BENCH="$BUILD_DIR/torture-bench"
PROBE="$BUILD_DIR/tune-probe"

if [ ! -f "$BENCH" ]; then
    echo -e "${RED}ERROR: torture-bench not found at $BENCH${NC}"
    exit 1
fi

# ── Step 3: Run tune-probe ─────────────────────────────────────────────────
echo ""
echo -e "${GREEN}[2/3] Running tune-probe...${NC}"
PROBE_OUT="$RESULTS_DIR/tune_probe_$(date +%Y%m%d_%H%M%S).txt"
mkdir -p "$RESULTS_DIR"
"$PROBE" 2>&1 | tee "$PROBE_OUT"

# ── Step 4: Run torture-bench ──────────────────────────────────────────────
echo ""
echo -e "${GREEN}[3/3] Running torture-bench (this takes a while)...${NC}"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
HOSTNAME=$(hostname | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
ARCH=$(uname -m)
if grep -qi microsoft /proc/version 2>/dev/null || grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
    OS=wsl
else
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
fi
JSON_FILE="$RESULTS_DIR/bench_${OS}_${ARCH}_${HOSTNAME}_${TIMESTAMP}.json"

"$BENCH" --tune -d 30 -o "$JSON_FILE" --json

echo ""
echo -e "${GREEN}  Results saved: $JSON_FILE${NC}"

echo ""
echo -e "${GREEN}  Pipeline complete.${NC}"
echo ""

# ── Open results page in default browser ──────────────────────────────────────
HTML_PAGE="${SCRIPT_DIR}/docs/torture_benchmark.html"
if [ -f "$HTML_PAGE" ]; then
    echo -e "${CYAN}  Opening results page...${NC}"
    if command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$HTML_PAGE" 2>/dev/null &
    elif command -v open >/dev/null 2>&1; then
        open "$HTML_PAGE" 2>/dev/null &
    elif command -v wslview >/dev/null 2>&1; then
        wslview "$HTML_PAGE" 2>/dev/null &
    elif command -v explorer.exe >/dev/null 2>&1; then
        explorer.exe "$(wslpath -w "$HTML_PAGE")" 2>/dev/null &
    fi
fi

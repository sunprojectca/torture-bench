#!/usr/bin/env bash
# build.sh — build torture-bench on Linux and macOS
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
GENERATOR=""

echo ""
echo "  torture-bench build script"
echo "  ────────────────────────────────────────────────────"

# ── Check deps ────────────────────────────────────────────────────────────
if ! command -v cmake >/dev/null 2>&1; then
    echo "  ERROR: cmake not found. Install it and retry."
    exit 1
fi

if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
elif command -v make >/dev/null 2>&1; then
    GENERATOR="Unix Makefiles"
else
    echo "  ERROR: neither ninja nor make was found. Install one and retry."
    exit 1
fi

# Prefer clang if available (matches Apple and Snapdragon native toolchain)
if command -v clang >/dev/null 2>&1; then
    export CC=clang
    echo "  Compiler: clang ($(clang --version | head -1))"
else
    export CC=gcc
    echo "  Compiler: gcc ($(gcc --version | head -1))"
fi

if grep -qi microsoft /proc/version 2>/dev/null || grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
    PLATFORM_NAME="WSL $(uname -m)"
else
    PLATFORM_NAME="$(uname -s) $(uname -m)"
fi

echo "  Generator: $GENERATOR"
echo "  Platform: $PLATFORM_NAME"
echo ""

# ── Normalize future timestamps (prevents make clock-skew warnings) ───────
NOW_REF="$(mktemp)"
touch "$NOW_REF"
while IFS= read -r -d '' src; do
    if [[ "$src" -nt "$NOW_REF" ]]; then
        touch "$src"
    fi
done < <(find "$SCRIPT_DIR" \
    -path "$BUILD_DIR" -prune -o \
    -type f \( -name "*.c" -o -name "*.h" -o -name "CMakeLists.txt" -o -name "*.cmake" -o -name "*.sh" \) \
    -print0)
rm -f "$NOW_REF"

# ── Configure ─────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    -G "$GENERATOR" \
    2>&1 | grep -v "^--" | head -30

echo ""

# ── Build ─────────────────────────────────────────────────────────────────
NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake --build . --parallel "$NPROC"

echo ""
echo "  Build complete:"
ls -lh "$BUILD_DIR/torture-bench" "$BUILD_DIR/tune-probe" 2>/dev/null || true
echo ""
echo "  Usage:"
echo "    ./build/tune-probe              # run anti-cheat probe first"
echo "    ./build/torture-bench --tune    # full benchmark with probe"
echo "    ./build/torture-bench -d 30     # 30s per module (thorough)"
echo "    ./build/torture-bench -o results.json --json"
echo ""

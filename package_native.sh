#!/usr/bin/env bash
# package_native.sh
# Builds torture-bench + tune-probe for the NATIVE architecture of this host.
# Verifies the built binary machine type matches native arch before packaging.
# Output: torture-bench-<os>-<arch>.tar.gz in repo root.
#
# Usage:
#   chmod +x package_native.sh
#   ./package_native.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/package_native.log"

log()  { local ts; ts="$(date '+%H:%M:%S')"; echo "[$ts] $*" | tee -a "$LOG"; }
fail() { log ""; log "ERROR: $*"; log "=== FAILED ==="; exit 1; }

echo "=== package_native.sh started $(date) ===" > "$LOG"

# ─── 1. Detect native architecture ──────────────────────────────────────────
log "[1/6] Detecting native architecture..."

OS="$(uname -s)"   # Linux | Darwin
RAW_ARCH="$(uname -m)"   # x86_64 | aarch64 | arm64 | riscv64 | ppc64le

# Normalise to a consistent label
case "$RAW_ARCH" in
    x86_64)           ARCH="x86_64" ;;
    aarch64|arm64)    ARCH="arm64" ;;
    armv7l|armv7)     ARCH="armv7" ;;
    riscv64)          ARCH="riscv64" ;;
    ppc64le|powerpc64le) ARCH="ppc64le" ;;
    ppc64|powerpc64)  ARCH="ppc64" ;;
    *)                ARCH="$RAW_ARCH" ;;
esac

case "$OS" in
    Linux)  OS_LABEL="linux" ;;
    Darwin) OS_LABEL="macos" ;;
    *)      OS_LABEL="$(echo "$OS" | tr '[:upper:]' '[:lower:]')" ;;
esac

PACKAGE_NAME="torture-bench-${OS_LABEL}-${ARCH}"
BUILD_DIR="$SCRIPT_DIR/pkg-build-${ARCH}"
OUT_DIR="$SCRIPT_DIR/$PACKAGE_NAME"
TARBALL="$SCRIPT_DIR/${PACKAGE_NAME}.tar.gz"

log "  OS           : $OS ($OS_LABEL)"
log "  Native arch  : $RAW_ARCH  =>  $ARCH"
log "  Package      : $TARBALL"

# ─── 2. Detect compiler targeting native arch ────────────────────────────────
log ""
log "[2/6] Selecting native compiler..."

# Prefer clang on macOS (Apple native), gcc on Linux
if [ "$OS" = "Darwin" ]; then
    if command -v clang >/dev/null 2>&1; then
        export CC=clang
        CC_LABEL="clang ($(clang --version 2>&1 | head -1))"
    else
        fail "clang not found. On macOS run: xcode-select --install"
    fi
else
    if command -v gcc >/dev/null 2>&1; then
        export CC=gcc
        CC_LABEL="gcc ($(gcc --version | head -1))"
    elif command -v clang >/dev/null 2>&1; then
        export CC=clang
        CC_LABEL="clang ($(clang --version 2>&1 | head -1))"
    else
        fail "No C compiler found. Install gcc or clang."
    fi
fi

# Verify the compiler targets native arch, not cross-compiling accidentally
COMPILER_TARGET="$($CC -dumpmachine 2>/dev/null || echo 'unknown')"
log "  Compiler     : $CC  =>  $CC_LABEL"
log "  Compiler target (dumpmachine): $COMPILER_TARGET"

EMULATION_WARN=0
case "$ARCH" in
    x86_64)
        if echo "$COMPILER_TARGET" | grep -qvE '(x86_64|amd64)'; then
            log "  *** WARNING: compiler target '$COMPILER_TARGET' does not match native $ARCH"
            EMULATION_WARN=1
        fi ;;
    arm64)
        if echo "$COMPILER_TARGET" | grep -qvE '(aarch64|arm64)'; then
            log "  *** WARNING: compiler target '$COMPILER_TARGET' does not match native $ARCH"
            EMULATION_WARN=1
        fi ;;
    aarch64)
        if echo "$COMPILER_TARGET" | grep -qvE '(aarch64|arm64)'; then
            log "  *** WARNING: compiler target '$COMPILER_TARGET' does not match native $ARCH"
            EMULATION_WARN=1
        fi ;;
esac

if [ "$EMULATION_WARN" -eq 1 ]; then
    log "  *** Continuing, but verify results. The binary may be cross-compiled or emulated."
fi

# ─── 3. cmake and build tool check ──────────────────────────────────────────
log ""
log "[3/6] Checking build tools..."
command -v cmake >/dev/null 2>&1 || fail "cmake not found. Install it (e.g. brew install cmake / apt install cmake)."
log "  cmake: $(cmake --version | head -1)"

if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
    log "  generator: Ninja"
elif command -v make >/dev/null 2>&1; then
    GENERATOR="Unix Makefiles"
    log "  generator: Unix Makefiles"
else
    fail "Neither ninja nor make found. Install one and retry."
fi

# ─── 4. Build ────────────────────────────────────────────────────────────────
log ""
log "[4/6] Building (native $ARCH)..."

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
    -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    2>&1 | tee -a "$LOG"

if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
else
    JOBS=4
fi

cmake --build "$BUILD_DIR" --parallel "$JOBS" 2>&1 | tee -a "$LOG"

# ─── 5. Verify binary architecture ──────────────────────────────────────────
log ""
log "[5/6] Verifying binary architecture..."

TB_BIN="$BUILD_DIR/torture-bench"
TP_BIN="$BUILD_DIR/tune-probe"
[ -f "$TB_BIN" ] || fail "torture-bench not found at $TB_BIN"
[ -f "$TP_BIN" ] || fail "tune-probe not found at $TP_BIN"

if command -v file >/dev/null 2>&1; then
    TB_FILE_OUT="$(file "$TB_BIN")"
    log "  file output: $TB_FILE_OUT"

    # Check for unexpected arch in 'file' output
    ARCH_OK=1
    case "$ARCH" in
        x86_64)
            echo "$TB_FILE_OUT" | grep -qiE '(x86.64|x86-64|ELF 64-bit LSB.*x86)' || ARCH_OK=0 ;;
        arm64|aarch64)
            echo "$TB_FILE_OUT" | grep -qiE '(aarch64|arm64|ARM aarch64)' || ARCH_OK=0 ;;
        riscv64)
            echo "$TB_FILE_OUT" | grep -qiE '(RISC-V|riscv)' || ARCH_OK=0 ;;
    esac

    if [ "$ARCH_OK" -eq 0 ]; then
        fail "Binary arch mismatch. 'file' says: $TB_FILE_OUT  —  expected $ARCH native binary. Emulation/cross-compile detected."
    fi
    log "  Arch check PASSED"
else
    log "  'file' command not available — skipping arch verification"
fi

# ─── 6. Package ──────────────────────────────────────────────────────────────
log ""
log "[6/6] Packaging..."
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

cp "$TB_BIN" "$OUT_DIR/torture-bench"
cp "$TP_BIN" "$OUT_DIR/tune-probe"
chmod +x "$OUT_DIR/torture-bench" "$OUT_DIR/tune-probe"

# Include the shell runner
if [ -f "$SCRIPT_DIR/bench.sh" ]; then
    cp "$SCRIPT_DIR/bench.sh" "$OUT_DIR/bench.sh"
    chmod +x "$OUT_DIR/bench.sh"
fi

cat > "$OUT_DIR/README.txt" <<EOF
torture-bench — ${OS_LABEL} ${ARCH} package
Built: $(date '+%Y-%m-%d %H:%M:%S')
Compiler target: ${COMPILER_TARGET}

Quick run:
  ./torture-bench

Or use the full pipeline:
  bash bench.sh
EOF

rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$SCRIPT_DIR" "$PACKAGE_NAME"

# Clean up staging
rm -rf "$BUILD_DIR" "$OUT_DIR"

log "  Package: $TARBALL"
log ""
log "=== DONE: $TARBALL ==="
echo "=== SUCCEEDED ===" >> "$LOG"

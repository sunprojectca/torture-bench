# build_release.ps1
# Compiles torture-bench.exe and tune-probe.exe into the repo root folder.
# - On error: prints error and keeps window open.
# - On success: closes automatically.
# All output is also written to build_release.log

$ErrorActionPreference = "SilentlyContinue"

$RootDir  = $PSScriptRoot
$BuildDir = Join-Path $RootDir "ci-build-release"
$LogFile  = Join-Path $RootDir "build_release.log"

# Tee all output to log and console
function Log($msg) {
    $ts = Get-Date -Format "HH:mm:ss"
    $line = "[$ts] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line
}

function Fail($msg) {
    Log ""
    Log "ERROR: $msg"
    Log ""
    Log "Build FAILED. Window kept open for review."
    Add-Content -Path $LogFile -Value "=== FAILED ==="
    Write-Host ""
    Write-Host "Press any key to close..." -ForegroundColor Red
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    exit 1
}

# Start fresh log
Set-Content -Path $LogFile -Value "=== build_release.ps1 started $(Get-Date) ==="

Log "Root   : $RootDir"
Log "Build  : $BuildDir"
Log "Log    : $LogFile"
Log ""

# ── 1. Locate CMake ──────────────────────────────────────────────────────────
Log "[1/4] Locating CMake..."
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    Fail "cmake not found. Install CMake (https://cmake.org/download/) and add it to PATH."
}
Log "  cmake: $($cmake.Source)"

# ── 2. Clean previous ci-build-release ──────────────────────────────────────
Log "[2/4] Preparing build directory..."
if (Test-Path $BuildDir) {
    Log "  Removing old $BuildDir ..."
    Remove-Item $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $BuildDir | Out-Null

# ── 3. CMake configure ───────────────────────────────────────────────────────
Log "[3/4] Configuring with CMake..."
$configOut = & cmake -S $RootDir -B $BuildDir -DCMAKE_BUILD_TYPE=Release 2>&1
$configOut | ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed (exit $LASTEXITCODE)." }

# ── 4. Build ─────────────────────────────────────────────────────────────────
Log "[4/4] Building (Release)..."
$buildOut = & cmake --build $BuildDir --config Release --parallel 2>&1
$buildOut | ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -ne 0) { Fail "CMake build failed (exit $LASTEXITCODE)." }

# ── Copy EXEs to repo root ───────────────────────────────────────────────────
Log ""
Log "Copying executables to repo root..."

$candidates = @(
    (Join-Path $BuildDir "Release\torture-bench.exe"),
    (Join-Path $BuildDir "torture-bench.exe")
)
$tbSrc = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1

$candidates2 = @(
    (Join-Path $BuildDir "Release\tune-probe.exe"),
    (Join-Path $BuildDir "tune-probe.exe")
)
$tpSrc = $candidates2 | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $tbSrc) { Fail "torture-bench.exe not found in build output." }
if (-not $tpSrc) { Fail "tune-probe.exe not found in build output." }

Copy-Item $tbSrc (Join-Path $RootDir "torture-bench.exe") -Force
Copy-Item $tpSrc (Join-Path $RootDir "tune-probe.exe")    -Force

Log "  -> torture-bench.exe"
Log "  -> tune-probe.exe"
Log ""
Log "=== Build SUCCEEDED ==="
Add-Content -Path $LogFile -Value "=== SUCCEEDED ==="

@echo off
REM run_and_submit.bat — Build, run both executables, and save results
setlocal EnableDelayedExpansion

:parse_args
if "%~1"=="" goto after_args
if /I "%~1"=="-h" goto usage
if /I "%~1"=="--help" goto usage
echo ERROR: Unknown option: %~1
goto usage_error

:usage
echo Usage: run_and_submit.bat [-h]
echo.
echo Build the project, run tune-probe and torture-bench, and save results locally.
exit /b 0

:usage_error
echo.
echo Use --help to see available options.
exit /b 1

:after_args

echo.
echo   ╔═══════════════════════════════════════════════════╗
echo   ║     TORTURE-BENCH  Run ^& Save Pipeline            ║
echo   ╚═══════════════════════════════════════════════════╝
echo.

set SCRIPT_DIR=%~dp0
set BUILD_DIR=%SCRIPT_DIR%build
set RESULTS_DIR=%SCRIPT_DIR%results
call :detect_native_arch

REM ── Step 1: Build ────────────────────────────────────────────────────────
echo [1/3] Building...
call "%SCRIPT_DIR%build.bat"
if %ERRORLEVEL% NEQ 0 exit /b 1

REM ── Step 2: Find binaries ───────────────────────────────────────────────
set BENCH=
if exist "%BUILD_DIR%\Release\torture-bench.exe" (
    set BENCH=%BUILD_DIR%\Release\torture-bench.exe
    set PROBE=%BUILD_DIR%\Release\tune-probe.exe
) else (
    if exist "%BUILD_DIR%\torture-bench.exe" (
        set BENCH=%BUILD_DIR%\torture-bench.exe
        set PROBE=%BUILD_DIR%\tune-probe.exe
    )
)

if "!BENCH!"=="" (
    echo ERROR: torture-bench.exe not found
    exit /b 1
)

REM ── Step 2: Run tune-probe ──────────────────────────────────────────────
echo.
echo [2/3] Running tune-probe...
if not exist "%RESULTS_DIR%" mkdir "%RESULTS_DIR%"
"!PROBE!"

REM ── Step 3: Run torture-bench ───────────────────────────────────────────
echo.
echo [3/3] Running torture-bench (this takes a while)...

for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set TIMESTAMP=%%I
set JSON_FILE=%RESULTS_DIR%\bench_windows_%RESULT_ARCH%_%COMPUTERNAME%_%TIMESTAMP%.json

"!BENCH!" --tune -d 30 -o "%JSON_FILE%" --json

echo.
echo   Results saved: %JSON_FILE%

echo.
echo   Pipeline complete.
echo.

rem ── Open results page in default browser ──────────────────────────────────
if exist "%SCRIPT_DIR%docs\torture_benchmark.html" (
    echo   Opening results page...
    start "" "%SCRIPT_DIR%docs\torture_benchmark.html"
)

endlocal
exit /b 0

:detect_native_arch
set "NATIVE_ARCH="
set "RESULT_ARCH=amd64"
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToUpper()" 2^>nul`) do (
    if not defined NATIVE_ARCH set "NATIVE_ARCH=%%~I"
)
if /I "%NATIVE_ARCH%"=="ARM64" (
    set "RESULT_ARCH=arm64"
    exit /b 0
)
if /I "%NATIVE_ARCH%"=="X86" (
    set "RESULT_ARCH=x86"
    exit /b 0
)
if /I "%PROCESSOR_ARCHITEW6432%"=="ARM64" (
    set "RESULT_ARCH=arm64"
    exit /b 0
)
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
    set "RESULT_ARCH=arm64"
    exit /b 0
)
if /I "%PROCESSOR_ARCHITECTURE%"=="X86" (
    set "RESULT_ARCH=x86"
    exit /b 0
)
exit /b 0

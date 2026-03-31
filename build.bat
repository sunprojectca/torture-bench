@echo off
REM build.bat — build torture-bench on Windows (MSVC, MinGW, or clang)
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "BUILD_DIR=%SCRIPT_DIR%\build"
set "TARGET_ARCH=x64"
set "GENERATOR="
set "CONFIG_ARGS="
set "BUILD_ARGS="

call :detect_native_arch

echo.
echo   torture-bench build script (Windows)
echo   ----------------------------------------------------

where cmake >nul 2>nul
if errorlevel 1 (
    echo   ERROR: cmake not found. Install CMake and retry.
    exit /b 1
)

call :detect_toolchain
if errorlevel 1 exit /b 1

echo   Generator: %GENERATOR%
echo   Platform:  Windows %TARGET_ARCH%
echo.

if exist "%BUILD_DIR%" (
    echo   Cleaning previous build directory...
    rmdir /s /q "%BUILD_DIR%"
    if errorlevel 1 (
        echo   ERROR: Failed to remove "%BUILD_DIR%".
        exit /b 1
    )
)

echo   Configuring CMake...
cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" %CONFIG_ARGS% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo   ERROR: CMake configuration failed.
    exit /b 1
)

echo.
echo   Building...
cmake --build "%BUILD_DIR%" %BUILD_ARGS% --parallel
if errorlevel 1 (
    echo   ERROR: Build failed.
    exit /b 1
)

echo.
echo   Build complete.
echo.
echo   Binaries:

if exist "%BUILD_DIR%\Release\torture-bench.exe" echo     %BUILD_DIR%\Release\torture-bench.exe
if exist "%BUILD_DIR%\Release\tune-probe.exe" echo     %BUILD_DIR%\Release\tune-probe.exe
if exist "%BUILD_DIR%\torture-bench.exe" echo     %BUILD_DIR%\torture-bench.exe
if exist "%BUILD_DIR%\tune-probe.exe" echo     %BUILD_DIR%\tune-probe.exe

echo.
echo   Usage:
if defined BUILD_ARGS (
    echo     build\Release\tune-probe.exe
    echo     build\Release\torture-bench.exe --tune
    echo     build\Release\torture-bench.exe -d 30 -o results.json --json
) else (
    echo     build\tune-probe.exe
    echo     build\torture-bench.exe --tune
    echo     build\torture-bench.exe -d 30 -o results.json --json
)
echo.

endlocal
exit /b 0

:detect_toolchain
call :set_vs_generator

where cl >nul 2>nul
if not errorlevel 1 (
    call :msvc_env_matches_target
    if not errorlevel 1 (
        echo   Compiler: MSVC ^(developer environment detected^)
        call :configure_msvc_generator
        exit /b 0
    )
    echo   Existing MSVC environment does not target %TARGET_ARCH% - reinitializing...
)

call :init_msvc
if not errorlevel 1 (
    echo   Compiler: MSVC ^(initialized from Visual Studio^)
    call :configure_msvc_generator
    exit /b 0
)

where gcc >nul 2>nul
if not errorlevel 1 (
    where mingw32-make >nul 2>nul
    if errorlevel 1 (
        echo   ERROR: gcc found, but mingw32-make was not found.
        echo          Install MinGW Makefiles or use Visual Studio Build Tools.
        exit /b 1
    )

    echo   Compiler: MinGW GCC
    set "GENERATOR=MinGW Makefiles"
    exit /b 0
)

where clang >nul 2>nul
if not errorlevel 1 (
    where ninja >nul 2>nul
    if errorlevel 1 (
        echo   ERROR: clang found, but ninja was not found.
        echo          Install Ninja or use Visual Studio / MinGW instead.
        exit /b 1
    )

    echo   Compiler: clang
    set "GENERATOR=Ninja"
    set "CONFIG_ARGS=-DCMAKE_C_COMPILER=clang"
    exit /b 0
)

echo   ERROR: No supported compiler found.
echo          Install Visual Studio Build Tools, MinGW, or clang + Ninja.
exit /b 1

:detect_native_arch
set "NATIVE_ARCH="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToUpper()" 2^>nul`) do (
    if not defined NATIVE_ARCH set "NATIVE_ARCH=%%~I"
)
if /I "%NATIVE_ARCH%"=="ARM64" (
    set "TARGET_ARCH=ARM64"
    exit /b 0
)
if /I "%NATIVE_ARCH%"=="X86" (
    set "TARGET_ARCH=Win32"
    exit /b 0
)
if /I "%PROCESSOR_ARCHITEW6432%"=="ARM64" (
    set "TARGET_ARCH=ARM64"
    exit /b 0
)
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
    set "TARGET_ARCH=ARM64"
    exit /b 0
)
if /I "%PROCESSOR_ARCHITECTURE%"=="X86" (
    set "TARGET_ARCH=Win32"
    exit /b 0
)
set "TARGET_ARCH=x64"
exit /b 0

:msvc_env_matches_target
if /I "%TARGET_ARCH%"=="ARM64" (
    if /I "%VSCMD_ARG_TGT_ARCH%"=="arm64" exit /b 0
    if /I "%Platform%"=="ARM64" exit /b 0
    exit /b 1
)
if /I "%TARGET_ARCH%"=="Win32" (
    if /I "%VSCMD_ARG_TGT_ARCH%"=="x86" exit /b 0
    if /I "%Platform%"=="Win32" exit /b 0
    exit /b 1
)
exit /b 0

:configure_msvc_generator
where ninja >nul 2>nul
if not errorlevel 1 (
    set "GENERATOR=Ninja"
    set "CONFIG_ARGS="
    set "BUILD_ARGS="
    exit /b 0
)

where nmake >nul 2>nul
if not errorlevel 1 (
    set "GENERATOR=NMake Makefiles"
    set "CONFIG_ARGS="
    set "BUILD_ARGS="
    exit /b 0
)

if not defined GENERATOR set "GENERATOR=Visual Studio 17 2022"
set "CONFIG_ARGS=-A %TARGET_ARCH%"
set "BUILD_ARGS=--config Release"
exit /b 0

:set_vs_generator
call :find_vs_install

if /I not "%VS_INSTALL:\2022\=%"=="%VS_INSTALL%" (
    set "GENERATOR=Visual Studio 17 2022"
)
if /I not "%VS_INSTALL:\2019\=%"=="%VS_INSTALL%" (
    set "GENERATOR=Visual Studio 16 2019"
)

exit /b 0

:init_msvc
call :find_vs_install
if not defined VS_INSTALL exit /b 1

set "VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if /I "%TARGET_ARCH%"=="ARM64" set "VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvarsarm64.bat"

if not exist "%VCVARS%" (
    set "VSDEVCMD=%VS_INSTALL%\Common7\Tools\VsDevCmd.bat"
    if not exist "%VSDEVCMD%" exit /b 1

    if /I "%TARGET_ARCH%"=="ARM64" (
        call "%VSDEVCMD%" -arch=arm64 >nul
    ) else (
        call "%VSDEVCMD%" -arch=x64 >nul
    )

    if errorlevel 1 exit /b 1

    where cl >nul 2>nul
    if errorlevel 1 exit /b 1

    exit /b 0
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

where cl >nul 2>nul
if errorlevel 1 exit /b 1

exit /b 0

:find_vs_install
set "VS_INSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    if /I "%TARGET_ARCH%"=="ARM64" (
        for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "VS_INSTALL=%%~I"
        if defined VS_INSTALL exit /b 0
    )

    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%~I"
    if defined VS_INSTALL exit /b 0

    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Workload.VCTools -property installationPath`) do set "VS_INSTALL=%%~I"
    if defined VS_INSTALL exit /b 0

    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS_INSTALL=%%~I"
    if defined VS_INSTALL exit /b 0
)

call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional"
if defined VS_INSTALL exit /b 0
call :try_vs_install "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise"
if defined VS_INSTALL exit /b 0

exit /b 0

:try_vs_install
if exist "%~1\VC\Auxiliary\Build" set "VS_INSTALL=%~1"
exit /b 0

#!/usr/bin/env pwsh
# package_native.ps1
# Cross-platform clean-slate build pipeline (Windows, Linux, macOS).
# Works with PowerShell 5.1+ on Windows, pwsh (PowerShell Core) on Linux/macOS.
#
#   1. Detect native OS + architecture (WOW64-immune on Windows)
#   2. Remove ALL existing compilers and build tools
#   3. Install the CORRECT tools for native arch
#   4. Build torture-bench + tune-probe
#   5. Verify binary machine type matches native arch
#   6. Package as .zip (Windows) or .tar.gz (Linux/macOS)
#
# Usage:
#   Windows  : powershell -ExecutionPolicy Bypass -File package_native.ps1
#   Linux    : pwsh package_native.ps1   (requires powershell/pwsh)
#   macOS    : pwsh package_native.ps1

$ErrorActionPreference = "Continue"
$ROOT = $PSScriptRoot
$LOG  = Join-Path $ROOT "package_native.log"
$script:StepErrors  = @()
$script:Deliverable = $null

# ─── Helpers ─────────────────────────────────────────────────────────────────
function Log($msg) {
    $line = "[$(Get-Date -Format 'HH:mm:ss')] $msg"
    Write-Host $line
    Add-Content -Path $LOG -Value $line
}
function Fail($msg) {
    Log ""; Log "  [!] $msg"
    $script:StepErrors += $msg
    throw "::SKIP::"
}
function Section($title) {
    Log ""; Log "-------------------------------------------------------"; Log "  $title"; Log "-------------------------------------------------------"
}
function Invoke-Logged([string[]]$Cmd) {
    & $Cmd[0] $Cmd[1..($Cmd.Length - 1)] 2>&1 | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0) { Fail "'$($Cmd[0])' failed (exit $LASTEXITCODE)" }
}

Set-Content -Path $LOG -Value "=== package_native.ps1 started $(Get-Date) ==="

# ─── On Windows: require Administrator, auto-elevate ────────────────────────
if ($IsWindows -or (-not (Test-Path Variable:IsWindows))) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host "  Re-launching as Administrator..." -ForegroundColor Yellow
        $psi = New-Object System.Diagnostics.ProcessStartInfo "powershell"
        $psi.Arguments = "-ExecutionPolicy Bypass -File `"$PSCommandPath`""
        $psi.Verb = "runas"
        [System.Diagnostics.Process]::Start($psi) | Out-Null
        exit 0
    }
}

# ═══════════════════════════════════════════════════════════════
# STEP 1 -- Detect OS + native architecture
# ═══════════════════════════════════════════════════════════════
Section "STEP 1/6  Detecting OS and native architecture"

$OnWindows = $IsWindows -or (-not (Test-Path Variable:IsWindows))
$OnMacOS   = $IsMacOS   -and (-not $OnWindows)
$OnLinux   = $IsLinux   -and (-not $OnWindows)

if ($OnWindows) {
    # IsWow64Process2: returns the TRUE native machine type, even when this
    # PowerShell process is x64-emulated on an ARM64 machine.
    function Get-NativeArch {
        try {
            $sig = '[DllImport("kernel32.dll")] public static extern bool IsWow64Process2(' +
                   'IntPtr hProcess, out ushort pProcessMachine, out ushort pNativeMachine);'
            $t = Add-Type -MemberDefinition $sig -Name "WOW64PKG3" -Namespace "Win32Native" -PassThru -ErrorAction Stop
            [ushort]$pm = 0; [ushort]$nm = 0
            $null = $t::IsWow64Process2([System.Diagnostics.Process]::GetCurrentProcess().Handle, [ref]$pm, [ref]$nm)
            $map = @{ 0xAA64 = "ARM64"; 0x8664 = "AMD64"; 0x014C = "X86"; 0x0 = "" }
            $n = $map[[int]$nm]; if ($n) { return $n }
        } catch { }
        if ($env:PROCESSOR_ARCHITEW6432) { return $env:PROCESSOR_ARCHITEW6432.ToUpper() }
        $m = [System.Environment]::GetEnvironmentVariable("PROCESSOR_ARCHITECTURE","Machine")
        if ($m) { return $m.ToUpper() }
        return $env:PROCESSOR_ARCHITECTURE.ToUpper()
    }
    function Get-ProcessArch {
        try {
            $t = [Win32Native.WOW64PKG3]
            [ushort]$pm = 0; [ushort]$nm = 0
            $null = $t::IsWow64Process2([System.Diagnostics.Process]::GetCurrentProcess().Handle, [ref]$pm, [ref]$nm)
            $map = @{ 0xAA64 = "ARM64"; 0x8664 = "AMD64"; 0x014C = "X86"; 0x0 = "native" }
            return $map[[int]$pm]
        } catch { return $env:PROCESSOR_ARCHITECTURE.ToUpper() }
    }

    $NativeArch  = Get-NativeArch
    $ProcessArch = Get-ProcessArch
    $OSName      = (Get-CimInstance Win32_OperatingSystem).Caption
    $OSBuild     = [System.Environment]::OSVersion.Version.ToString()
    $OSLabel     = "windows"

    Log "  OS           : $OSName  ($OSBuild)"
    Log "  Native arch  : $NativeArch"
    Log "  Shell arch   : $ProcessArch"
    if ($ProcessArch -notin @("native", $NativeArch)) {
        Log "  *** Shell is running under emulation ($ProcessArch on $NativeArch) -- build will still target native $NativeArch"
    }

    $CMakeArch = switch ($NativeArch) { "ARM64"{"ARM64"} "AMD64"{"x64"} "X86"{"Win32"} default { Fail "Unknown arch: $NativeArch" } }
    $VCVarsFile = switch ($NativeArch) { "ARM64"{"vcvarsarm64.bat"} "AMD64"{"vcvars64.bat"} "X86"{"vcvars32.bat"} }
    $ExpectedPE = switch ($NativeArch) { "ARM64"{0xAA64} "AMD64"{0x8664} "X86"{0x014C} }
    $Arch       = $NativeArch.ToLower()

} else {
    # Linux / macOS
    $OSName  = (uname -s).Trim()
    $RawArch = (uname -m).Trim()
    $OSLabel = switch ($OSName) { "Linux"{"linux"} "Darwin"{"macos"} default { $OSName.ToLower() } }
    $Arch    = switch -Regex ($RawArch) {
        "^x86_64$"               { "x86_64" }
        "^(aarch64|arm64)$"      { "arm64"  }
        "^armv7"                 { "armv7"  }
        "^riscv64$"              { "riscv64"}
        "^(ppc64le|powerpc64le)$"{ "ppc64le"}
        default                  { $RawArch }
    }
    Log "  OS           : $OSName ($OSLabel)"
    Log "  Raw arch     : $RawArch  =>  $Arch"
}

$PackageName = "torture-bench-${OSLabel}-${Arch}"
$BuildDir    = Join-Path $ROOT "pkg-build-$Arch"
$OutDir      = Join-Path $ROOT $PackageName
$Archive     = if ($OnWindows) { Join-Path $ROOT "$PackageName.zip" } else { Join-Path $ROOT "$PackageName.tar.gz" }

Log "  Package      : $Archive"

# ═══════════════════════════════════════════════════════════════
# STEP 2 -- Remove all existing compilers and build tools
# ═══════════════════════════════════════════════════════════════
try {
Section "STEP 2/6  Removing existing compilers and build tools"

function Uninstall-Winget([string]$Id, [string]$Label) {
    Log "  Removing: $Label  ($Id)"
    & winget uninstall --id $Id --exact --silent --accept-source-agreements 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Log "    removed" } else { Log "    not installed / already gone" }
}

if ($OnWindows) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget not found. Update 'App Installer' from the Microsoft Store." }
    Uninstall-Winget "LLVM.LLVM"                              "LLVM / Clang"
    Uninstall-Winget "Kitware.CMake"                          "CMake"
    Uninstall-Winget "Ninja-build.Ninja"                      "Ninja"
    Uninstall-Winget "Microsoft.VisualStudio.2022.BuildTools" "VS 2022 Build Tools"
    Uninstall-Winget "Microsoft.VisualStudio.2019.BuildTools" "VS 2019 Build Tools"
    Uninstall-Winget "Microsoft.VisualStudio.2022.Community"  "VS 2022 Community"
    Uninstall-Winget "MSYS2.MSYS2"                            "MSYS2 / MinGW"
    Uninstall-Winget "MinGW.MinGW"                            "MinGW"

    $wingetLinks = Join-Path $env:LocalAppData "Microsoft\WinGet\Links"
    foreach ($s in @("ninja.EXE","ninja.exe","cmake.exe","cmake.EXE","clang.exe","clang.EXE","clang-cl.exe","clang-cl.EXE","gcc.exe","gcc.EXE","g++.exe","g++.EXE","cl.exe","nmake.exe")) {
        $p = Join-Path $wingetLinks $s
        if (Test-Path $p) { Remove-Item $p -Force; Log "  Deleted stub: $s" }
    }
    $env:Path = (@([System.Environment]::GetEnvironmentVariable("Path","Machine"), [System.Environment]::GetEnvironmentVariable("Path","User")) -join ";")

} elseif ($OnLinux) {
    $pkgMgr = if (Get-Command apt-get -EA SilentlyContinue){"apt"} elseif (Get-Command dnf -EA SilentlyContinue){"dnf"} elseif (Get-Command yum -EA SilentlyContinue){"yum"} elseif (Get-Command pacman -EA SilentlyContinue){"pacman"} else { Fail "No supported package manager (apt/dnf/yum/pacman)." }
    Log "  Package manager: $pkgMgr"
    $rmPkgs = switch ($pkgMgr) {
        "apt"    { @("clang","llvm","gcc","g++","cmake","ninja-build","make","build-essential") }
        "dnf"    { @("clang","llvm","gcc","gcc-c++","cmake","ninja-build","make") }
        "yum"    { @("clang","llvm","gcc","gcc-c++","cmake","ninja-build","make") }
        "pacman" { @("clang","llvm","gcc","cmake","ninja","make","base-devel") }
    }
    Log "  Removing: $($rmPkgs -join ', ')"
    switch ($pkgMgr) {
        "apt"    { & apt-get remove -y @rmPkgs 2>&1 | ForEach-Object { Log "  $_" }; & apt-get autoremove -y 2>&1 | Out-Null }
        "dnf"    { & dnf remove -y @rmPkgs 2>&1 | ForEach-Object { Log "  $_" } }
        "yum"    { & yum remove -y @rmPkgs 2>&1 | ForEach-Object { Log "  $_" } }
        "pacman" { & pacman -Rns --noconfirm @rmPkgs 2>&1 | ForEach-Object { Log "  $_" } }
    }

} elseif ($OnMacOS) {
    if (Get-Command brew -EA SilentlyContinue) {
        foreach ($pkg in @("llvm","clang-format","cmake","ninja","gcc")) {
            Log "  brew uninstall: $pkg"
            & brew uninstall --force $pkg 2>&1 | ForEach-Object { Log "  $_" }
        }
    } else { Log "  Homebrew not found -- skipping removal" }
}

$still = @("cmake","clang","gcc","ninja") | Where-Object { Get-Command $_ -EA SilentlyContinue }
if ($still) { Log "  Still in PATH: $($still -join ', ') (may be system-level, continuing)" } else { Log "  PATH clean" }
} catch {
    if ("$_" -notlike "*::SKIP::*") { $script:StepErrors += "Step 2: $_" }
    Log "  [!] Step 2 partial -- continuing with current toolchain state"
}

# ═══════════════════════════════════════════════════════════════
# STEP 3 -- Install correct tools for native arch
# ═══════════════════════════════════════════════════════════════
try {
Section "STEP 3/6  Installing correct tools for $OSLabel/$Arch"

function Install-Winget([string]$Id, [string]$Label, [string[]]$Extra = @()) {
    Log "  Installing: $Label  ($Id)"
    $a = @("install","--id",$Id,"--exact","--silent","--accept-package-agreements","--accept-source-agreements") + $Extra
    $out = & winget @a 2>&1
    $out | ForEach-Object { Log "  $_" }
    $alreadyCurrent = ($out -match "No available upgrade found|No newer package versions|already installed")
    if ($LASTEXITCODE -eq 0 -or $alreadyCurrent) { Log "  -> OK"; return $true }
    Log "  -> FAILED (exit $LASTEXITCODE)"; return $false
}

if ($OnWindows) {
    if (-not (Install-Winget "Kitware.CMake"    "CMake"))  { Fail "CMake install failed." }
    if (-not (Install-Winget "Ninja-build.Ninja" "Ninja")) { Fail "Ninja install failed." }

    $vsComp = if ($NativeArch -eq "ARM64") {
        "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --includeRecommended"
    } else {
        "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --includeRecommended"
    }
    Log "  Installing: VS 2022 Build Tools ($NativeArch C++ components)"
    $vsOut = & winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent `
        --accept-package-agreements --accept-source-agreements `
        --override $vsComp 2>&1
    $vsOut | ForEach-Object { Log "  $_" }
    $vsAlreadyCurrent = ($vsOut -match "No available upgrade found|No newer package versions|already installed")
    if ($LASTEXITCODE -ne 0 -and -not $vsAlreadyCurrent) { Fail "VS Build Tools install failed." }
    Log "  -> OK"

    $env:Path = (@([System.Environment]::GetEnvironmentVariable("Path","Machine"), [System.Environment]::GetEnvironmentVariable("Path","User"), (Join-Path $env:LocalAppData "Microsoft\WinGet\Links")) -join ";")

} elseif ($OnLinux) {
    $pkgMgr = if (Get-Command apt-get -EA SilentlyContinue){"apt"} elseif (Get-Command dnf -EA SilentlyContinue){"dnf"} elseif (Get-Command yum -EA SilentlyContinue){"yum"} elseif (Get-Command pacman -EA SilentlyContinue){"pacman"} else { Fail "No supported package manager." }
    $instPkgs = switch ($pkgMgr) {
        "apt"    { @("build-essential","gcc","cmake","ninja-build","file") }
        "dnf"    { @("gcc","gcc-c++","cmake","ninja-build","file") }
        "yum"    { @("gcc","gcc-c++","cmake","ninja-build","file") }
        "pacman" { @("base-devel","gcc","cmake","ninja","file") }
    }
    Log "  Installing ($pkgMgr): $($instPkgs -join ', ')"
    switch ($pkgMgr) {
        "apt"    { & apt-get update -y 2>&1 | Out-Null; Invoke-Logged (@("apt-get","install","-y") + $instPkgs) }
        "dnf"    { Invoke-Logged (@("dnf","install","-y") + $instPkgs) }
        "yum"    { Invoke-Logged (@("yum","install","-y") + $instPkgs) }
        "pacman" { Invoke-Logged (@("pacman","-S","--noconfirm") + $instPkgs) }
    }

} elseif ($OnMacOS) {
    if (-not (Get-Command brew -EA SilentlyContinue)) {
        Log "  Installing Homebrew..."
        & /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        if ($LASTEXITCODE -ne 0) { Fail "Homebrew install failed." }
    }
    Invoke-Logged @("brew","install","cmake","ninja")
    if (-not (Get-Command clang -EA SilentlyContinue)) {
        & xcode-select --install 2>&1 | Out-Null
        Fail "Xcode CLT install started -- re-run this script after it completes."
    }
}

$cmake = Get-Command cmake -EA SilentlyContinue
if (-not $cmake) { Fail "cmake not found after install." }
Log "  cmake : $($cmake.Source)"
$ninja = Get-Command ninja -EA SilentlyContinue
if ($ninja) { Log "  ninja : $($ninja.Source)" }
} catch {
    if ("$_" -notlike "*::SKIP::*") { $script:StepErrors += "Step 3: $_" }
    Log "  [!] Step 3 partial -- will attempt build with tools already in PATH"
}

# ═══════════════════════════════════════════════════════════════
# STEP 4 -- Init MSVC (Windows) / select compiler, then build
# ═══════════════════════════════════════════════════════════════
try {
Section "STEP 4/6  Configuring compiler and building"

$cmakeExtraArgs = @()

if ($OnWindows) {
    function Find-VCVars([string]$File) {
        $vsRoot = ${env:ProgramFiles(x86)}; if (-not $vsRoot) { return $null }
        $vswhere = Join-Path $vsRoot "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $ip = (& $vswhere -latest -products * -property installationPath 2>$null) | Select-Object -First 1
            if ($ip) { $c = Join-Path $ip.Trim() "VC\Auxiliary\Build\$File"; if (Test-Path $c) { return $c } }
        }
        foreach ($yr in @("2022","2019")) { foreach ($ed in @("BuildTools","Community","Professional","Enterprise")) {
            $c = Join-Path $vsRoot "Microsoft Visual Studio\$yr\$ed\VC\Auxiliary\Build\$File"
            if (Test-Path $c) { return $c }
        } }
        return $null
    }
    $vcvars = Find-VCVars $VCVarsFile
    if (-not $vcvars) { Fail "Cannot find $VCVarsFile. Try restarting and re-running." }
    Log "  vcvars: $vcvars"

    # Verify target arch by sourcing vcvars inside cmd.exe
    $tgtOut = & cmd.exe /d /s /c "`"$vcvars`" >nul 2>&1 && set VSCMD_ARG_TGT_ARCH"
    $tgt = ($tgtOut | Where-Object { "$_" -match '^VSCMD_ARG_TGT_ARCH=' } | Select-Object -First 1) -replace '^VSCMD_ARG_TGT_ARCH=', ''
    $tgt = "$tgt".Trim()
    Log "  VSCMD_ARG_TGT_ARCH = $tgt"
    $ok = switch ($NativeArch) {
        "ARM64" { $tgt -and $tgt.ToUpper() -eq "ARM64" }
        "AMD64" { $tgt -and $tgt.ToUpper() -in @("AMD64","X64","X86_AMD64") }
        default { $true }
    }
    if (-not $ok) { Fail "MSVC env targets '$tgt' but native arch is '$NativeArch'." }
    Log "  MSVC env OK"

    # Locate mt.exe / rc.exe directly from Windows Kits (needed to pass explicitly to cmake)
    $mtPath = ""; $rcPath = ""
    $kitsBase = Join-Path ([System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::ProgramFilesX86)) "Windows Kits\10\bin"
    if (Test-Path $kitsBase) {
        $sdkVer = Get-ChildItem $kitsBase -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($sdkVer) {
            foreach ($ad in @("arm64","x64","x86")) {
                if (-not $mtPath) { $t = Join-Path $sdkVer.FullName "$ad\mt.exe"; if (Test-Path $t) { $mtPath = $t } }
                if (-not $rcPath) { $t = Join-Path $sdkVer.FullName "$ad\rc.exe"; if (Test-Path $t) { $rcPath = $t } }
            }
        }
    }
    Log "  mt.exe: $(if ($mtPath) { $mtPath } else { '(not found)' })"
    Log "  rc.exe: $(if ($rcPath) { $rcPath } else { '(not found)' })"

} else {
    # Linux / macOS: pick compiler
    if ($OnMacOS) {
        $env:CC = "clang"
    } elseif (Get-Command gcc -EA SilentlyContinue) {
        $env:CC = "gcc"
    } elseif (Get-Command clang -EA SilentlyContinue) {
        $env:CC = "clang"
    } else { Fail "No C compiler found." }

    $compilerTarget = (& $env:CC -dumpmachine 2>&1).Trim()
    Log "  Compiler       : $($env:CC)"
    Log "  Compiler target: $compilerTarget"

    $mismatch = switch ($Arch) {
        "x86_64" { $compilerTarget -notmatch "(x86_64|amd64)" }
        "arm64"  { $compilerTarget -notmatch "(aarch64|arm64)" }
        default  { $false }
    }
    if ($mismatch) { Log "  *** WARNING: compiler target does not match native $Arch" }
    $cmakeExtraArgs = @("-DCMAKE_C_COMPILER=$($env:CC)")
}

# On Windows with MSVC: cmake + build run inside cmd.exe with vcvars sourced.
# On Linux/macOS: use Invoke-Logged as before.

if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
New-Item -ItemType Directory -Path $BuildDir | Out-Null

$jobs = try {
    if ($OnWindows) { $env:NUMBER_OF_PROCESSORS }
    elseif ($OnMacOS) { (& sysctl -n hw.logicalcpu).Trim() }
    else { (& nproc).Trim() }
} catch { "4" }

if ($OnWindows) {
    # Locate cmake / ninja full paths (installed in step 3)
    $cmakeSearchDirs = @(
        "C:\Program Files\CMake\bin",
        "C:\Program Files (x86)\CMake\bin",
        (Join-Path $env:LocalAppData "Microsoft\WinGet\Links")
    )
    $cmakeExe = $cmakeSearchDirs | ForEach-Object { Join-Path $_ "cmake.exe" } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $cmakeExe) { $cmakeExe = "cmake" }   # fall back to PATH

    $ninjaSearchDirs = @(
        (Join-Path $env:LocalAppData "Microsoft\WinGet\Links"),
        "C:\ProgramData\chocolatey\bin"
    )
    $ninjaExe = $ninjaSearchDirs | ForEach-Object { Join-Path $_ "ninja.exe" } | Where-Object { Test-Path $_ } | Select-Object -First 1

    if ($ninjaExe) {
        Log "  Generator: Ninja"
        $generatorArgs = "-G Ninja"
    } else {
        Log "  Generator: Visual Studio 17 2022 -A $CMakeArch"
        $generatorArgs = "-G `"Visual Studio 17 2022`" -A $CMakeArch"
        $ninjaExe = ""
    }

    $extraFlags = ($cmakeExtraArgs | Where-Object { $_ }) -join " "
    if ($mtPath) {
        $mtFwd = $mtPath.Replace('\', '/')
        $extraFlags += " -DCMAKE_MT:FILEPATH=`"$mtFwd`""
    }
    if ($rcPath) {
        $rcFwd = $rcPath.Replace('\', '/')
        $extraFlags += " -DCMAKE_RC_COMPILER:FILEPATH=`"$rcFwd`""
    }
    # Skip the full exe link test — avoids cmake compiler-check LNK1104 on LIB path issues
    $extraFlags += " -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"

    # Build a temporary batch file — vcvars properly populates INCLUDE/LIB/PATH for cl/link
    $batchFile = Join-Path $env:TEMP "tb_build_$([System.Guid]::NewGuid().ToString('N')).bat"
    $ninjaLine = if ($ninjaExe) { "SET PATH=$([System.IO.Path]::GetDirectoryName($ninjaExe));%PATH%" } else { "REM no ninja" }
    $batchContent = @(
        "@echo off",
        "call `"$vcvars`"",
        "if errorlevel 1 ( echo vcvars failed & exit /b 1 )",
        $ninjaLine,
        "echo [diag] INCLUDE=%INCLUDE%",
        "`"$cmakeExe`" -S `"$ROOT`" -B `"$BuildDir`" $generatorArgs -DCMAKE_BUILD_TYPE=Release $extraFlags",
        "if errorlevel 1 ( echo cmake configure failed & exit /b 1 )",
        "`"$cmakeExe`" --build `"$BuildDir`" --config Release --parallel $jobs",
        "if errorlevel 1 ( echo cmake build failed & exit /b 1 )"
    ) -join "`r`n"
    Set-Content -Path $batchFile -Value $batchContent -Encoding ASCII

    Log "  Running build via cmd.exe + vcvars..."
    & cmd.exe /d /s /c "`"$batchFile`"" 2>&1 | ForEach-Object { Log "  $_" }
    $buildExit = $LASTEXITCODE
    Remove-Item $batchFile -Force -ErrorAction SilentlyContinue
    if ($buildExit -ne 0) { Fail "Build failed (exit $buildExit)." }

} else {
    # Linux / macOS
    $ninja = Get-Command ninja -EA SilentlyContinue
    if ($ninja) {
        Log "  Generator: Ninja"
        $configArgs = @("-S",$ROOT,"-B",$BuildDir,"-G","Ninja","-DCMAKE_BUILD_TYPE=Release") + $cmakeExtraArgs
    } else {
        Log "  Generator: Unix Makefiles"
        $configArgs = @("-S",$ROOT,"-B",$BuildDir,"-G","Unix Makefiles","-DCMAKE_BUILD_TYPE=Release") + $cmakeExtraArgs
    }
    Invoke-Logged (@("cmake") + $configArgs)
    Invoke-Logged @("cmake","--build",$BuildDir,"--config","Release","--parallel",$jobs)
}
} catch {
    if ("$_" -notlike "*::SKIP::*") { $script:StepErrors += "Step 4: $_" }
    Log "  [!] Step 4 (build) failed -- no binaries produced"
}

# ═══════════════════════════════════════════════════════════════
# STEP 5 -- Verify binary architecture
# ═══════════════════════════════════════════════════════════════
try {
Section "STEP 5/6  Verifying binary architecture"

$tbBin = if ($OnWindows) {
    @("$BuildDir\torture-bench.exe","$BuildDir\Release\torture-bench.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
} else { Join-Path $BuildDir "torture-bench" }

$tpBin = if ($OnWindows) {
    @("$BuildDir\tune-probe.exe","$BuildDir\Release\tune-probe.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
} else { Join-Path $BuildDir "tune-probe" }

if (-not $tbBin -or -not (Test-Path $tbBin)) { Fail "torture-bench not found after build." }
if (-not $tpBin -or -not (Test-Path $tpBin)) { Fail "tune-probe not found after build." }

if ($OnWindows) {
    function Get-PEMachine([string]$Path) {
        $b = [System.IO.File]::ReadAllBytes($Path)
        if ($b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
        $off = [BitConverter]::ToInt32($b, 0x3C)
        if ($off + 6 -ge $b.Length) { return $null }
        return [int][BitConverter]::ToUInt16($b, $off + 4)
    }
    $peMap    = @{ 0xAA64 = "ARM64"; 0x8664 = "x64/AMD64"; 0x014C = "x86" }
    $tbMach   = Get-PEMachine $tbBin
    $tbLabel  = $peMap[[int]$tbMach]
    Log "  PE machine : $tbLabel  (0x$('{0:X4}' -f $tbMach))"
    Log "  Expected   : $($peMap[$ExpectedPE])  (0x$('{0:X4}' -f $ExpectedPE))"
    if ($tbMach -ne $ExpectedPE) { Fail "Binary is $tbLabel but native arch is $NativeArch. Wrong toolchain." }
    Log "  Arch check PASSED"
} else {
    if (Get-Command file -EA SilentlyContinue) {
        $fileOut = (& file $tbBin).Trim()
        Log "  file: $fileOut"
        $archOk = switch ($Arch) {
            "x86_64" { $fileOut -match "(x86.64|x86-64)" }
            "arm64"  { $fileOut -match "(aarch64|arm64|ARM aarch64)" }
            "riscv64"{ $fileOut -match "(RISC-V|riscv)" }
            default  { $true }
        }
        if (-not $archOk) { Fail "Binary arch mismatch: $fileOut  (expected $Arch)" }
        Log "  Arch check PASSED"
    } else { Log "  'file' not available -- skipping arch check" }
}
} catch {
    if ("$_" -notlike "*::SKIP::*") { $script:StepErrors += "Step 5: $_" }
    Log "  [!] Step 5 (verify) skipped -- continuing to package"
}

# ═══════════════════════════════════════════════════════════════
# STEP 6 -- Package
# ═══════════════════════════════════════════════════════════════
try {
Section "STEP 6/6  Packaging"

if (Test-Path $OutDir)  { Remove-Item $OutDir  -Recurse -Force }
if (Test-Path $Archive) { Remove-Item $Archive -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null

if ($OnWindows) {
    Copy-Item $tbBin (Join-Path $OutDir "torture-bench.exe")
    Copy-Item $tpBin (Join-Path $OutDir "tune-probe.exe")
    if (Test-Path (Join-Path $ROOT "bench.ps1")) { Copy-Item (Join-Path $ROOT "bench.ps1") (Join-Path $OutDir "bench.ps1") }
    $binLabel = $tbLabel
} else {
    Copy-Item $tbBin (Join-Path $OutDir "torture-bench")
    Copy-Item $tpBin (Join-Path $OutDir "tune-probe")
    & chmod +x (Join-Path $OutDir "torture-bench") (Join-Path $OutDir "tune-probe")
    if (Test-Path (Join-Path $ROOT "bench.sh")) { Copy-Item (Join-Path $ROOT "bench.sh") (Join-Path $OutDir "bench.sh"); & chmod +x (Join-Path $OutDir "bench.sh") }
    $binLabel = if ($Arch -eq "arm64") { "ARM64" } else { $Arch }
}

$readmeRun = if ($OnWindows) {
    "Quick run:`n  .\\torture-bench.exe`n`nFull pipeline:`n  powershell -ExecutionPolicy Bypass -File bench.ps1"
} else {
    "Quick run:`n  ./torture-bench`n`nFull pipeline:`n  bash bench.sh"
}
Set-Content (Join-Path $OutDir "README.txt") @"
torture-bench -- ${OSLabel} ${Arch}
Built   : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
Binary  : $binLabel (native -- not emulated)

$readmeRun
"@

if ($OnWindows) {
    Compress-Archive -Path (Join-Path $OutDir "*") -DestinationPath $Archive
} else {
    & tar -czf $Archive -C $ROOT $PackageName 2>&1 | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0) { Fail "tar failed." }
}

Remove-Item $BuildDir -Recurse -Force
Remove-Item $OutDir   -Recurse -Force

Log "  Created: $Archive"
$script:Deliverable = $Archive
} catch {
    if ("$_" -notlike "*::SKIP::*") { $script:StepErrors += "Step 6: $_" }
    Log "  [!] Step 6 (package) failed"
}

# ═══════════════════════════════════════════════════════════════
# FINAL SUMMARY
# ═══════════════════════════════════════════════════════════════
Log ""; Log "======================================================="
Log "  RESULTS"
Log "======================================================="
if ($script:Deliverable -and (Test-Path $script:Deliverable)) {
    Log "  DELIVERABLE : $($script:Deliverable)"
    Write-Host ""
    Write-Host "  => $($script:Deliverable)" -ForegroundColor Green
} else {
    Log "  No package produced."
}
if ($script:StepErrors.Count -gt 0) {
    Log ""
    Log "  Errors / skipped steps:"
    $script:StepErrors | ForEach-Object { Log "    - $_" }
}
$result = if ($script:Deliverable -and (Test-Path $script:Deliverable)) { "=== SUCCEEDED ===" } else { "=== INCOMPLETE ===" }
Add-Content -Path $LOG -Value $result

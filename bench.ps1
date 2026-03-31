# ----------------------------------------------------------------------------
# torture-bench one-liner for Windows PowerShell
#
# Usage:
#   irm https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.ps1 | iex
#
# With Geekbench scores:
#   $env:GB_SINGLE=2800; $env:GB_MULTI=18000; irm https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.ps1 | iex
#
# Flags (all optional, via env vars):
#   $env:GB_SINGLE   Geekbench 6 single-core score
#   $env:GB_MULTI    Geekbench 6 multi-core score
# ----------------------------------------------------------------------------

$ErrorActionPreference = "Stop"

$REPO      = "https://github.com/sunprojectca/torture-bench.git"
$DIR       = "$HOME\torture-bench"
$DURATION  = "30"
$GBSingle  = if ($env:GB_SINGLE) { $env:GB_SINGLE } else { "0" }
$GBMulti   = if ($env:GB_MULTI)  { $env:GB_MULTI }  else { "0" }

function Step($n, $msg) { Write-Host "[$n/5] $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }
function Warn($msg) { Write-Host "WARN: $msg" -ForegroundColor Yellow }

if ($env:OS -ne "Windows_NT") {
    Fail "bench.ps1 must run on native Windows PowerShell. For Linux/WSL use: curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | bash"
}

function Get-NativeWindowsArch {
    try {
        $arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToUpperInvariant()
        switch ($arch) {
            "X64"  { return "AMD64" }
            "X86"  { return "X86" }
            "ARM64" { return "ARM64" }
            default { return $arch }
        }
    } catch { }

    if ($env:PROCESSOR_ARCHITEW6432) { return $env:PROCESSOR_ARCHITEW6432.ToUpper() }
    if ($env:PROCESSOR_ARCHITECTURE) { return $env:PROCESSOR_ARCHITECTURE.ToUpper() }
    return "UNKNOWN"
}

function ConvertTo-CMakeArch([string]$Arch) {
    switch ($Arch) {
        "AMD64" { return "x64" }
        "ARM64" { return "ARM64" }
        "X86"   { return "Win32" }
        default { return "x64" }
    }
}

function ConvertTo-ResultArch([string]$Arch) {
    switch ($Arch) {
        "AMD64" { return "amd64" }
        "ARM64" { return "arm64" }
        "X86"   { return "x86" }
        default { return $Arch.ToLowerInvariant() }
    }
}

$ProcessArchRaw = if ($env:PROCESSOR_ARCHITECTURE) { $env:PROCESSOR_ARCHITECTURE.ToUpper() } else { "UNKNOWN" }
$ProcArchRaw = Get-NativeWindowsArch
$CMakeArch = ConvertTo-CMakeArch $ProcArchRaw
$ResultArch = ConvertTo-ResultArch $ProcArchRaw
$VSRequiredComponents = if ($ProcArchRaw -eq "ARM64") {
    @(
        "Microsoft.VisualStudio.Component.VC.Tools.ARM64",
        "Microsoft.VisualStudio.Workload.VCTools"
    )
} else {
    @(
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "Microsoft.VisualStudio.Workload.VCTools"
    )
}
if ($ProcArchRaw -eq "UNKNOWN") {
    Warn "PROCESSOR_ARCHITECTURE is not set; defaulting CMake architecture to x64."
}

function Add-SessionPathEntry {
    param([string]$PathEntry)

    if ([string]::IsNullOrWhiteSpace($PathEntry) -or -not (Test-Path $PathEntry)) { return }

    $current = @($env:Path -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($current -contains $PathEntry) { return }

    $env:Path = "$PathEntry;$env:Path"
}

function Add-CommonToolPaths {
    $candidates = @()

    if ($env:LocalAppData) {
        $candidates += Join-Path $env:LocalAppData "Microsoft\WinGet\Links"
    }
    if ($env:ProgramFiles) {
        $candidates += Join-Path $env:ProgramFiles "LLVM\bin"
        $candidates += Join-Path $env:ProgramFiles "Git\cmd"
        $candidates += Join-Path $env:ProgramFiles "CMake\bin"
    }

    $pf86 = ${env:ProgramFiles(x86)}
    if ($pf86) {
        $candidates += Join-Path $pf86 "LLVM\bin"
        $candidates += Join-Path $pf86 "Git\cmd"
        $candidates += Join-Path $pf86 "CMake\bin"
    }
    if ($env:ProgramData) {
        $candidates += Join-Path $env:ProgramData "chocolatey\bin"
    }
    if ($env:ChocolateyInstall) {
        $candidates += Join-Path $env:ChocolateyInstall "bin"
    }

    foreach ($candidate in $candidates) {
        Add-SessionPathEntry $candidate
    }
}

function Get-CompilerFromPath {
    Add-CommonToolPaths

    if (Get-Command cl -ErrorAction SilentlyContinue) { return "MSVC" }

    $hasNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
    $hasMinGWMake = [bool](Get-Command mingw32-make -ErrorAction SilentlyContinue)

    if (Get-Command gcc -ErrorAction SilentlyContinue) {
        if ($hasNinja -or $hasMinGWMake) { return "gcc" }
    }
    if (Get-Command clang -ErrorAction SilentlyContinue) {
        if ($hasNinja) { return "clang" }
    }
    if (Get-Command gcc -ErrorAction SilentlyContinue) { return "gcc" }
    if (Get-Command clang -ErrorAction SilentlyContinue) { return "clang" }
    return $null
}

function Get-VSInstallPath {
    param([string[]]$RequiredComponents = $VSRequiredComponents)

    $vsRoot = ${env:ProgramFiles(x86)}
    if ($vsRoot) {
        $vswhere = Join-Path $vsRoot "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            foreach ($component in $RequiredComponents) {
                $args = @(
                    "-latest",
                    "-products", "*",
                    "-requires", $component,
                    "-property", "installationPath"
                )
                $path = & $vswhere @args
                if ($LASTEXITCODE -eq 0 -and $path) {
                    return ($path | Select-Object -First 1).Trim()
                }
            }

            $fallbackArgs = @(
                "-latest",
                "-products", "*",
                "-property", "installationPath"
            )
            $path = & $vswhere @fallbackArgs
            if ($LASTEXITCODE -eq 0 -and $path) {
                $candidate = ($path | Select-Object -First 1).Trim()
                if (Test-Path (Join-Path $candidate "VC\Auxiliary\Build")) {
                    return $candidate
                }
            }
        }
    }

    if (-not $vsRoot) { return $null }

    $fallbacks = @(
        "Microsoft Visual Studio\2022\BuildTools",
        "Microsoft Visual Studio\2022\Community",
        "Microsoft Visual Studio\2022\Professional",
        "Microsoft Visual Studio\2022\Enterprise",
        "Microsoft Visual Studio\2019\BuildTools",
        "Microsoft Visual Studio\2019\Community",
        "Microsoft Visual Studio\2019\Professional",
        "Microsoft Visual Studio\2019\Enterprise"
    )

    foreach ($relativePath in $fallbacks) {
        $candidate = Join-Path $vsRoot $relativePath
        if (Test-Path (Join-Path $candidate "VC\Auxiliary\Build")) {
            return $candidate
        }
    }

    return $null
}

function Get-VSAnyInstallPath {
    $vsRoot = ${env:ProgramFiles(x86)}
    if ($vsRoot) {
        $vswhere = Join-Path $vsRoot "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $args = @(
                "-latest",
                "-products", "*",
                "-property", "installationPath"
            )
            $path = & $vswhere @args
            if ($LASTEXITCODE -eq 0 -and $path) {
                $candidate = ($path | Select-Object -First 1).Trim()
                if (Test-Path $candidate) {
                    return $candidate
                }
            }
        }
    }

    if (-not $vsRoot) { return $null }

    $fallbacks = @(
        "Microsoft Visual Studio\2022\BuildTools",
        "Microsoft Visual Studio\2022\Community",
        "Microsoft Visual Studio\2022\Professional",
        "Microsoft Visual Studio\2022\Enterprise",
        "Microsoft Visual Studio\2019\BuildTools",
        "Microsoft Visual Studio\2019\Community",
        "Microsoft Visual Studio\2019\Professional",
        "Microsoft Visual Studio\2019\Enterprise"
    )

    foreach ($relativePath in $fallbacks) {
        $candidate = Join-Path $vsRoot $relativePath
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    return $null
}

function Test-VSCompilerTools {
    param(
        [Parameter(Mandatory=$true)][string]$InstallPath
    )

    if ($ProcArchRaw -eq "ARM64") {
        return (Test-Path (Join-Path $InstallPath "VC\Auxiliary\Build\vcvarsarm64.bat"))
    }

    return (Test-Path (Join-Path $InstallPath "VC\Auxiliary\Build\vcvars64.bat"))
}

function Install-VSCompilerTools {
    param(
        [Parameter(Mandatory=$true)][string]$InstallPath
    )

    $vsRoot = ${env:ProgramFiles(x86)}
    if (-not $vsRoot) { return $false }

    $setupExe = Join-Path $vsRoot "Microsoft Visual Studio\Installer\setup.exe"
    if (-not (Test-Path $setupExe)) { return $false }

    $args = @(
        "modify",
        "--installPath", $InstallPath,
        "--add", "Microsoft.VisualStudio.Workload.VCTools",
        "--includeRecommended",
        "--passive",
        "--norestart"
    )

    if ($ProcArchRaw -eq "ARM64") {
        $args += @("--add", "Microsoft.VisualStudio.Component.VC.Tools.ARM64")
    } else {
        $args += @("--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
    }

    Write-Host "  Updating existing Visual Studio install with C++ build tools..." -ForegroundColor Yellow
    $proc = Start-Process -FilePath $setupExe -ArgumentList $args -Wait -PassThru
    if ($proc.ExitCode -ne 0) { return $false }
    return (Test-VSCompilerTools -InstallPath $InstallPath)
}

function Get-VSGeneratorName([string]$InstallPath) {
    if ($InstallPath -match '[\\/]2022[\\/]') { return "Visual Studio 17 2022" }
    if ($InstallPath -match '[\\/]2019[\\/]') { return "Visual Studio 16 2019" }
    return "Visual Studio 17 2022"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][scriptblock]$Action
    )
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

function Try-WingetInstall {
    param(
        [Parameter(Mandatory=$true)][string]$Id,
        [Parameter(Mandatory=$true)][string]$Label,
        [string[]]$ExtraArgs = @()
    )
    $args = @(
        "install", "--id", $Id, "--exact", "--silent",
        "--accept-package-agreements", "--accept-source-agreements"
    ) + $ExtraArgs
    Write-Host "  -> winget: $Label ($Id)"
    & winget @args
    if ($LASTEXITCODE -eq 0) { return $true }
    Warn "winget install failed for $Label (exit $LASTEXITCODE)"
    return $false
}

function Try-ChocoInstall {
    param(
        [Parameter(Mandatory=$true)][string[]]$Packages,
        [string[]]$ExtraArgs = @()
    )
    $joined = ($Packages -join ", ")
    Write-Host "  -> choco: $joined"
    & choco install @Packages @ExtraArgs -y
    if ($LASTEXITCODE -eq 0) { return $true }
    Warn "choco install failed for $joined (exit $LASTEXITCODE)"
    return $false
}

function Invoke-GitQuiet {
    param(
        [Parameter(Mandatory=$true)][string[]]$Arguments,
        [Parameter(Mandatory=$true)][string]$WorkingDirectory
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "git"
    $psi.Arguments = [string]::Join(" ", $Arguments)
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()

    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    return [pscustomobject]@{
        ExitCode = $proc.ExitCode
        StdOut   = $stdout
        StdErr   = $stderr
    }
}

function New-BuildPlan {
    param(
        [Parameter(Mandatory=$true)][string]$GeneratorName,
        [Parameter(Mandatory=$true)][string[]]$GeneratorArgs,
        [string[]]$CompilerArgs = @(),
        [string[]]$BuildArgs = @()
    )

    return [pscustomobject]@{
        GeneratorName = $GeneratorName
        GeneratorArgs = $GeneratorArgs
        CompilerArgs   = $CompilerArgs
        BuildArgs      = $BuildArgs
    }
}

function Import-BatchEnvironment {
    param(
        [Parameter(Mandatory=$true)][string]$BatchPath,
        [string[]]$Arguments = @()
    )

    if (-not (Test-Path $BatchPath)) { return $false }

    $argString = if ($Arguments.Count -gt 0) { " " + ($Arguments -join " ") } else { "" }
    $output = & cmd.exe /d /s /c "`"$BatchPath`"$argString >nul && set"
    if ($LASTEXITCODE -ne 0) { return $false }

    foreach ($line in $output) {
        if ($line -match '^(.*?)=(.*)$') {
            Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
        }
    }

    return $true
}

function Test-MSVCEnvironmentMatchesTarget {
    switch ($ProcArchRaw) {
        "ARM64" {
            if ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH.ToLowerInvariant() -eq "arm64") { return $true }
            if ($env:Platform -and $env:Platform.ToUpperInvariant() -eq "ARM64") { return $true }
            return $false
        }
        "X86" {
            if ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH.ToLowerInvariant() -eq "x86") { return $true }
            if ($env:Platform -and $env:Platform.ToUpperInvariant() -eq "WIN32") { return $true }
            return $false
        }
        default {
            return $true
        }
    }
}

function Initialize-MSVCEnvironment {
    param(
        [Parameter(Mandatory=$false)][string]$InstallPath = $null
    )

    if (-not $InstallPath) { $InstallPath = Get-VSAnyInstallPath }
    if (-not $InstallPath) { return $false }

    $vcVarsFile = switch ($ProcArchRaw) {
        "ARM64" { "vcvarsarm64.bat" }
        "X86"   { "vcvars32.bat" }
        default { "vcvars64.bat" }
    }
    $vcvars = Join-Path $InstallPath "VC\Auxiliary\Build\$vcVarsFile"
    if (Test-Path $vcvars) {
        return (Import-BatchEnvironment -BatchPath $vcvars)
    }

    $vsDevCmd = Join-Path $InstallPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) { return $false }

    $arch = switch ($ProcArchRaw) {
        "ARM64" { "arm64" }
        "X86"   { "x86" }
        default { "x64" }
    }

    return (Import-BatchEnvironment -BatchPath $vsDevCmd -Arguments @("-arch=$arch"))
}

function Remove-DirectorySafe {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Root
    )

    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $separator = [System.IO.Path]::DirectorySeparatorChar.ToString()
    if (-not $resolvedRoot.EndsWith($separator)) {
        $resolvedRoot += $separator
    }

    if (-not $resolvedPath.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "Refusing to remove directory outside repo root: $resolvedPath"
    }

    if (Test-Path $resolvedPath) {
        Remove-Item -LiteralPath $resolvedPath -Recurse -Force
    }
}

function Get-MSVCBuildPlan {
    param(
        [Parameter(Mandatory=$false)][string]$InstallPath = $null
    )

    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninja) {
        return New-BuildPlan -GeneratorName "Ninja" -GeneratorArgs @("-G", "Ninja")
    }

    $nmake = Get-Command nmake -ErrorAction SilentlyContinue
    if ($nmake) {
        return New-BuildPlan -GeneratorName "NMake Makefiles" -GeneratorArgs @("-G", "NMake Makefiles")
    }

    if (-not $InstallPath) { $InstallPath = Get-VSAnyInstallPath }
    if (-not $InstallPath) { return $null }

    $generatorName = Get-VSGeneratorName $InstallPath
    return New-BuildPlan -GeneratorName $generatorName -GeneratorArgs @("-G", $generatorName, "-A", $CMakeArch) -BuildArgs @("--config", "Release")
}

Write-Host ""
Write-Host "  +-------------------------------------------------------+" -ForegroundColor Cyan
Write-Host "  |   torture-bench - one-command benchmark pipeline       |" -ForegroundColor White
Write-Host "  +-------------------------------------------------------+" -ForegroundColor Cyan
Write-Host ""
$PlatformLabel = "Windows $ProcArchRaw"
if ($ProcessArchRaw -ne "UNKNOWN" -and $ProcessArchRaw -ne $ProcArchRaw) {
    $PlatformLabel += " (shell $ProcessArchRaw)"
}
Write-Host "  Platform: $PlatformLabel  |  Duration: ${DURATION}s/module"
if ([int]$GBSingle -gt 0) { Write-Host "  Geekbench: single=$GBSingle multi=$GBMulti" }
Write-Host ""

# ==============================================================================
# STEP 1: Install & check dependencies
# ==============================================================================
Step 1 "Checking & installing dependencies..."

function Refresh-SessionPath {
    $parts = @(
        [System.Environment]::GetEnvironmentVariable("Path", "Machine"),
        [System.Environment]::GetEnvironmentVariable("Path", "User")
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    $env:Path = ($parts -join ";")
    Add-CommonToolPaths
}

function Get-MissingDependencies {
    $missing = @()
    if (-not (Get-Command git -ErrorAction SilentlyContinue))   { $missing += "git" }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $missing += "cmake" }
    $compilerInPath = Get-CompilerFromPath
    $vsInstallPath = Get-VSInstallPath
    $hasNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
    $hasNMake = [bool](Get-Command nmake -ErrorAction SilentlyContinue)
    $hasMinGWMake = [bool](Get-Command mingw32-make -ErrorAction SilentlyContinue)

    $needsNinja = $false
    if (-not $compilerInPath) {
        if (-not $vsInstallPath -and $ProcArchRaw -ne "ARM64") {
            $needsNinja = (-not $hasNinja)
        }
    } else {
        switch ($compilerInPath) {
            "MSVC" { $needsNinja = (-not $hasNinja -and -not $hasNMake -and -not $vsInstallPath) }
            "clang" { $needsNinja = (-not $hasNinja -and -not $vsInstallPath) }
            "gcc" { $needsNinja = (-not $hasNinja -and -not $hasMinGWMake -and -not $vsInstallPath) }
            default {
                if (-not $vsInstallPath) {
                    $needsNinja = (-not $hasNinja -and -not $hasNMake -and -not $hasMinGWMake)
                }
            }
        }
    }
    if ($needsNinja) { $missing += "ninja" }
    if (-not $compilerInPath -and -not $vsInstallPath) { $missing += "compiler" }
    return $missing
}

Refresh-SessionPath
$missing = Get-MissingDependencies

if ($missing.Count -gt 0) {
    Write-Host "  Missing: $($missing -join ', ') - installing..." -ForegroundColor Yellow

    $hasWinget = Get-Command winget -ErrorAction SilentlyContinue
    $hasChoco  = Get-Command choco  -ErrorAction SilentlyContinue
    $installFailures = @()

    if ($hasWinget) {
        if ("git"      -in $missing) { if (-not (Try-WingetInstall -Id "Git.Git" -Label "git")) { $installFailures += "git" } }
        if ("cmake"    -in $missing) { if (-not (Try-WingetInstall -Id "Kitware.CMake" -Label "cmake")) { $installFailures += "cmake" } }
        if ("ninja"    -in $missing) { if (-not (Try-WingetInstall -Id "Ninja-build.Ninja" -Label "ninja")) { $installFailures += "ninja" } }
        if ("compiler" -in $missing) {
            $compilerOk = $false
            $existingVS = Get-VSAnyInstallPath
            if ($existingVS) {
                if (Install-VSCompilerTools -InstallPath $existingVS) {
                    Refresh-SessionPath
                    if ((Get-CompilerFromPath) -or (Get-VSInstallPath)) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk -and $ProcArchRaw -eq "ARM64") {
                $vsOverride = "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.ARM64 --includeRecommended"
                if (Try-WingetInstall -Id "Microsoft.VisualStudio.2022.BuildTools" -Label "Visual Studio Build Tools (C++)" -ExtraArgs @("--override", $vsOverride)) {
                    Refresh-SessionPath
                    if ((Get-CompilerFromPath) -or (Get-VSInstallPath)) { $compilerOk = $true }
                }
                if (-not $compilerOk -and (Try-WingetInstall -Id "LLVM.LLVM" -Label "LLVM (clang)")) {
                    Refresh-SessionPath
                    if (Get-CompilerFromPath) { $compilerOk = $true }
                }
            } else {
                if (Try-WingetInstall -Id "LLVM.LLVM" -Label "LLVM (clang)") {
                    Refresh-SessionPath
                    if (Get-CompilerFromPath) { $compilerOk = $true }
                }
                if (-not $compilerOk) {
                    $vsOverride = "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
                    if (Try-WingetInstall -Id "Microsoft.VisualStudio.2022.BuildTools" -Label "Visual Studio Build Tools (C++)" -ExtraArgs @("--override", $vsOverride)) {
                        Refresh-SessionPath
                        if ((Get-CompilerFromPath) -or (Get-VSInstallPath)) { $compilerOk = $true }
                    }
                }
            }
            if (-not $compilerOk) { $installFailures += "compiler" }
        }
        Refresh-SessionPath
    } elseif ($hasChoco) {
        if ("git"      -in $missing) { if (-not (Try-ChocoInstall -Packages @("git"))) { $installFailures += "git" } }
        if ("cmake"    -in $missing) { if (-not (Try-ChocoInstall -Packages @("cmake") -ExtraArgs @("--installargs", "ADD_CMAKE_TO_PATH=System"))) { $installFailures += "cmake" } }
        if ("ninja"    -in $missing) { if (-not (Try-ChocoInstall -Packages @("ninja"))) { $installFailures += "ninja" } }
        if ("compiler" -in $missing) {
            $compilerOk = $false
            $existingVS = Get-VSAnyInstallPath
            if ($existingVS) {
                if (Install-VSCompilerTools -InstallPath $existingVS) {
                    Refresh-SessionPath
                    if ((Get-CompilerFromPath) -or (Get-VSInstallPath)) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk) {
                if (Try-ChocoInstall -Packages @("llvm")) {
                    Refresh-SessionPath
                    if (Get-CompilerFromPath) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk -and $ProcArchRaw -ne "ARM64") {
                if (Try-ChocoInstall -Packages @("mingw")) {
                    Refresh-SessionPath
                    if (Get-CompilerFromPath) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk -and (Get-VSInstallPath)) { $compilerOk = $true }
            if (-not $compilerOk) { $installFailures += "compiler" }
        }
        Refresh-SessionPath
    } else {
        Write-Host "  No package manager found - installing Chocolatey..." -ForegroundColor Yellow
        [System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12
        Set-ExecutionPolicy Bypass -Scope Process -Force
        Invoke-Checked -Name "Chocolatey bootstrap" -Action { Invoke-Expression ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1')) }
        Refresh-SessionPath

        if ("git"      -in $missing) { if (-not (Try-ChocoInstall -Packages @("git"))) { $installFailures += "git" } }
        if ("cmake"    -in $missing) { if (-not (Try-ChocoInstall -Packages @("cmake") -ExtraArgs @("--installargs", "ADD_CMAKE_TO_PATH=System"))) { $installFailures += "cmake" } }
        if ("ninja"    -in $missing) { if (-not (Try-ChocoInstall -Packages @("ninja"))) { $installFailures += "ninja" } }
        if ("compiler" -in $missing) {
            $compilerOk = $false
            $existingVS = Get-VSAnyInstallPath
            if ($existingVS) {
                if (Install-VSCompilerTools -InstallPath $existingVS) {
                    Refresh-SessionPath
                    if ((Get-CompilerFromPath) -or (Get-VSInstallPath)) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk) {
                if (Try-ChocoInstall -Packages @("llvm")) {
                    Refresh-SessionPath
                    if (Get-CompilerFromPath) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk -and $ProcArchRaw -ne "ARM64") {
                if (Try-ChocoInstall -Packages @("mingw")) {
                    Refresh-SessionPath
                    if (Get-CompilerFromPath) { $compilerOk = $true }
                }
            }
            if (-not $compilerOk -and (Get-VSInstallPath)) { $compilerOk = $true }
            if (-not $compilerOk) { $installFailures += "compiler" }
        }
        Refresh-SessionPath
    }
    if ($installFailures.Count -eq 0) {
        Write-Host "  [ok] Package install step completed" -ForegroundColor Green
    } else {
        Warn "Some installs failed: $($installFailures -join ', ')"
    }
}

# Verify
if (-not (Get-Command git -ErrorAction SilentlyContinue))   { Fail "git still not found after install" }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "cmake still not found after install" }

Refresh-SessionPath
$Compiler = Get-CompilerFromPath
$VSInstallPath = Get-VSInstallPath
if (-not $Compiler -and -not $VSInstallPath) {
    $existingVS = Get-VSAnyInstallPath
    if ($existingVS) {
        if (Install-VSCompilerTools -InstallPath $existingVS) {
            Refresh-SessionPath
            $Compiler = Get-CompilerFromPath
            $VSInstallPath = Get-VSInstallPath
        }
    }
    if (-not $Compiler -and -not $VSInstallPath) {
        if ($ProcArchRaw -eq "ARM64") {
            Write-Host "  Visual Studio Build Tools is already installed. Add 'MSVC v143 - VS 2022 C++ ARM64 build tools' in Visual Studio Installer." -ForegroundColor Cyan
            Fail "No C/C++ toolchain found. Install or modify Visual Studio Build Tools, then rerun."
        }
        Fail "No C compiler/toolchain found after install. You may need to rerun in an elevated PowerShell and then restart the terminal."
    }
}

$Ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $Ninja -and -not $VSInstallPath) { Fail "ninja not found, and Visual Studio toolchain is not available for generator fallback." }

if ($Compiler) {
    Write-Host "  Compiler in PATH: $Compiler"
} else {
    Write-Host "  Compiler in PATH: (not detected)"
}
if ($VSInstallPath) {
    Write-Host "  Visual Studio toolchain: detected"
}

# ==============================================================================
# STEP 2: Clone or update
# ==============================================================================
Step 2 "Getting source..."

if (Test-Path "$DIR\.git") {
    Set-Location $DIR
    $status = Invoke-GitQuiet -Arguments @("status", "--porcelain") -WorkingDirectory $DIR

    if ($status.ExitCode -ne 0) {
        $freshDir = "$DIR-fresh-$(Get-Date -Format yyyyMMdd_HHmmss)"
        Write-Host "  Repo status check failed - cloning fresh to $freshDir" -ForegroundColor Yellow
        git clone $REPO $freshDir
        if ($LASTEXITCODE -ne 0) { Fail "git clone failed" }
        $DIR = $freshDir
        Set-Location $DIR
    } elseif (-not [string]::IsNullOrWhiteSpace($status.StdOut)) {
        $freshDir = "$DIR-fresh-$(Get-Date -Format yyyyMMdd_HHmmss)"
        Write-Host "  Repo has local changes - cloning fresh to $freshDir" -ForegroundColor Yellow
        git clone $REPO $freshDir
        if ($LASTEXITCODE -ne 0) { Fail "git clone failed" }
        $DIR = $freshDir
        Set-Location $DIR
    } else {
        Write-Host "  Repo exists - pulling latest..."
        $pull = Invoke-GitQuiet -Arguments @("pull", "--rebase") -WorkingDirectory $DIR
        if ($pull.ExitCode -ne 0) {
            $pull = Invoke-GitQuiet -Arguments @("pull") -WorkingDirectory $DIR
            if ($pull.ExitCode -ne 0) {
                $freshDir = "$DIR-fresh-$(Get-Date -Format yyyyMMdd_HHmmss)"
                Write-Host "  Pull failed - cloning fresh to $freshDir" -ForegroundColor Yellow
                git clone $REPO $freshDir
                if ($LASTEXITCODE -ne 0) { Fail "git clone failed" }
                $DIR = $freshDir
                Set-Location $DIR
            }
        }
    }
} else {
    Write-Host "  Cloning $REPO -> $DIR"
    git clone $REPO $DIR
    if ($LASTEXITCODE -ne 0) { Fail "git clone failed" }
    Set-Location $DIR
}

# ==============================================================================
# STEP 3: Build
# ==============================================================================
Step 3 "Building..."

$BuildDir = Join-Path $DIR "build"
Remove-DirectorySafe -Path $BuildDir -Root $DIR
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Location $DIR

$Compiler = Get-CompilerFromPath
$VSInstallPath = Get-VSInstallPath
$VSAnyPath = Get-VSAnyInstallPath

if ($Compiler -eq "MSVC" -and -not (Test-MSVCEnvironmentMatchesTarget)) {
    $msvcSource = if ($VSInstallPath) { $VSInstallPath } else { $VSAnyPath }
    if ($msvcSource -and (Initialize-MSVCEnvironment -InstallPath $msvcSource)) {
        $Compiler = Get-CompilerFromPath
        $VSInstallPath = Get-VSInstallPath
    }
}

if (-not $Compiler -and ($VSInstallPath -or $VSAnyPath)) {
    $msvcSource = if ($VSInstallPath) { $VSInstallPath } else { $VSAnyPath }
    if ($msvcSource -and (Initialize-MSVCEnvironment -InstallPath $msvcSource)) {
        $Compiler = Get-CompilerFromPath
        $VSInstallPath = Get-VSInstallPath
    }
}

if (-not $Compiler) { Fail "No usable compiler found after tool setup." }

$Plan = $null
$Ninja = Get-Command ninja -ErrorAction SilentlyContinue
$NMake = Get-Command nmake -ErrorAction SilentlyContinue
$MinGWMake = Get-Command mingw32-make -ErrorAction SilentlyContinue

switch ($Compiler) {
    "MSVC" {
        $Plan = Get-MSVCBuildPlan -InstallPath $VSInstallPath
        if (-not $Plan) { Fail "No usable CMake generator found (need ninja, nmake, or Visual Studio Build Tools)." }
    }
    "clang" {
        if ($Ninja) {
            $Plan = New-BuildPlan -GeneratorName "Ninja" -GeneratorArgs @("-G", "Ninja") -CompilerArgs @("-DCMAKE_C_COMPILER=clang")
        } elseif ($VSInstallPath -or $VSAnyPath) {
            $msvcSource = if ($VSInstallPath) { $VSInstallPath } else { $VSAnyPath }
            if ($msvcSource -and (Initialize-MSVCEnvironment -InstallPath $msvcSource)) {
                $Compiler = Get-CompilerFromPath
                $VSInstallPath = Get-VSInstallPath
                $Plan = Get-MSVCBuildPlan -InstallPath $VSInstallPath
            }
        }
        if (-not $Plan) { Fail "clang found, but ninja was not found and Visual Studio could not be initialized." }
    }
    "gcc" {
        if ($Ninja) {
            $Plan = New-BuildPlan -GeneratorName "Ninja" -GeneratorArgs @("-G", "Ninja") -CompilerArgs @("-DCMAKE_C_COMPILER=gcc")
        } elseif ($MinGWMake) {
            $Plan = New-BuildPlan -GeneratorName "MinGW Makefiles" -GeneratorArgs @("-G", "MinGW Makefiles")
        } elseif ($VSInstallPath -or $VSAnyPath) {
            $msvcSource = if ($VSInstallPath) { $VSInstallPath } else { $VSAnyPath }
            if ($msvcSource -and (Initialize-MSVCEnvironment -InstallPath $msvcSource)) {
                $Compiler = Get-CompilerFromPath
                $VSInstallPath = Get-VSInstallPath
                $Plan = Get-MSVCBuildPlan -InstallPath $VSInstallPath
            }
        }
        if (-not $Plan) { Fail "gcc found, but no usable generator was found." }
    }
    default {
        if ($VSInstallPath -or $VSAnyPath) {
            $msvcSource = if ($VSInstallPath) { $VSInstallPath } else { $VSAnyPath }
            if ($msvcSource -and (Initialize-MSVCEnvironment -InstallPath $msvcSource)) {
                $Compiler = Get-CompilerFromPath
                $VSInstallPath = Get-VSInstallPath
                $Plan = Get-MSVCBuildPlan -InstallPath $VSInstallPath
            }
        }
        if (-not $Plan) { Fail "No usable compiler found after tool setup." }
    }
}

Write-Host "  Compiler: $Compiler"
Write-Host "  Generator: $($Plan.GeneratorName)"
if ($Compiler -eq "MSVC" -and -not $VSInstallPath -and $Plan.GeneratorName -eq "Ninja") {
    Warn "Building with the current MSVC environment rather than a Visual Studio install path."
}

$ConfigureArgs = @("-S", $DIR, "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release") + $Plan.GeneratorArgs + $Plan.CompilerArgs
cmake @ConfigureArgs 2>&1 | Select-Object -Last 6
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }

$ParallelJobs = if ($env:NUMBER_OF_PROCESSORS) { $env:NUMBER_OF_PROCESSORS } else { "4" }
$BuildArgs = @("--parallel", $ParallelJobs) + $Plan.BuildArgs
& cmake --build $BuildDir @BuildArgs
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }

$Bench = $null
$Probe = $null
foreach ($p in @("$BuildDir\Release\torture-bench.exe", "$BuildDir\torture-bench.exe")) {
    if (Test-Path $p) { $Bench = $p; break }
}
foreach ($p in @("$BuildDir\Release\tune-probe.exe", "$BuildDir\tune-probe.exe")) {
    if (Test-Path $p) { $Probe = $p; break }
}
if (-not $Bench) { Fail "torture-bench.exe not found after build" }
Write-Host "  [ok] Built successfully" -ForegroundColor Green

# ==============================================================================
# STEP 4: Run benchmark
# ==============================================================================
Step 4 "Running torture-bench (${DURATION}s x 20 modules)..."

Set-Location $DIR
New-Item -ItemType Directory -Force -Path results | Out-Null

# Run tune-probe first if available
if ($Probe -and (Test-Path $Probe)) {
    Write-Host "  Running tune-probe..."
    & $Probe 2>&1 | Out-Null
}

$ts   = Get-Date -Format "yyyyMMdd_HHmmss"
$hn   = ($env:COMPUTERNAME).ToLower() -replace '[\s\.]', '_'
$arch = $ResultArch
$JsonFile = "results\bench_windows_${arch}_${hn}_${ts}.json"
$TxtFile  = "results\bench_windows_${arch}_${hn}_${ts}.txt"

Write-Host ""
& $Bench --tune -d $DURATION -o $JsonFile --json
Write-Host ""
Write-Host "  [ok] Results saved" -ForegroundColor Green

# ==============================================================================
# STEP 5: Generate report & finish
# ==============================================================================
Step 5 "Generating report..."

$jsonPath = "$DIR\$JsonFile"

# Copy the .txt report to Desktop for easy access
$DesktopPath = [Environment]::GetFolderPath("Desktop")
$ReportDest  = "$DesktopPath\torture-bench-report_${ts}.txt"

# The C binary already writes a .txt alongside the .json via -o flag
$txtPath = "$DIR\$TxtFile"
if (Test-Path $txtPath) {
    Copy-Item $txtPath $ReportDest -Force
} else {
    # Generate a report from the JSON if the .txt wasn't created
    if (Test-Path $jsonPath) {
        $data = Get-Content $jsonPath -Raw | ConvertFrom-Json
        $report = @()
        $report += "  +====================================================================="
        $report += "  |              TORTURE-BENCH RESULTS REPORT"
        $report += "  +====================================================================="
        $report += ""
        $report += "  Generated     : $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')"
        $report += "  Composite     : $($data.composite_score)"
        $report += "  Verdict       : $($data.verdict)"
        $report += "  Chain Hash    : $($data.chain_proof_hash)"
        $report += ""
        $report += "  Platform"
        $report += "  ---------------------------------------------------------------------"
        $report += "  OS            : $($data.platform.os)"
        $report += "  Arch          : $($data.platform.arch)"
        $report += "  CPU           : $($data.platform.cpu)"
        $report += "  Cores         : $($data.platform.logical_cores)"
        $report += "  RAM           : $([math]::Round([double]$data.platform.ram_gb, 1)) GB"
        $report += ""
        $report += "  Module Results"
        $report += "  ---------------------------------------------------------------------"
        $report += "  {0,-24} {1,12} {2,14} {3,10}" -f "Module","Score","Ops/sec","Time(s)"
        $report += "  {0,-24} {1,12} {2,14} {3,10}" -f "------------------------","------------","--------------","----------"
        foreach ($m in $data.modules) {
            $report += "  {0,-24} {1,12:N2} {2,14:N0} {3,10:N2}" -f $m.name, [double]$m.score, [double]$m.ops_per_sec, [double]$m.wall_time_sec
        }
        $report += ""
        $report += "  ====================================================================="
        $report += "  COMPOSITE SCORE : $($data.composite_score)"
        $report += "  VERDICT         : $($data.verdict)"
        $report += "  ====================================================================="
        $report | Out-File -FilePath $ReportDest -Encoding UTF8
    }
}

# Show scores in terminal
Write-Host ""
Write-Host "  +-----------------------------------------------------------+" -ForegroundColor Cyan
Write-Host "  |   BENCHMARK RESULTS                                      |" -ForegroundColor White
Write-Host "  +-----------------------------------------------------------+" -ForegroundColor Cyan

if (Test-Path $jsonPath) {
    try {
        $data = Get-Content $jsonPath -Raw | ConvertFrom-Json
        foreach ($m in $data.modules) {
            $name  = $m.name.PadRight(28)
            $score = '{0,12:N2}' -f [double]$m.score
            Write-Host "  |  $name $score       |" -ForegroundColor White
        }
        Write-Host "  +-----------------------------------------------------------+" -ForegroundColor Cyan
        $total = '{0,12:N4}' -f [double]$data.composite_score
        Write-Host "  |  $('TOTAL SCORE'.PadRight(28)) $total       |" -ForegroundColor Green
        $verdict = ('{0,18}' -f $data.verdict)
        Write-Host "  |  $('Verdict'.PadRight(28)) $verdict |" -ForegroundColor White
    } catch {
        Write-Host "  |  (Could not parse results)                               |" -ForegroundColor Yellow
    }
}

Write-Host "  +-----------------------------------------------------------+" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Report      : $ReportDest" -ForegroundColor White
Write-Host "  Raw JSON    : $DIR\$JsonFile" -ForegroundColor White
Write-Host ""

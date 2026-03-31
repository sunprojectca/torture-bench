#!/usr/bin/env python3
"""
detect_arch.py
Detects OS, CPU architecture, compiler architecture, CMake arch, and make/ninja arch.
Useful for diagnosing WOW64 / emulation mismatches on ARM64 Windows.
"""

import os
import sys
import platform
import subprocess
import struct
import shutil

SEP = "-" * 60

def section(title):
    print(f"\n{SEP}")
    print(f"  {title}")
    print(SEP)

def run(cmd, shell=False):
    """Run a command, return (stdout, stderr, returncode)."""
    try:
        r = subprocess.run(
            cmd, shell=shell, capture_output=True, text=True, timeout=10
        )
        return r.stdout.strip(), r.stderr.strip(), r.returncode
    except FileNotFoundError:
        return None, "not found", -1
    except Exception as e:
        return None, str(e), -1

def pointer_bits():
    return struct.calcsize("P") * 8

# ─── 1. Python / Process ────────────────────────────────────────────────────
section("Python Process")
print(f"  Python version   : {sys.version}")
print(f"  Python executable: {sys.executable}")
print(f"  Pointer size     : {pointer_bits()}-bit  (this process is {pointer_bits()}-bit)")
print(f"  sys.platform     : {sys.platform}")

# ─── 2. OS ───────────────────────────────────────────────────────────────────
section("Operating System")
print(f"  platform.system()         : {platform.system()}")
print(f"  platform.release()        : {platform.release()}")
print(f"  platform.version()        : {platform.version()}")
print(f"  platform.node()           : {platform.node()}")

# ─── 3. CPU / Native Architecture ────────────────────────────────────────────
section("CPU / Native Architecture")
print(f"  platform.machine()        : {platform.machine()}")
print(f"  platform.processor()      : {platform.processor()}")

if sys.platform == "win32":
    # PROCESSOR_ARCHITECTURE can lie under WOW64 emulation
    pa  = os.environ.get("PROCESSOR_ARCHITECTURE", "NOT SET")
    pa6 = os.environ.get("PROCESSOR_ARCHITEW6432", "NOT SET")
    print(f"  PROCESSOR_ARCHITECTURE    : {pa}")
    print(f"  PROCESSOR_ARCHITEW6432    : {pa6}")
    print()
    if pa6 and pa6 != "NOT SET":
        print("  *** WOW64 detected: process is running under emulation.")
        print(f"      Native CPU is : {pa6}")
        print(f"      Process arch  : {pa}")
    else:
        print(f"  Native CPU appears to be  : {pa}")

    # Use PowerShell environment variable — no Add-Type, instant
    ps_cmd = "[System.Environment]::GetEnvironmentVariable('PROCESSOR_ARCHITECTURE','Machine')"
    out, err, rc = run(["powershell", "-NoProfile", "-Command", ps_cmd])
    print(f"\n  Machine PROCESSOR_ARCH    : {out if rc == 0 else f'FAILED ({err})'}")

    # Check if current process is x64 emulated on ARM64 via IsWow64Process2 (Win10+)
    import ctypes
    try:
        pm, nm = ctypes.c_ushort(0), ctypes.c_ushort(0)
        ctypes.windll.kernel32.IsWow64Process2(
            ctypes.windll.kernel32.GetCurrentProcess(),
            ctypes.byref(pm), ctypes.byref(nm)
        )
        # IMAGE_FILE_MACHINE values
        machine_map = {0x014c: "x86", 0x8664: "x64/AMD64", 0xAA64: "ARM64", 0x01c4: "ARM", 0x0: "native"}
        print(f"  IsWow64Process2 native    : {machine_map.get(nm.value, hex(nm.value))}")
        print(f"  IsWow64Process2 process   : {machine_map.get(pm.value, hex(pm.value))}")
    except Exception as e:
        print(f"  IsWow64Process2           : unavailable ({e})")

    # wmic
    out3, _, rc3 = run(["wmic", "cpu", "get", "Architecture", "/value"])
    if rc3 == 0 and out3:
        mapping = {"0":"x86","5":"ARM","6":"ia64","9":"x86_64/AMD64","12":"ARM64"}
        for line in out3.splitlines():
            if "Architecture" in line and "=" in line:
                val = line.split("=")[1].strip()
                label = mapping.get(val, f"unknown({val})")
                print(f"  WMIC CPU Architecture     : {val} => {label}")
else:
    out, _, _ = run(["uname", "-m"])
    print(f"  uname -m                  : {out}")
    out2, _, _ = run(["uname", "-p"])
    print(f"  uname -p                  : {out2}")

# ─── 4. Compiler Architecture ────────────────────────────────────────────────
section("Compiler Architecture")

compilers = []
for name in ["cl", "clang", "gcc", "cc"]:
    path = shutil.which(name)
    if path:
        compilers.append((name, path))

if not compilers:
    print("  No compiler found in PATH.")
else:
    for name, path in compilers:
        print(f"\n  [{name}] => {path}")
        if name == "cl":
            # cl prints version to stderr, arch is in the banner
            out, err, rc = run([name])
            banner = (out or "") + (err or "")
            for line in banner.splitlines()[:3]:
                print(f"    {line}")
            if "ARM64" in banner or "arm64" in banner:
                print("    => Compiler target: ARM64")
            elif "x64" in banner or "AMD64" in banner:
                print("    => Compiler target: x64 (AMD64)")
            elif "x86" in banner:
                print("    => Compiler target: x86 (32-bit)")
        else:
            out, err, rc = run([name, "-dumpmachine"])
            print(f"    -dumpmachine : {out or err}")
            out2, err2, rc2 = run([name, "-v"])
            if rc2 == 0 or err2:
                for line in (err2 or out2 or "").splitlines():
                    if "Target:" in line or "target:" in line:
                        print(f"    {line.strip()}")
                        break

# ─── 5. CMake Architecture ───────────────────────────────────────────────────
section("CMake Architecture")
cmake_path = shutil.which("cmake")
if not cmake_path:
    print("  cmake not found in PATH.")
else:
    print(f"  cmake path : {cmake_path}")
    out, _, rc = run(["cmake", "--version"])
    print(f"  version    : {out.splitlines()[0] if out else 'unknown'}")

    # cmake -E capabilities is fast (no configure step needed)
    import json as _json
    out2, err2, rc2 = run(["cmake", "-E", "capabilities"])
    if rc2 == 0 and out2:
        try:
            caps = _json.loads(out2)
            gen_names = [g.get("name", "") for g in caps.get("generators", [])]
            preferred = [g for g in gen_names if any(k in g for k in ("Visual Studio", "Ninja", "Unix"))][:5]
            print(f"  Preferred generators      : {', '.join(preferred) if preferred else '(none matched)'}")
        except Exception:
            pass

    # Tiny cmake -P script using cmake_host_system_information — instant
    import tempfile, pathlib
    with tempfile.TemporaryDirectory() as tmp:
        script = pathlib.Path(tmp) / "detect.cmake"
        script.write_text(
            'cmake_minimum_required(VERSION 3.10)\n'
            'cmake_host_system_information(RESULT _proc  QUERY PROCESSOR_DESCRIPTION)\n'
            'cmake_host_system_information(RESULT _bits  QUERY IS_64BIT)\n'
            'cmake_host_system_information(RESULT _ncpu  QUERY NUMBER_OF_LOGICAL_CORES)\n'
            'message(STATUS "HOST_PROCESSOR_DESCRIPTION=${_proc}")\n'
            'message(STATUS "HOST_IS_64BIT=${_bits}")\n'
            'message(STATUS "HOST_LOGICAL_CORES=${_ncpu}")\n'
        )
        out3, err3, rc3 = run(["cmake", "-P", str(script)])
        output = (err3 or out3 or "")
        for line in output.splitlines():
            if any(k in line for k in ("HOST_PROCESSOR_DESCRIPTION", "HOST_IS_64BIT", "HOST_LOGICAL_CORES")):
                print(f"  cmake host info           : {line.strip().lstrip('-- ')}")

# ─── 6. Make / Ninja ─────────────────────────────────────────────────────────
section("Build Tools")
for tool in ["ninja", "make", "mingw32-make", "nmake"]:
    path = shutil.which(tool)
    if path:
        out, err, rc = run([tool, "--version"])
        ver = (out or err or "").splitlines()[0] if (out or err) else "?"
        print(f"  {tool:20s}: {path}")
        print(f"  {'':20s}  version: {ver}")
    else:
        print(f"  {tool:20s}: not found")

# ─── 7. Summary & Mismatch Warning ───────────────────────────────────────────
section("Summary")
native = platform.machine()
proc   = pointer_bits()
print(f"  platform.machine() reports : {native}")
print(f"  This Python process is     : {proc}-bit")

if sys.platform == "win32":
    pa  = os.environ.get("PROCESSOR_ARCHITECTURE", "")
    pa6 = os.environ.get("PROCESSOR_ARCHITEW6432", "")
    true_arch = pa6 if pa6 else pa
    print(f"  True native Windows arch   : {true_arch}")

    warnings = []
    if "ARM64" in true_arch and proc != 64:
        warnings.append("Python is not 64-bit on an ARM64 machine.")
    if "ARM64" in true_arch and pa == "AMD64" and not pa6:
        warnings.append("PROCESSOR_ARCHITEW6432 not set — process may be running under x64 emulation.")
    if "ARM64" in true_arch and "AMD64" in native:
        warnings.append(
            "platform.machine() reports AMD64 but native arch is ARM64. "
            "This Python is x64-emulated — compilers/cmake may also target x64!"
        )

    if warnings:
        print()
        for w in warnings:
            print(f"  *** WARNING: {w}")
    else:
        print()
        print("  No architecture mismatches detected.")

print(f"\n{SEP}\n")

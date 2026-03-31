# torture-bench

A cross-platform CPU benchmark that measures real performance and detects when hardware is secretly cheating.

**One command. Any machine. Fair scores.**

---

## Windows ARM (Snapdragon) — Start Here

Most issues people hit are on Windows ARM laptops (Snapdragon X Elite / Plus, etc.). Here's the fast path.

### One-liner (PowerShell)

```powershell
irm https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.ps1 | iex
```

This downloads, builds for ARM64, runs all 20 modules, and saves results to your Desktop. Takes ~3 minutes.

### Manual build (if the one-liner fails)

```bat
git clone https://github.com/sunprojectca/torture-bench.git
cd torture-bench
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A ARM64
cmake --build . --config Release
cd ..
build\Release\torture-bench.exe --tune -d 10 -o results\my_run.json --json
```

> **Key flag:** `-A ARM64` — without this, CMake defaults to x64 and the binary runs under emulation (slower and wrong).

### Verify your binary is really ARM64

```powershell
dumpbin /headers build\Release\torture-bench.exe | Select-String "machine"
```

You should see **`AA64`** (ARM64). If you see `8664` (x64) you built the wrong arch.

### Common Problems and Fixes

| Problem | Cause | Fix |
|---|---|---|
| `cmake` says "no generator found" | Visual Studio not installed or wrong version | Install [VS Build Tools 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022) with "Desktop development with C++" |
| Build succeeds but runs slow | Built x64 instead of ARM64 — runs under emulation | Rebuild with `-A ARM64` instead of `-A x64` |
| `LNK1104: cannot open file 'kernel32.lib'` | ARM64 Windows SDK/libs not installed | In VS Installer, add "MSVC ARM64/ARM64EC build tools" and "Windows SDK" |
| `cmake` picks Ninja instead of MSVC | Ninja on PATH (e.g. from Android SDK) | Explicitly pass `-G "Visual Studio 17 2022"` |
| Script fails with "not recognized" | Running `.sh` script in PowerShell or `.ps1` in cmd | Use `bench.ps1` in PowerShell, `bench.sh` in bash/WSL |
| Crash or illegal instruction | Running an x64 binary on ARM64 | Delete `build/`, rebuild with `-A ARM64` |
| RAM shows as -0.4 GB | Known cosmetic bug on some Snapdragon devices | Ignore — does not affect scores |
| `xcode-select` error on Mac | Not on Mac — wrong script | Use `bench.ps1` on Windows, `bench.sh` on Mac |
| `Access is denied` running `.ps1` | PowerShell execution policy | Run `Set-ExecutionPolicy -Scope Process Bypass` first |

---

## Quick Start — All Platforms

### Mac / Linux / WSL

```bash
curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | bash
```

### Windows (PowerShell)

```powershell
irm https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.ps1 | iex
```

The script will:

1. Download the code
2. Install any missing build tools (cmake, ninja, compiler)
3. Compile for your machine's native architecture
4. Run the full benchmark suite (~3 minutes)
5. Save a formatted report to your Desktop

> The benchmark runs entirely locally. No tokens, no Python required.

Results are saved as:

- `results/bench_<os>_<arch>_<host>_<timestamp>.json` — structured data
- `results/bench_<os>_<arch>_<host>_<timestamp>.txt` — human-readable report

---

## Understanding Your Results

After the benchmark runs, you'll see output like this:

```
  ╔═══════════════════════════════════════════════════════╗
  ║         TORTURE-BENCH  v1.0  CPU Fairness             ║
  ╚═══════════════════════════════════════════════════════╝

  Platform:
  OS      : macOS
  Arch    : arm64
  CPU     : Apple M2 Pro
  Cores   : 12 logical
  RAM     : 32.0 GB
  SIMD    : NEON
  SOC     : Apple Silicon
```

Then each module runs and prints its score. At the end:

```
  Composite Score: 5068.30
  Coprocessor Warnings: 1
  Verdict: MINOR_ACCELERATION_DETECTED
```

### What the verdict means

| Verdict | Meaning |
|---|---|
| **PURE_CPU_FAIR** | All tests ran on the CPU with no hidden acceleration. Clean score. |
| **MINOR_ACCELERATION_DETECTED** | One or two tests may have used hardware accelerators (like AES-NI for encryption). Scores are still mostly fair. |
| **SIGNIFICANT_ACCELERATION_DETECTED** | Multiple tests detected coprocessor use. The composite score is inflated compared to pure CPU performance. |

A "minor" verdict is normal on modern hardware — almost every CPU made after 2015 has AES-NI encryption acceleration. It doesn't mean anything is wrong.

### What the modules test

| Module | What it measures | In plain English |
|---|---|---|
| cpu_single | Single-core integer + floating point | How fast is one core? |
| cpu_parallel | All cores running simultaneously | How fast are all cores together? |
| cpu_sustained | Performance over time (sampled every 0.2s) | Does it slow down when it gets hot? |
| memory_bandwidth | STREAM triad (read+write throughput) | How fast can it move data? |
| memory_latency | Pointer-chasing random access | How quickly can it find data in RAM? |
| cache_thrash | L1/L2/L3 cache separately | How fast are the CPU's built-in caches? |
| branch_chaos | Unpredictable if/else decisions | How well does it guess what code does next? |
| hash_chain | SHA-256 cryptographic hashing | Raw crypto speed (detects SHA hardware) |
| raytracer | 3D path tracing, no GPU | Pure CPU graphics rendering |
| simd_dispatch | NEON/AVX2 vector math vs scalar | Does the CPU have wide math instructions? |
| crypto_stress | AES + ChaCha20 encryption | Detects hardware crypto engines |
| ml_matmul | Matrix multiply (FP32/INT8/BF16) | AI/ML inference speed |
| lattice_geometry | Post-quantum crypto operations | Kyber/Dilithium lattice math |
| linear_algebra | GEMM / LU / Cholesky decomposition | Dense math workloads |
| exotic_chaos | Random mix of 10 different algorithms | Unpredictable mixed workload |
| ips_micro | Instructions per second + latency | Raw instruction throughput |

### The composite score

The **composite score** is the average of all module scores. Because modules measure very different things (memory latency in nanoseconds vs matrix multiply in GFLOPS), the raw numbers vary wildly. The composite is useful for comparing the _same machine over time_ or _similar machines against each other_, but comparing an M2 Mac composite against an Intel desktop composite is apples-to-oranges — look at individual module scores instead.

---

## Viewing the Scoreboard

Open the local HTML dashboard at `docs/torture_benchmark.html` to see your results.

The scoreboard shows:

- **Composite score timeline** — how scores change over time across different machines
- **Selected run module chart** — visual breakdown of strengths and warnings for one run
- **Module comparison bars** — pick any module and compare across all visible runs
- **System spec panel** — CPU, RAM, caches, SIMD flags, source, commit, and config
- **Detailed module table** — score, ops/sec, wall time, flags, and notes for every module
- **Leaderboard** — filter by OS, architecture, verdict, and search text

---

## Customizing Your Run

Set environment variables before running:

```bash
# Run each test for 30 seconds instead of 10 (more accurate, takes longer)
curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | BENCH_DURATION=30 bash

# Use only 4 threads instead of all cores
curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | BENCH_THREADS=4 bash

# Clone to a specific directory
curl -sL https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.sh | BENCH_DIR=/tmp/mybench bash
```

On Windows PowerShell:

```powershell
$env:BENCH_DURATION = 30
irm https://raw.githubusercontent.com/sunprojectca/torture-bench/main/bench.ps1 | iex
```

---

## For Developers

### Manual build

```bash
git clone https://github.com/sunprojectca/torture-bench.git
cd torture-bench
bash build.sh
```

On Windows with Visual Studio:

```bat
git clone https://github.com/sunprojectca/torture-bench.git
cd torture-bench
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

On Snapdragon / Qualcomm Windows laptops, use `-A ARM64` instead of `-A x64`.

### Running manually

```bash
# Run the anti-cheat probe first, then full benchmark
./build/tune-probe
./build/torture-bench --tune -d 10 -o results.json --json

# Or choose a custom text report path explicitly
./build/torture-bench --tune -d 10 -o results.json --txt results_report.txt

# Quick 3-second pass
./build/torture-bench -d 3

# Run only one module
./build/torture-bench --only raytracer -d 60

# Skip specific modules
./build/torture-bench --skip raytracer --skip ml_matmul

# Set a deterministic seed for reproducibility
./build/torture-bench -s deadbeefcafebabe
```

### All CLI options

| Option | Argument | Default | Description |
|---|---|---|---|
| `-d` | `<sec>` | `10` | Duration per module |
| `-t` | `<n>` | `0` (all cores) | Thread count |
| `-s` | `<hex>` | time-based | Initial chain seed |
| `-o` | `<file>` | none | Write JSON to file and a matching `.txt` report |
| `--txt` | `<file>` | auto sidecar | Write a detailed human-readable text report |
| `-c` | `<file>` | none | Append CSV row |
| `--tune` | — | off | Run anti-cheat probe first |
| `--verbose` | — | off | Extra output |
| `--list` | — | — | List modules and exit |
| `--only` | `<name>` | — | Run only this module |
| `--skip` | `<name>` | — | Skip this module (repeatable) |
| `--json` | — | off | Print JSON to stdout |

### Platform support

| OS | Architecture | Compiler | Status |
|---|---|---|---|
| Linux | x86_64 | gcc, clang | ✅ |
| Linux | ARM64 | gcc, clang | ✅ |
| WSL2 | x86_64 / ARM64 | gcc, clang | ✅ |
| macOS | ARM64 (Apple Silicon) | clang | ✅ |
| macOS | x86_64 (Intel) | clang | ✅ |
| Windows | x86_64 | MSVC, MinGW, clang | ✅ |
| Windows | ARM64 (Snapdragon) | MSVC | ✅ |

### How chaining works

Every module receives a `chain_seed` from the previous module's output. It mixes that seed into its workload and produces a `chain_out` that feeds the next module. This creates a cryptographic proof that all modules ran in order — you can't skip a module or reorder them without breaking the final `chain_proof_hash`.

### Anti-cheat detection thresholds

| Detection | Method | Threshold |
|---|---|---|
| Cache pre-seeding | Cold vs warm run ratio | >2× |
| AES-NI | AES vs ChaCha20 speed ratio | >10× |
| SHA-NI | Hashes/sec vs scalar ceiling | >500k/s |
| AMX/BLAS | GEMM GFLOPS vs scalar ceiling | >10 GFLOPS |
| GPU raytracing | Rays/sec vs scalar ceiling | >2M rays/s |
| Thermal throttle | First vs last 3 samples | >15% drop |
| Turbo boost | 1s burst vs 10s sustained | >30% gap |

### CI / GitHub Actions

Every push to `main` triggers builds on Linux, macOS, and Windows via GitHub Actions. The static dashboard lives under `docs/`. See `.github/workflows/bench.yml`.

### Adding a new module

1. Create `modules/your_module.c` implementing `bench_result_t module_your_module(uint64_t chain_seed, int thread_count, int duration_sec)`
2. Add the extern declaration in `harness/orchestrator.c`
3. Add to the `MODULE_TABLE` with name + description
4. Add the source file to `CMakeLists.txt`
5. Rebuild and test

---

## Project Structure

```
torture-bench/
├── bench.sh              ← one-liner for Mac/Linux
├── bench.ps1             ← one-liner for Windows
├── CMakeLists.txt        ← cross-platform build config
├── build.sh / build.bat  ← platform build scripts
├── harness/
│   ├── main.c            ← entry point, CLI parsing
│   ├── orchestrator.c/h  ← runs modules in sequence
│   ├── reporter.c/h      ← JSON + CSV output
│   ├── platform.c/h      ← OS/CPU/SIMD detection
│   ├── common.h          ← shared types + timing
│   └── bench_thread.h    ← portable threading (pthreads / Win32)
├── modules/
│   ├── cpu_single.c      ← single-core torture
│   ├── cpu_parallel.c    ← all-core parallel
│   ├── ... (20 modules)
│   └── anticache_guard.c ← cache flush / anti-cheat
├── tools/
│   └── tune_probe.c      ← standalone anti-cheat diagnostics
├── results/              ← benchmark JSON/CSV outputs
├── docs/
│   ├── index.html        ← redirect to the canonical dashboard entrypoint
│   ├── torture_benchmark.html ← canonical static dashboard
│   └── data/             ← fallback snapshot data + raw published runs
└── .github/workflows/
    └── bench.yml         ← CI: build + smoke test
```

---

## License

MIT

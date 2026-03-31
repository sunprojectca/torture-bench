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

These 20 modules fall into a few categories: core throughput, memory and cache behavior, speculation and pipeline stress, and accelerator detection.

| Module | Detailed description |
|---|---|
| cpu_single | Single-core integer and floating-point throughput on the shared CPU path. |
| cpu_parallel | All-core throughput on the same worker path, used to measure scaling and scheduler behavior. |
| cpu_sustained | Samples throughput every 0.2 seconds across the full run to expose thermal throttling or power limiting. |
| memory_bandwidth | A STREAM-style triad that streams large arrays through read/write operations to measure bandwidth. |
| memory_latency | A shuffled pointer-chasing ring that measures true RAM latency instead of cached throughput. |
| cache_thrash | Separate L1, L2, and L3-sized phases that keep each cache level under pressure. |
| branch_chaos | Deeply nested unpredictable branches and switch dispatch that hammer the branch predictor. |
| hash_chain | A pure-C SHA-256 chain with no OpenSSL or hardware SHA helpers, so crypto acceleration is visible. |
| raytracer | Scalar CPU path tracing against a fixed scene, with no GPU calls or SIMD shortcuts. |
| simd_dispatch | Runs the same math scalar, with SIMD intrinsics, and with compiler auto-vectorization to compare paths. |
| crypto_stress | Exercises AES-128, ChaCha20, RSA modexp, and ECDH-style scalar math to expose crypto acceleration. |
| ml_matmul | Runs FP32 GEMM, INT8 matmul, BF16-style multiply, and attention-like work to detect matrix accelerators. |
| lattice_geometry | Performs NTT, LWE-style matrix-vector work, Babai nearest-plane steps, and Gram-Schmidt orthogonalization. |
| linear_algebra | Benchmarks dense GEMM, LU, Cholesky, power iteration, and conjugate gradient in pure C. |
| exotic_chaos | Randomly mixes ten unrelated algorithms, including Mandelbrot, Game of Life, sort, BFS, and RC4. |
| ips_micro | Measures integer IPS, float IPS, branch behavior, memory latency, TLB pressure, and store-load forwarding. |
| pipeline_torture | Creates ILP bursts, execution-unit contention, and I-cache stress to hit fetch, decode, execute, and retire. |
| ooo_execution | Compares 1, 4, and 16 independent chains, then adds ROB and load/store pressure to reveal OOO width. |
| dependency_chain | Stacks serial integer and FP chains, diamond dependency graphs, memory dependencies, and register pressure. |
| speculation_stress | Mixes predictable and unpredictable branches, BTB stress, deep recursion, and speculative loads. |

### ASIC-Resistant Code

In this repo, ASIC-resistant code means workload patterns that are hard to turn into a narrow special-purpose accelerator. They are not impossible to speed up, but they resist the kind of fixed-function hardware that loves one tiny, regular kernel and struggles with changing control flow, data dependencies, or memory access patterns.

The most ASIC-resistant modules here are the ones with unpredictable branches, pointer chasing, dependency chains, mixed instruction mixes, or changing control flow:

- `branch_chaos`
- `memory_latency`
- `pipeline_torture`
- `ooo_execution`
- `dependency_chain`
- `speculation_stress`
- `exotic_chaos`

By contrast, modules like `hash_chain`, `crypto_stress`, `ml_matmul`, and `simd_dispatch` are intentionally more accelerator-friendly. That is useful too, because if a machine scores strangely high on those workloads, it can reveal hidden hardware help such as AES instructions, matrix engines, or wider SIMD units.

Examples of ASIC-resistant patterns:

```c
/* Hard to prefetch or map to one fixed fast path */
while (bench_now_sec() < deadline) {
    idx = buf[idx];
    idx = buf[idx];
}
```

```c
/* Hard to predict: control flow changes constantly */
uint64_t v = xorshift64(&rng);
if (v & 1) acc += v;
if (v & 2) acc ^= v;
switch ((v >> 13) & 7) {
    case 0: acc += v * 3; break;
    case 1: acc ^= v << 5; break;
    case 2: acc -= v >> 2; break;
    case 3: acc |= v * 7; break;
    case 4: acc &= v + 1; break;
    case 5: acc *= v | 1; break;
    case 6: acc ^= acc >> 11; break;
    case 7: acc += acc << 3; break;
}
```

```c
/* More accelerator-friendly: regular dense math */
for (size_t i = 0; i < n; i++)
    c[i] = a[i] + s * b[i];
```

The last pattern is not bad code; it is just easier to accelerate with SIMD, BLAS, matrix engines, or vendor helpers. This benchmark keeps both kinds of workloads on purpose so you can see the difference clearly.

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

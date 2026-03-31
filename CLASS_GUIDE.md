# torture-bench: Purpose and "Class" Guide

> This project is written in **C**, so it does not use classes in the OOP sense.
> In this guide, "classes" means the **structs + modules + APIs** that act like class responsibilities.

## 1) Whole purpose of the project

`torture-bench` exists to measure CPU performance **fairly** across different machines while detecting hidden acceleration paths that can distort benchmark results.

### Primary goals

- Measure broad CPU behavior (single-core, multi-core, sustained, memory, branch, SIMD, crypto, math-heavy kernels).
- Chain module outputs so every stage must run (`chain_in` -> `chain_out` -> final proof hash).
- Detect suspicious acceleration (coprocessors, prefetch/cache effects, turbo/throttling behavior).
- Produce reproducible output in JSON/CSV for comparison.

---

## 2) Architecture at a glance

Execution flow:

1. `harness/main.c` parses CLI options and builds `bench_config_t`.
2. `orchestrator_init` + `orchestrator_register_all` prepare platform info and module table.
3. `orchestrator_run` executes enabled modules in sequence, including anti-cache and entropy spacing.
4. `orchestrator_print_summary` prints final verdict.
5. `reporter_write_json` / `reporter_write_csv` emit machine-readable artifacts.

---

## 3) "Class" guide (structs and their roles)

## `bench_result_t` (in `harness/common.h`)

**Role (class-like):** result object for one benchmark module.

**Key fields**

- `module_name`
- `score`
- `wall_time_sec`
- `ops_per_sec`
- `chain_in`, `chain_out`
- `coprocessor_suspected`
- `flags`, `notes`

**Why it exists:** standard output contract so all modules report in the same schema.

## `bench_module_t` (in `harness/common.h`)

**Role:** module descriptor + function pointer interface.

**Key fields**

- `name`
- `description`
- `enabled`
- `run` callback: `bench_result_t (*run)(uint64_t, int, int)`

**Why it exists:** allows the orchestrator to treat all benchmark modules polymorphically.

## `bench_config_t` (in `harness/common.h`)

**Role:** runtime configuration object.

**Key fields**

- `thread_count`
- `duration_sec`
- `tuning_mode`
- `verbose`
- `json_output`
- `output_file`
- `initial_seed`

**Why it exists:** one shared config source across main/orchestrator/reporter.

## `platform_info_t` (in `harness/platform.h`)

**Role:** detected hardware/OS capabilities.

**Key fields**

- OS/arch/CPU brand
- core and RAM data
- cache sizes
- SIMD support (`neon`, `avx2`, `avx512`, `sve`)
- SoC hints (`is_apple_silicon`, `is_snapdragon`)

**Why it exists:** benchmark behavior and reporting depend on platform features.

## `orchestrator_t` (in `harness/orchestrator.h`)

**Role:** top-level runtime coordinator (the main controller object).

**Contains**

- module registry (`modules[]`, `count`)
- active config (`config`)
- detected platform (`platform`)
- collected module results (`results[]`)
- `final_chain_hash`

**Why it exists:** central state and lifecycle management for the benchmark session.

## `anticache_report_t` (in `modules/anticache_guard.h`)

**Role:** anti-cache probe result object.

**Key fields**

- first/second run timings
- `ratio`
- suspicion flag + verdict string

**Why it exists:** isolates cache-warm/pre-seeding diagnostics from core benchmark logic.

---

## 4) Functional subsystems (class-like modules)

## `platform.*`

- Detect machine details (`platform_detect`)
- Print details (`platform_print`)
- Core count / cycle counter / thread pinning helpers

## `orchestrator.*`

- Registers module table
- Runs modules in sequence
- Applies anti-cache flush/barrier
- Applies entropy spacing between module runs
- Maintains chain proof

## `reporter.*`

- Exports full JSON (`reporter_write_json`)
- Exports compact CSV (`reporter_write_csv`)

## `anticache_guard.*`

- Allocates/flushes poison buffer
- Adds barriers
- Probes suspicious warm-cache behavior

## `modules/*.c`

- Each file implements one benchmark module returning `bench_result_t`
- All conform to the same function signature and chain contract

---

## 5) How module chaining works (integrity model)

Each module receives `chain_seed` from prior module output:

- Module computes workload
- Emits `chain_out` (mixed with local measured state)
- Orchestrator passes that to the next module

This creates a linked execution proof that reduces risk of skipping/reordering stages.

---

## 6) How to add a new "class" (module)

1. Create `modules/<name>.c` implementing:
   - `bench_result_t module_<name>(uint64_t chain_seed, int thread_count, int duration_sec)`
2. Fill all required `bench_result_t` fields.
3. Add extern declaration in `harness/orchestrator.c`.
4. Add entry to `MODULE_TABLE` with name + description + function.
5. Add source file to `CMakeLists.txt` module source list.
6. Rebuild and run quick smoke test.

---

## 7) Design conventions

- Keep module logic self-contained.
- Prefer deterministic pseudo-randomness from seed (`xorshift64`) instead of global RNG.
- Record anomalies in `notes` and machine hints in `flags`.
- Use `coprocessor_suspected` only when a clear threshold/heuristic is crossed.
- Preserve chain integrity and timing realism (`wall_time_sec` sanity checks).

---

## 8) Practical mental model

If you want OOP terms in C:

- **Classes:** `orchestrator_t`, `bench_config_t`, `platform_info_t`, `bench_result_t`
- **Interface:** `bench_module_t.run(...)`
- **Implementations:** each file in `modules/*.c`
- **Controller:** `orchestrator_run`
- **Serialization layer:** `reporter_write_json` / `reporter_write_csv`

That is the project’s "class system," expressed in idiomatic C.

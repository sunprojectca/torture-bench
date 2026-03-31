#include "orchestrator.h"
#include "common.h"
#include "platform.h"
#include "../modules/anticache_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int enabled_module_count(const orchestrator_t *o)
{
    int count = 0;
    for (int i = 0; i < o->count; i++)
    {
        if (o->modules[i].enabled)
            count++;
    }
    return count;
}

static int completed_result_count(const orchestrator_t *o)
{
    int count = 0;
    for (int i = 0; i < o->count; i++)
    {
        if (o->results[i].module_name)
            count++;
    }
    return count;
}

/* ── forward declarations for all modules ────────────────────────────────── */
extern bench_result_t module_cpu_single(uint64_t seed, int threads, int dur);
extern bench_result_t module_cpu_parallel(uint64_t seed, int threads, int dur);
extern bench_result_t module_cpu_sustained(uint64_t seed, int threads, int dur);
extern bench_result_t module_memory_bandwidth(uint64_t seed, int threads, int dur);
extern bench_result_t module_memory_latency(uint64_t seed, int threads, int dur);
extern bench_result_t module_cache_thrash(uint64_t seed, int threads, int dur);
extern bench_result_t module_branch_chaos(uint64_t seed, int threads, int dur);
extern bench_result_t module_hash_chain(uint64_t seed, int threads, int dur);
extern bench_result_t module_raytracer(uint64_t seed, int threads, int dur);
extern bench_result_t module_simd_dispatch(uint64_t seed, int threads, int dur);
extern bench_result_t module_crypto_stress(uint64_t seed, int threads, int dur);
extern bench_result_t module_ml_matmul(uint64_t seed, int threads, int dur);
extern bench_result_t module_lattice_geometry(uint64_t seed, int threads, int dur);
extern bench_result_t module_linear_algebra(uint64_t seed, int threads, int dur);
extern bench_result_t module_exotic_chaos(uint64_t seed, int threads, int dur);
extern bench_result_t module_ips_micro(uint64_t seed, int threads, int dur);
extern bench_result_t module_pipeline_torture(uint64_t seed, int threads, int dur);
extern bench_result_t module_ooo_execution(uint64_t seed, int threads, int dur);
extern bench_result_t module_dependency_chain(uint64_t seed, int threads, int dur);
extern bench_result_t module_speculation_stress(uint64_t seed, int threads, int dur);

/* ── wrapper to adapt module function signatures ──────────────────────────── */
typedef bench_result_t (*module_fn)(uint64_t, int, int);

static const struct
{
    const char *name;
    const char *desc;
    module_fn fn;
} MODULE_TABLE[] = {
    {"cpu_single", "Single-core integer and floating-point throughput", module_cpu_single},
    {"cpu_parallel", "All-core throughput on the shared CPU path", module_cpu_parallel},
    {"cpu_sustained", "Sampled throughput over time to expose throttling", module_cpu_sustained},
    {"memory_bandwidth", "STREAM triad read/write bandwidth", module_memory_bandwidth},
    {"memory_latency", "Pointer-chasing latency in nanoseconds", module_memory_latency},
    {"cache_thrash", "Separate L1, L2, and L3 cache pressure phases", module_cache_thrash},
    {"branch_chaos", "Nested unpredictable branches and switch dispatch", module_branch_chaos},
    {"hash_chain", "Pure-C SHA-256 chain without hardware SHA helpers", module_hash_chain},
    {"raytracer", "Scalar CPU path tracing on a fixed scene", module_raytracer},
    {"simd_dispatch", "Scalar vs SIMD vs auto-vectorized math", module_simd_dispatch},
    {"crypto_stress", "AES-128, ChaCha20, RSA, and ECDH-style math", module_crypto_stress},
    {"ml_matmul", "FP32/INT8/BF16 matmul and attention-like work", module_ml_matmul},
    {"lattice_geometry", "NTT, LWE, Babai, and Gram-Schmidt lattice math", module_lattice_geometry},
    {"linear_algebra", "GEMM, LU, Cholesky, eigen, and CG workloads", module_linear_algebra},
    {"exotic_chaos", "Random mix of ten unrelated algorithms", module_exotic_chaos},
    {"ips_micro", "Integer, float, branch, latency, and TLB microbenchmarks", module_ips_micro},
    {"pipeline_torture", "ILP, execution-unit contention, and I-cache stress", module_pipeline_torture},
    {"ooo_execution", "OOO width, ROB depth, and load/store buffers", module_ooo_execution},
    {"dependency_chain", "Serial, diamond, memory, and register pressure", module_dependency_chain},
    {"speculation_stress", "Branch prediction, BTB, RSB, and speculative loads", module_speculation_stress},
};
#define N_MODULES ((int)(sizeof(MODULE_TABLE) / sizeof(MODULE_TABLE[0])))

/* ── init ────────────────────────────────────────────────────────────────── */
void orchestrator_init(orchestrator_t *o, const bench_config_t *cfg)
{
    memset(o, 0, sizeof(*o));
    o->config = *cfg;
    platform_detect(&o->platform);
}

void orchestrator_register_all(orchestrator_t *o)
{
    for (int i = 0; i < N_MODULES && i < MAX_MODULES; i++)
    {
        o->modules[i].name = MODULE_TABLE[i].name;
        o->modules[i].description = MODULE_TABLE[i].desc;
        o->modules[i].enabled = 1;
    }
    o->count = N_MODULES;
}

/* ── run ─────────────────────────────────────────────────────────────────── */
int orchestrator_run(orchestrator_t *o)
{
    bench_config_t *cfg = &o->config;

    /* Allocate poison buffer for anti-cache-cheat: 2x L3 */
    size_t poison_sz = (size_t)(o->platform.cache_l3_kb * 1024) * 2;
    if (poison_sz < 32 * 1024 * 1024)
        poison_sz = 32 * 1024 * 1024; /* minimum 32MB */
    void *poison = anticache_alloc(poison_sz);

    if (cfg->verbose)
    {
        printf("\n  Poison buffer: %.0f MB\n", (double)poison_sz / (1024 * 1024));
    }

    uint64_t chain = cfg->initial_seed;
    uint64_t rnd_state = cfg->initial_seed ^ 0xA5A5A5A55A5A5A5AULL;

    /* ── optional tuning probe ───────────────────────────────────────────── */
    if (cfg->tuning_mode)
    {
        printf("\n[TUNING PROBE] Warming up and detecting pre-run cache seeding...\n");
        bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                             1024 + (int)(xorshift64(&rnd_state) & 1023));
        /* Run a quick hash chain twice with and without flush */
        bench_result_t r1 = module_hash_chain(chain, 1, 2);
        anticache_flush(poison, poison_sz);
        bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                             512 + (int)(xorshift64(&rnd_state) & 511));
        bench_result_t r2 = module_hash_chain(chain, 1, 2);
        double ratio = r1.ops_per_sec / (r2.ops_per_sec + 1e-9);
        printf("  Pre-flush score:  %.0f ops/sec\n", r1.ops_per_sec);
        printf("  Post-flush score: %.0f ops/sec\n", r2.ops_per_sec);
        if (ratio > 1.3)
            printf("  WARN: Pre-flush %.1fx faster — OS/hardware was pre-seeding cache\n", ratio);
        else
            printf("  OK: Cache state consistent (ratio=%.2f)\n", ratio);
    }

    printf("\n[BENCHMARK START] chain_seed=0x%016llx\n",
           (unsigned long long)chain);
    int enabled_count = enabled_module_count(o);
    printf("  Modules: %d | Duration/module: %ds | Threads: %d\n\n",
           enabled_count, cfg->duration_sec,
           cfg->thread_count > 0 ? cfg->thread_count : o->platform.logical_cores);

    for (int i = 0; i < o->count; i++)
    {
        if (!o->modules[i].enabled)
            continue;

        const char *name = MODULE_TABLE[i].name;
        module_fn fn = MODULE_TABLE[i].fn;

        if (cfg->verbose)
             printf("  [%2d/%d] %-20s chain_in=0x%016llx\n",
                 i + 1, o->count, name, (unsigned long long)chain);

        /* Flush caches before each module */
        anticache_flush(poison, poison_sz);
        anticache_barrier();
        bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                             2048 + (int)(xorshift64(&rnd_state) & 2047));

        double t0 = bench_now_sec();
        bench_result_t res = fn(chain, cfg->thread_count, cfg->duration_sec);
        double wall = bench_now_sec() - t0;
        bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                             512 + (int)(xorshift64(&rnd_state) & 1023));

        /* Sanity: if module reported less time than wall, correct it */
        if (res.wall_time_sec < wall * 0.5)
            res.wall_time_sec = wall;

        o->results[i] = res;
        chain = res.chain_out; /* pass hash to next module */

        /* Print result */
        printf("  %-22s score=%10.2f  ops/s=%12.0f  %.1fs",
               name, res.score, res.ops_per_sec, res.wall_time_sec);
        if (res.coprocessor_suspected)
            printf("  [!COPROCESSOR]");
        printf("\n");
        if (cfg->verbose && res.flags[0])
            printf("    flags: %s\n", res.flags);
        if (res.notes[0])
            printf("    note:  %s\n", res.notes);
    }

    o->final_chain_hash = chain;

    if (poison)
        anticache_free(poison, poison_sz);
    return 0;
}

/* ── summary ─────────────────────────────────────────────────────────────── */
void orchestrator_print_summary(const orchestrator_t *o)
{
    printf("\n%s\n", "=======================================================");
    printf("  BENCHMARK COMPLETE\n");
    printf("  Chain proof hash : 0x%016llx\n",
           (unsigned long long)o->final_chain_hash);
    printf("%s\n\n", "=======================================================");

    /* Count coprocessor warnings */
    int warnings = 0;
    for (int i = 0; i < o->count; i++)
        if (o->results[i].module_name && o->results[i].coprocessor_suspected)
            warnings++;

    if (warnings > 0)
    {
        printf("  [!] COPROCESSOR / ACCELERATION WARNINGS: %d module(s)\n", warnings);
        for (int i = 0; i < o->count; i++)
        {
            if (o->results[i].module_name && o->results[i].coprocessor_suspected)
                printf("     - %-20s %s\n",
                       o->results[i].module_name, o->results[i].notes);
        }
        printf("\n  Results from this machine may not be fairly comparable to\n");
        printf("  pure-CPU results from platforms without these coprocessors.\n\n");
    }
    else
    {
        printf("  [OK] No coprocessor acceleration detected - results are fair\n\n");
    }

    /* Composite score */
    double composite = 0.0;
    int result_count = completed_result_count(o);
    for (int i = 0; i < o->count; i++)
    {
        if (!o->results[i].module_name)
            continue;
        composite += o->results[i].score;
    }
    composite /= (result_count > 0 ? result_count : 1);

    printf("  Composite score  : %.2f\n", composite);
    printf("  Platform         : %s %s\n",
           o->platform.cpu_brand, o->platform.arch);
    printf("  Cores            : %d logical\n", o->platform.logical_cores);
    printf("%s\n", "-------------------------------------------------------");
}

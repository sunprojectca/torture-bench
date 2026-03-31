/*
 * pipeline_torture.c
 * Stresses the CPU instruction pipeline across all cores:
 *   Phase 1: ILP burst — 16 independent accumulator chains saturate
 *            execution ports (fetch/decode/execute/retire bandwidth)
 *   Phase 2: Functional unit contention — interleaved int mul, FP mul,
 *            int div, FP div, sqrt, transcendentals hammer all ports
 *   Phase 3: I-cache thrash — large dispatch table with 64 unique
 *            code blocks defeats instruction cache and prefetcher
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

/* ── Phase 1: ILP burst — 16 independent chains ─────────────────────────── */
static NOINLINE uint64_t phase_ilp(uint64_t seed, double duration)
{
    uint64_t a0 = seed,      a1 = seed ^ 1,  a2 = seed ^ 2,  a3 = seed ^ 3;
    uint64_t a4 = seed ^ 4,  a5 = seed ^ 5,  a6 = seed ^ 6,  a7 = seed ^ 7;
    uint64_t a8 = seed ^ 8,  a9 = seed ^ 9,  aA = seed ^ 10, aB = seed ^ 11;
    uint64_t aC = seed ^ 12, aD = seed ^ 13, aE = seed ^ 14, aF = seed ^ 15;

    const uint64_t k1 = 0x9E3779B97F4A7C15ULL;
    const uint64_t k2 = 0x6C62272E07BB0142ULL;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* All 16 multiplies are completely independent — CPU should
           dispatch them to all available integer multiply ports */
        a0 = a0 * k1 + k2;  a1 = a1 * k1 + k2;
        a2 = a2 * k1 + k2;  a3 = a3 * k1 + k2;
        a4 = a4 * k1 + k2;  a5 = a5 * k1 + k2;
        a6 = a6 * k1 + k2;  a7 = a7 * k1 + k2;
        a8 = a8 * k1 + k2;  a9 = a9 * k1 + k2;
        aA = aA * k1 + k2;  aB = aB * k1 + k2;
        aC = aC * k1 + k2;  aD = aD * k1 + k2;
        aE = aE * k1 + k2;  aF = aF * k1 + k2;
        iters += 16;
    }
    return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7 ^
           a8 ^ a9 ^ aA ^ aB ^ aC ^ aD ^ aE ^ aF ^ iters;
}

/* ── Phase 2: Functional unit contention ─────────────────────────────────── */
static NOINLINE uint64_t phase_fu_mix(uint64_t seed, double duration)
{
    uint64_t iacc = seed;
    uint64_t rng  = seed ^ 0xCAFEBABEDEADFEEDULL;
    double   facc = (double)(seed & 0xFFFF) + 1.0;
    double   gacc = 3.14159265;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* Integer multiply — ALU port 0/1 */
        iacc = iacc * 6364136223846793005ULL + xorshift64(&rng);
        /* FP multiply-add — FP port */
        facc = facc * 1.00000017 + 0.5;
        /* Integer shift+xor — ALU */
        iacc ^= iacc >> 17;
        iacc += (iacc << 5) | (iacc >> 59);
        /* FP sqrt — slow FP port, long latency */
        gacc = sqrt(fabs(gacc * 1.0001 + facc * 0.001));
        /* Integer multiply chain */
        iacc = iacc * iacc + rng;
        /* FP division — very long latency FP operation */
        facc = facc / (fabs(gacc) + 1.0);
        /* Transcendental — microcoded, many micro-ops */
        gacc = gacc + sin(facc * 1e-8);
        /* Integer divide — slow integer operation */
        iacc = iacc / ((rng & 0xFF) | 1) + iacc;
        /* More FP to keep ports busy */
        facc = facc * gacc + cos(gacc * 1e-9);
        gacc = exp(fabs(facc) * 1e-15 + 0.001) * gacc;
        /* Integer bit manipulation */
        iacc ^= (iacc >> 31) | (iacc << 33);
        iacc += xorshift64(&rng) * 7;
        iters += 13;
    }
    return iacc ^ (uint64_t)(facc * 1e12) ^ (uint64_t)(gacc * 1e12) ^ iters;
}

/* ── Phase 3: I-cache thrash via large dispatch table ────────────────────── */
/*
 * 64 unique code blocks selected randomly. Each block is NOINLINE and does
 * distinct arithmetic, so the compiler can't merge them. The random dispatch
 * pattern defeats I-cache prefetching.
 */
#define IC_BODY(N) \
    x = x * (0x9E3779B97F4A7C15ULL + (uint64_t)(N)*17) + y; \
    y ^= x >> (7 + (N) % 11); \
    x += y * ((uint64_t)(N)*31 + 1); \
    y = (y << ((N) % 13 + 1)) | (y >> (63 - (N) % 13)); \
    x -= y ^ ((uint64_t)(N)*0xDEADBEEFULL); \
    y *= x | 1; \
    x ^= y >> ((N) % 17 + 3); \
    y += x * ((uint64_t)(N)*7 + 3); \
    x = (x << ((N) % 11 + 2)) | (x >> (62 - (N) % 11)); \
    y ^= x + (uint64_t)(N)*0xCAFEBABEULL; \
    x *= y + (uint64_t)(N)*0x13371337ULL; \
    y -= x >> ((N) % 15 + 1); \
    x += y * ((uint64_t)(N)*13 + 7); \
    y ^= (x << ((N) % 9 + 4)) | (x >> (60 - (N) % 9)); \
    x -= y * ((uint64_t)(N)*19 + 11);

static NOINLINE uint64_t icache_stress(uint64_t x, uint64_t y,
                                        uint64_t *rng, double duration)
{
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        int which = (int)(xorshift64(rng) & 63);
        switch (which) {
        case 0:  IC_BODY(0)  break; case 1:  IC_BODY(1)  break;
        case 2:  IC_BODY(2)  break; case 3:  IC_BODY(3)  break;
        case 4:  IC_BODY(4)  break; case 5:  IC_BODY(5)  break;
        case 6:  IC_BODY(6)  break; case 7:  IC_BODY(7)  break;
        case 8:  IC_BODY(8)  break; case 9:  IC_BODY(9)  break;
        case 10: IC_BODY(10) break; case 11: IC_BODY(11) break;
        case 12: IC_BODY(12) break; case 13: IC_BODY(13) break;
        case 14: IC_BODY(14) break; case 15: IC_BODY(15) break;
        case 16: IC_BODY(16) break; case 17: IC_BODY(17) break;
        case 18: IC_BODY(18) break; case 19: IC_BODY(19) break;
        case 20: IC_BODY(20) break; case 21: IC_BODY(21) break;
        case 22: IC_BODY(22) break; case 23: IC_BODY(23) break;
        case 24: IC_BODY(24) break; case 25: IC_BODY(25) break;
        case 26: IC_BODY(26) break; case 27: IC_BODY(27) break;
        case 28: IC_BODY(28) break; case 29: IC_BODY(29) break;
        case 30: IC_BODY(30) break; case 31: IC_BODY(31) break;
        case 32: IC_BODY(32) break; case 33: IC_BODY(33) break;
        case 34: IC_BODY(34) break; case 35: IC_BODY(35) break;
        case 36: IC_BODY(36) break; case 37: IC_BODY(37) break;
        case 38: IC_BODY(38) break; case 39: IC_BODY(39) break;
        case 40: IC_BODY(40) break; case 41: IC_BODY(41) break;
        case 42: IC_BODY(42) break; case 43: IC_BODY(43) break;
        case 44: IC_BODY(44) break; case 45: IC_BODY(45) break;
        case 46: IC_BODY(46) break; case 47: IC_BODY(47) break;
        case 48: IC_BODY(48) break; case 49: IC_BODY(49) break;
        case 50: IC_BODY(50) break; case 51: IC_BODY(51) break;
        case 52: IC_BODY(52) break; case 53: IC_BODY(53) break;
        case 54: IC_BODY(54) break; case 55: IC_BODY(55) break;
        case 56: IC_BODY(56) break; case 57: IC_BODY(57) break;
        case 58: IC_BODY(58) break; case 59: IC_BODY(59) break;
        case 60: IC_BODY(60) break; case 61: IC_BODY(61) break;
        case 62: IC_BODY(62) break; case 63: IC_BODY(63) break;
        }
        iters++;
    }
    return x ^ y ^ iters;
}

/* ── thread worker ───────────────────────────────────────────────────────── */
static void *pipeline_worker(void *arg)
{
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    double sub = (double)a->duration_sec / 3.0;
    uint64_t rng = a->seed;
    uint64_t acc = a->seed;
    uint64_t iters = 0;

    /* Phase 1: ILP burst */
    double t0 = bench_now_sec();
    acc ^= phase_ilp(a->seed, sub);

    /* Phase 2: Functional unit contention */
    acc ^= phase_fu_mix(acc, sub);

    /* Phase 3: I-cache thrash */
    acc ^= icache_stress(acc, acc ^ 0xFF, &rng, sub);

    double elapsed = bench_now_sec() - t0;
    a->ops = (double)a->duration_sec / (elapsed > 0 ? elapsed : 1.0)
             * 1e6; /* normalized ops */
    a->hash_out = mix64(a->seed, acc);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_pipeline_torture(uint64_t chain_seed,
                                       int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "pipeline_torture";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec,
                 pipeline_worker, &total_ops, &combined_hash);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_ops;
    r.score = total_ops / 1e6;
    r.chain_out = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags),
             "ILP16+FU_MIX+ICACHE64 ncores=%d", ncores);
    return r;
}

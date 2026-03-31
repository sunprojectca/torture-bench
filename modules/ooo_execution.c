/*
 * ooo_execution.c
 * Stresses the out-of-order execution engine on all cores:
 *   Phase 1: Chain scaling — measures throughput with 1, 4, and 16
 *            independent chains to reveal OOO width and ROB depth
 *   Phase 2: Reorder buffer (ROB) pressure — maximum instructions
 *            in flight via interleaved independent operations
 *   Phase 3: Load/store buffer saturation — scattered memory accesses
 *            that fill the MOB (memory order buffer)
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Phase 1a: single serial chain (baseline, latency-bound) ─────────── */
static uint64_t chain_serial(uint64_t seed, double duration)
{
    volatile uint64_t acc = seed;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* Every operation depends on the previous — no ILP possible */
        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
        acc ^= acc >> 17;
        acc = acc * 2862933555777941757ULL + acc;
        acc ^= acc >> 31;
        acc = acc * 7046029254386353131ULL + 1;
        acc ^= acc >> 11;
        acc *= acc | 1;
        acc ^= acc >> 23;
        iters += 8;
    }
    return (uint64_t)acc ^ iters;
}

/* ── Phase 1b: 4 independent chains (moderate ILP) ───────────────────── */
static uint64_t chain_x4(uint64_t seed, double duration)
{
    uint64_t a = seed, b = seed ^ 1, c = seed ^ 2, d = seed ^ 3;
    const uint64_t k = 6364136223846793005ULL;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        a = a * k + 1; a ^= a >> 17; a = a * k + a;
        b = b * k + 2; b ^= b >> 17; b = b * k + b;
        c = c * k + 3; c ^= c >> 17; c = c * k + c;
        d = d * k + 4; d ^= d >> 17; d = d * k + d;
        a = a * k + 5; a ^= a >> 13;
        b = b * k + 6; b ^= b >> 13;
        c = c * k + 7; c ^= c >> 13;
        d = d * k + 8; d ^= d >> 13;
        iters += 16;
    }
    return a ^ b ^ c ^ d ^ iters;
}

/* ── Phase 1c: 16 independent chains (maximum ILP) ──────────────────── */
static uint64_t chain_x16(uint64_t seed, double duration)
{
    uint64_t c0  = seed,      c1  = seed ^ 1,  c2  = seed ^ 2,  c3  = seed ^ 3;
    uint64_t c4  = seed ^ 4,  c5  = seed ^ 5,  c6  = seed ^ 6,  c7  = seed ^ 7;
    uint64_t c8  = seed ^ 8,  c9  = seed ^ 9,  c10 = seed ^ 10, c11 = seed ^ 11;
    uint64_t c12 = seed ^ 12, c13 = seed ^ 13, c14 = seed ^ 14, c15 = seed ^ 15;
    const uint64_t k = 6364136223846793005ULL;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        c0  = c0  * k + 1;  c1  = c1  * k + 2;
        c2  = c2  * k + 3;  c3  = c3  * k + 4;
        c4  = c4  * k + 5;  c5  = c5  * k + 6;
        c6  = c6  * k + 7;  c7  = c7  * k + 8;
        c8  = c8  * k + 9;  c9  = c9  * k + 10;
        c10 = c10 * k + 11; c11 = c11 * k + 12;
        c12 = c12 * k + 13; c13 = c13 * k + 14;
        c14 = c14 * k + 15; c15 = c15 * k + 16;
        c0  ^= c0  >> 17;   c1  ^= c1  >> 17;
        c2  ^= c2  >> 17;   c3  ^= c3  >> 17;
        c4  ^= c4  >> 17;   c5  ^= c5  >> 17;
        c6  ^= c6  >> 17;   c7  ^= c7  >> 17;
        c8  ^= c8  >> 17;   c9  ^= c9  >> 17;
        c10 ^= c10 >> 17;   c11 ^= c11 >> 17;
        c12 ^= c12 >> 17;   c13 ^= c13 >> 17;
        c14 ^= c14 >> 17;   c15 ^= c15 >> 17;
        iters += 32;
    }
    return c0^c1^c2^c3^c4^c5^c6^c7^c8^c9^c10^c11^c12^c13^c14^c15^iters;
}

/* ── Phase 2: ROB pressure — maximum independent ops in flight ───────── */
static uint64_t rob_pressure(uint64_t seed, double duration)
{
    /* 8 independent integer chains + 8 independent FP chains = 16 in-flight
       streams. Each chain does multiply, shift, XOR — different operation
       types to stress scheduler and physical register file */
    uint64_t i0 = seed, i1 = ~seed, i2 = seed*3, i3 = seed*5;
    uint64_t i4 = seed*7, i5 = seed*11, i6 = seed*13, i7 = seed*17;
    double   f0 = 1.1, f1 = 2.2, f2 = 3.3, f3 = 4.4;
    double   f4 = 5.5, f5 = 6.6, f6 = 7.7, f7 = 8.8;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* 8 independent integer chains */
        i0 = i0 * 0x123456789ULL + 1; i1 = i1 * 0x234567891ULL + 2;
        i2 = i2 * 0x345678912ULL + 3; i3 = i3 * 0x456789123ULL + 4;
        i4 = i4 * 0x567891234ULL + 5; i5 = i5 * 0x678912345ULL + 6;
        i6 = i6 * 0x789123456ULL + 7; i7 = i7 * 0x891234567ULL + 8;
        i0 ^= i0 >> 13; i1 ^= i1 >> 13; i2 ^= i2 >> 13; i3 ^= i3 >> 13;
        i4 ^= i4 >> 13; i5 ^= i5 >> 13; i6 ^= i6 >> 13; i7 ^= i7 >> 13;
        /* 8 independent FP chains (separate FP register file) */
        f0 = f0 * 1.0000001 + 0.01; f1 = f1 * 1.0000002 + 0.02;
        f2 = f2 * 1.0000003 + 0.03; f3 = f3 * 1.0000004 + 0.04;
        f4 = f4 * 1.0000005 + 0.05; f5 = f5 * 1.0000006 + 0.06;
        f6 = f6 * 1.0000007 + 0.07; f7 = f7 * 1.0000008 + 0.08;
        iters += 32;
    }
    return i0^i1^i2^i3^i4^i5^i6^i7 ^
           (uint64_t)(f0*1e12) ^ (uint64_t)(f1*1e12) ^
           (uint64_t)(f2*1e12) ^ (uint64_t)(f3*1e12) ^
           (uint64_t)(f4*1e12) ^ (uint64_t)(f5*1e12) ^
           (uint64_t)(f6*1e12) ^ (uint64_t)(f7*1e12) ^ iters;
}

/* ── Phase 3: Load/store buffer saturation ────────────────────────────── */
#define MOB_N (1 << 18)  /* 2MB of uint64_t = 2M entries */

static uint64_t mob_stress(uint64_t seed, double duration)
{
    uint64_t *buf = (uint64_t *)malloc(MOB_N * sizeof(uint64_t));
    if (!buf) return seed;

    uint64_t rng = seed;
    for (size_t i = 0; i < MOB_N; i++)
        buf[i] = xorshift64(&rng);

    uint64_t acc = seed;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* 8 scattered loads — stress load buffer (typically 64-72 entries) */
        size_t i0 = (size_t)(xorshift64(&rng) % MOB_N);
        size_t i1 = (size_t)(xorshift64(&rng) % MOB_N);
        size_t i2 = (size_t)(xorshift64(&rng) % MOB_N);
        size_t i3 = (size_t)(xorshift64(&rng) % MOB_N);
        acc ^= buf[i0] + buf[i1] + buf[i2] + buf[i3];

        /* 4 scattered stores — stress store buffer (typically 32-56 entries) */
        size_t s0 = (size_t)(xorshift64(&rng) % MOB_N);
        size_t s1 = (size_t)(xorshift64(&rng) % MOB_N);
        size_t s2 = (size_t)(xorshift64(&rng) % MOB_N);
        size_t s3 = (size_t)(xorshift64(&rng) % MOB_N);
        buf[s0] = acc ^ rng;
        buf[s1] = acc + rng;
        buf[s2] = acc - rng;
        buf[s3] = acc * (rng | 1);

        /* Interleaved load-after-store to stress disambiguator */
        acc += buf[s0] ^ buf[s2];

        iters += 10;
    }

    acc ^= buf[0] ^ buf[MOB_N / 2];
    free(buf);
    return acc ^ iters;
}

/* ── thread worker ───────────────────────────────────────────────────────── */
typedef struct {
    parallel_arg_t base;
    double rate_serial;
    double rate_x4;
    double rate_x16;
} ooo_worker_arg_t;

static void *ooo_worker(void *arg)
{
    ooo_worker_arg_t *a = (ooo_worker_arg_t *)arg;
    platform_pin_thread(a->base.core_id);

    double sub = (double)a->base.duration_sec / 6.0;
    uint64_t acc = a->base.seed;

    /* Phase 1: Chain scaling — measure throughput at 1, 4, 16 chains */
    double t0 = bench_now_sec();
    acc ^= chain_serial(acc, sub);
    a->rate_serial = sub / (bench_now_sec() - t0 + 1e-15);

    t0 = bench_now_sec();
    acc ^= chain_x4(acc, sub);
    a->rate_x4 = sub / (bench_now_sec() - t0 + 1e-15);

    t0 = bench_now_sec();
    acc ^= chain_x16(acc, sub);
    a->rate_x16 = sub / (bench_now_sec() - t0 + 1e-15);

    /* Phase 2: ROB pressure */
    acc ^= rob_pressure(acc, sub);

    /* Phase 3: Load/store buffer saturation */
    acc ^= mob_stress(acc, sub * 2);

    a->base.ops = 1e6; /* normalized */
    a->base.hash_out = mix64(a->base.seed, acc);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_ooo_execution(uint64_t chain_seed,
                                     int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "ooo_execution";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);

    ooo_worker_arg_t *args = (ooo_worker_arg_t *)malloc(
        (size_t)ncores * sizeof(ooo_worker_arg_t));
    bench_thread_t *tids = (bench_thread_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = (bench_thread_trampoline_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].base.seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].base.core_id = i;
        args[i].base.duration_sec = duration_sec;
        args[i].base.hash_out = 0;
        args[i].base.ops = 0;
        args[i].rate_serial = 0;
        args[i].rate_x4 = 0;
        args[i].rate_x16 = 0;
        bench_thread_create(&tids[i], &tramps[i], ooo_worker, &args[i]);
    }

    uint64_t hash = chain_seed;
    double avg_serial = 0, avg_x4 = 0, avg_x16 = 0;
    double total_ops = 0;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        hash ^= args[i].base.hash_out;
        total_ops += args[i].base.ops;
        avg_serial += args[i].rate_serial;
        avg_x4 += args[i].rate_x4;
        avg_x16 += args[i].rate_x16;
    }
    avg_serial /= ncores;
    avg_x4 /= ncores;
    avg_x16 /= ncores;

    double ilp_ratio = (avg_serial > 0) ? avg_x16 / avg_serial : 1.0;

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_ops;
    r.score = ilp_ratio * total_ops / 1e6;
    r.chain_out = mix64(chain_seed, hash);

    snprintf(r.flags, sizeof(r.flags),
             "ILP_RATIO=%.1fx ncores=%d", ilp_ratio, ncores);
    snprintf(r.notes, sizeof(r.notes),
             "chain_scaling x1=%.2f x4=%.2f x16=%.2f ROB+MOB_stress",
             avg_serial, avg_x4, avg_x16);

    free(args); free(tids); free(tramps);
    return r;
}

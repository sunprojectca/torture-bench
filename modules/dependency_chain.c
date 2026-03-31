/*
 * dependency_chain.c
 * Stresses the CPU dependency tracking hardware on all cores:
 *   Phase 1: Serial integer multiply chain (latency-bound,
 *            tests scoreboard/bypass network)
 *   Phase 2: Serial FP multiply-add chain (longer FP latency,
 *            stresses FP forwarding)
 *   Phase 3: Diamond dependency patterns (fork-join: A->B, A->C, B+C->D,
 *            tests rename/merge capability)
 *   Phase 4: Memory dependency chain (store->load forwarding,
 *            stresses memory order buffer and disambiguator)
 *   Phase 5: Register pressure explosion (32 simultaneously live values
 *            in a dependency cycle, stresses physical register file)
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── Phase 1: Serial integer dependency chain ────────────────────────────── */
static uint64_t dep_serial_int(uint64_t seed, double duration)
{
    /* Every operation depends strictly on the previous result.
       CPU cannot execute any op until the prior one retires.
       This measures integer multiply + shift latency through the bypass. */
    volatile uint64_t x = seed;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        x = x * 6364136223846793005ULL + 1;
        x ^= x >> 17;
        x = x * 2862933555777941757ULL + x;
        x ^= x >> 31;
        x = x * 7046029254386353131ULL + 1;
        x ^= x >> 11;
        x = x * 1103515245ULL + 12345;
        x ^= x >> 23;
        x = (x << 13) | (x >> 51);
        x *= x | 1;
        x = (x << 7) | (x >> 57);
        x ^= x >> 5;
        iters += 12;
    }
    return (uint64_t)x ^ iters;
}

/* ── Phase 2: Serial FP dependency chain ─────────────────────────────────── */
static uint64_t dep_serial_fp(uint64_t seed, double duration)
{
    /* FP multiply has longer latency than integer (typically 4-5 cycles
       vs 3 for integer). Each FP op depends on the previous. */
    volatile double x = (double)(seed & 0xFFFFFF) / 1e6 + 1.0;
    volatile double y = (double)((seed >> 24) & 0xFFFFFF) / 1e6 + 1.0;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        x = x * 1.0000000017 + y * 0.001;
        y = y * 0.9999999983 + x * 0.001;
        x = x * x - y * 0.5 + 1.0;
        y = sqrt(fabs(y * 2.0 + x * 0.1)) + 0.001;
        x = sin(x * 1e-10) * 1e10 + y;
        y = cos(y * 1e-10) * 1e10 + x * 0.9;
        x = (x + y) * 0.5;
        y = (x - y) * 0.5 + 1.0;
        x = exp(fabs(x) * 1e-15) + y * 0.001;
        y = log(fabs(y) + 1.0) + x * 0.001;
        iters += 10;
    }
    return (uint64_t)(x * 1e12) ^ (uint64_t)(y * 1e12) ^ iters;
}

/* ── Phase 3: Diamond dependency pattern ─────────────────────────────────── */
static uint64_t dep_diamond(uint64_t seed, double duration)
{
    /*  Diamond pattern per iteration:
     *       a
     *      / \
     *     b   c    (b and c are independent, both depend on a)
     *      \ /
     *       d      (d depends on both b and c — merge point)
     *       |
     *       a'     (next iteration)
     *
     *  The CPU must track that b and c can execute in parallel,
     *  but d must wait for both. Tests rename tracking and merge. */
    volatile uint64_t a = seed;
    uint64_t rng = seed ^ 0xA5A5A5A5A5A5A5A5ULL;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* 4 diamond patterns per iteration for throughput */
        uint64_t b, c, d;

        /* Diamond 1 */
        b = a * 6364136223846793005ULL + 1;
        c = a ^ (a << 17) ^ (a >> 7);
        d = b * c + (b ^ c);
        a = d ^ (d >> 13);

        /* Diamond 2 (more complex ops) */
        b = a * 2862933555777941757ULL - 3;
        c = (a << 23) | (a >> 41);
        c *= c + 1;
        d = (b ^ c) * (b + c);
        a = d ^ (d >> 29);

        /* Diamond 3 (triple fork) */
        b = a * 7046029254386353131ULL;
        c = a ^ xorshift64(&rng);
        d = b + c;
        uint64_t e = b ^ c;
        a = d * e + (d ^ e);

        /* Diamond 4 (deep fork) */
        b = a * 1103515245ULL + 12345;
        c = a ^ (a >> 33);
        uint64_t f = b * 3 + c;
        uint64_t g = b ^ (c << 5);
        d = f ^ g;
        e = f + g;
        a = d * e;

        iters += 4;
    }
    return (uint64_t)a ^ iters;
}

/* ── Phase 4: Memory dependency chain (store-load forwarding) ────────────── */
#define MEMDEP_N 4096

static uint64_t dep_memory(uint64_t seed, double duration)
{
    /* Store → Load → Compute → Store chain.
       Each load depends on a previous store to the SAME address.
       This tests store-load forwarding latency and memory disambiguator.
       Random index selection makes hardware prediction harder. */
    uint64_t *buf = (uint64_t *)malloc(MEMDEP_N * sizeof(uint64_t));
    if (!buf) return seed;

    uint64_t rng = seed;
    for (size_t i = 0; i < MEMDEP_N; i++)
        buf[i] = xorshift64(&rng);

    volatile uint64_t acc = seed;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* Chain: compute index → store → load → compute → store → load */
        size_t i0 = (size_t)((uint64_t)acc % MEMDEP_N);
        buf[i0] = (uint64_t)acc * 0x9E3779B97F4A7C15ULL;  /* store */
        acc = buf[i0];                                       /* load (forwarded) */

        size_t i1 = (size_t)((uint64_t)acc % MEMDEP_N);
        buf[i1] = (uint64_t)acc ^ ((uint64_t)acc >> 17);   /* store */
        acc = buf[i1] + buf[i0];                             /* load (may alias) */

        size_t i2 = (size_t)((uint64_t)acc % MEMDEP_N);
        buf[i2] = (uint64_t)acc * ((uint64_t)acc | 1);     /* store */
        acc = buf[i2] ^ buf[i1];                             /* load chain */

        /* Cross-talk: store to one index, load from another that was
           recently stored — tests memory disambiguator speculation */
        size_t i3 = (size_t)(xorshift64(&rng) % MEMDEP_N);
        buf[i3] = (uint64_t)acc;
        acc = buf[(size_t)((uint64_t)acc % MEMDEP_N)] + buf[i3];

        iters += 8;
    }

    free(buf);
    return (uint64_t)acc ^ iters;
}

/* ── Phase 5: Register pressure explosion ────────────────────────────────── */
static uint64_t dep_regpressure(uint64_t seed, double duration)
{
    /* 32 simultaneously live values in a circular dependency chain.
       r[i] depends on r[i-1] and r[i+16] (mod 32).
       The CPU's physical register file must hold all 32 values
       simultaneously — if it runs out, execution stalls.
       x86-64 has ~180 physical integer registers, ARM ~128. */
    uint64_t r[32];
    for (int i = 0; i < 32; i++)
        r[i] = seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);

    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* Full rotation: each r[i] depends on r[(i-1)%32] and r[(i+16)%32]
           creating a web of dependencies that keeps all 32 values live */
        r[0]  = r[0]  * r[31] + r[16]; r[1]  = r[1]  * r[0]  + r[17];
        r[2]  = r[2]  * r[1]  + r[18]; r[3]  = r[3]  * r[2]  + r[19];
        r[4]  = r[4]  * r[3]  + r[20]; r[5]  = r[5]  * r[4]  + r[21];
        r[6]  = r[6]  * r[5]  + r[22]; r[7]  = r[7]  * r[6]  + r[23];
        r[8]  = r[8]  * r[7]  + r[24]; r[9]  = r[9]  * r[8]  + r[25];
        r[10] = r[10] * r[9]  + r[26]; r[11] = r[11] * r[10] + r[27];
        r[12] = r[12] * r[11] + r[28]; r[13] = r[13] * r[12] + r[29];
        r[14] = r[14] * r[13] + r[30]; r[15] = r[15] * r[14] + r[31];
        r[16] = r[16] * r[15] + r[0];  r[17] = r[17] * r[16] + r[1];
        r[18] = r[18] * r[17] + r[2];  r[19] = r[19] * r[18] + r[3];
        r[20] = r[20] * r[19] + r[4];  r[21] = r[21] * r[20] + r[5];
        r[22] = r[22] * r[21] + r[6];  r[23] = r[23] * r[22] + r[7];
        r[24] = r[24] * r[23] + r[8];  r[25] = r[25] * r[24] + r[9];
        r[26] = r[26] * r[25] + r[10]; r[27] = r[27] * r[26] + r[11];
        r[28] = r[28] * r[27] + r[12]; r[29] = r[29] * r[28] + r[13];
        r[30] = r[30] * r[29] + r[14]; r[31] = r[31] * r[30] + r[15];
        iters += 32;
    }

    uint64_t acc = 0;
    for (int i = 0; i < 32; i++) acc ^= r[i];
    return acc ^ iters;
}

/* ── thread worker ───────────────────────────────────────────────────────── */
static void *dep_worker(void *arg)
{
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    double sub = (double)a->duration_sec / 5.0;
    uint64_t acc = a->seed;

    acc ^= dep_serial_int(acc, sub);
    acc ^= dep_serial_fp(acc, sub);
    acc ^= dep_diamond(acc, sub);
    acc ^= dep_memory(acc, sub);
    acc ^= dep_regpressure(acc, sub);

    a->ops = 1e6;
    a->hash_out = mix64(a->seed, acc);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_dependency_chain(uint64_t chain_seed,
                                        int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "dependency_chain";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec,
                 dep_worker, &total_ops, &combined_hash);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_ops;
    r.score = total_ops / 1e6;
    r.chain_out = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags),
             "SERIAL_INT+FP+DIAMOND+MEMDEP+REGPRESSURE ncores=%d", ncores);
    snprintf(r.notes, sizeof(r.notes),
             "5-phase dependency tracking stress test");
    return r;
}

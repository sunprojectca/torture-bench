/*
 * ips_micro.c
 * Micro-benchmarks that measure raw IPS and specific CPU characteristics:
 *   - Integer IPS (add, mul, div)
 *   - Float IPS (fadd, fmul, fdiv, sqrt, sin)
 *   - Branch predictor stress
 *   - Memory latency (pointer chase)
 *   - TLB pressure
 *   - Store-load forwarding latency
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ── integer IPS ─────────────────────────────────────────────────────────── */
static double measure_int_ips(double duration_sec)
{
    volatile uint64_t a = 0x123456789ABCDEFULL;
    volatile uint64_t b = 0xFEDCBA987654321ULL;
    volatile uint64_t c = 0;
    uint64_t rnd = 0xA1B2C3D4E5F60718ULL;
    uint64_t iters = 0;
    double t0 = bench_now_sec();
    double deadline = t0 + duration_sec;
    while (bench_now_sec() < deadline)
    {
        uint64_t rv = xorshift64(&rnd);
        /* 8 ops per iteration to reduce loop overhead ratio */
        c = a * b + c + (rv & 0xFF);
        a = c ^ (a << 13);
        b = a + b * 7;
        c = b ^ (c >> 17);
        a = c * 31 + a;
        b = a ^ (b + 99 + (rv >> 8 & 0x3F));
        c = b * a ^ c;
        a = (c + b) ^ a;
        iters += 8;
    }
    (void)a;
    (void)b;
    (void)c; /* prevent DCE */
    return (double)iters / (bench_now_sec() - t0);
}

/* ── float IPS ───────────────────────────────────────────────────────────── */
static double measure_float_ips(double duration_sec)
{
    volatile double a = 1.000001, b = 2.000003, c = 0.0;
    uint64_t rnd = 0x1020304050607080ULL;
    uint64_t iters = 0;
    double t0 = bench_now_sec();
    double deadline = t0 + duration_sec;
    while (bench_now_sec() < deadline)
    {
        double jitter = (double)(xorshift64(&rnd) & 0xFF) * 1e-9;
        c = a * b + c;
        a = c - a * (0.9999999 + jitter);
        b = sqrt(fabs(a + b));
        c = sin(a) * cos(b);
        a = a + b * 1.000001;
        b = c / (fabs(a) + 1.0);
        c = a * a - b * b;
        a = fabs(c) + 1e-10;
        iters += 8;
    }
    (void)a;
    (void)b;
    (void)c;
    return (double)iters / (bench_now_sec() - t0);
}

/* ── branch predictor stress ─────────────────────────────────────────────── */
static double measure_branch_ips(double duration_sec, uint64_t seed)
{
    /* Pattern: unpredictable branches based on xorshift output */
    uint64_t rnd = seed;
    volatile uint64_t acc = 0;
    uint64_t iters = 0;
    double t0 = bench_now_sec();
    double deadline = t0 + duration_sec;
    while (bench_now_sec() < deadline)
    {
        uint64_t r = xorshift64(&rnd);
        /* 8 unpredictable branches */
        if (r & (1ULL << 0))
            acc += r;
        if (r & (1ULL << 7))
            acc ^= r;
        if (r & (1ULL << 13))
            acc -= r;
        if (r & (1ULL << 19))
            acc += r >> 3;
        if (r & (1ULL << 23))
            acc ^= r << 5;
        if (r & (1ULL << 31))
            acc += 1;
        if (r & (1ULL << 41))
            acc *= 3;
        if (r & (1ULL << 53))
            acc ^= acc >> 7;
        iters += 8;
    }
    (void)acc;
    return (double)iters / (bench_now_sec() - t0);
}

/* ── memory latency: pointer chase ──────────────────────────────────────── */
#define CHASE_N (1 << 20) /* 8MB — larger than typical L2 */

/* ── TLB pressure: stride > page size ───────────────────────────────────── */
#define TLB_PAGES 512
#define PAGE_SIZE 4096

/* ── per-thread IPS worker ────────────────────────────────────────────────── */
typedef struct {
    uint64_t seed;
    int      core_id;
    int      duration_sec;
    uint64_t hash_out;
    double   int_ips;
    double   float_ips;
    double   branch_ips;
    double   lat_ns;
    double   tlb_ns;
} ips_arg_t;

static void *ips_worker(void *varg) {
    ips_arg_t *a = (ips_arg_t *)varg;
    double sub = (double)a->duration_sec / 6.0;

    /* Init per-thread pointer chase buffer */
    size_t *my_chase = malloc(CHASE_N * sizeof(size_t));
    for (size_t i = 0; i < CHASE_N; i++) my_chase[i] = i;
    uint64_t rng = a->seed;
    for (size_t i = CHASE_N - 1; i > 0; i--) {
        size_t j = (size_t)(xorshift64(&rng) % (i + 1));
        size_t t = my_chase[i]; my_chase[i] = my_chase[j]; my_chase[j] = t;
    }

    /* Per-thread TLB buffer */
    uint8_t *my_tlb = malloc((size_t)TLB_PAGES * PAGE_SIZE);

    a->int_ips   = measure_int_ips(sub);
    a->float_ips = measure_float_ips(sub);
    a->branch_ips = measure_branch_ips(sub, a->seed);

    /* Pointer-chase latency with per-thread buffer */
    {
        volatile size_t idx = 0;
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        double deadline = t0 + sub;
        while (bench_now_sec() < deadline) {
            idx = my_chase[idx]; idx = my_chase[idx];
            idx = my_chase[idx]; idx = my_chase[idx];
            idx = my_chase[idx]; idx = my_chase[idx];
            idx = my_chase[idx]; idx = my_chase[idx];
            iters += 8;
        }
        a->lat_ns = ((bench_now_sec() - t0) / iters) * 1e9;
    }

    /* TLB pressure with per-thread buffer */
    if (my_tlb) {
        volatile uint8_t acc = 0;
        uint64_t tlb_rnd = 0x55AA55AA55AA55AAULL ^ a->seed;
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        double deadline = t0 + sub;
        while (bench_now_sec() < deadline) {
            for (int i = 0; i < TLB_PAGES; i++)
                acc ^= my_tlb[i * PAGE_SIZE];
            acc ^= (uint8_t)xorshift64(&tlb_rnd);
            iters += TLB_PAGES;
        }
        (void)acc;
        a->tlb_ns = ((bench_now_sec() - t0) / iters) * 1e9;
    }

    a->hash_out = mix64(a->seed,
        (uint64_t)(a->int_ips + a->float_ips + a->branch_ips));

    free(my_chase);
    free(my_tlb);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_ips_micro(uint64_t chain_seed,
                                int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "ips_micro";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    ips_arg_t *args = calloc(ncores, sizeof(ips_arg_t));
    bench_thread_t *tids = malloc(ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = malloc(ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].core_id = i;
        args[i].duration_sec = duration_sec;
        bench_thread_create(&tids[i], &tramps[i], ips_worker, &args[i]);
    }

    double total_int = 0, total_float = 0, total_branch = 0;
    double avg_lat = 0, avg_tlb = 0;
    uint64_t hash = chain_seed;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        total_int   += args[i].int_ips;
        total_float += args[i].float_ips;
        total_branch += args[i].branch_ips;
        avg_lat += args[i].lat_ns;
        avg_tlb += args[i].tlb_ns;
        hash ^= args[i].hash_out;
    }
    avg_lat /= ncores;
    avg_tlb /= ncores;

    r.wall_time_sec = (double)duration_sec;
    r.ops_per_sec = total_int;
    r.score = (total_int + total_float + total_branch) / 3.0 / 1e9;
    r.chain_out = mix64(chain_seed, hash);

    snprintf(r.flags, sizeof(r.flags),
             "%dT int=%.2fG float=%.2fG branch=%.2fG lat=%.1fns tlb=%.1fns",
             ncores, total_int / 1e9, total_float / 1e9, total_branch / 1e9,
             avg_lat, avg_tlb);

    snprintf(r.notes, sizeof(r.notes),
             "mem_latency_ns=%.1f tlb_miss_ns=%.1f lat_verdict=%s",
             avg_lat, avg_tlb,
             avg_lat < 5.0 ? "SUSPICIOUS_FAST(<5ns)"
                           : avg_lat < 20.0 ? "L1/L2_HIT"
                           : avg_lat < 80.0 ? "L3_HIT"
                                            : "DRAM");

    if (avg_lat < 5.0) {
        r.coprocessor_suspected = 1;
        strncat(r.notes, " WARN:cache_prefetch_suspected",
                sizeof(r.notes) - strlen(r.notes) - 1);
    }

    free(args); free(tids); free(tramps);
    return r;
}

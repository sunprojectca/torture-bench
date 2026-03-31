/* memory_latency.c — pointer-chase latency (all cores) */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LATENCY_N (1 << 22)

static void *memlat_worker(void *arg) {
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    size_t *buf = malloc(LATENCY_N * sizeof(size_t));
    if (!buf) return NULL;

    for (size_t i = 0; i < LATENCY_N; i++) buf[i] = i;
    uint64_t rng = a->seed;
    for (size_t i = LATENCY_N-1; i > 0; i--) {
        size_t j = (size_t)(xorshift64(&rng) % (i+1));
        size_t t = buf[i]; buf[i] = buf[j]; buf[j] = t;
    }

    volatile size_t idx = 0;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + a->duration_sec;
    while (bench_now_sec() < deadline) {
        idx = buf[idx]; idx = buf[idx];
        idx = buf[idx]; idx = buf[idx];
        iters += 4;
    }

    a->ops = (double)iters;
    a->hash_out = mix64(a->seed, (uint64_t)idx);
    free(buf);
    return NULL;
}

bench_result_t module_memory_latency(uint64_t chain_seed,
                                      int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "memory_latency";
    r.chain_in    = chain_seed;

    int nthreads = resolve_threads(thread_count);
    double total_ops = 0.0;
    uint64_t combined_hash = chain_seed;
    parallel_run(nthreads, chain_seed, duration_sec, memlat_worker, &total_ops, &combined_hash);

    double elapsed = (double)duration_sec;
    double per_core_ops = total_ops / nthreads;
    double lat_ns = (elapsed / per_core_ops) * 1e9;

    r.wall_time_sec = elapsed;
    r.ops_per_sec   = total_ops / elapsed;
    r.score         = 1000.0 / lat_ns;
    r.chain_out     = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags),
        "lat=%.1fns %s %dT", lat_ns,
        lat_ns < 10 ? "SUSPICIOUS" : lat_ns < 60 ? "L3_HIT" : "DRAM", nthreads);

    if (lat_ns < 5.0) r.coprocessor_suspected = 1;
    return r;
}

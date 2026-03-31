/* memory_bandwidth.c — STREAM-style memory bandwidth test (all cores) */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MEM_N (1 << 22) /* 4M doubles = 32MB per array per thread */

static void *membw_worker(void *arg) {
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    double *aa = malloc(MEM_N * sizeof(double));
    double *bb = malloc(MEM_N * sizeof(double));
    double *cc = malloc(MEM_N * sizeof(double));
    if (!aa || !bb || !cc) { free(aa); free(bb); free(cc); return NULL; }

    for (size_t i = 0; i < MEM_N; i++) { aa[i] = 1.0; bb[i] = 2.0; cc[i] = 0.0; }

    double scalar = 3.14159;
    uint64_t rnd = a->seed;
    uint64_t acc = a->seed;
    double bytes = 0.0;
    double deadline = bench_now_sec() + a->duration_sec;

    while (bench_now_sec() < deadline) {
        for (size_t i = 0; i < MEM_N; i++)
            cc[i] = aa[i] + scalar * bb[i];
        bytes += MEM_N * 3 * sizeof(double);
        size_t si = (size_t)(xorshift64(&rnd) % MEM_N);
        acc ^= (uint64_t)(cc[si] * 1e10) ^ rnd;
    }

    a->ops = bytes;
    a->hash_out = acc;
    free(aa); free(bb); free(cc);
    return NULL;
}

bench_result_t module_memory_bandwidth(uint64_t chain_seed,
                                       int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "memory_bandwidth";
    r.chain_in = chain_seed;

    int nthreads = resolve_threads(thread_count);
    double total_ops = 0.0;
    uint64_t combined_hash = chain_seed;
    parallel_run(nthreads, chain_seed, duration_sec, membw_worker, &total_ops, &combined_hash);

    double gb_s = total_ops / (double)duration_sec / (1024.0 * 1024.0 * 1024.0);
    r.wall_time_sec = duration_sec;
    r.ops_per_sec = gb_s * 1e9;
    r.score = gb_s;
    r.chain_out = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags), "STREAM_TRIAD %.2f GB/s %dT", gb_s, nthreads);
    return r;
}

/*
 * cpu_single.c — Single-threaded CPU integer + float torture
 * Uses the same worker path as cpu_parallel with exactly one thread.
 */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include "cpu_shared_path.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bench_result_t module_cpu_single(uint64_t chain_seed,
                                 int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "cpu_single";
    r.chain_in = chain_seed;
    int nthreads = resolve_threads(thread_count);
    double total_ops = 0.0;
    uint64_t combined_hash = chain_seed;
    parallel_run(nthreads, chain_seed, duration_sec, cpu_shared_worker, &total_ops, &combined_hash);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_ops;
    r.score = total_ops / 1e6;
    r.chain_out = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags), "THREADS=%d shared_cpu_path", nthreads);
    return r;
}

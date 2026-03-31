/*
 * cpu_parallel.c — All-core parallel torture.
 * Uses the same worker path as cpu_single, only with N threads.
 */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include "cpu_shared_path.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bench_result_t module_cpu_parallel(uint64_t chain_seed,
                                   int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "cpu_parallel";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops = 0.0;
    uint64_t chain_acc = chain_seed;
    parallel_run(ncores, chain_seed, duration_sec, cpu_shared_worker, &total_ops, &chain_acc);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_ops;
    r.score = total_ops / 1e6;
    r.chain_out = mix64(chain_seed, chain_acc);
    snprintf(r.flags, sizeof(r.flags), "THREADS=%d shared_cpu_path", ncores);
    return r;
}

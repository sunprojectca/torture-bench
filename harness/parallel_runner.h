/*
 * parallel_runner.h — Generic parallel execution helper for benchmark modules.
 *
 * Provides a simple pattern to run any worker function on all CPU cores.
 * Each worker gets a unique seed, core pinning, and reports ops + hash.
 */
#pragma once
#ifndef PARALLEL_RUNNER_H
#define PARALLEL_RUNNER_H

#include "bench_thread.h"
#include "platform.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

/* Per-thread context passed to worker functions */
typedef struct {
    uint64_t seed;          /* unique seed for this thread               */
    int      core_id;       /* which core to pin to                      */
    int      duration_sec;  /* how long to run                           */
    uint64_t hash_out;      /* output: hash contribution                 */
    double   ops;           /* output: operations completed              */
} parallel_arg_t;

/* Worker function signature — same as pthreads */
typedef void *(*parallel_worker_fn)(void *);

/*
 * Run `worker` on `ncores` threads pinned to separate cores.
 * Fills total_ops and combined_hash from all workers.
 */
static inline void parallel_run(
    int ncores,
    uint64_t base_seed,
    int duration_sec,
    parallel_worker_fn worker,
    double *total_ops,
    uint64_t *combined_hash)
{
    parallel_arg_t *args = (parallel_arg_t *)malloc(
        (size_t)ncores * sizeof(parallel_arg_t));
    bench_thread_t *tids = (bench_thread_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = (bench_thread_trampoline_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].seed         = base_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].core_id      = i;
        args[i].duration_sec = duration_sec;
        args[i].hash_out     = 0;
        args[i].ops          = 0.0;
        bench_thread_create(&tids[i], &tramps[i], worker, &args[i]);
    }

    double ops_sum = 0.0;
    uint64_t hash = base_seed;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        ops_sum += args[i].ops;
        hash ^= args[i].hash_out;
    }

    *total_ops = ops_sum;
    *combined_hash = hash;

    free(args);
    free(tids);
    free(tramps);
}

/* Convenience: resolve thread count (0 = all cores) */
static inline int resolve_threads(int thread_count) {
    return (thread_count > 0) ? thread_count : platform_get_ncpus();
}

#endif /* PARALLEL_RUNNER_H */

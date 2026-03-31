/*
 * cpu_shared_path.h — shared single-thread and multi-thread CPU torture path.
 *
 * Both cpu_single and cpu_parallel use this exact worker so comparisons are
 * apples-to-apples; only thread count changes.
 */
#pragma once

#include "../harness/parallel_runner.h"
#include <math.h>

static inline void *cpu_shared_worker(void *arg)
{
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    uint64_t rnd = a->seed ^ ((uint64_t)a->core_id * 0xDEADBEEF00000001ULL);
    volatile uint64_t acc = rnd;
    volatile double facc = (double)a->core_id + 1.0;
    uint64_t iters = 0;

    double t0 = bench_now_sec();
    double deadline = t0 + a->duration_sec;

    while (bench_now_sec() < deadline)
    {
        uint64_t r1 = xorshift64(&rnd);
        uint64_t r2 = xorshift64(&rnd);
        acc = acc * 6364136223846793005ULL + r1;
        acc ^= r2 * acc;
        acc += (acc >> 17) ^ r1;
        facc = facc * 1.0000001 + sin((double)(acc & 0xFFFF) * 9.5873e-5);
        iters++;
    }

    a->ops = (double)iters / (bench_now_sec() - t0);
    a->hash_out = mix64(a->seed, acc ^ (uint64_t)(facc * 1e15));
    return NULL;
}
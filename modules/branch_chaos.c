/* branch_chaos.c — unpredictable branch patterns (all-core) */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void *branch_worker(void *arg) {
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    uint64_t rng = a->seed;
    volatile uint64_t acc = 0;
    uint64_t iters = 0;
    double t0 = bench_now_sec();
    double deadline = t0 + a->duration_sec;

    while (bench_now_sec() < deadline) {
        uint64_t v = xorshift64(&rng);
        if (v & 1)   { if (v & 2)   { acc += v;      } else { acc ^= v; } }
        if (v & 4)   { if (v & 8)   { acc -= v;      } else { acc *= 3; } }
        if (v & 16)  { if (v & 32)  { acc += v >> 3;  } else { acc ^= v << 2; } }
        if (v & 64)  { if (v & 128) { acc |= v;      } else { acc &= ~v; } }
        if (v & 256) { acc = (acc > v) ? acc - v : acc + v; }
        switch ((v >> 13) & 7) {
            case 0: acc += v * 3;   break;
            case 1: acc ^= v << 5;  break;
            case 2: acc -= v >> 2;  break;
            case 3: acc |= v * 7;   break;
            case 4: acc &= v + 1;   break;
            case 5: acc *= v | 1;   break;
            case 6: acc ^= acc >> 11; break;
            case 7: acc += acc << 3;  break;
        }
        iters++;
    }

    a->ops = (double)iters / (bench_now_sec() - t0);
    a->hash_out = mix64(a->seed, acc);
    return NULL;
}

bench_result_t module_branch_chaos(uint64_t chain_seed,
                                    int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "branch_chaos";
    r.chain_in    = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec,
                 branch_worker, &total_ops, &combined_hash);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec   = total_ops;
    r.score         = total_ops / 1e6;
    r.chain_out     = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags), "BRANCH_UNPREDICTABLE ncores=%d", ncores);
    return r;
}

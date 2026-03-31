/* cache_thrash.c — thrash L1/L2/L3 cache on all cores */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define CT_L1_SZ (32 * 1024)
#define CT_L2_SZ (512 * 1024)
#define CT_L3_SZ (8 * 1024 * 1024)

static void *cache_worker(void *arg) {
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    uint8_t *l1_buf = malloc(CT_L1_SZ);
    uint8_t *l2_buf = malloc(CT_L2_SZ);
    uint8_t *l3_buf = malloc(CT_L3_SZ);
    if (!l1_buf || !l2_buf || !l3_buf) { free(l1_buf); free(l2_buf); free(l3_buf); return NULL; }

    uint64_t rng = a->seed;
    uint64_t acc = a->seed;
    uint64_t iters = 0;
    double sub = (double)a->duration_sec / 3.0;
    double t0 = bench_now_sec();
    double deadlines[3] = {t0 + sub, t0 + 2*sub, t0 + 3*sub};

    size_t sizes[3] = {CT_L1_SZ, CT_L2_SZ, CT_L3_SZ};
    uint8_t *bufs[3] = {l1_buf, l2_buf, l3_buf};

    for (int phase = 0; phase < 3; phase++) {
        memset(bufs[phase], 0, sizes[phase]);
        while (bench_now_sec() < deadlines[phase]) {
            size_t idx = (size_t)(xorshift64(&rng) % sizes[phase]);
            bufs[phase][idx] ^= (uint8_t)(rng & 0xFF);
            acc ^= bufs[phase][idx];
            iters++;
        }
    }

    a->ops = (double)iters;
    a->hash_out = acc;
    free(l1_buf); free(l2_buf); free(l3_buf);
    return NULL;
}

bench_result_t module_cache_thrash(uint64_t chain_seed,
                                   int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "cache_thrash";
    r.chain_in = chain_seed;

    int nthreads = resolve_threads(thread_count);
    double total_ops = 0.0;
    uint64_t combined_hash = chain_seed;
    parallel_run(nthreads, chain_seed, duration_sec, cache_worker, &total_ops, &combined_hash);

    double elapsed = (double)duration_sec;
    r.wall_time_sec = elapsed;
    r.ops_per_sec = total_ops / elapsed;
    r.score = r.ops_per_sec / 1e6;
    r.chain_out = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags), "L1+L2+L3 %dT %.0fM ops/s", nthreads, total_ops / elapsed / 1e6);
    return r;
}

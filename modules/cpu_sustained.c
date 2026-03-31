/* cpu_sustained.c — detects thermal throttling by tracking ops/sec over time (all-core) */
#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SUSTAINED_SAMPLE_SEC 0.20

typedef struct {
    parallel_arg_t base;
    int sample_count;
    double *samples;
    uint64_t acc;
} sustained_arg_t;

static void *sustained_worker(void *arg) {
    sustained_arg_t *a = (sustained_arg_t *)arg;
    platform_pin_thread(a->base.core_id);

    uint64_t rnd = a->base.seed;
    uint64_t acc = a->base.seed;
    double interval = SUSTAINED_SAMPLE_SEC;

    for (int s = 0; s < a->sample_count; s++) {
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        double deadline = t0 + interval;
        while (bench_now_sec() < deadline) {
            acc = acc * 6364136223846793005ULL + xorshift64(&rnd);
            iters++;
        }
        a->samples[s] = (double)iters / (bench_now_sec() - t0);
    }

    a->acc = acc;
    a->base.hash_out = mix64(a->base.seed, acc);
    /* Use average rate across all samples for fair scoring */
    double sum = 0.0;
    for (int s = 0; s < a->sample_count; s++) sum += a->samples[s];
    a->base.ops = sum / a->sample_count;
    return NULL;
}

bench_result_t module_cpu_sustained(uint64_t chain_seed,
                                    int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "cpu_sustained";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    double duration_target = (duration_sec > 0) ? (double)duration_sec : 1.0;
    double interval = SUSTAINED_SAMPLE_SEC;
    int sample_count = (int)(duration_target / interval + 0.5);
    if (sample_count < 2) sample_count = 2;

    sustained_arg_t *args = (sustained_arg_t *)malloc(
        (size_t)ncores * sizeof(sustained_arg_t));
    bench_thread_t *tids = (bench_thread_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = (bench_thread_trampoline_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].base.seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].base.core_id = i;
        args[i].base.duration_sec = duration_sec;
        args[i].base.hash_out = 0;
        args[i].base.ops = 0;
        args[i].sample_count = sample_count;
        args[i].samples = (double *)malloc((size_t)sample_count * sizeof(double));
        bench_thread_create(&tids[i], &tramps[i], sustained_worker, &args[i]);
    }

    /* Collect: aggregate across all cores, use core-0 for throttle detection */
    uint64_t hash_acc = chain_seed;
    double total_avg = 0;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        hash_acc ^= args[i].base.hash_out;
        total_avg += args[i].base.ops;
    }

    /* Throttle detection from core 0 */
    double *s0 = args[0].samples;
    int window = (sample_count >= 6) ? 3 : (sample_count / 2);
    if (window < 1) window = 1;
    double early = 0.0, late = 0.0, peak = 0.0, valley = 1e30;
    for (int i = 0; i < sample_count; i++) {
        if (s0[i] > peak) peak = s0[i];
        if (s0[i] < valley) valley = s0[i];
    }
    for (int i = 0; i < window; i++) {
        early += s0[i];
        late += s0[sample_count - window + i];
    }
    early /= (double)window;
    late /= (double)window;
    double throttle_pct = (early > 0) ? ((early - late) / early * 100.0) : 0.0;
    double variance_pct = (peak > 0) ? ((peak - valley) / peak * 100.0) : 0.0;

    r.wall_time_sec = sample_count * interval;
    r.ops_per_sec = total_avg;
    r.score = total_avg / 1e6;
    r.chain_out = mix64(chain_seed, hash_acc);

    snprintf(r.flags, sizeof(r.flags),
             "%dT dt=%.2fs n=%d early=%.2fM late=%.2fM peak=%.2fM throttle=%.1f%% var=%.1f%%",
             ncores, interval, sample_count,
             early / 1e6, late / 1e6, peak / 1e6,
             throttle_pct, variance_pct);

    if (throttle_pct > 15.0)
        snprintf(r.notes, sizeof(r.notes),
                 "THROTTLED: %.1f%% drop (early=%.1fM late=%.1fM peak=%.1fM valley=%.1fM)",
                 throttle_pct, early/1e6, late/1e6, peak/1e6, valley/1e6);
    else if (variance_pct > 20.0)
        snprintf(r.notes, sizeof(r.notes),
                 "UNSTABLE: %.1f%% variance (peak=%.1fM valley=%.1fM) but no sustained drop",
                 variance_pct, peak/1e6, valley/1e6);
    else
        snprintf(r.notes, sizeof(r.notes),
                 "STABLE: %.1f%% throttle, %.1f%% variance — sustained performance OK",
                 throttle_pct, variance_pct);

    for (int i = 0; i < ncores; i++) free(args[i].samples);
    free(args); free(tids); free(tramps);
    return r;
}

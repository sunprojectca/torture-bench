/*
 * tune_probe.c
 * Standalone tool that detects whether the platform is pre-seeding caches,
 * engaging prefetchers, or using coprocessors before the main benchmark runs.
 *
 * Run this BEFORE torture-bench on each machine and include the output
 * with your results so readers can assess fairness.
 *
 * Tests:
 *   1. Cache warm ratio: same workload with/without cache flush
 *   2. Prefetcher detection: strided vs random access time ratio
 *   3. Thread scheduler variance: same work across cores
 *   4. Frequency stability: measure over 30s with 0.2s samples
 *   5. Turbo boost detection: short burst vs sustained performance
 */

#include "../harness/common.h"
#include "../harness/platform.h"
#include "../modules/anticache_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../harness/bench_thread.h"

/* ── 1. Cache warm ratio ─────────────────────────────────────────────────── */
static void cache_warm_test(void *poison, size_t poison_sz)
{
    printf("\n[1] CACHE WARM RATIO TEST\n");

    const size_t N = 1 << 22; /* 32MB pointer chase */
    size_t *buf = anticache_alloc(N * sizeof(size_t));
    uint64_t rnd = 0xDEADBEEF12345678ULL;
    for (size_t i = 0; i < N; i++)
        buf[i] = i;
    /* shuffle */
    for (size_t i = N - 1; i > 0; i--)
    {
        size_t j = (size_t)(xorshift64(&rnd) % (i + 1));
        size_t t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
    }

    double times[4];
    for (int trial = 0; trial < 4; trial++)
    {
        if (trial % 2 == 0)
            anticache_flush(poison, poison_sz); /* flush evens */
        volatile size_t idx = 0;
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        while (bench_now_sec() < t0 + 2.0)
        {
            idx = buf[idx];
            idx = buf[idx];
            idx = buf[idx];
            idx = buf[idx];
            iters += 4;
        }
        double elapsed = bench_now_sec() - t0;
        times[trial] = (elapsed / iters) * 1e9;
        printf("  Trial %d (%s): %.1f ns/access\n",
               trial, (trial % 2 == 0) ? "cold" : "warm", times[trial]);
    }

    double cold_avg = (times[0] + times[2]) / 2.0;
    double warm_avg = (times[1] + times[3]) / 2.0;
    double ratio = cold_avg / (warm_avg + 0.01);

    printf("  Cold avg: %.1f ns | Warm avg: %.1f ns | Ratio: %.2fx\n",
           cold_avg, warm_avg, ratio);
    if (ratio > 3.0)
        printf("  STATUS: WARN — large cold/warm difference, L3 eviction working\n");
    else if (ratio > 1.5)
        printf("  STATUS: OK — moderate warm effect (normal)\n");
    else
        printf("  STATUS: SUSPICIOUS — cold == warm, flush may not be working\n");

    anticache_free(buf, N * sizeof(size_t));
}

/* ── 2. Prefetcher detection ─────────────────────────────────────────────── */
static void prefetcher_test(void)
{
    printf("\n[2] HARDWARE PREFETCHER DETECTION\n");

    const size_t N = 1 << 20; /* 8MB */
    uint8_t *buf = malloc(N);
    memset(buf, 1, N);

    /* Sequential access (prefetcher helps) */
    volatile uint64_t acc = 0;
    uint64_t iters = 0;
    double t0 = bench_now_sec();
    while (bench_now_sec() < t0 + 2.0)
    {
        for (size_t i = 0; i < N; i += 64)
            acc += buf[i];
        iters += N / 64;
    }
    double seq_rate = (double)iters / (bench_now_sec() - t0);

    /* Random access (prefetcher blind) */
    uint64_t rnd = 0x123456789ABCDEFULL;
    iters = 0;
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + 2.0)
    {
        size_t idx = (size_t)(xorshift64(&rnd) % N);
        acc += buf[idx];
        iters++;
    }
    double rnd_rate = (double)iters / (bench_now_sec() - t0);
    double ratio = seq_rate / (rnd_rate + 1);

    printf("  Sequential: %.2f Mops/s\n", seq_rate / 1e6);
    printf("  Random:     %.2f Mops/s\n", rnd_rate / 1e6);
    printf("  Ratio:      %.1fx\n", ratio);
    if (ratio > 20.0)
        printf("  STATUS: WARN — aggressive prefetcher or out-of-order may help sequential\n");
    else
        printf("  STATUS: OK (ratio %.1fx is normal)\n", ratio);

    (void)acc;
    free(buf);
}

/* ── 3. Frequency stability ──────────────────────────────────────────────── */
static void frequency_stability_test(void)
{
    const double window_sec = 30.0;
    const double sample_sec = 0.20;
    int sample_count = (int)(window_sec / sample_sec + 0.5);
    if (sample_count < 2)
        sample_count = 2;

    printf("\n[3] FREQUENCY STABILITY (%.0fs, %.2fs samples)\n",
           window_sec, sample_sec);

    double *samples = malloc((size_t)sample_count * sizeof(double));
    if (!samples)
    {
        printf("  STATUS: SKIP — unable to allocate sample buffer\n");
        return;
    }

    double min_rate = 1e18, max_rate = 0.0;

    for (int s = 0; s < sample_count; s++)
    {
        volatile uint64_t acc = 0x1234567890ABCDEFULL;
        uint64_t rnd = (uint64_t)s ^ 0x9E3779B97F4A7C15ULL;
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        while (bench_now_sec() < t0 + sample_sec)
        {
            acc = acc * 6364136223846793005ULL + xorshift64(&rnd);
            iters++;
        }
        samples[s] = (double)iters / (bench_now_sec() - t0);
        if (samples[s] < min_rate)
            min_rate = samples[s];
        if (samples[s] > max_rate)
            max_rate = samples[s];
        printf("  t=%5.1fs: %.2f Mops/s%s",
               (s + 1) * sample_sec, samples[s] / 1e6,
               (s + 1) % 5 == 0 ? "\n" : "  ");
        fflush(stdout);
        (void)acc;
    }
    printf("\n");

    double range_pct = (max_rate - min_rate) / max_rate * 100.0;
    printf("  Min: %.2f M | Max: %.2f M | Range: %.1f%%\n",
           min_rate / 1e6, max_rate / 1e6, range_pct);

    if (range_pct > 20.0)
        printf("  STATUS: WARN — >20%% variance = thermal throttling or Turbo Boost\n");
    else if (range_pct > 8.0)
        printf("  STATUS: MODERATE — some frequency variation (%.1f%%)\n", range_pct);
    else
        printf("  STATUS: STABLE (%.1f%% variance)\n", range_pct);

    free(samples);
}

/* ── 4. Turbo burst detection ────────────────────────────────────────────── */
static void turbo_detection_test(void)
{
    printf("\n[4] TURBO / BURST DETECTION\n");

    /* Short burst (1s) vs long sustained (10s) */
    double burst_rate, sustained_rate;

    {
        volatile uint64_t acc = 0;
        uint64_t rnd = 0x0123456789ABCDEFULL;
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        while (bench_now_sec() < t0 + 1.0)
        {
            acc *= 6364136223846793005ULL;
            acc += xorshift64(&rnd) | 1ULL;
            iters++;
        }
        burst_rate = (double)iters / (bench_now_sec() - t0);
        (void)acc;
    }

    {
        volatile uint64_t acc = 0;
        uint64_t rnd = 0xFEDCBA9876543210ULL;
        uint64_t iters = 0;
        double t0 = bench_now_sec();
        while (bench_now_sec() < t0 + 10.0)
        {
            acc *= 6364136223846793005ULL;
            acc += xorshift64(&rnd) | 1ULL;
            iters++;
        }
        sustained_rate = (double)iters / (bench_now_sec() - t0);
        (void)acc;
    }

    double ratio = burst_rate / sustained_rate;
    printf("  Burst  (1s):  %.2f Mops/s\n", burst_rate / 1e6);
    printf("  Sustained (10s): %.2f Mops/s\n", sustained_rate / 1e6);
    printf("  Burst/Sustained: %.2fx\n", ratio);

    if (ratio > 1.3)
        printf("  STATUS: WARN — Turbo Boost detected (%.0f%% faster short-burst)\n",
               (ratio - 1.0) * 100.0);
    else
        printf("  STATUS: OK — consistent frequency (no significant turbo)\n");
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    platform_info_t pinfo;
    platform_detect(&pinfo);
    uint64_t rnd_state = 0xC0FFEE1234ABCDEFULL;

    printf("\n");
    printf("  ╔═══════════════════════════════════════════════════╗\n");
    printf("  ║       TUNE-PROBE  — Anti-Cheat Diagnostics        ║\n");
    printf("  ╚═══════════════════════════════════════════════════╝\n\n");
    platform_print(&pinfo);

    /* Allocate poison buffer */
    size_t poison_sz = (size_t)(pinfo.cache_l3_kb * 1024) * 2;
    if (poison_sz < 32 * 1024 * 1024)
        poison_sz = 32 * 1024 * 1024;
    void *poison = anticache_alloc(poison_sz);
    printf("\n  Poison buffer: %.0f MB\n", (double)poison_sz / (1024 * 1024));

    bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                         1024 + (int)(xorshift64(&rnd_state) & 1023));
    cache_warm_test(poison, poison_sz);
    bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                         1024 + (int)(xorshift64(&rnd_state) & 1023));
    prefetcher_test();
    bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                         1024 + (int)(xorshift64(&rnd_state) & 1023));
    frequency_stability_test();
    bench_entropy_spacer(&rnd_state, (volatile uint8_t *)poison, poison_sz,
                         1024 + (int)(xorshift64(&rnd_state) & 1023));
    turbo_detection_test();

    anticache_free(poison, poison_sz);

    printf("\n[DONE] Run torture-bench with --tune to integrate these checks.\n\n");
    return 0;
}

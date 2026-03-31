/*
 * simd_dispatch.c
 * Tests SIMD throughput and detects which SIMD unit is being used.
 * Runs the same workload three ways:
 *   1. Pure scalar (baseline)
 *   2. SIMD intrinsics (NEON on ARM64, AVX2 on x86)
 *   3. Auto-vectorized (compiler decides — flagged separately)
 *
 * Ratio of SIMD/scalar throughput is used to detect:
 *   - Hardware SIMD running as expected
 *   - Unexpected acceleration (ratio >> theoretical SIMD width)
 */

#include "../harness/common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../harness/parallel_runner.h"

#if defined(ARCH_ARM64) && defined(HAS_NEON)
#include <arm_neon.h>
#endif

#if defined(ARCH_X86_64) && defined(HAS_AVX2)
#include <immintrin.h>
#endif

#define SIMD_N (1 << 20) /* 1M floats = 4MB */

/* ── scalar dot product ─────────────────────────────────────────────────── */
static double scalar_dot(const float *restrict a,
                         const float *restrict b, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++)
        acc += (double)a[i] * (double)b[i];
    return acc;
}

/* ── NEON dot product (ARM64) ────────────────────────────────────────────── */
#if defined(ARCH_ARM64) && defined(HAS_NEON)
static double neon_dot(const float *restrict a,
                       const float *restrict b, size_t n)
{
    float32x4_t acc0 = vdupq_n_f32(0.0f);
    float32x4_t acc1 = vdupq_n_f32(0.0f);
    float32x4_t acc2 = vdupq_n_f32(0.0f);
    float32x4_t acc3 = vdupq_n_f32(0.0f);
    size_t i = 0;
    size_t limit = n & ~(size_t)15;
    /* Unrolled 4x for throughput */
    for (; i < limit; i += 16)
    {
        acc0 = vmlaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        acc2 = vmlaq_f32(acc2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        acc3 = vmlaq_f32(acc3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    acc0 = vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3));
    float r[4];
    vst1q_f32(r, acc0);
    double result = (double)r[0] + r[1] + r[2] + r[3];
    for (; i < n; i++)
        result += (double)a[i] * b[i];
    return result;
}

static double neon_saxpy(float *restrict y, const float *restrict x,
                         float alpha, size_t n)
{
    float32x4_t valpha = vdupq_n_f32(alpha);
    size_t i = 0;
    size_t limit = n & ~(size_t)3;
    for (; i < limit; i += 4)
        vst1q_f32(y + i, vmlaq_f32(vld1q_f32(y + i), valpha, vld1q_f32(x + i)));
    for (; i < n; i++)
        y[i] += alpha * x[i];
    return (double)y[0]; /* prevent DCE */
}
#endif /* ARCH_ARM64 */

/* ── AVX2 dot product (x86_64) ───────────────────────────────────────────── */
#if defined(ARCH_X86_64) && defined(HAS_AVX2)
static double avx2_dot(const float *restrict a,
                       const float *restrict b, size_t n)
{
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    size_t i = 0;
    size_t limit = n & ~(size_t)15;
    for (; i < limit; i += 16)
    {
        acc0 = _mm256_add_ps(_mm256_mul_ps(_mm256_loadu_ps(a + i),
                                          _mm256_loadu_ps(b + i)), acc0);
        acc1 = _mm256_add_ps(_mm256_mul_ps(_mm256_loadu_ps(a + i + 8),
                                          _mm256_loadu_ps(b + i + 8)), acc1);
    }
    acc0 = _mm256_add_ps(acc0, acc1);
    float tmp[8];
    _mm256_storeu_ps(tmp, acc0);
    double r = 0;
    for (int k = 0; k < 8; k++)
        r += tmp[k];
    for (; i < n; i++)
        r += (double)a[i] * b[i];
    return r;
}

static double avx2_saxpy(float *restrict y, const float *restrict x,
                         float alpha, size_t n)
{
    __m256 va = _mm256_set1_ps(alpha);
    size_t i = 0;
    size_t limit = n & ~(size_t)7;
    for (; i < limit; i += 8)
        _mm256_storeu_ps(y + i,
                         _mm256_add_ps(_mm256_mul_ps(va, _mm256_loadu_ps(x + i)),
                                      _mm256_loadu_ps(y + i)));
    for (; i < n; i++)
        y[i] += alpha * x[i];
    return (double)y[0];
}
#endif /* ARCH_X86_64 */

/* ── per-thread SIMD worker ───────────────────────────────────────────────── */
typedef struct {
    uint64_t seed;
    int      core_id;
    int      duration_sec;
    uint64_t hash_out;
    double   simd_rate;
    double   scalar_rate;
} simd_arg_t;

static void *simd_worker(void *varg) {
    simd_arg_t *a = (simd_arg_t *)varg;

    float *wa = malloc(SIMD_N * sizeof(float));
    float *wb = malloc(SIMD_N * sizeof(float));
    float *wy = malloc(SIMD_N * sizeof(float));
    if (!wa || !wb || !wy) { free(wa); free(wb); free(wy); return NULL; }

    uint64_t rng = a->seed;
    for (size_t i = 0; i < SIMD_N; i++) {
        wa[i] = (float)((int32_t)xorshift64(&rng)) / (float)(1 << 20);
        wb[i] = (float)((int32_t)xorshift64(&rng)) / (float)(1 << 20);
        wy[i] = wa[i];
    }

    double sub = (double)a->duration_sec / 3.0;

    /* 1. Scalar baseline */
    uint64_t scalar_iters = 0;
    double t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        volatile double d = scalar_dot(wa, wb, SIMD_N); (void)d;
        scalar_iters++;
    }
    a->scalar_rate = (double)scalar_iters / sub;

    /* 2. SIMD */
    uint64_t simd_iters = 0;
    a->simd_rate = 0.0;

#if defined(ARCH_ARM64) && defined(HAS_NEON)
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        volatile double d = neon_dot(wa, wb, SIMD_N); (void)d;
        neon_saxpy(wy, wa, 0.9999f, SIMD_N);
        simd_iters++;
    }
    a->simd_rate = (double)simd_iters / sub;
#elif defined(ARCH_X86_64) && defined(HAS_AVX2)
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        volatile double d = avx2_dot(wa, wb, SIMD_N); (void)d;
        avx2_saxpy(wy, wa, 0.9999f, SIMD_N);
        simd_iters++;
    }
    a->simd_rate = (double)simd_iters / sub;
#else
    a->simd_rate = a->scalar_rate;
#endif

    a->hash_out = mix64(a->seed, (uint64_t)(a->simd_rate * a->scalar_rate));
    free(wa); free(wb); free(wy);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_simd_dispatch(uint64_t chain_seed,
                                    int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "simd_dispatch";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    simd_arg_t *args = calloc(ncores, sizeof(simd_arg_t));
    bench_thread_t *tids = malloc(ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = malloc(ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].core_id = i;
        args[i].duration_sec = duration_sec;
        bench_thread_create(&tids[i], &tramps[i], simd_worker, &args[i]);
    }

    double total_simd = 0, total_scalar = 0;
    uint64_t hash = chain_seed;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        total_simd += args[i].simd_rate;
        total_scalar += args[i].scalar_rate;
        hash ^= args[i].hash_out;
    }

    /* Per-core ratio from core 0 for coprocessor detection */
    double core0_speedup = (args[0].scalar_rate > 0)
        ? args[0].simd_rate / args[0].scalar_rate : 1.0;

    const char *simd_name = "NONE";
#if defined(ARCH_ARM64) && defined(HAS_NEON)
    simd_name = "NEON";
#elif defined(ARCH_X86_64) && defined(HAS_AVX2)
    simd_name = "AVX2";
#else
    simd_name = "SCALAR_FALLBACK";
#endif

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_simd;
    r.score = total_simd / 1e6;
    r.chain_out = mix64(chain_seed, hash);

    snprintf(r.flags, sizeof(r.flags),
             "%dT %s scalar=%.2fM simd=%.2fM speedup=%.1fx",
             ncores, simd_name, total_scalar / 1e6, total_simd / 1e6, core0_speedup);

    double expected = 1.0;
#if defined(HAS_NEON)
    expected = 4.0;
#elif defined(HAS_AVX2)
    expected = 8.0;
#endif
    if (core0_speedup > expected * 2.5) {
        r.coprocessor_suspected = 1;
        snprintf(r.notes, sizeof(r.notes),
                 "WARN: speedup=%.1fx exceeds expected %.0fx — AMX or wider SIMD suspected",
                 core0_speedup, expected);
    } else {
        snprintf(r.notes, sizeof(r.notes), "OK: speedup=%.1fx (expected ~%.0fx)",
                 core0_speedup, expected);
    }

    free(args); free(tids); free(tramps);
    return r;
}

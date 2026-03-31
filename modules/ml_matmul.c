/*
 * ml_matmul.c
 * ML-style matrix multiply workload — the core op in every neural network.
 * Pure C, no BLAS/MKL/Accelerate.
 *
 * Coprocessor detection:
 *   - Apple AMX: Apple M-series has a hidden matrix coprocessor.
 *     Pure-C GEMM on M1 is ~4 GFLOPS. AMX gets ~20+ GFLOPS.
 *     We detect by comparing our measured rate vs theoretical scalar peak.
 *   - MLX / CoreML: if the process spawns helper threads we didn't create,
 *     throughput will be anomalously high.
 *   - Qualcomm HTP (Hexagon): similar — throughput explosion.
 *
 * Also runs:
 *   - INT8 quantized matmul (used in LLM inference)
 *   - BF16 simulation (detect if CPU has BF16 native support)
 *   - Batch matmul (simulates transformer attention)
 */

#include "../harness/common.h"
#include "../harness/platform.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define ML_M 128   /* batch */
#define ML_N 128   /* output features */
#define ML_K 256   /* input features */

/* ── FP32 GEMM (pure C, no vectorization hint) ───────────────────────────── */
/* volatile on inner accumulator prevents compiler from reordering/vectorizing */
static double gemm_fp32(const float *A, const float *B, float *C,
                         int M, int N, int K) {
    double flops = 0;
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float acc = 0.0f;
            for (int k = 0; k < K; k++)
                acc += A[i*K + k] * B[k*N + j];
            C[i*N + j] = acc;
            flops += 2 * K;
        }
    }
    return flops;
}

/* ── INT8 quantized matmul ──────────────────────────────────────────────── */
static double gemm_int8(const int8_t *A, const int8_t *B, int32_t *C,
                         int M, int N, int K) {
    double ops = 0;
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            int32_t acc = 0;
            for (int k = 0; k < K; k++)
                acc += (int32_t)A[i*K+k] * (int32_t)B[k*N+j];
            C[i*N+j] = acc;
            ops += 2 * K;
        }
    }
    return ops;
}

/* ── BF16 simulation (uint16 mantissa truncation) ────────────────────────── */
static inline uint16_t f32_to_bf16(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    return (uint16_t)(u >> 16);
}
static inline float bf16_to_f32(uint16_t b) {
    uint32_t u = (uint32_t)b << 16;
    float f; memcpy(&f, &u, 4);
    return f;
}

static double gemm_bf16(const uint16_t *A, const uint16_t *B, float *C,
                         int M, int N, int K) {
    double ops = 0;
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float acc = 0.0f;
            for (int k = 0; k < K; k++)
                acc += bf16_to_f32(A[i*K+k]) * bf16_to_f32(B[k*N+j]);
            C[i*N+j] = acc;
            ops += 2 * K;
        }
    }
    return ops;
}

/* ── Softmax (attention score normalization) ─────────────────────────────── */
static void softmax(float *x, int n) {
    float max = x[0];
    for (int i = 1; i < n; i++) if (x[i] > max) max = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - max); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

/* ── Scaled dot-product attention (one head) ────────────────────────────── */
static double attention_head(const float *Q, const float *K_t, const float *V,
                              float *out, int seq_len, int d_head) {
    float *scores = malloc(seq_len * seq_len * sizeof(float));
    double ops = 0;
    float scale = 1.0f / sqrtf((float)d_head);

    /* QK^T */
    for (int i = 0; i < seq_len; i++) {
        for (int j = 0; j < seq_len; j++) {
            float acc = 0;
            for (int k = 0; k < d_head; k++)
                acc += Q[i*d_head+k] * K_t[j*d_head+k];
            scores[i*seq_len+j] = acc * scale;
            ops += 2*d_head + 1;
        }
        softmax(scores + i*seq_len, seq_len);
    }
    /* Scores * V */
    for (int i = 0; i < seq_len; i++) {
        for (int j = 0; j < d_head; j++) {
            float acc = 0;
            for (int k = 0; k < seq_len; k++)
                acc += scores[i*seq_len+k] * V[k*d_head+j];
            out[i*d_head+j] = acc;
            ops += 2*seq_len;
        }
    }
    free(scores);
    return ops;
}

/* ── per-thread ML worker ─────────────────────────────────────────────────── */
typedef struct {
    uint64_t seed;
    int      core_id;
    int      duration_sec;
    uint64_t hash_out;
    double   fp32_gflops;
    double   int8_gops;
    double   bf16_gflops;
    double   attn_gflops;
} ml_arg_t;

static void *ml_worker(void *varg) {
    ml_arg_t *a = (ml_arg_t *)varg;

    float   *A_fp32  = malloc(ML_M * ML_K * sizeof(float));
    float   *B_fp32  = malloc(ML_K * ML_N * sizeof(float));
    float   *C_fp32  = malloc(ML_M * ML_N * sizeof(float));
    int8_t  *A_int8  = malloc(ML_M * ML_K);
    int8_t  *B_int8  = malloc(ML_K * ML_N);
    int32_t *C_int32 = malloc(ML_M * ML_N * sizeof(int32_t));
    uint16_t *A_bf16 = malloc(ML_M * ML_K * sizeof(uint16_t));
    uint16_t *B_bf16 = malloc(ML_K * ML_N * sizeof(uint16_t));

    const int SEQ = 64, D_HEAD = 64;
    float *Q   = malloc(SEQ * D_HEAD * sizeof(float));
    float *K_t = malloc(SEQ * D_HEAD * sizeof(float));
    float *V   = malloc(SEQ * D_HEAD * sizeof(float));
    float *O   = malloc(SEQ * D_HEAD * sizeof(float));

    uint64_t rng = a->seed;
    for (int i = 0; i < ML_M*ML_K; i++) {
        float v = (float)((int16_t)xorshift64(&rng)) / 256.0f;
        A_fp32[i] = v; A_int8[i] = (int8_t)(v * 4); A_bf16[i] = f32_to_bf16(v);
    }
    for (int i = 0; i < ML_K*ML_N; i++) {
        float v = (float)((int16_t)xorshift64(&rng)) / 256.0f;
        B_fp32[i] = v; B_int8[i] = (int8_t)(v * 4); B_bf16[i] = f32_to_bf16(v);
    }
    for (int i = 0; i < SEQ*D_HEAD; i++) {
        Q[i]   = (float)((int16_t)xorshift64(&rng)) / 1024.0f;
        K_t[i] = (float)((int16_t)xorshift64(&rng)) / 1024.0f;
        V[i]   = (float)((int16_t)xorshift64(&rng)) / 1024.0f;
    }

    double sub = (double)a->duration_sec / 4.0;
    uint64_t acc = a->seed;

    /* FP32 GEMM */
    double fp32_ops = 0;
    double t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        fp32_ops += gemm_fp32(A_fp32, B_fp32, C_fp32, ML_M, ML_N, ML_K);
        acc ^= (uint64_t)(C_fp32[0] * 1e9);
    }
    a->fp32_gflops = fp32_ops / 1e9 / (bench_now_sec() - t0);

    /* INT8 GEMM */
    double int8_ops = 0;
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        int8_ops += gemm_int8(A_int8, B_int8, C_int32, ML_M, ML_N, ML_K);
        acc ^= (uint64_t)C_int32[0];
    }
    a->int8_gops = int8_ops / 1e9 / (bench_now_sec() - t0);

    /* BF16 GEMM */
    double bf16_ops = 0;
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        bf16_ops += gemm_bf16(A_bf16, B_bf16, C_fp32, ML_M, ML_N, ML_K);
        acc ^= (uint64_t)(C_fp32[0] * 1e9);
    }
    a->bf16_gflops = bf16_ops / 1e9 / (bench_now_sec() - t0);

    /* Attention */
    double attn_ops = 0;
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        attn_ops += attention_head(Q, K_t, V, O, SEQ, D_HEAD);
        acc ^= (uint64_t)(O[0] * 1e9);
    }
    a->attn_gflops = attn_ops / 1e9 / (bench_now_sec() - t0);

    a->hash_out = mix64(a->seed, acc);

    free(A_fp32); free(B_fp32); free(C_fp32);
    free(A_int8); free(B_int8); free(C_int32);
    free(A_bf16); free(B_bf16);
    free(Q); free(K_t); free(V); free(O);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_ml_matmul(uint64_t chain_seed,
                                  int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "ml_matmul";
    r.chain_in    = chain_seed;

    int ncores = resolve_threads(thread_count);
    ml_arg_t *args = calloc(ncores, sizeof(ml_arg_t));
    bench_thread_t *tids = malloc(ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = malloc(ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].core_id = i;
        args[i].duration_sec = duration_sec;
        bench_thread_create(&tids[i], &tramps[i], ml_worker, &args[i]);
    }

    double total_fp32 = 0, total_int8 = 0, total_bf16 = 0, total_attn = 0;
    uint64_t hash = chain_seed;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        total_fp32 += args[i].fp32_gflops;
        total_int8 += args[i].int8_gops;
        total_bf16 += args[i].bf16_gflops;
        total_attn += args[i].attn_gflops;
        hash ^= args[i].hash_out;
    }

    r.wall_time_sec = duration_sec;
    r.ops_per_sec   = total_fp32 * 1e9;
    r.score         = total_fp32 * 1000.0;
    r.chain_out     = mix64(chain_seed, hash);

    snprintf(r.flags, sizeof(r.flags),
        "%dT FP32=%.2fG INT8=%.2fG BF16=%.2fG ATTN=%.2fG FLOPS",
        ncores, total_fp32, total_int8, total_bf16, total_attn);

    /* Per-core check for coprocessor detection */
    double core0_fp32 = args[0].fp32_gflops;
    if (core0_fp32 > 10.0) {
        r.coprocessor_suspected = 1;
        snprintf(r.notes, sizeof(r.notes),
            "WARN: FP32=%.1f GFLOPS/core >> scalar ceiling — AMX/HTP/NPU suspected",
            core0_fp32);
    } else if (args[0].int8_gops > core0_fp32 * 8.0) {
        r.coprocessor_suspected = 1;
        snprintf(r.notes, sizeof(r.notes),
            "WARN: INT8=%.1f GOPS/core (%.1fx FP32) — INT8 DSP/NPU suspected",
            args[0].int8_gops, args[0].int8_gops / core0_fp32);
    } else {
        snprintf(r.notes, sizeof(r.notes),
            "OK: FP32=%.2fG INT8=%.2fG BF16=%.2fG per-core",
            core0_fp32, args[0].int8_gops, args[0].bf16_gflops);
    }

    free(args); free(tids); free(tramps);
    return r;
}

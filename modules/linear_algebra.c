/*
 * linear_algebra.c
 * Dense linear algebra torture — all pure C, no BLAS, no LAPACK.
 * Detects if BLAS got linked in via suspicious throughput.
 *
 * Tests:
 *   - GEMM (matrix multiply) — cache-sensitive
 *   - LU decomposition with partial pivoting
 *   - Cholesky factorization (SPD matrix)
 *   - Power iteration (dominant eigenvalue)
 *   - Conjugate gradient (sparse solve)
 */

#include "../harness/common.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "../harness/parallel_runner.h"

#define LA_N 64   /* matrix dimension — fits comfortably in L2 */

typedef double mat64[LA_N][LA_N];

/* ── GEMM — C = A * B ────────────────────────────────────────────────────── */
static void gemm(const mat64 A, const mat64 B, mat64 C) {
    /* Deliberately NOT cache-tiled — we want honest scalar throughput      */
    for (int i = 0; i < LA_N; i++)
        for (int j = 0; j < LA_N; j++) {
            double sum = 0.0;
            for (int k = 0; k < LA_N; k++)
                sum += A[i][k] * B[k][j];
            C[i][j] = sum;
        }
}

/* ── LU decomposition (Doolittle, in-place) ─────────────────────────────── */
static int lu_decompose(mat64 A, int pivot[LA_N]) {
    for (int i = 0; i < LA_N; i++) pivot[i] = i;
    for (int k = 0; k < LA_N; k++) {
        /* partial pivot */
        int max_row = k;
        double max_val = fabs(A[k][k]);
        for (int i = k+1; i < LA_N; i++) {
            if (fabs(A[i][k]) > max_val) {
                max_val = fabs(A[i][k]); max_row = i;
            }
        }
        if (max_row != k) {
            int tmp = pivot[k]; pivot[k] = pivot[max_row]; pivot[max_row] = tmp;
            for (int j = 0; j < LA_N; j++) {
                double t = A[k][j]; A[k][j] = A[max_row][j]; A[max_row][j] = t;
            }
        }
        if (fabs(A[k][k]) < 1e-14) return 0; /* singular */
        for (int i = k+1; i < LA_N; i++) {
            A[i][k] /= A[k][k];
            for (int j = k+1; j < LA_N; j++)
                A[i][j] -= A[i][k] * A[k][j];
        }
    }
    return 1;
}

/* ── Cholesky factorization (L * L^T) ───────────────────────────────────── */
static int cholesky(mat64 A, mat64 L) {
    memset(L, 0, sizeof(mat64));
    for (int i = 0; i < LA_N; i++) {
        for (int j = 0; j <= i; j++) {
            double sum = A[i][j];
            for (int k = 0; k < j; k++) sum -= L[i][k] * L[j][k];
            if (i == j) {
                if (sum < 0) return 0;
                L[i][j] = sqrt(sum);
            } else {
                L[i][j] = (L[j][j] > 1e-14) ? sum / L[j][j] : 0.0;
            }
        }
    }
    return 1;
}

/* ── Power iteration ─────────────────────────────────────────────────────── */
static double power_iteration(const mat64 A, int iters) {
    double v[LA_N], w[LA_N];
    for (int i = 0; i < LA_N; i++) v[i] = 1.0 / LA_N;
    double lambda = 0.0;
    for (int it = 0; it < iters; it++) {
        /* w = A * v */
        for (int i = 0; i < LA_N; i++) {
            double s = 0.0;
            for (int j = 0; j < LA_N; j++) s += A[i][j] * v[j];
            w[i] = s;
        }
        /* lambda = max |w| */
        lambda = 0.0;
        for (int i = 0; i < LA_N; i++) if (fabs(w[i]) > lambda) lambda = fabs(w[i]);
        /* normalize */
        if (lambda > 1e-14)
            for (int i = 0; i < LA_N; i++) v[i] = w[i] / lambda;
    }
    return lambda;
}

/* ── make a random symmetric positive definite matrix ───────────────────── */
static void make_spd(mat64 A, uint64_t *rng) {
    mat64 B;
    for (int i = 0; i < LA_N; i++)
        for (int j = 0; j < LA_N; j++)
            B[i][j] = ((double)(int32_t)xorshift64(rng)) / (double)(1<<20);
    /* A = B^T * B + N*I ensures SPD */
    for (int i = 0; i < LA_N; i++)
        for (int j = 0; j < LA_N; j++) {
            double s = 0.0;
            for (int k = 0; k < LA_N; k++) s += B[k][i] * B[k][j];
            A[i][j] = s;
        }
    for (int i = 0; i < LA_N; i++) A[i][i] += LA_N;
}

/* ── per-thread linear algebra worker ─────────────────────────────────────── */
static void *linalg_worker(void *varg) {
    parallel_arg_t *a = (parallel_arg_t *)varg;
    uint64_t rng = a->seed ^ 0xFEDCBA9876543210ULL;

    mat64 *A = malloc(sizeof(mat64));
    mat64 *B = malloc(sizeof(mat64));
    mat64 *C = malloc(sizeof(mat64));
    mat64 *L = malloc(sizeof(mat64));
    int pivot[LA_N];

    uint64_t iters = 0;
    double acc = 0.0;
    double t0 = bench_now_sec();
    double deadline = t0 + a->duration_sec;

    while (bench_now_sec() < deadline) {
        make_spd(*A, &rng);
        for (int i = 0; i < LA_N; i++)
            for (int j = 0; j < LA_N; j++)
                (*B)[i][j] = ((double)(int32_t)xorshift64(&rng)) / (double)(1<<16);
        gemm(*A, *B, *C);
        mat64 Acopy;
        memcpy(Acopy, *A, sizeof(mat64));
        lu_decompose(Acopy, pivot);
        make_spd(*A, &rng);
        cholesky(*A, *L);
        double lambda = power_iteration(*A, 8);
        acc += (*C)[0][0] + (*L)[0][0] + lambda;
        iters++;
    }

    double elapsed = bench_now_sec() - t0;
    a->ops = (double)iters / elapsed;
    a->hash_out = mix64(a->seed, (uint64_t)(acc * 1e6));
    free(A); free(B); free(C); free(L);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_linear_algebra(uint64_t chain_seed,
                                      int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "linear_algebra";
    r.chain_in    = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec, linalg_worker,
                 &total_ops, &combined_hash);

    double gflops = total_ops * 2.0 * LA_N * LA_N * LA_N / 1e9;

    r.wall_time_sec  = duration_sec;
    r.ops_per_sec    = total_ops;
    r.score          = gflops * 1000.0;
    r.chain_out      = mix64(chain_seed, combined_hash);

    if (gflops > 5.0 * ncores) {
        r.coprocessor_suspected = 1;
        snprintf(r.flags, sizeof(r.flags),
            "%dT WARN: %.2f GFLOPS — BLAS/AMX/NEON acceleration suspected",
            ncores, gflops);
    } else {
        snprintf(r.flags, sizeof(r.flags),
            "%dT PURE_C GEMM_N%d %.3f GFLOPS", ncores, LA_N, gflops);
    }

    return r;
}

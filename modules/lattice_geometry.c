/*
 * lattice_geometry.c
 * Tests CPU throughput on lattice-based computations:
 *   - NTT (Number Theoretic Transform) — core of modern post-quantum crypto
 *   - LWE (Learning With Errors) matrix-vector multiply
 *   - Babai's nearest plane (lattice reduction approximation)
 *   - Random lattice basis generation and Gram-Schmidt
 *
 * Pure integer + fixed-point arithmetic. No floating point in NTT.
 */

#include "../harness/common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../harness/parallel_runner.h"

/* ── NTT parameters (Kyber-style) ───────────────────────────────────────── */
#define NTT_N    256
#define NTT_Q    3329   /* Kyber prime */

/* Modular multiplication avoiding overflow */
static inline int32_t mod_mul(int32_t a, int32_t b, int32_t q) {
    return (int32_t)(((int64_t)a * b) % q);
}

static inline int32_t mod_add(int32_t a, int32_t b, int32_t q) {
    int32_t r = a + b;
    if (r >= q) r -= q;
    if (r < 0)  r += q;
    return r;
}

/* ── precomputed NTT twiddle factors ────────────────────────────────────── */
static int32_t zetas[128];
static int ntt_initialized = 0;

static int32_t power_mod(int32_t base, int32_t exp, int32_t mod) {
    int32_t result = 1;
    base %= mod;
    while (exp > 0) {
        if (exp & 1) result = mod_mul(result, base, mod);
        base = mod_mul(base, base, mod);
        exp >>= 1;
    }
    return result;
}

static void ntt_init(void) {
    if (ntt_initialized) return;
    /* generator = 17 for Kyber q=3329 */
    int32_t root = 17;
    for (int i = 0; i < 128; i++) {
        /* bit-reversed zeta schedule */
        int idx = 0;
        int j = i;
        for (int b = 0; b < 7; b++) { idx = (idx << 1) | (j & 1); j >>= 1; }
        zetas[i] = power_mod(root, idx, NTT_Q);
    }
    ntt_initialized = 1;
}

/* Cooley-Tukey NTT in-place (Kyber reference style) */
static void ntt_forward(int32_t f[NTT_N]) {
    int k = 1;
    for (int len = 128; len >= 2; len >>= 1) {
        for (int start = 0; start < NTT_N; start += 2*len) {
            int32_t zeta = zetas[k++];
            for (int j = start; j < start + len; j++) {
                int32_t t = mod_mul(zeta, f[j+len], NTT_Q);
                f[j+len]  = mod_add(f[j], -t, NTT_Q);
                f[j]      = mod_add(f[j],  t, NTT_Q);
            }
        }
    }
}

/* Pointwise multiply in NTT domain */
static void ntt_pointwise_mul(int32_t *r, const int32_t *a, const int32_t *b) {
    for (int i = 0; i < NTT_N; i++)
        r[i] = mod_mul(a[i], b[i], NTT_Q);
}

/* ── LWE matrix-vector multiply ─────────────────────────────────────────── */
#define LWE_N   256
#define LWE_K   4
#define LWE_Q   NTT_Q

static void lwe_matrix_vec_mul(
    int32_t out[LWE_K][LWE_N],
    int32_t A[LWE_K][LWE_K][LWE_N],
    int32_t s[LWE_K][LWE_N])
{
    for (int i = 0; i < LWE_K; i++) {
        memset(out[i], 0, LWE_N * sizeof(int32_t));
        for (int j = 0; j < LWE_K; j++) {
            int32_t tmp[LWE_N];
            ntt_pointwise_mul(tmp, A[i][j], s[j]);
            for (int l = 0; l < LWE_N; l++)
                out[i][l] = mod_add(out[i][l], tmp[l], LWE_Q);
        }
    }
}

/* ── Gram-Schmidt orthogonalization (floating point lattice) ────────────── */
#define GS_DIM 8

static void gram_schmidt(double basis[GS_DIM][GS_DIM],
                          double ortho[GS_DIM][GS_DIM]) {
    for (int i = 0; i < GS_DIM; i++) {
        memcpy(ortho[i], basis[i], GS_DIM * sizeof(double));
        for (int j = 0; j < i; j++) {
            double num = 0, den = 0;
            for (int k = 0; k < GS_DIM; k++) {
                num += basis[i][k] * ortho[j][k];
                den += ortho[j][k] * ortho[j][k];
            }
            double mu = (den > 1e-14) ? (num / den) : 0.0;
            for (int k = 0; k < GS_DIM; k++)
                ortho[i][k] -= mu * ortho[j][k];
        }
    }
}

/* ── per-thread lattice worker ────────────────────────────────────────────── */
static void *lattice_worker(void *varg) {
    parallel_arg_t *a = (parallel_arg_t *)varg;
    uint64_t rng = a->seed ^ 0x1234567890ABCDEFULL;

    int32_t (*A)[LWE_K][LWE_N] = malloc(LWE_K * sizeof(*A));
    int32_t (*s)[LWE_N]        = malloc(LWE_K * sizeof(*s));
    int32_t (*out)[LWE_N]      = malloc(LWE_K * sizeof(*out));
    int32_t poly_a[NTT_N], poly_b[NTT_N], poly_c[NTT_N];

    for (int i = 0; i < LWE_K; i++)
        for (int j = 0; j < LWE_K; j++)
            for (int l = 0; l < LWE_N; l++)
                A[i][j][l] = (int32_t)(xorshift64(&rng) % NTT_Q);
    for (int i = 0; i < LWE_K; i++)
        for (int l = 0; l < LWE_N; l++)
            s[i][l] = (int32_t)(xorshift64(&rng) % 3);
    for (int i = 0; i < LWE_K; i++)
        for (int j = 0; j < LWE_K; j++)
            ntt_forward(A[i][j]);
    for (int i = 0; i < LWE_K; i++)
        ntt_forward(s[i]);

    uint64_t iters = 0, acc = 0;
    double t0 = bench_now_sec();
    double deadline = t0 + a->duration_sec;

    while (bench_now_sec() < deadline) {
        for (int l = 0; l < NTT_N; l++)
            poly_a[l] = (int32_t)(xorshift64(&rng) % NTT_Q);
        memcpy(poly_b, poly_a, sizeof(poly_a));
        ntt_forward(poly_a);
        ntt_pointwise_mul(poly_c, poly_a, poly_b);
        lwe_matrix_vec_mul(out, A, s);
        double basis[GS_DIM][GS_DIM], ortho[GS_DIM][GS_DIM];
        for (int i = 0; i < GS_DIM; i++)
            for (int j = 0; j < GS_DIM; j++)
                basis[i][j] = (double)(int32_t)xorshift64(&rng) / (double)(1<<20);
        gram_schmidt(basis, ortho);
        for (int i = 0; i < LWE_K; i++)
            acc ^= (uint64_t)out[i][0];
        acc ^= (uint64_t)poly_c[0];
        acc ^= (uint64_t)(ortho[0][0] * 1e9);
        iters++;
    }

    double elapsed = bench_now_sec() - t0;
    a->ops = (double)iters / elapsed;
    a->hash_out = mix64(a->seed, acc);
    free(A); free(s); free(out);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_lattice_geometry(uint64_t chain_seed,
                                        int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "lattice_geometry";
    r.chain_in    = chain_seed;

    ntt_init();

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec, lattice_worker,
                 &total_ops, &combined_hash);

    r.wall_time_sec  = duration_sec;
    r.ops_per_sec    = total_ops;
    r.score          = total_ops;
    r.chain_out      = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags), "%dT NTT_KYBER LWE_K%d GS_DIM%d",
             ncores, LWE_K, GS_DIM);

    return r;
}

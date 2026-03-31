/*
 * speculation_stress.c
 * Stresses CPU speculation and rollback mechanisms on all cores:
 *   Phase 1: Branch prediction — sorted (predictable) vs random
 *            (unpredictable) conditional branches, measures
 *            misprediction penalty
 *   Phase 2: Branch target buffer (BTB) — indirect dispatch through
 *            64 targets selected randomly, defeats BTB prediction
 *   Phase 3: Return stack buffer (RSB) — deep recursive call chains
 *            that overflow the RSB (typically 16-32 entries),
 *            forcing mispredicted returns
 *   Phase 4: Speculative load stress — memory accesses gated behind
 *            unpredictable branches, causing speculative load
 *            pipeline squashes
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

#define BRANCH_N 65536

/* ── Phase 1: Branch prediction stress ───────────────────────────────────── */
typedef struct {
    double sorted_rate;
    double random_rate;
} branch_result_t;

static branch_result_t phase_branch_predict(uint64_t seed, double duration)
{
    branch_result_t br = {0, 0};

    /* Allocate and fill random data */
    uint32_t *data = (uint32_t *)malloc(BRANCH_N * sizeof(uint32_t));
    if (!data) return br;

    uint64_t rng = seed;
    for (int i = 0; i < BRANCH_N; i++)
        data[i] = (uint32_t)(xorshift64(&rng) & 0xFFFF);

    uint32_t threshold = 0x8000; /* 50% of values below */

    /* Phase 1a: Sort the data — branches become highly predictable.
       First half: all below threshold (always taken).
       Second half: all above (always not-taken). */
    /* Simple insertion sort on 64K elements (part of the stress) */
    for (int i = 1; i < BRANCH_N; i++) {
        uint32_t key = data[i];
        int j = i - 1;
        while (j >= 0 && data[j] > key) {
            data[j + 1] = data[j];
            j--;
        }
        data[j + 1] = key;
    }

    volatile uint64_t sum = 0;
    uint64_t sorted_iters = 0;
    double sub = duration / 2.0;
    double t0 = bench_now_sec();
    double deadline = t0 + sub;

    while (bench_now_sec() < deadline) {
        for (int i = 0; i < BRANCH_N; i++) {
            /* Predictable branch (sorted data): pattern is 000...0111...1 */
            if (data[i] < threshold)
                sum += data[i];
            else
                sum += 1;
        }
        sorted_iters++;
    }
    double sorted_elapsed = bench_now_sec() - t0;
    br.sorted_rate = (double)sorted_iters * BRANCH_N / sorted_elapsed;

    /* Phase 1b: Shuffle the data — branches become ~50% unpredictable */
    for (int i = BRANCH_N - 1; i > 0; i--) {
        int j = (int)(xorshift64(&rng) % ((uint64_t)i + 1));
        uint32_t t = data[i]; data[i] = data[j]; data[j] = t;
    }

    uint64_t random_iters = 0;
    t0 = bench_now_sec();
    deadline = t0 + sub;

    while (bench_now_sec() < deadline) {
        for (int i = 0; i < BRANCH_N; i++) {
            /* Unpredictable branch (random data): ~50% misprediction */
            if (data[i] < threshold)
                sum += data[i];
            else
                sum += 1;
        }
        random_iters++;
    }
    double random_elapsed = bench_now_sec() - t0;
    br.random_rate = (double)random_iters * BRANCH_N / random_elapsed;

    (void)sum;
    free(data);
    return br;
}

/* ── Phase 2: BTB stress — indirect dispatch ─────────────────────────────── */
/*
 * 64 unique operations dispatched via switch. The random selection
 * defeats the Branch Target Buffer's pattern history, causing
 * indirect branch mispredictions on every call.
 */
#define BTB_OP(N) \
    x = x * (0x9E3779B97F4A7C15ULL + (uint64_t)(N)*31) + (uint64_t)(N); \
    x ^= x >> (7 + (N) % 11); \
    x = (x << ((N) % 13 + 1)) | (x >> (63 - (N) % 13)); \
    x *= x + (uint64_t)(N)*0x13371337ULL;

static uint64_t phase_btb_stress(uint64_t seed, double duration)
{
    uint64_t x = seed;
    uint64_t rng = seed ^ 0xAAAAAAAAAAAAAAAAULL;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* Random dispatch — BTB can't predict the target */
        switch ((int)(xorshift64(&rng) & 63)) {
        case 0:  BTB_OP(0)  break; case 1:  BTB_OP(1)  break;
        case 2:  BTB_OP(2)  break; case 3:  BTB_OP(3)  break;
        case 4:  BTB_OP(4)  break; case 5:  BTB_OP(5)  break;
        case 6:  BTB_OP(6)  break; case 7:  BTB_OP(7)  break;
        case 8:  BTB_OP(8)  break; case 9:  BTB_OP(9)  break;
        case 10: BTB_OP(10) break; case 11: BTB_OP(11) break;
        case 12: BTB_OP(12) break; case 13: BTB_OP(13) break;
        case 14: BTB_OP(14) break; case 15: BTB_OP(15) break;
        case 16: BTB_OP(16) break; case 17: BTB_OP(17) break;
        case 18: BTB_OP(18) break; case 19: BTB_OP(19) break;
        case 20: BTB_OP(20) break; case 21: BTB_OP(21) break;
        case 22: BTB_OP(22) break; case 23: BTB_OP(23) break;
        case 24: BTB_OP(24) break; case 25: BTB_OP(25) break;
        case 26: BTB_OP(26) break; case 27: BTB_OP(27) break;
        case 28: BTB_OP(28) break; case 29: BTB_OP(29) break;
        case 30: BTB_OP(30) break; case 31: BTB_OP(31) break;
        case 32: BTB_OP(32) break; case 33: BTB_OP(33) break;
        case 34: BTB_OP(34) break; case 35: BTB_OP(35) break;
        case 36: BTB_OP(36) break; case 37: BTB_OP(37) break;
        case 38: BTB_OP(38) break; case 39: BTB_OP(39) break;
        case 40: BTB_OP(40) break; case 41: BTB_OP(41) break;
        case 42: BTB_OP(42) break; case 43: BTB_OP(43) break;
        case 44: BTB_OP(44) break; case 45: BTB_OP(45) break;
        case 46: BTB_OP(46) break; case 47: BTB_OP(47) break;
        case 48: BTB_OP(48) break; case 49: BTB_OP(49) break;
        case 50: BTB_OP(50) break; case 51: BTB_OP(51) break;
        case 52: BTB_OP(52) break; case 53: BTB_OP(53) break;
        case 54: BTB_OP(54) break; case 55: BTB_OP(55) break;
        case 56: BTB_OP(56) break; case 57: BTB_OP(57) break;
        case 58: BTB_OP(58) break; case 59: BTB_OP(59) break;
        case 60: BTB_OP(60) break; case 61: BTB_OP(61) break;
        case 62: BTB_OP(62) break; case 63: BTB_OP(63) break;
        }
        iters++;
    }
    return x ^ iters;
}

/* ── Phase 3: Return stack buffer stress ─────────────────────────────────── */
/*
 * Deep recursive call chain. The RSB (return stack buffer) is typically
 * 16-32 entries. Calls deeper than that cause return mispredictions.
 * Each level does real work to prevent tail-call optimization.
 */
static NOINLINE uint64_t rsb_chain(uint64_t x, int depth)
{
    if (depth <= 0)
        return x * 0x9E3779B97F4A7C15ULL;
    /* Do enough work at each level to prevent inlining/TCO */
    x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    x ^= x >> 17;
    x = x * 2862933555777941757ULL + depth;
    uint64_t child = rsb_chain(x, depth - 1);
    /* Use child result to prevent TCO */
    return (child ^ x) * 7046029254386353131ULL + (uint64_t)depth;
}

static uint64_t phase_rsb_stress(uint64_t seed, double duration)
{
    uint64_t acc = seed;
    uint64_t iters = 0;
    uint64_t rng = seed ^ 0x5555555555555555ULL;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        /* Varying depth makes RSB prediction even harder.
           Depths 48-128 well exceed typical RSB size of 16-32. */
        int depth = 48 + (int)(xorshift64(&rng) % 81); /* 48..128 */
        acc ^= rsb_chain(acc, depth);
        iters++;
    }
    return acc ^ iters;
}

/* ── Phase 4: Speculative load stress ────────────────────────────────────── */
#define SPECLOAD_N (1 << 16)

static uint64_t phase_spec_load(uint64_t seed, double duration)
{
    /* Allocate two arrays. Loads from array A are gated behind
       unpredictable branches. When the CPU mis-speculates, it
       issues loads that get squashed — wasting load port bandwidth
       and stalling the pipeline on rollback. */
    uint64_t *A = (uint64_t *)malloc(SPECLOAD_N * sizeof(uint64_t));
    uint64_t *B = (uint64_t *)malloc(SPECLOAD_N * sizeof(uint64_t));
    if (!A || !B) { if (A) free(A); if (B) free(B); return seed; }

    uint64_t rng = seed;
    for (int i = 0; i < SPECLOAD_N; i++) {
        A[i] = xorshift64(&rng);
        B[i] = xorshift64(&rng);
    }

    volatile uint64_t acc = seed;
    uint64_t iters = 0;
    double deadline = bench_now_sec() + duration;

    while (bench_now_sec() < deadline) {
        uint64_t v = xorshift64(&rng);
        size_t ia = (size_t)(v % SPECLOAD_N);
        size_t ib = (size_t)((v >> 16) % SPECLOAD_N);

        /* Unpredictable branch — CPU speculates and issues loads
           from whichever path it guesses. ~50% of the time it's
           wrong and must squash those loads + computation. */
        if (v & 0x8000) {
            acc += A[ia] * B[ib] + A[(ia + 1) % SPECLOAD_N];
            acc ^= B[(ib + 3) % SPECLOAD_N] * A[(ia + 7) % SPECLOAD_N];
        } else {
            acc += B[ib] * A[ia] - B[(ib + 2) % SPECLOAD_N];
            acc ^= A[(ia + 5) % SPECLOAD_N] * B[(ib + 11) % SPECLOAD_N];
        }

        /* Nested unpredictable branch — double speculation depth */
        if (acc & 0x4000) {
            if (v & 0x2000)
                acc += A[(size_t)(acc % SPECLOAD_N)];
            else
                acc -= B[(size_t)((acc >> 7) % SPECLOAD_N)];
        } else {
            if (v & 0x1000)
                acc ^= A[(size_t)((acc >> 11) % SPECLOAD_N)] * 3;
            else
                acc += B[(size_t)((acc >> 19) % SPECLOAD_N)] + 7;
        }

        /* Variable-trip inner loop — loop branch predictor can't lock on */
        int trips = (int)((v >> 32) & 15) + 1;
        for (int j = 0; j < trips; j++) {
            size_t idx = (size_t)((acc + (uint64_t)j) % SPECLOAD_N);
            acc ^= A[idx] + B[idx];
        }

        iters++;
    }

    (void)acc;
    free(A);
    free(B);
    return (uint64_t)acc ^ iters;
}

/* ── thread worker ───────────────────────────────────────────────────────── */
typedef struct {
    parallel_arg_t base;
    double sorted_rate;
    double random_rate;
} spec_worker_arg_t;

static void *spec_worker(void *arg)
{
    spec_worker_arg_t *a = (spec_worker_arg_t *)arg;
    platform_pin_thread(a->base.core_id);

    double sub = (double)a->base.duration_sec / 4.0;
    uint64_t acc = a->base.seed;

    /* Phase 1: Branch prediction */
    branch_result_t br = phase_branch_predict(acc, sub);
    a->sorted_rate = br.sorted_rate;
    a->random_rate = br.random_rate;
    acc ^= (uint64_t)(br.sorted_rate + br.random_rate);

    /* Phase 2: BTB indirect dispatch */
    acc ^= phase_btb_stress(acc, sub);

    /* Phase 3: RSB overflow */
    acc ^= phase_rsb_stress(acc, sub);

    /* Phase 4: Speculative load stress */
    acc ^= phase_spec_load(acc, sub);

    a->base.ops = br.random_rate + br.sorted_rate;
    a->base.hash_out = mix64(a->base.seed, acc);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_speculation_stress(uint64_t chain_seed,
                                          int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "speculation_stress";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    if (ncores <= 0) {
        snprintf(r.flags, sizeof(r.flags), "BRANCH+BTB64+RSB+SPECLOAD ncores=0");
        snprintf(r.notes, sizeof(r.notes), "thread resolution failed");
        return r;
    }

    spec_worker_arg_t *args = (spec_worker_arg_t *)malloc(
        (size_t)ncores * sizeof(spec_worker_arg_t));
    bench_thread_t *tids = (bench_thread_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = (bench_thread_trampoline_t *)malloc(
        (size_t)ncores * sizeof(bench_thread_trampoline_t));

    if (!args || !tids || !tramps) {
        if (args) free(args);
        if (tids) free(tids);
        if (tramps) free(tramps);
        snprintf(r.flags, sizeof(r.flags), "BRANCH+BTB64+RSB+SPECLOAD ncores=%d", ncores);
        snprintf(r.notes, sizeof(r.notes), "allocation failed");
        return r;
    }

    int started = 0;
    for (int i = 0; i < ncores; i++) {
        args[i].base.seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].base.core_id = i;
        args[i].base.duration_sec = duration_sec;
        args[i].base.hash_out = 0;
        args[i].base.ops = 0;
        args[i].sorted_rate = 0;
        args[i].random_rate = 0;
        if (bench_thread_create(&tids[i], &tramps[i], spec_worker, &args[i]) != 0) {
            break;
        }
        started++;
    }

    if (started == 0) {
        free(args);
        free(tids);
        free(tramps);
        snprintf(r.flags, sizeof(r.flags), "BRANCH+BTB64+RSB+SPECLOAD ncores=%d", ncores);
        snprintf(r.notes, sizeof(r.notes), "thread creation failed");
        return r;
    }

    uint64_t hash = chain_seed;
    double total_ops = 0;
    double avg_sorted = 0, avg_random = 0;
    for (int i = 0; i < started; i++) {
        bench_thread_join(tids[i]);
        hash ^= args[i].base.hash_out;
        total_ops += args[i].base.ops;
        avg_sorted += args[i].sorted_rate;
        avg_random += args[i].random_rate;
    }
    avg_sorted /= started;
    avg_random /= started;

    double mispredict_penalty = (avg_random > 0)
        ? (avg_sorted / avg_random - 1.0) * 100.0 : 0.0;

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_ops;
    r.score = total_ops / 1e6;
    r.chain_out = mix64(chain_seed, hash);

    snprintf(r.flags, sizeof(r.flags),
             "BRANCH+BTB64+RSB+SPECLOAD ncores=%d", started);
    snprintf(r.notes, sizeof(r.notes),
             "sorted=%.0fM/s random=%.0fM/s mispredict_cost=%.0f%% slowdown",
             avg_sorted / 1e6, avg_random / 1e6, mispredict_penalty);

    free(args); free(tids); free(tramps);
    return r;
}

/*
 * exotic_chaos.c
 * Randomly sequences algorithms in unpredictable patterns.
 * The selection sequence itself is derived from the chain hash —
 * so the CPU cannot predict what's coming next.
 *
 * Algorithm pool:
 *   0: Mandelbrot escape-time (complex arithmetic)
 *   1: Conway's Game of Life (bit manipulation)
 *   2: Sieve of Eratosthenes (memory + branch)
 *   3: Sorting (quicksort random data)
 *   4: Fibonacci mod large prime (integer + branch)
 *   5: XOR tree hash (bitwise)
 *   6: String matching (Knuth-Morris-Pratt)
 *   7: BFS on random graph (pointer chasing)
 *   8: RC4 stream cipher (table lookup)
 *   9: 3D cellular automaton (array ops)
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define CHAOS_ALGOS 10

/* ── 0: Mandelbrot ──────────────────────────────────────────────────────── */
static uint64_t algo_mandelbrot(uint64_t seed) {
    const int W = 128, H = 96, MAX_ITER = 128;
    uint64_t acc = 0;
    uint64_t rng = seed;
    double cx0 = -2.5 + ((double)(xorshift64(&rng) & 0xFFFF) / 65535.0) * 0.5;
    double cy0 = -1.2 + ((double)(xorshift64(&rng) & 0xFFFF) / 65535.0) * 0.5;
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            double cx = cx0 + (double)px / W * 3.5;
            double cy = cy0 + (double)py / H * 2.4;
            double zx = 0, zy = 0;
            int iter;
            for (iter = 0; iter < MAX_ITER && zx*zx+zy*zy < 4.0; iter++) {
                double tmp = zx*zx - zy*zy + cx;
                zy = 2*zx*zy + cy;
                zx = tmp;
            }
            acc += iter;
        }
    }
    return acc;
}

/* ── 1: Game of Life ────────────────────────────────────────────────────── */
#define GOL_W 128
#define GOL_H 128
static _Thread_local uint8_t gol_a[GOL_W * GOL_H], gol_b[GOL_W * GOL_H];

static uint64_t algo_gol(uint64_t seed) {
    uint64_t rng = seed;
    for (int i = 0; i < GOL_W * GOL_H; i++)
        gol_a[i] = (uint8_t)(xorshift64(&rng) & 1);
    uint64_t acc = 0;
    for (int gen = 0; gen < 32; gen++) {
        for (int y = 1; y < GOL_H-1; y++) {
            for (int x = 1; x < GOL_W-1; x++) {
                int n = gol_a[(y-1)*GOL_W+x-1] + gol_a[(y-1)*GOL_W+x] +
                        gol_a[(y-1)*GOL_W+x+1] + gol_a[y*GOL_W+x-1] +
                        gol_a[y*GOL_W+x+1] + gol_a[(y+1)*GOL_W+x-1] +
                        gol_a[(y+1)*GOL_W+x] + gol_a[(y+1)*GOL_W+x+1];
                int live = gol_a[y*GOL_W+x];
                gol_b[y*GOL_W+x] = (live && (n==2||n==3)) || (!live && n==3);
                acc += gol_b[y*GOL_W+x];
            }
        }
        memcpy(gol_a, gol_b, GOL_W * GOL_H);
    }
    return acc;
}

/* ── 2: Sieve of Eratosthenes ───────────────────────────────────────────── */
#define SIEVE_N 65536
static _Thread_local uint8_t sieve_buf[SIEVE_N];

static uint64_t algo_sieve(uint64_t seed) {
    (void)seed;
    memset(sieve_buf, 1, SIEVE_N);
    sieve_buf[0] = sieve_buf[1] = 0;
    for (int i = 2; (uint64_t)i*i < SIEVE_N; i++) {
        if (sieve_buf[i]) {
            for (int j = i*i; j < SIEVE_N; j += i)
                sieve_buf[j] = 0;
        }
    }
    uint64_t cnt = 0;
    for (int i = 0; i < SIEVE_N; i++) cnt += sieve_buf[i];
    return cnt;
}

/* ── 3: Quicksort ───────────────────────────────────────────────────────── */
#define SORT_N 4096
static _Thread_local uint32_t sort_buf[SORT_N];

static void qsort_r32(uint32_t *a, int lo, int hi) {
    if (lo >= hi) return;
    uint32_t pivot = a[(lo+hi)/2];
    int i = lo, j = hi;
    while (i <= j) {
        while (a[i] < pivot) i++;
        while (a[j] > pivot) j--;
        if (i <= j) { uint32_t t=a[i]; a[i]=a[j]; a[j]=t; i++; j--; }
    }
    qsort_r32(a, lo, j);
    qsort_r32(a, i, hi);
}

static uint64_t algo_sort(uint64_t seed) {
    uint64_t rng = seed;
    for (int i = 0; i < SORT_N; i++)
        sort_buf[i] = (uint32_t)xorshift64(&rng);
    qsort_r32(sort_buf, 0, SORT_N-1);
    return (uint64_t)sort_buf[SORT_N/2]; /* median */
}

/* ── 4: Fibonacci mod prime ─────────────────────────────────────────────── */
#define FIB_P 1000000007ULL

static uint64_t algo_fib(uint64_t seed) {
    uint64_t a = seed % FIB_P, b = (seed >> 32) % FIB_P + 1;
    for (int i = 0; i < 100000; i++) {
        uint64_t c = (a + b) % FIB_P;
        a = b; b = c;
    }
    return b;
}

/* ── 5: XOR tree hash ────────────────────────────────────────────────────── */
#define XOR_N 65536
static _Thread_local uint64_t xor_buf[XOR_N];

static uint64_t algo_xor_tree(uint64_t seed) {
    uint64_t rng = seed;
    for (int i = 0; i < XOR_N; i++) xor_buf[i] = xorshift64(&rng);
    /* Reduce tree */
    int n = XOR_N;
    while (n > 1) {
        for (int i = 0; i < n/2; i++)
            xor_buf[i] = mix64(xor_buf[2*i], xor_buf[2*i+1]);
        n /= 2;
    }
    return xor_buf[0];
}

/* ── 6: KMP string matching ─────────────────────────────────────────────── */
#define KMP_TEXT_LEN   8192
#define KMP_PAT_LEN    16

static uint64_t algo_kmp(uint64_t seed) {
    uint64_t rng = seed;
    char text[KMP_TEXT_LEN+1], pat[KMP_PAT_LEN+1];
    int  fail[KMP_PAT_LEN];
    for (int i = 0; i < KMP_TEXT_LEN; i++)
        text[i] = 'a' + (char)(xorshift64(&rng) % 4);
    for (int i = 0; i < KMP_PAT_LEN; i++)
        pat[i] = 'a' + (char)(xorshift64(&rng) % 4);
    /* Build failure function */
    fail[0] = -1;
    for (int i = 1; i < KMP_PAT_LEN; i++) {
        int k = fail[i-1];
        while (k >= 0 && pat[k+1] != pat[i]) k = fail[k];
        fail[i] = (pat[k+1] == pat[i]) ? k+1 : -1;
    }
    /* Search */
    uint64_t matches = 0;
    int k = -1;
    for (int i = 0; i < KMP_TEXT_LEN; i++) {
        while (k >= 0 && pat[k+1] != text[i]) k = fail[k];
        if (pat[k+1] == text[i]) k++;
        if (k == KMP_PAT_LEN-1) { matches++; k = fail[k]; }
    }
    return matches;
}

/* ── 7: BFS on random graph ──────────────────────────────────────────────── */
#define BFS_NODES  512
#define BFS_EDGES  4096

static _Thread_local int bfs_adj[BFS_NODES][8]; /* adjacency: up to 8 neighbors */
static _Thread_local int bfs_deg[BFS_NODES];

static uint64_t algo_bfs(uint64_t seed) {
    uint64_t rng = seed;
    memset(bfs_deg, 0, sizeof(bfs_deg));
    for (int e = 0; e < BFS_EDGES; e++) {
        int u = (int)(xorshift64(&rng) % BFS_NODES);
        int v = (int)(xorshift64(&rng) % BFS_NODES);
        if (bfs_deg[u] < 8) bfs_adj[u][bfs_deg[u]++] = v;
        if (bfs_deg[v] < 8) bfs_adj[v][bfs_deg[v]++] = u;
    }
    /* BFS from node 0 */
    int visited[BFS_NODES];
    int queue[BFS_NODES];
    memset(visited, 0, sizeof(visited));
    int head = 0, tail = 0;
    queue[tail++] = 0; visited[0] = 1;
    while (head < tail) {
        int u = queue[head++];
        for (int i = 0; i < bfs_deg[u]; i++) {
            int v = bfs_adj[u][i];
            if (!visited[v]) { visited[v] = 1; queue[tail++] = v; }
        }
    }
    uint64_t acc = 0;
    for (int i = 0; i < BFS_NODES; i++) acc += visited[i];
    return acc;
}

/* ── 8: RC4 stream cipher ────────────────────────────────────────────────── */
static uint64_t algo_rc4(uint64_t seed) {
    uint8_t S[256];
    uint8_t key[16];
    /* Key from seed */
    for (int i = 0; i < 16; i++)
        key[i] = (uint8_t)(seed >> (i * 4));
    /* KSA */
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % 16]) & 255;
        uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
    }
    /* PRGA over 65536 bytes */
    uint64_t acc = 0;
    int ii = 0; j = 0;
    for (int n = 0; n < 65536; n++) {
        ii = (ii + 1) & 255;
        j  = (j + S[ii]) & 255;
        uint8_t t = S[ii]; S[ii] = S[j]; S[j] = t;
        acc += S[(S[ii] + S[j]) & 255];
    }
    return acc;
}

/* ── 9: 3D cellular automaton ────────────────────────────────────────────── */
#define CA3D_D 16 /* 16^3 = 4096 cells */
static _Thread_local uint8_t ca3d_a[CA3D_D][CA3D_D][CA3D_D];
static _Thread_local uint8_t ca3d_b[CA3D_D][CA3D_D][CA3D_D];

static uint64_t algo_ca3d(uint64_t seed) {
    uint64_t rng = seed;
    for (int x = 0; x < CA3D_D; x++)
        for (int y = 0; y < CA3D_D; y++)
            for (int z = 0; z < CA3D_D; z++)
                ca3d_a[x][y][z] = (uint8_t)(xorshift64(&rng) & 1);
    uint64_t acc = 0;
    for (int gen = 0; gen < 4; gen++) {
        for (int x = 1; x < CA3D_D-1; x++)
          for (int y = 1; y < CA3D_D-1; y++)
            for (int z = 1; z < CA3D_D-1; z++) {
                int n = 0;
                for (int dx=-1;dx<=1;dx++)
                  for (int dy=-1;dy<=1;dy++)
                    for (int dz=-1;dz<=1;dz++)
                        if (!(dx==0&&dy==0&&dz==0))
                            n += ca3d_a[x+dx][y+dy][z+dz];
                int live = ca3d_a[x][y][z];
                ca3d_b[x][y][z] = (live && n>=4 && n<=6) || (!live && n==5);
                acc += ca3d_b[x][y][z];
            }
        memcpy(ca3d_a, ca3d_b, sizeof(ca3d_a));
    }
    return acc;
}

/* ── algorithm dispatch table ────────────────────────────────────────────── */
typedef uint64_t (*algo_fn)(uint64_t seed);
static const algo_fn algos[CHAOS_ALGOS] = {
    algo_mandelbrot, algo_gol,   algo_sieve, algo_sort, algo_fib,
    algo_xor_tree,   algo_kmp,   algo_bfs,   algo_rc4,  algo_ca3d
};
static const char *algo_names[CHAOS_ALGOS] = {
    "mandelbrot","game_of_life","sieve","quicksort","fibonacci_mod",
    "xor_tree","kmp_match","bfs_graph","rc4_cipher","ca_3d"
};

/* ── parallel worker ─────────────────────────────────────────────────────── */
static void *chaos_worker(void *arg) {
    parallel_arg_t *a = (parallel_arg_t *)arg;
    platform_pin_thread(a->core_id);

    uint64_t rng   = a->seed;
    uint64_t iters = 0;
    uint64_t acc   = a->seed;
    double   t0    = bench_now_sec();
    double   deadline = t0 + a->duration_sec;

    while (bench_now_sec() < deadline) {
        int which = (int)(xorshift64(&rng) % CHAOS_ALGOS);
        uint64_t result = algos[which](acc);
        acc = mix64(acc, result);
        iters++;
    }

    a->ops = (double)iters / (bench_now_sec() - t0);
    a->hash_out = mix64(a->seed, acc);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_exotic_chaos(uint64_t chain_seed,
                                    int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "exotic_chaos";
    r.chain_in    = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec,
                 chaos_worker, &total_ops, &combined_hash);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec   = total_ops;
    r.score         = total_ops;
    r.chain_out     = mix64(chain_seed, combined_hash);
    snprintf(r.flags, sizeof(r.flags), "CHAOS_RANDOM_SEQ ncores=%d", ncores);
    snprintf(r.notes, sizeof(r.notes), "all-core random algorithm dispatch");

    return r;
}

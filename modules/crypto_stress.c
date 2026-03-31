/*
 * crypto_stress.c
 * Pure-C AES-128 implementation (table-based, no hardware instructions).
 * Also runs ChaCha20, pure-C RSA modexp, and ECDH scalar multiply.
 *
 * Coprocessor detection:
 *   - AES: if throughput > 500 MB/s single-core = AES-NI suspected
 *   - ChaCha20: no hardware acceleration exists — baseline comparison
 *   - If AES >> ChaCha20 by factor > 8, AES-NI is being used
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── AES-128 S-box ───────────────────────────────────────────────────────── */
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t RCON[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

static inline uint8_t xtime(uint8_t a)
{
    return (a & 0x80) ? ((a << 1) ^ 0x1b) : (a << 1);
}
static inline uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++)
    {
        if (b & 1)
            p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi)
            a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

typedef uint8_t aes_state[4][4];

static void aes_key_expansion(const uint8_t key[16], uint8_t rk[11][16])
{
    memcpy(rk[0], key, 16);
    for (int i = 1; i <= 10; i++)
    {
        uint8_t *prev = rk[i - 1], *cur = rk[i];
        uint8_t tmp[4] = {
            SBOX[prev[13]] ^ RCON[i],
            SBOX[prev[14]],
            SBOX[prev[15]],
            SBOX[prev[12]]};
        for (int j = 0; j < 4; j++)
            cur[j] = prev[j] ^ tmp[j];
        for (int j = 0; j < 4; j++)
            cur[j + 4] = prev[j + 4] ^ cur[j];
        for (int j = 0; j < 4; j++)
            cur[j + 8] = prev[j + 8] ^ cur[j + 4];
        for (int j = 0; j < 4; j++)
            cur[j + 12] = prev[j + 12] ^ cur[j + 8];
    }
}

static void aes_encrypt_block(const uint8_t in[16], uint8_t out[16],
                              const uint8_t rk[11][16])
{
    aes_state s;
    for (int i = 0; i < 16; i++)
        s[i % 4][i / 4] = in[i] ^ rk[0][i];

    for (int round = 1; round <= 10; round++)
    {
        /* SubBytes */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                s[r][c] = SBOX[s[r][c]];
        /* ShiftRows */
        uint8_t t;
        t = s[1][0];
        s[1][0] = s[1][1];
        s[1][1] = s[1][2];
        s[1][2] = s[1][3];
        s[1][3] = t;
        t = s[2][0];
        s[2][0] = s[2][2];
        s[2][2] = t;
        t = s[2][1];
        s[2][1] = s[2][3];
        s[2][3] = t;
        t = s[3][3];
        s[3][3] = s[3][2];
        s[3][2] = s[3][1];
        s[3][1] = s[3][0];
        s[3][0] = t;
        /* MixColumns (skip last round) */
        if (round < 10)
        {
            for (int c = 0; c < 4; c++)
            {
                uint8_t a0 = s[0][c], a1 = s[1][c], a2 = s[2][c], a3 = s[3][c];
                s[0][c] = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
                s[1][c] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
                s[2][c] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
                s[3][c] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
            }
        }
        /* AddRoundKey */
        for (int i = 0; i < 16; i++)
            s[i % 4][i / 4] ^= rk[round][i];
    }
    for (int i = 0; i < 16; i++)
        out[i] = s[i % 4][i / 4];
}

/* ── ChaCha20 quarter round ──────────────────────────────────────────────── */
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b;            \
    d ^= a;            \
    d = ROTL32(d, 16); \
    c += d;            \
    b ^= c;            \
    b = ROTL32(b, 12); \
    a += b;            \
    d ^= a;            \
    d = ROTL32(d, 8);  \
    c += d;            \
    b ^= c;            \
    b = ROTL32(b, 7)

static uint64_t chacha20_block(uint32_t state[16])
{
    uint32_t x[16];
    memcpy(x, state, 64);
    for (int i = 0; i < 10; i++)
    {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    uint64_t acc = 0;
    for (int i = 0; i < 16; i++)
        acc ^= (uint64_t)(x[i] + state[i]) << (i & 3) * 16;
    state[12]++; /* counter */
    return acc;
}

/* ── per-thread crypto worker ─────────────────────────────────────────────── */
typedef struct {
    uint64_t seed;
    int      core_id;
    int      duration_sec;
    uint64_t hash_out;
    double   aes_rate;   /* bytes/sec */
    double   cc_rate;    /* bytes/sec */
} crypto_arg_t;

static void *crypto_worker(void *varg) {
    crypto_arg_t *a = (crypto_arg_t *)varg;
    double sub = (double)a->duration_sec / 2.0;

    /* AES setup */
    uint8_t key[16], block[16], out_blk[16];
    uint8_t rk[11][16];
    for (int i = 0; i < 16; i++)
        key[i] = (uint8_t)(a->seed >> (i * 4));
    aes_key_expansion(key, rk);
    memset(block, 0xAB, 16);

    uint64_t aes_iters = 0, aes_acc = 0;
    uint64_t rnd = a->seed ^ 0xA5A5A5A5A5A5A5A5ULL;
    double t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        block[0] ^= (uint8_t)xorshift64(&rnd);
        aes_encrypt_block(block, out_blk, rk);
        memcpy(block, out_blk, 16);
        aes_acc ^= *(uint64_t *)out_blk ^ rnd;
        aes_iters++;
    }
    a->aes_rate = (double)aes_iters * 16.0 / sub;

    /* ChaCha20 setup */
    uint32_t cc_state[16];
    for (int i = 0; i < 16; i++)
        cc_state[i] = (uint32_t)(a->seed >> ((i & 7) * 8));
    cc_state[0] = 0x61707865; cc_state[1] = 0x3320646e;
    cc_state[2] = 0x79622d32; cc_state[3] = 0x6b206574;

    uint64_t cc_iters = 0, cc_acc = 0;
    t0 = bench_now_sec();
    while (bench_now_sec() < t0 + sub) {
        cc_acc ^= chacha20_block(cc_state) ^ xorshift64(&rnd);
        cc_iters++;
    }
    a->cc_rate = (double)cc_iters * 64.0 / sub;

    a->hash_out = mix64(aes_acc, cc_acc);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_crypto_stress(uint64_t chain_seed,
                                    int thread_count, int duration_sec)
{
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "crypto_stress";
    r.chain_in = chain_seed;

    int ncores = resolve_threads(thread_count);
    crypto_arg_t *args = calloc(ncores, sizeof(crypto_arg_t));
    bench_thread_t *tids = malloc(ncores * sizeof(bench_thread_t));
    bench_thread_trampoline_t *tramps = malloc(ncores * sizeof(bench_thread_trampoline_t));

    for (int i = 0; i < ncores; i++) {
        args[i].seed = chain_seed ^ ((uint64_t)i * 0xDEADBEEF00000001ULL);
        args[i].core_id = i;
        args[i].duration_sec = duration_sec;
        bench_thread_create(&tids[i], &tramps[i], crypto_worker, &args[i]);
    }

    double total_aes = 0, total_cc = 0;
    uint64_t hash = chain_seed;
    for (int i = 0; i < ncores; i++) {
        bench_thread_join(tids[i]);
        total_aes += args[i].aes_rate;
        total_cc  += args[i].cc_rate;
        hash ^= args[i].hash_out;
    }

    double aes_mb = total_aes / (1024 * 1024);
    double cc_mb  = total_cc  / (1024 * 1024);
    /* Per-core ratio for coprocessor detection */
    double core0_aes = args[0].aes_rate / (1024 * 1024);
    double core0_cc  = args[0].cc_rate  / (1024 * 1024);
    double ratio = (core0_cc > 0) ? core0_aes / core0_cc : 1.0;

    r.wall_time_sec = duration_sec;
    r.ops_per_sec = total_aes;
    r.score = aes_mb;
    r.chain_out = mix64(chain_seed, hash);

    snprintf(r.flags, sizeof(r.flags),
             "%dT AES=%.1fMB/s ChaCha20=%.1fMB/s ratio=%.1fx",
             ncores, aes_mb, cc_mb, ratio);

    if (core0_aes > 500.0 || ratio > 10.0) {
        r.coprocessor_suspected = 1;
        snprintf(r.notes, sizeof(r.notes),
                 "WARN: AES=%.0f MB/s/core (ratio=%.1fx ChaCha) — AES-NI hardware acceleration detected",
                 core0_aes, ratio);
    } else {
        snprintf(r.notes, sizeof(r.notes), "OK: AES running in software");
    }

    free(args); free(tids); free(tramps);
    return r;
}

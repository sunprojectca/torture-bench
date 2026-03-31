/*
 * hash_chain.c
 * Pure-C SHA-256 implementation — deliberately NOT using OpenSSL or any
 * hardware AES/SHA extensions so results reflect raw CPU integer throughput.
 *
 * The output hash becomes the seed for the next module (chain mechanism).
 */

#include "../harness/common.h"
#include "../harness/parallel_runner.h"
#include "anticache_guard.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── SHA-256 constants ───────────────────────────────────────────────────── */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)   (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)  (((x)&(y))^((x)&(z))^((y)&(z)))
#define S0(x)  (ROTR32(x,2)^ROTR32(x,13)^ROTR32(x,22))
#define S1(x)  (ROTR32(x,6)^ROTR32(x,11)^ROTR32(x,25))
#define s0(x)  (ROTR32(x,7)^ROTR32(x,18)^((x)>>3))
#define s1(x)  (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

typedef struct {
    uint32_t state[8];
    uint8_t  buf[64];
    uint64_t count;
} sha256_ctx;

static void sha256_init(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_transform(sha256_ctx *ctx, const uint8_t *data) {
    uint32_t w[64], a,b,c,d,e,f,g,h,t1,t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)data[i*4]<<24)|((uint32_t)data[i*4+1]<<16)|
               ((uint32_t)data[i*4+2]<<8)|(uint32_t)data[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + S1(e) + CH(e,f,g) + K[i] + w[i];
        t2 = S0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len) {
    size_t i = (size_t)(ctx->count & 63);
    ctx->count += len;
    for (size_t j = 0; j < len; j++) {
        ctx->buf[i++] = data[j];
        if (i == 64) { sha256_transform(ctx, ctx->buf); i = 0; }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[32]) {
    uint64_t bits = ctx->count * 8;
    uint8_t pad[72]; int padlen;
    size_t i = (size_t)(ctx->count & 63);
    padlen = (i < 56) ? (56 - (int)i) : (120 - (int)i);
    memset(pad, 0, padlen);
    pad[0] = 0x80;
    for (int k = 0; k < 8; k++) pad[padlen+k] = (uint8_t)(bits >> (56-8*k));
    sha256_update(ctx, pad, padlen + 8);
    for (int k = 0; k < 8; k++) {
        out[k*4]   = (uint8_t)(ctx->state[k] >> 24);
        out[k*4+1] = (uint8_t)(ctx->state[k] >> 16);
        out[k*4+2] = (uint8_t)(ctx->state[k] >>  8);
        out[k*4+3] = (uint8_t)(ctx->state[k]);
    }
}

/* ── public: hash a buffer, return first 8 bytes as uint64 ──────────────── */
uint64_t hash_chain_sha256(const uint8_t *data, size_t len) {
    sha256_ctx ctx;
    uint8_t digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
    uint64_t out = 0;
    for (int i = 0; i < 8; i++) out = (out << 8) | digest[i];
    return out;
}

/* ── per-thread hash worker ───────────────────────────────────────────────── */
static void *hash_worker(void *varg) {
    parallel_arg_t *a = (parallel_arg_t *)varg;

    const size_t MSG_LEN = 65536;
    uint8_t *msg = (uint8_t *)malloc(MSG_LEN);
    uint64_t state = a->seed ^ 0xdeadbeefcafe0000ULL;
    for (size_t i = 0; i < MSG_LEN; i += 8) {
        uint64_t v = xorshift64(&state);
        memcpy(msg + i, &v, 8);
    }

    uint64_t iters = 0;
    uint64_t running_hash = a->seed;
    double t0 = bench_now_sec();
    double deadline = t0 + a->duration_sec;

    while (bench_now_sec() < deadline) {
        memcpy(msg, &running_hash, 8);
        running_hash = hash_chain_sha256(msg, MSG_LEN);
        iters++;
    }

    double elapsed = bench_now_sec() - t0;
    a->ops = (double)iters / elapsed;
    a->hash_out = running_hash;
    free(msg);
    return NULL;
}

/* ── module entry point ──────────────────────────────────────────────────── */
bench_result_t module_hash_chain(uint64_t chain_seed,
                                  int thread_count, int duration_sec) {
    bench_result_t r;
    memset(&r, 0, sizeof(r));
    r.module_name = "hash_chain";
    r.chain_in    = chain_seed;

    int ncores = resolve_threads(thread_count);
    double total_ops;
    uint64_t combined_hash;
    parallel_run(ncores, chain_seed, duration_sec, hash_worker,
                 &total_ops, &combined_hash);

    r.wall_time_sec = duration_sec;
    r.ops_per_sec   = total_ops;
    r.score         = total_ops / 1000.0; /* kH/s */
    r.chain_out     = combined_hash;

    /* Per-core rate for coprocessor detection */
    double per_core = total_ops / ncores;
    if (per_core > 500000.0) {
        r.coprocessor_suspected = 1;
        snprintf(r.flags, sizeof(r.flags),
            "%dT WARN: %.0f H/s/core — possible SHA-NI hardware acceleration",
            ncores, per_core);
    } else {
        snprintf(r.flags, sizeof(r.flags), "%dT PURE_C_SHA256", ncores);
    }

    return r;
}

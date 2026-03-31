#pragma once
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ── result from a single module run ─────────────────────────────────────── */
typedef struct
{
    const char *module_name;
    double score;              /* normalized score, higher = better        */
    double wall_time_sec;      /* actual elapsed wall time                 */
    double ops_per_sec;        /* raw throughput metric                    */
    uint64_t chain_in;         /* hash fed in from previous module         */
    uint64_t chain_out;        /* hash this module emits to next           */
    int coprocessor_suspected; /* 1 if offload detected             */
    char flags[256];           /* e.g. "NEON AVX2 PURE_C"                 */
    char notes[512];           /* any warnings, anomalies                  */
} bench_result_t;

/* ── module descriptor ────────────────────────────────────────────────────── */
typedef struct
{
    const char *name;
    const char *description;
    int enabled;
    /* run the module; chain_seed is the previous module's chain_out        */
    bench_result_t (*run)(uint64_t chain_seed, int thread_count, int tuning_mode);
} bench_module_t;

/* ── global config ────────────────────────────────────────────────────────── */
typedef struct
{
    int thread_count; /* 0 = auto-detect                          */
    int duration_sec; /* per-module run time                      */
    int tuning_mode;  /* 1 = run anti-cheat tuning probe first    */
    int verbose;
    int json_output;
    char output_file[512];
    uint64_t initial_seed; /* master random seed                       */
} bench_config_t;

/* ── timing helpers ───────────────────────────────────────────────────────── */
static inline double bench_now_sec(void)
{
#ifdef OS_WINDOWS
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER count;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* ── portable popcount ────────────────────────────────────────────────────── */
static inline int bench_popcount64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#elif defined(_MSC_VER)
    return (int)__popcnt64(x);
#else
    int c = 0;
    while (x)
    {
        c += x & 1;
        x >>= 1;
    }
    return c;
#endif
}

/* ── xorshift64 — fast, no stdlib rand() ────────────────────────────────── */
static inline uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (*state = x);
}

/* ── mix two 64-bit values (avalanche) ───────────────────────────────────── */
static inline uint64_t mix64(uint64_t a, uint64_t b)
{
    a ^= b;
    a *= 0x9e3779b97f4a7c15ULL;
    a ^= a >> 30;
    a *= 0xbf58476d1ce4e5b9ULL;
    a ^= a >> 27;
    return a;
}

/* ── entropy spacer: random work between benchmark functions ────────────── */
static inline void bench_entropy_spacer(uint64_t *rnd_state,
                                        volatile uint8_t *buf,
                                        size_t buf_size,
                                        int rounds)
{
    if (!rnd_state)
        return;

    uint64_t rnd = *rnd_state ? *rnd_state : 0x9e3779b97f4a7c15ULL;
    volatile uint64_t acc = rnd;
    int n = (rounds > 0) ? rounds : 128;

    for (int i = 0; i < n; i++)
    {
        uint64_t r = xorshift64(&rnd);
        acc ^= mix64(r, acc + (uint64_t)i);
        acc += (r >> (i & 15));

        if (buf && buf_size > 0)
        {
            size_t idx = (size_t)(r % buf_size);
            buf[idx] ^= (uint8_t)(r & 0xFF);
            acc += buf[idx];
        }
    }

    *rnd_state = rnd ^ acc;
}

#endif /* BENCH_COMMON_H */

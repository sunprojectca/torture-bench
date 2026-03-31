#include "anticache_guard.h"
#include "../harness/common.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_MSC_VER) && defined(ARCH_X86_64)
#  include <immintrin.h>
#endif

#ifdef OS_LINUX
#  include <sys/mman.h>
#endif

/* ── portable cache line flush ────────────────────────────────────────────── */
static inline void clflush_line(void *p) {
#if defined(ARCH_X86_64)
#  if defined(_MSC_VER)
    _mm_clflush(p);
#  else
    __asm__ volatile ("clflush (%0)" :: "r"(p) : "memory");
#  endif
#elif defined(ARCH_ARM64)
#  if defined(_MSC_VER)
    (void)p;
    MemoryBarrier();
#  else
    /* DC CIVAC — clean and invalidate by VA to PoC */
    __asm__ volatile ("dc civac, %0" :: "r"(p) : "memory");
#  endif
#else
    (void)p; /* fallback: memory barrier only */
#  if defined(_MSC_VER)
    _ReadWriteBarrier();
#  else
    __asm__ volatile ("" ::: "memory");
#  endif
#endif
}

/* ── allocate large poison buffer ─────────────────────────────────────────── */
void *anticache_alloc(size_t size_bytes) {
#ifdef OS_LINUX
    void *p = mmap(NULL, size_bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                   -1, 0);
    if (p == MAP_FAILED) return NULL;
    /* Advise kernel not to use huge pages — we want fine-grained control  */
    madvise(p, size_bytes, MADV_RANDOM);
    return p;
#else
    return malloc(size_bytes);
#endif
}

void anticache_free(void *buf, size_t size_bytes) {
#ifdef OS_LINUX
    munmap(buf, size_bytes);
#else
    (void)size_bytes;
    free(buf);
#endif
}

/* ── flush via random-stride traversal ───────────────────────────────────── */
void anticache_flush(void *buf, size_t size_bytes) {
    volatile uint8_t *p = (volatile uint8_t *)buf;
    const size_t stride = 64; /* cache line size */

    /* Forward pass — sequential, warms then evicts */
    for (size_t i = 0; i < size_bytes; i += stride) {
        p[i] ^= (uint8_t)(i & 0xFF);
    }

    /* Reverse pass — evicts L1/L2 */
    for (size_t i = size_bytes; i >= stride; i -= stride) {
        p[i - stride] ^= (uint8_t)((i >> 6) & 0xFF);
    }

    /* Explicit cache line flush where available */
    for (size_t i = 0; i < size_bytes; i += stride) {
        clflush_line((void *)(p + i));
    }

    anticache_barrier();
}

/* ── full barrier ────────────────────────────────────────────────────────── */
void anticache_barrier(void) {
#if defined(ARCH_X86_64)
#  if defined(_MSC_VER)
    _mm_mfence();
#  else
    __asm__ volatile ("mfence" ::: "memory");
#  endif
#elif defined(ARCH_ARM64)
#  if defined(_MSC_VER)
    MemoryBarrier();
#  else
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb"    ::: "memory");
#  endif
#else
#  if defined(_MSC_VER)
    _ReadWriteBarrier();
#  else
    __asm__ volatile ("" ::: "memory");
#  endif
#endif
}

/* ── probe: run fn twice, detect cache warming ───────────────────────────── */
anticache_report_t anticache_probe(
    void (*fn)(void *arg), void *arg,
    void *poison_buf, size_t poison_size)
{
    anticache_report_t r;
    memset(&r, 0, sizeof(r));

    /* First run — cold cache */
    anticache_flush(poison_buf, poison_size);
    anticache_barrier();
    double t0 = bench_now_sec();
    fn(arg);
    r.first_run_time = bench_now_sec() - t0;

    /* Second run — warm cache (no flush between) */
    anticache_barrier();
    t0 = bench_now_sec();
    fn(arg);
    r.second_run_time = bench_now_sec() - t0;

    /* Compute ratio */
    if (r.second_run_time > 0.0)
        r.ratio = r.first_run_time / r.second_run_time;
    else
        r.ratio = 1.0;

    /*
     * If second run is >30% faster than first, caches are helping.
     * If second run is >2x faster, coprocessor or aggressive prefetch.
     */
    if (r.ratio > 2.0) {
        r.cache_cheat_suspected = 1;
        snprintf(r.verdict, sizeof(r.verdict),
            "WARN: 2nd run %.1fx faster — aggressive prefetch or coprocessor offload suspected",
            r.ratio);
    } else if (r.ratio > 1.3) {
        r.cache_cheat_suspected = 0; /* normal cache warming */
        snprintf(r.verdict, sizeof(r.verdict),
            "INFO: 2nd run %.1fx faster — normal L3 cache warm effect",
            r.ratio);
    } else {
        snprintf(r.verdict, sizeof(r.verdict),
            "OK: runs consistent (ratio=%.2f)", r.ratio);
    }

    return r;
}

int anticache_suspicious(double observed_sec, double baseline_sec,
                          double threshold_ratio) {
    if (baseline_sec <= 0.0) return 0;
    double ratio = baseline_sec / observed_sec;
    return ratio > threshold_ratio;
}

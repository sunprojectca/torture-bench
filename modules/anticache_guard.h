#pragma once
#ifndef ANTICACHE_GUARD_H
#define ANTICACHE_GUARD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Anti-cache-cheat subsystem.
 *
 * Some CPUs (and some OS schedulers in combination with hardware prefetchers)
 * can make a benchmark look faster by pre-warming caches between runs.
 * This module:
 *   1. Flushes L1/L2/L3 by touching a large poison buffer
 *   2. Randomizes memory access patterns so prefetchers can't learn them
 *   3. Issues memory barriers to prevent speculative execution leaking
 *   4. Detects suspiciously fast repeat runs (cache-warm detection)
 */

typedef struct {
    double first_run_time;
    double second_run_time;
    double ratio;               /* >1.3 = suspicious cache warm between runs */
    int    cache_cheat_suspected;
    char   verdict[256];
} anticache_report_t;

/* Allocate and return a poison buffer large enough to evict all caches.
   size_bytes should be > 2 * L3 cache size.                              */
void *anticache_alloc(size_t size_bytes);
void  anticache_free(void *buf, size_t size_bytes);

/* Thrash the poison buffer to evict caches before a module run           */
void  anticache_flush(void *buf, size_t size_bytes);

/* Full memory + compiler barrier                                         */
void  anticache_barrier(void);

/* Run fn twice with a flush between, compare times, report suspicion     */
anticache_report_t anticache_probe(
    void (*fn)(void *arg), void *arg,
    void *poison_buf, size_t poison_size);

/* Detect if a benchmark result is suspiciously fast compared to baseline */
int anticache_suspicious(double observed_sec, double baseline_sec,
                          double threshold_ratio);

#endif

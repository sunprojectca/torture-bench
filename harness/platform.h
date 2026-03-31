#pragma once
#ifndef BENCH_PLATFORM_H
#define BENCH_PLATFORM_H

#include <stdint.h>

typedef struct {
    char os[64];
    char arch[64];
    char cpu_brand[256];
    int  logical_cores;
    int  physical_cores;
    long ram_bytes;
    int  has_neon;
    int  has_avx2;
    int  has_avx512;
    int  has_sve;          /* ARM SVE                                       */
    int  is_apple_silicon;
    int  is_snapdragon;
    int  is_wsl;
    int  cache_l1_kb;
    int  cache_l2_kb;
    int  cache_l3_kb;
} platform_info_t;

void     platform_detect(platform_info_t *info);
void     platform_print(const platform_info_t *info);
int      platform_get_ncpus(void);
uint64_t platform_rdtsc(void);   /* cycle counter where available          */
void     platform_pin_thread(int core_id); /* pin calling thread to core   */

#endif

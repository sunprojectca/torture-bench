#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef OS_LINUX
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <sys/utsname.h>
#endif

#ifdef OS_MACOS
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <pthread.h>
#endif

/* ── cpu brand string ─────────────────────────────────────────────────────── */
static void get_cpu_brand(char *out, int maxlen)
{
    if (!out || maxlen <= 0)
        return;

    out[0] = '\0';

#if defined(ARCH_X86_64) && (defined(__GNUC__) || defined(__clang__))
    /* CPUID leaf 0x80000002..4 — GCC/Clang inline asm */
    uint32_t regs[4];
    char brand[49];
    memset(brand, 0, sizeof(brand));
    for (int i = 0; i < 3; i++)
    {
        __asm__ volatile(
            "cpuid"
            : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
            : "a"(0x80000002 + i));
        memcpy(brand + i * 16, &regs[0], 4);
        memcpy(brand + i * 16 + 4, &regs[1], 4);
        memcpy(brand + i * 16 + 8, &regs[2], 4);
        memcpy(brand + i * 16 + 12, &regs[3], 4);
    }
    strncpy(out, brand, maxlen - 1);
#elif defined(ARCH_X86_64) && defined(_MSC_VER)
    /* CPUID via MSVC intrinsic */
    int regs[4];
    char brand[49];
    memset(brand, 0, sizeof(brand));
    for (int i = 0; i < 3; i++)
    {
        __cpuid(regs, 0x80000002 + i);
        memcpy(brand + i * 16,      &regs[0], 4);
        memcpy(brand + i * 16 + 4,  &regs[1], 4);
        memcpy(brand + i * 16 + 8,  &regs[2], 4);
        memcpy(brand + i * 16 + 12, &regs[3], 4);
    }
    strncpy(out, brand, maxlen - 1);
#elif defined(OS_MACOS)
    size_t sz = maxlen;
    sysctlbyname("machdep.cpu.brand_string", out, &sz, NULL, 0);
#elif defined(OS_LINUX)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
    {
        strncpy(out, "unknown", maxlen - 1);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "model name", 10) == 0 ||
            strncmp(line, "Model name", 10) == 0 ||
            strncmp(line, "Processor", 9) == 0)
        {
            char *p = strchr(line, ':');
            if (p)
            {
                p += 2;
                char *nl = strchr(p, '\n');
                if (nl)
                    *nl = 0;
                strncpy(out, p, maxlen - 1);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
    strncpy(out, "unknown", maxlen - 1);
#elif defined(OS_WINDOWS) && defined(ARCH_ARM64)
    /* Windows on ARM — read from registry */
    HKEY hKey = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD sz = (DWORD)maxlen;
        LONG status = RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL,
                                       (LPBYTE)out, &sz);
        RegCloseKey(hKey);
        if (status == ERROR_SUCCESS && out[0] != '\0')
        {
            out[maxlen - 1] = '\0';
            return;
        }
    }

    strncpy(out, "Windows ARM64 CPU", maxlen - 1);
    out[maxlen - 1] = '\0';
#else
    strncpy(out, "unknown", maxlen - 1);
#endif
}

/* ── cache sizes ──────────────────────────────────────────────────────────── */
static void get_cache_sizes(int *l1, int *l2, int *l3)
{
    *l1 = 32;
    *l2 = 256;
    *l3 = 8192; /* safe defaults in KB */
#if defined(OS_LINUX)
    FILE *f;
    char buf[64];
    for (int i = 0; i < 8; i++)
    {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu0/cache/index%d/size", i);
        f = fopen(path, "r");
        if (!f)
            break;
        if (fgets(buf, sizeof(buf), f))
        {
            int sz = atoi(buf);
            char lpath[128];
            snprintf(lpath, sizeof(lpath),
                     "/sys/devices/system/cpu/cpu0/cache/index%d/level", i);
            FILE *lf = fopen(lpath, "r");
            if (lf)
            {
                int lv = 0;
                if (fscanf(lf, "%d", &lv) != 1)
                    lv = 0;
                fclose(lf);
                if (lv == 1)
                    *l1 = sz;
                else if (lv == 2)
                    *l2 = sz;
                else if (lv == 3)
                    *l3 = sz;
            }
        }
        fclose(f);
    }
#elif defined(OS_MACOS)
    uint64_t val;
    size_t sz = sizeof(val);
    if (!sysctlbyname("hw.l1dcachesize", &val, &sz, NULL, 0))
        *l1 = (int)(val / 1024);
    sz = sizeof(val);
    if (!sysctlbyname("hw.l2cachesize", &val, &sz, NULL, 0))
        *l2 = (int)(val / 1024);
    sz = sizeof(val);
    if (!sysctlbyname("hw.l3cachesize", &val, &sz, NULL, 0))
        *l3 = (int)(val / 1024);
#elif defined(OS_WINDOWS)
    DWORD bufLen = 0;
    GetLogicalProcessorInformation(NULL, &bufLen);
    if (bufLen > 0)
    {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(bufLen);
        if (buf && GetLogicalProcessorInformation(buf, &bufLen))
        {
            DWORD count = bufLen / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            for (DWORD i = 0; i < count; i++)
            {
                if (buf[i].Relationship == RelationCache)
                {
                    CACHE_DESCRIPTOR *c = &buf[i].Cache;
                    int kb = (int)(c->Size / 1024);
                    if (c->Level == 1 && c->Type == CacheData)
                        *l1 = kb;
                    else if (c->Level == 2)
                        *l2 = kb;
                    else if (c->Level == 3)
                        *l3 = kb;
                }
            }
        }
        free(buf);
    }
#endif
}

/* ── SIMD feature detection ───────────────────────────────────────────────── */
static void detect_simd(platform_info_t *info)
{
#if defined(ARCH_ARM64)
    info->has_neon = 1; /* mandatory on AArch64 */
#if defined(OS_LINUX)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            if (strstr(line, "Features") && strstr(line, " sve"))
                info->has_sve = 1;
        }
        fclose(f);
    }
#elif defined(OS_WINDOWS)
    if (IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE))
        info->has_neon = 1;
    /* Windows doesn't expose SVE detection easily */
#endif
#elif defined(ARCH_X86_64)
#if defined(__GNUC__) || defined(__clang__)
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    uint32_t ebx7 = 0;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx7), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    info->has_avx2 = (ebx7 >> 5) & 1;
    info->has_avx512 = (ebx7 >> 16) & 1;
#elif defined(_MSC_VER)
    int regs[4];
    __cpuid(regs, 7);
    info->has_avx2 = (regs[1] >> 5) & 1;
    info->has_avx512 = (regs[1] >> 16) & 1;
#endif
#endif
}

/* ── Apple Silicon / Snapdragon detection ────────────────────────────────── */
static int detect_wsl(void)
{
#ifdef OS_LINUX
    FILE *f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f)
    {
        char line[256];
        if (fgets(line, sizeof(line), f))
        {
            fclose(f);
            if (strstr(line, "Microsoft") || strstr(line, "microsoft"))
                return 1;
        }
        else
        {
            fclose(f);
        }
    }

    f = fopen("/proc/version", "r");
    if (f)
    {
        char line[256];
        if (fgets(line, sizeof(line), f))
        {
            fclose(f);
            if (strstr(line, "Microsoft") || strstr(line, "microsoft"))
                return 1;
        }
        else
        {
            fclose(f);
        }
    }
#endif
    return 0;
}

/* ── Apple Silicon / Snapdragon detection ────────────────────────────────── */
static void detect_soc(platform_info_t *info)
{
#ifdef OS_MACOS
    char chip[128] = {0};
    size_t sz = sizeof(chip);
    sysctlbyname("machdep.cpu.brand_string", chip, &sz, NULL, 0);
    if (strstr(chip, "Apple M") || strstr(chip, "Apple A"))
        info->is_apple_silicon = 1;
#endif
#ifdef OS_LINUX
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            if (strstr(line, "Snapdragon") || strstr(line, "Qualcomm"))
                info->is_snapdragon = 1;
        }
        fclose(f);
    }
#endif
#ifdef OS_WINDOWS
    /* Check brand string for Snapdragon / Qualcomm on WoA devices */
    if (strstr(info->cpu_brand, "Snapdragon") ||
    strstr(info->cpu_brand, "snapdragon") ||
        strstr(info->cpu_brand, "Qualcomm"))
        info->is_snapdragon = 1;
#endif
}

/* ── RAM ─────────────────────────────────────────────────────────────────── */
static long get_ram_bytes(void)
{
#ifdef OS_LINUX
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return pages * page_size;
#elif defined(OS_MACOS)
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
    return (long)mem;
#elif defined(OS_WINDOWS)
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    return (long)ms.ullTotalPhys;
#else
    return 0;
#endif
}

/* ── public API ───────────────────────────────────────────────────────────── */
void platform_detect(platform_info_t *info)
{
    memset(info, 0, sizeof(*info));

#ifdef OS_LINUX
    strncpy(info->os, "Linux", sizeof(info->os) - 1);
    info->is_wsl = detect_wsl();
    if (info->is_wsl)
        strncpy(info->os, "WSL", sizeof(info->os) - 1);
#elif defined(OS_MACOS)
    strncpy(info->os, "macOS", sizeof(info->os) - 1);
#elif defined(OS_WINDOWS)
    strncpy(info->os, "Windows", sizeof(info->os) - 1);
#else
    strncpy(info->os, "Unknown", sizeof(info->os) - 1);
#endif

#if defined(ARCH_ARM64)
    strncpy(info->arch, "arm64", sizeof(info->arch) - 1);
#elif defined(ARCH_X86_64)
    strncpy(info->arch, "x86_64", sizeof(info->arch) - 1);
#elif defined(ARCH_RISCV64)
    strncpy(info->arch, "riscv64", sizeof(info->arch) - 1);
#elif defined(ARCH_PPC64)
    strncpy(info->arch, "ppc64", sizeof(info->arch) - 1);
#else
    strncpy(info->arch, "generic", sizeof(info->arch) - 1);
#endif

    get_cpu_brand(info->cpu_brand, sizeof(info->cpu_brand));
    info->logical_cores = platform_get_ncpus();
    info->ram_bytes = get_ram_bytes();
    detect_simd(info);
    detect_soc(info);
    get_cache_sizes(&info->cache_l1_kb, &info->cache_l2_kb, &info->cache_l3_kb);
}

void platform_print(const platform_info_t *info)
{
    printf("  OS      : %s\n", info->os);
    printf("  Arch    : %s\n", info->arch);
    printf("  CPU     : %s\n", info->cpu_brand);
    printf("  Cores   : %d logical\n", info->logical_cores);
    printf("  RAM     : %.1f GB\n", (double)info->ram_bytes / (1024.0 * 1024.0 * 1024.0));
    printf("  L1/L2/L3: %d / %d / %d KB\n",
           info->cache_l1_kb, info->cache_l2_kb, info->cache_l3_kb);
    printf("  SIMD    :");
    if (info->has_neon)
        printf(" NEON");
    if (info->has_sve)
        printf(" SVE");
    if (info->has_avx2)
        printf(" AVX2");
    if (info->has_avx512)
        printf(" AVX512");
    printf("\n");
    if (info->is_apple_silicon)
        printf("  SOC     : Apple Silicon\n");
    if (info->is_snapdragon)
        printf("  SOC     : Snapdragon\n");
    if (info->is_wsl)
        printf("  ENV     : WSL\n");
}

int platform_get_ncpus(void)
{
#ifdef OS_LINUX
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(OS_MACOS)
    int n = 1;
    size_t sz = sizeof(n);
    sysctlbyname("hw.logicalcpu", &n, &sz, NULL, 0);
    return n;
#elif defined(OS_WINDOWS)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    return 1;
#endif
}

uint64_t platform_rdtsc(void)
{
#if defined(ARCH_X86_64) && (defined(__GNUC__) || defined(__clang__))
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(ARCH_X86_64) && defined(_MSC_VER)
    return __rdtsc();
#elif defined(ARCH_ARM64) && (defined(__GNUC__) || defined(__clang__))
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(OS_WINDOWS)
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)count.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

void platform_pin_thread(int core_id)
{
#ifdef OS_LINUX
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core_id, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#elif defined(OS_WINDOWS)
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << core_id);
#else
    (void)core_id; /* macOS doesn't allow pinning */
#endif
}

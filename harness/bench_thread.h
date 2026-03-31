/*
 * bench_thread.h — Portable threading abstraction
 *
 * Maps to pthreads on Linux/macOS, Win32 threads on Windows.
 * Drop-in replacement: use bench_thread_t, bench_thread_create, bench_thread_join.
 */
#pragma once
#ifndef BENCH_THREAD_H
#define BENCH_THREAD_H

#ifdef OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef HANDLE bench_thread_t;

typedef struct {
    void *(*func)(void *);
    void *arg;
} bench_thread_trampoline_t;

static DWORD WINAPI bench_thread_entry(LPVOID param)
{
    bench_thread_trampoline_t *t = (bench_thread_trampoline_t *)param;
    t->func(t->arg);
    return 0;
}

static inline int bench_thread_create(bench_thread_t *thread,
                                       bench_thread_trampoline_t *tramp,
                                       void *(*func)(void *), void *arg)
{
    tramp->func = func;
    tramp->arg = arg;
    *thread = CreateThread(NULL, 0, bench_thread_entry, tramp, 0, NULL);
    return (*thread != NULL) ? 0 : -1;
}

static inline int bench_thread_join(bench_thread_t thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

#else /* POSIX */
#include <pthread.h>

typedef pthread_t bench_thread_t;

typedef struct {
    void *(*func)(void *);
    void *arg;
} bench_thread_trampoline_t;

static inline int bench_thread_create(bench_thread_t *thread,
                                       bench_thread_trampoline_t *tramp,
                                       void *(*func)(void *), void *arg)
{
    (void)tramp; /* not needed for pthreads */
    return pthread_create(thread, NULL, func, arg);
}

static inline int bench_thread_join(bench_thread_t thread)
{
    return pthread_join(thread, NULL);
}

#endif /* OS_WINDOWS */
#endif /* BENCH_THREAD_H */

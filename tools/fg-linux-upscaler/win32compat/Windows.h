// Minimal Win32 compatibility layer so the FidelityFX Vulkan upscaler provider builds on Linux.
// Only the primitives the shared Vulkan backend can reference: events, threads, QPC, CloseHandle, TEXT, INFINITE.
#pragma once
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef void* HANDLE;
typedef uint32_t DWORD;
typedef int BOOL;
typedef union { long long QuadPart; } LARGE_INTEGER;

#define INFINITE 0xFFFFFFFFu
#ifndef WINAPI
#define WINAPI
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define TEXT(x) x
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u

typedef struct mk8_handle {
    int is_thread;
    pthread_t thread;
    pthread_mutex_t m;
    pthread_cond_t c;
    int signaled, manual;
} mk8_handle;

static inline HANDLE CreateEvent(void* sa, BOOL manual, BOOL initial, const char* name) {
    (void)sa; (void)name;
    mk8_handle* h = (mk8_handle*)calloc(1, sizeof *h);
    if (!h) return NULL;
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, NULL);
    h->manual = manual;
    h->signaled = initial;
    return h;
}

static inline BOOL SetEvent(HANDLE handle) {
    mk8_handle* h = (mk8_handle*)handle;
    if (!h) return 0;
    pthread_mutex_lock(&h->m);
    h->signaled = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return 1;
}

typedef DWORD (*mk8_thread_fn)(void*);

static inline HANDLE CreateThread(void* sa, size_t stack, mk8_thread_fn fn, void* arg, DWORD flags, DWORD* id) {
    (void)sa; (void)stack; (void)flags; (void)id;
    mk8_handle* h = (mk8_handle*)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->is_thread = 1;
    if (pthread_create(&h->thread, NULL, (void* (*)(void*))fn, arg) != 0) { free(h); return NULL; }
    return h;
}

static inline DWORD WaitForSingleObject(HANDLE handle, DWORD ms) {
    mk8_handle* h = (mk8_handle*)handle;
    if (!h) return (DWORD)-1;
    if (h->is_thread) {
        pthread_join(h->thread, NULL);
        return WAIT_OBJECT_0;
    }
    pthread_mutex_lock(&h->m);
    while (!h->signaled) {
        if (ms == INFINITE) {
            pthread_cond_wait(&h->c, &h->m);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&h->c, &h->m, &ts) != 0) break;
        }
    }
    int was = h->signaled;
    if (!h->manual) h->signaled = 0;
    pthread_mutex_unlock(&h->m);
    return was ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

static inline BOOL CloseHandle(HANDLE handle) {
    mk8_handle* h = (mk8_handle*)handle;
    if (!h) return 0;
    if (!h->is_thread) {
        pthread_mutex_destroy(&h->m);
        pthread_cond_destroy(&h->c);
    }
    free(h);
    return 1;
}

static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* out) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    out->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}

static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* out) {
    out->QuadPart = 1000000000LL;
    return 1;
}

static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000u); }

typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint8_t  UINT8;
typedef int32_t  INT32;
typedef uint64_t UINT_PTR;
typedef void VOID;
typedef void* LPVOID;
typedef const void* LPCVOID;
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#endif

// CRITICAL_SECTION over a recursive pthread mutex.
typedef struct _CRITICAL_SECTION {
    pthread_mutex_t m;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION* cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(&cs->m); }
static inline void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(&cs->m); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(&cs->m) == 0; }

#define THREAD_PRIORITY_HIGHEST 2
static inline BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return 1; }
static inline BOOL SetThreadDescription(HANDLE h, const wchar_t* d) { (void)h; (void)d; return 1; }

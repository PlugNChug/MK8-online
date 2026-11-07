// Sampling profiler (Windows, diagnostic): MK8_SAMPLE=<first presented frame>:<seconds>[:<period us>].
// From that frame on, every thread of the process is suspended briefly about once per period (default 1000 us) and
// its instruction pointer recorded; at the end the log gets, per busy thread, where its samples fell: recompiled game
// functions as f_<guest address> (runtime/dispatch.c), the runtime's own functions by name (DbgHelp with
// mk8drecomp.pdb), other modules as [module]. A thread blocked in a wait lands in [ntdll.dll] / [KERNELBASE.dll].
// Nothing allocates or locks while a thread is suspended. Off unless the variable is set.
#include "rt.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>

extern uint64_t vk_frame_counter(void);
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);

#define MK8_SAMPLE_THREADS 128
#define MK8_SAMPLE_MAX (1u << 22)

typedef struct { uint32_t thread; uint64_t pc, caller; } Mk8Sample;  // caller: in system code, the first own frame
static struct { uint64_t lo, hi; } mk8_own[16], mk8_sys[16];
static int mk8_nown, mk8_nsys;
static int mk8_in(const void *r, int n, uint64_t pc) {
    const uint64_t (*m)[2] = r;
    for (int i = 0; i < n; i++) if (pc >= m[i][0] && pc < m[i][1]) return 1;
    return 0;
}

typedef struct { char name[112]; uint64_t count; } Mk8Bucket;

typedef BOOL(WINAPI *SymInit)(HANDLE, PCSTR, BOOL);
typedef BOOL(WINAPI *SymFrom)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef DWORD(WINAPI *SymOpts)(DWORD);
typedef BOOL(WINAPI *SymLine)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
static SymLine mk8_sym_line;

static void mk8_sample_name(uint64_t pc, char *out, size_t n, SymFrom from) {
    uint64_t off = 0;
    const char *guest = rt_host_fn_name(pc, &off);
    if (guest) { snprintf(out, n, "%s", guest); return; }
    HMODULE m = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(uintptr_t)pc, &m);
    if (m && m == GetModuleHandleA(NULL) && from) {
        union { SYMBOL_INFO info; char space[sizeof(SYMBOL_INFO) + 256]; } s;
        memset(&s, 0, sizeof s);
        s.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        s.info.MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (from(GetCurrentProcess(), pc, &displacement, &s.info)) {
            IMAGEHLP_LINE64 line = {.SizeOfStruct = sizeof line};
            DWORD col = 0;
            if (mk8_sym_line && mk8_sym_line(GetCurrentProcess(), pc, &col, &line) && line.FileName) {
                const char *f = strrchr(line.FileName, '\\');
                if (!f) f = strrchr(line.FileName, '/');
                snprintf(out, n, "%s (%s:%lu)", s.info.Name, f ? f + 1 : line.FileName, (unsigned long)line.LineNumber);
            } else snprintf(out, n, "%s", s.info.Name);
            return;
        }
    }
    char path[MAX_PATH] = "?";
    if (m) GetModuleFileNameA(m, path, sizeof path);
    const char *base = strrchr(path, '\\');
    snprintf(out, n, "[%s]", base ? base + 1 : path);
}

static int mk8_sample_cmp(const void *a, const void *b) {
    const Mk8Sample *x = a, *y = b;
    if (x->thread != y->thread) return x->thread < y->thread ? -1 : 1;
    if (x->pc != y->pc) return x->pc < y->pc ? -1 : 1;
    return x->caller < y->caller ? -1 : x->caller > y->caller;
}
static int mk8_bucket_cmp(const void *a, const void *b) {
    const Mk8Bucket *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : strcmp(x->name, y->name);
}
static int mk8_name_cmp(const void *a, const void *b) { return strcmp(((const Mk8Bucket *)a)->name, ((const Mk8Bucket *)b)->name); }

static double mk8_thread_cpu(HANDLE h) {  // (kernel + user seconds: tells heap work in ntdll from waiting there)
    FILETIME a, b, k, u;
    if (!GetThreadTimes(h, &a, &b, &k, &u)) return 0;
    return ((double)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (double)(((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
}

static void mk8_sample_report(Mk8Sample *samples, uint32_t ns, const DWORD *tids, const double *cpu, uint32_t nthreads, double seconds) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    SymInit init = dbg ? (SymInit)(void *)GetProcAddress(dbg, "SymInitialize") : NULL;
    SymFrom from = dbg ? (SymFrom)(void *)GetProcAddress(dbg, "SymFromAddr") : NULL;
    SymOpts opts = dbg ? (SymOpts)(void *)GetProcAddress(dbg, "SymSetOptions") : NULL;
    mk8_sym_line = dbg ? (SymLine)(void *)GetProcAddress(dbg, "SymGetLineFromAddr64") : NULL;
    if (opts) opts(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_ANYTHING | SYMOPT_LOAD_LINES);
    char exe_dir[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    char *slash = strrchr(exe_dir, '\\');
    if (slash) *slash = 0;  // (the PDB sits next to the executable; the working directory is the data root)
    if (!init || !init(GetCurrentProcess(), exe_dir, TRUE)) from = NULL;
    if (from) {  // which symbols the executable got: PDB, or exports only (then names are the nearest export)
        typedef BOOL(WINAPI *ModInfo)(HANDLE, DWORD64, PIMAGEHLP_MODULE64);
        ModInfo info = (ModInfo)(void *)GetProcAddress(dbg, "SymGetModuleInfo64");
        IMAGEHLP_MODULE64 mi = {.SizeOfStruct = sizeof mi};
        if (info && info(GetCurrentProcess(), (DWORD64)(uintptr_t)GetModuleHandleA(NULL), &mi))
            LOG("[sample] executable symbols: type %d (3 = PDB, 5 = exports only) %s\n", (int)mi.SymType, mi.LoadedPdbName);
    }
    qsort(samples, ns, sizeof *samples, mk8_sample_cmp);
    LOG("[sample] %u samples over %.1f s, %u threads (symbols %s)\n", ns, seconds, nthreads, from ? "on" : "off");
    Mk8Bucket *b = calloc(ns ? ns : 1, sizeof *b);
    if (!b) return;
    for (uint32_t i = 0; i < ns;) {  // one thread at a time: its samples sorted by address, so a function is one run
        uint32_t t = samples[i].thread, nb = 0, total = 0;
        char name[112] = "";
        uint64_t last_pc = ~0ull, last_caller = ~0ull;
        for (; i < ns && samples[i].thread == t; i++, total++) {
            if (samples[i].pc != last_pc || samples[i].caller != last_caller) {
                mk8_sample_name(samples[i].pc, name, sizeof name, from);
                if (samples[i].caller) {  // [module] <- the own function that called into it
                    char up[96];
                    mk8_sample_name(samples[i].caller, up, sizeof up, from);
                    size_t l = strlen(name);
                    snprintf(name + l, sizeof name - l, " <- %s", up);
                }
                last_pc = samples[i].pc, last_caller = samples[i].caller;
            }
            if (!nb || strcmp(b[nb - 1].name, name)) { snprintf(b[nb].name, sizeof b[nb].name, "%s", name); b[nb].count = 0; nb++; }
            b[nb - 1].count++;
        }
        qsort(b, nb, sizeof *b, mk8_name_cmp);  // (merge runs of one module name split by other addresses)
        uint32_t m = 0;
        for (uint32_t k = 0; k < nb; k++) {
            if (m && !strcmp(b[m - 1].name, b[k].name)) b[m - 1].count += b[k].count;
            else b[m++] = b[k];
        }
        qsort(b, m, sizeof *b, mk8_bucket_cmp);
        uint64_t wait = 0;
        for (uint32_t k = 0; k < m; k++)
            if (!strncmp(b[k].name, "[ntdll", 6) || !strncmp(b[k].name, "[KERNELBASE", 11) || !strncmp(b[k].name, "[win32u", 7))
                wait += b[k].count;
        if (total - wait < total / 20 && cpu[t] < seconds / 10) continue;  // (mostly waiting)
        LOG("[sample] thread %lu: %u samples, CPU %.0f%% of a core, %.0f%% of samples outside ntdll/KERNELBASE/win32u\n",
            (unsigned long)tids[t], total, 100.0 * cpu[t] / seconds, 100.0 * (double)(total - wait) / (double)total);
        for (uint32_t k = 0; k < m && k < 40; k++) LOG("[sample]   %5.1f%%  %s\n", 100.0 * (double)b[k].count / total, b[k].name);
    }
    free(b);
}

static DWORD WINAPI mk8_sampler(LPVOID arg) {
    (void)arg;
    unsigned long first = 0, period_us = 1000;
    double seconds = 10;
    if (sscanf(getenv("MK8_SAMPLE"), "%lu:%lf:%lu", &first, &seconds, &period_us) < 1 || seconds <= 0 || period_us < 100) return 0;
    while (vk_frame_counter() < first) Sleep(20);
    Mk8Sample *samples = calloc(MK8_SAMPLE_MAX, sizeof *samples);
    if (!samples) return 0;
    DWORD self = GetCurrentThreadId(), tids[MK8_SAMPLE_THREADS];
    HANDLE handles[MK8_SAMPLE_THREADS];
    uint32_t n = 0, ns = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok && n < MK8_SAMPLE_THREADS; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) { handles[n] = h; tids[n] = te.th32ThreadID; n++; }
    }
    CloseHandle(snap);
    HANDLE mods = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);  // own code: the executable and the game DLLs
    MODULEENTRY32 me = {.dwSize = sizeof me};
    for (BOOL ok = Module32First(mods, &me); ok; ok = Module32Next(mods, &me)) {
        uint64_t lo = (uint64_t)(uintptr_t)me.modBaseAddr, hi = lo + me.modBaseSize;
        bool own = (HMODULE)me.modBaseAddr == GetModuleHandleA(NULL) || !_strnicmp(me.szModule, "mk8d", 4);
        bool sys = !_stricmp(me.szModule, "ntdll.dll") || !_stricmp(me.szModule, "KERNELBASE.dll") ||
                   !_stricmp(me.szModule, "ucrtbase.dll") || !_stricmp(me.szModule, "kernel32.dll") || !_stricmp(me.szModule, "win32u.dll");
        if (own && mk8_nown < 16) { mk8_own[mk8_nown].lo = lo; mk8_own[mk8_nown].hi = hi; mk8_nown++; }
        if (sys && mk8_nsys < 16) { mk8_sys[mk8_nsys].lo = lo; mk8_sys[mk8_nsys].hi = hi; mk8_nsys++; }
    }
    CloseHandle(mods);
    LOG("[sample] sampling %u threads from frame %lu for %.1f s every %lu us\n", n, first, seconds, period_us);
    double cpu[MK8_SAMPLE_THREADS];
    for (uint32_t i = 0; i < n; i++) cpu[i] = mk8_thread_cpu(handles[i]);
    LARGE_INTEGER f, t0, t, next;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    next = t0;
    uint64_t step = (uint64_t)f.QuadPart * period_us / 1000000;
    do {
        for (uint32_t i = 0; i < n && ns < MK8_SAMPLE_MAX; i++) {
            if (SuspendThread(handles[i]) == (DWORD)-1) continue;
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL};
            if (GetThreadContext(handles[i], &c)) {
                uint64_t caller = 0;
                if (mk8_in(mk8_sys, mk8_nsys, c.Rip))  // (its stack is ours to read: same process, thread stopped)
                    for (int k = 0; k < 48 && !caller; k++) {
                        uint64_t v = ((const uint64_t *)(uintptr_t)c.Rsp)[k];
                        if (!mk8_in(mk8_own, mk8_nown, v) || !mk8_in(mk8_own, mk8_nown, v - 6)) continue;
                        const uint8_t *q = (const uint8_t *)(uintptr_t)v;  // a return address follows a call
                        if (q[-5] == 0xE8 || (q[-6] == 0xFF && (q[-5] & 0x38) == 0x10) || (q[-2] == 0xFF && (q[-1] & 0x38) == 0x10) ||
                            (q[-3] == 0xFF && (q[-2] & 0x38) == 0x10))
                            caller = v;
                    }
                samples[ns++] = (Mk8Sample){i, c.Rip, caller};
            }
            ResumeThread(handles[i]);
        }
        next.QuadPart += (LONGLONG)step;
        do { YieldProcessor(); QueryPerformanceCounter(&t); } while (t.QuadPart < next.QuadPart);  // (Sleep is ~15 ms)
    } while ((double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart < seconds);
    for (uint32_t i = 0; i < n; i++) { cpu[i] = mk8_thread_cpu(handles[i]) - cpu[i]; CloseHandle(handles[i]); }
    mk8_sample_report(samples, ns, tids, cpu, n, seconds);
    free(samples);
    return 0;
}

__attribute__((constructor)) static void mk8_sampler_start(void) {
    if (getenv("MK8_SAMPLE")) CloseHandle(CreateThread(NULL, 0, mk8_sampler, NULL, 0, NULL));
}

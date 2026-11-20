#pragma once
// Portable shared-library loading for the upscaler probes.
//
// On Linux the probes load the provider built by tools/build_upscaler_provider.py; on Windows they load AMD's
// signed amd_fidelityfx_upscaler_vk.dll. Keep the call sites identical so both platforms exercise the same code.
#include <cstddef>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static inline void* mk8_lib_open(const char* path) { return (void*)LoadLibraryA(path); }
static inline void* mk8_lib_symbol(void* handle, const char* name)
{
    return (void*)GetProcAddress((HMODULE)handle, name);
}
static inline void mk8_lib_close(void* handle)
{
    if (handle) FreeLibrary((HMODULE)handle);
}
static inline const char* mk8_lib_error()
{
    static char message[512];
    DWORD code = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                             message, (DWORD)sizeof(message), nullptr);
    while (n && (message[n - 1] == '\r' || message[n - 1] == '\n')) message[--n] = 0;
    if (!n) snprintf(message, sizeof(message), "Win32 error %lu", (unsigned long)code);
    return message;
}
#else
#include <dlfcn.h>

static inline void* mk8_lib_open(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static inline void* mk8_lib_symbol(void* handle, const char* name) { return dlsym(handle, name); }
static inline void mk8_lib_close(void* handle)
{
    if (handle) dlclose(handle);
}
static inline const char* mk8_lib_error() { return dlerror(); }
#endif

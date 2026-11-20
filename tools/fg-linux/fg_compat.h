#pragma once
#define FFX_API_ENTRY __attribute__((visibility("default")))
#define FFX_UNUSED(x) ((void)(x))
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#endif

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#ifdef __cplusplus
static inline int strncpy_s(char* dst, size_t dstsz, const char* src, size_t count) {
    if (!dst || !dstsz) return 1;
    size_t n = count == (size_t)-1 ? dstsz - 1 : (count < dstsz ? count : dstsz - 1);
    size_t i = 0;
    if (src) { for (; i < n && src[i]; i++) dst[i] = src[i]; }
    for (; i < dstsz; i++) dst[i] = 0;
    return 0;
}
static inline int swprintf_s(wchar_t* dst, size_t dstsz, const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vswprintf(dst, dstsz, fmt, ap); va_end(ap); return r;
}
#endif

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifdef __cplusplus
#include <cstddef>
#include <cwchar>
#include <cmath>
static inline int wcscpy_s(wchar_t* dst, size_t dstsz, const wchar_t* src) {
    if (!dst || !dstsz) return 1;
    size_t i = 0; if (src) { for (; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i]; }
    dst[i] = 0; return 0;
}
static inline int strcpy_s(char* dst, size_t dstsz, const char* src) {
    if (!dst || !dstsz) return 1;
    size_t i = 0; if (src) { for (; i + 1 < dstsz && src[i]; ++i) dst[i] = src[i]; }
    dst[i] = 0; return 0;
}
static inline int wcsncpy_s(wchar_t* dst, size_t dstsz, const wchar_t* src, size_t count) {
    if (!dst || !dstsz) return 1;
    size_t i = 0; size_t n = (count == (size_t)-1) ? dstsz - 1 : count;
    if (src) { for (; i < n && i + 1 < dstsz && src[i]; ++i) dst[i] = src[i]; }
    dst[i] = 0; return 0;
}
#endif

#ifdef __cplusplus
// UTF-8 <-> UTF-16 without <codecvt> (removed in GCC 16).
static inline size_t mk8_utf8_to_utf16(const char* in, wchar_t* out, size_t outLen) {
    size_t o = 0;
    if (!outLen) return 0;
    if (in) {
        const unsigned char* s = (const unsigned char*)in;
        while (*s) {
            unsigned cp; int extra;
            if (*s < 0x80) { cp = *s; extra = 0; }
            else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F; extra = 1; }
            else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F; extra = 2; }
            else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07; extra = 3; }
            else { cp = 0xFFFD; extra = 0; }
            ++s;
            for (int i = 0; i < extra && *s; ++i, ++s) cp = (cp << 6) | (*s & 0x3F);
            if (o + 1 >= outLen) break;
            if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
                cp -= 0x10000;
                if (o + 2 >= outLen) break;
                out[o++] = (wchar_t)(0xD800 + (cp >> 10));
                out[o++] = (wchar_t)(0xDC00 + (cp & 0x3FF));
            } else {
                out[o++] = (wchar_t)cp;
            }
        }
    }
    out[o] = 0;
    return o;
}
static inline size_t mk8_utf16_to_utf8(const wchar_t* in, char* out, size_t outLen) {
    size_t o = 0;
    if (!outLen) return 0;
    if (in) {
        for (size_t i = 0; in[i]; ++i) {
            unsigned cp = (unsigned)in[i];
            if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && in[i+1]) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)in[++i] - 0xDC00);
            }
            char buf[4]; int n;
            if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
            else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
            else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
            else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
            if (o + (size_t)n >= outLen) break;
            for (int k = 0; k < n; ++k) out[o++] = buf[k];
        }
    }
    out[o] = 0;
    return o;
}
template <size_t N> static inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) { return wcscpy_s(dst, N, src); }
template <size_t N> static inline int strcpy_s(char (&dst)[N], const char* src) { return strcpy_s(dst, N, src); }
#endif

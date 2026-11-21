// Only the refresh token can be shared with another game started by this launcher.
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "net/secure_store.h"
#ifdef _WIN32
#include <windows.h>
#endif

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char refresh[600], username[64], backend[16] = "ours";

// The launcher owns this zero-initialized, pagefile-backed mapping. Open it only
// during a transfer so closing the launcher also ends reuse, even mid-race.
static void launcher_refresh(bool write_value) {
#ifdef _WIN32
    const char *name = getenv("MK8_NEXTENDO_SESSION");
    if (!name || strncmp(name, "Local\\MK8-Nextendo-", sizeof("Local\\MK8-Nextendo-")-1)) return;
    DWORD access = write_value ? FILE_MAP_WRITE : FILE_MAP_READ;
    HANDLE handle = OpenFileMappingA(access, FALSE, name);
    if (!handle) return;
    char *shared = MapViewOfFile(handle, access, 0, 0, sizeof refresh);
    if (shared) {
        if (write_value) memcpy(shared, refresh, sizeof refresh);
        else if (memchr(shared, 0, sizeof refresh)) memcpy(refresh, shared, sizeof refresh);
        UnmapViewOfFile(shared);
    }
    CloseHandle(handle);
#else
    (void)write_value;
#endif
}

bool mk8_nextendo_enabled(void) {
    const char *value = getenv("MK8_NEXTENDO_TEST");
    return value && !strcmp(value, "1");
}

static char *entry(const char *key, size_t *cap) {
    if (!strcmp(key, "refresh_token")) { *cap = sizeof refresh; return refresh; }
    if (!strcmp(key, "username")) { *cap = sizeof username; return username; }
    return NULL;
}

bool secure_get(const char *key, char *out, size_t cap) {
    pthread_mutex_lock(&session_lock);
    size_t size;
    char *value = entry(key, &size);
    if (value == refresh) launcher_refresh(false);
    bool ok = value && value[0] && strlen(value) < cap;
    if (cap) out[0] = 0;
    if (ok) strcpy(out, value);
    pthread_mutex_unlock(&session_lock);
    return ok;
}

bool secure_set(const char *key, const char *value) {
    pthread_mutex_lock(&session_lock);
    size_t size;
    char *dest = entry(key, &size);
    bool ok = dest && strlen(value) < size && !strpbrk(value, "\r\n");
    if (ok) { memset(dest, 0, size); strcpy(dest, value); }
    if (ok && dest == refresh) launcher_refresh(true);
    pthread_mutex_unlock(&session_lock);
    return ok;
}

void secure_del(const char *key) {
    pthread_mutex_lock(&session_lock);
    size_t size;
    char *dest = entry(key, &size);
    if (dest) { volatile char *p = dest; while (size--) *p++ = 0; }
    if (dest == refresh) launcher_refresh(true);
    pthread_mutex_unlock(&session_lock);
}

bool game_net_config(const char *key, char *out, size_t cap) {
    pthread_mutex_lock(&session_lock);
    bool ok = !strcmp(key, "backend") && strlen(backend) < cap;
    if (ok) strcpy(out, backend);
    pthread_mutex_unlock(&session_lock);
    return ok;
}

void game_net_config_set(const char *key, const char *value) {
    if (strcmp(key, "backend") || (strcmp(value, "ours") && strcmp(value, "nextendo"))) return;
    if (!strcmp(value, "nextendo") && !mk8_nextendo_enabled()) return;
    pthread_mutex_lock(&session_lock);
    strcpy(backend, value);
    pthread_mutex_unlock(&session_lock);
}

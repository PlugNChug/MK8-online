// Windows host I/O for the extracted RomFS. Guest paths and hashes stay UTF-8.
#pragma once
#include <errno.h>
#include <stdbool.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>

// Resolve relative paths and dot segments before switching to verbatim Win32 syntax.
static inline wchar_t *mk8_romfs_path(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (!n || n == 1) { errno = EINVAL; return NULL; }
    wchar_t *input = malloc((size_t)n * sizeof *input);
    if (!input) return NULL;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, n);
    for (wchar_t *p = input; *p; ++p) if (*p == L'/') *p = L'\\';
    if (!wcsncmp(input, L"\\\\?\\", 4)) return input;
    DWORD cap = GetFullPathNameW(input, 0, NULL, NULL);
    if (!cap || cap > 32767) { free(input); errno = ENAMETOOLONG; return NULL; }
    wchar_t *result = malloc(((size_t)cap + 8) * sizeof *result);
    if (!result) { free(input); return NULL; }
    wchar_t *absolute = result + 8;
    DWORD length = GetFullPathNameW(input, cap, absolute, NULL);
    free(input);
    if (!length || length >= cap) { free(result); errno = EINVAL; return NULL; }
    if (!wcsncmp(absolute, L"\\\\", 2)) {
        memmove(result + 8, absolute + 2, (length - 1) * sizeof *result);
        memcpy(result, L"\\\\?\\UNC\\", 8 * sizeof *result);
    } else {
        memmove(result + 4, absolute, (length + 1) * sizeof *result);
        memcpy(result, L"\\\\?\\", 4 * sizeof *result);
    }
    return result;
}

static inline int mk8_romfs_open(const char *path) {
    wchar_t *wide = mk8_romfs_path(path);
    if (!wide) return -1;
    int fd = _wopen(wide, _O_RDONLY | _O_BINARY);
    free(wide);
    return fd;
}

static inline FILE *mk8_romfs_input(const char *path) {
    wchar_t *wide = mk8_romfs_path(path);
    if (!wide) return NULL;
    FILE *f = _wfopen(wide, L"rb");
    free(wide);
    return f;
}

static inline int mk8_romfs_stat(const char *path, struct stat *out) {
    wchar_t *wide = mk8_romfs_path(path);
    if (!wide) return -1;
    // Read target metadata through the handle, including legacy file links.
    HANDLE file = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD error = GetLastError();
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        errno = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES;
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    if (!ok) { errno = EIO; return -1; }
    out->st_mode = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR : _S_IFREG;
    out->st_size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return 0;
}

// MinGW opendir (including its wide variant) uses a fixed MAX_PATH buffer.
typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW entry;
    bool first;
    char name[4 * MAX_PATH];
} Mk8RomfsDir;

static inline Mk8RomfsDir *mk8_romfs_opendir(const char *path) {
    size_t n = strlen(path);
    char *pattern = malloc(n + 3);
    if (!pattern) return NULL;
    memcpy(pattern, path, n);
    memcpy(pattern + n, "/*", 3);
    wchar_t *wide = mk8_romfs_path(pattern);
    free(pattern);
    if (!wide) return NULL;
    Mk8RomfsDir *d = calloc(1, sizeof *d);
    if (!d) { free(wide); return NULL; }
    d->handle = FindFirstFileW(wide, &d->entry);
    DWORD error = GetLastError();
    free(wide);
    if (d->handle == INVALID_HANDLE_VALUE && error != ERROR_FILE_NOT_FOUND) {
        free(d);
        errno = error == ERROR_PATH_NOT_FOUND ? ENOENT : EIO;
        return NULL;
    }
    d->first = true;
    return d;
}

static inline const char *mk8_romfs_next(Mk8RomfsDir *d) {
    errno = 0;
    if (d->handle == INVALID_HANDLE_VALUE) return NULL;
    if (!d->first && !FindNextFileW(d->handle, &d->entry)) {
        if (GetLastError() != ERROR_NO_MORE_FILES) errno = EIO;
        return NULL;
    }
    d->first = false;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, d->entry.cFileName, -1,
                             d->name, sizeof d->name, NULL, NULL)) {
        errno = EILSEQ;
        return NULL;
    }
    return d->name;
}

static inline void mk8_romfs_closedir(Mk8RomfsDir *d) {
    if (d->handle != INVALID_HANDLE_VALUE) FindClose(d->handle);
    free(d);
}
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
typedef DIR Mk8RomfsDir;
#define mk8_romfs_opendir opendir
#define mk8_romfs_closedir closedir
#define mk8_romfs_stat stat
#define mk8_romfs_open(path) open(path, O_RDONLY)
#define mk8_romfs_input(path) fopen(path, "rb")
static inline const char *mk8_romfs_next(DIR *d) {
    errno = 0;
    struct dirent *entry = readdir(d);
    return entry ? entry->d_name : NULL;
}
#endif

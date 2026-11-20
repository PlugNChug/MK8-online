// NVIDIA DLSS Super Resolution / DLAA bridge (host side, no guest changes).
//
// Loads the driver-provided NGX core and the DLSS feature plugin at first use, creates a
// SuperSampling feature for the game image and records one EvaluateFeature into the current
// command buffer. MK8_DLSS=1 selects it from fsr.cpp; when anything is missing the bridge
// returns VK_NULL_HANDLE with one reasoned line and the existing FSR/bilinear path runs.
//
// Reused inputs, same as the FSR bridge: colour at render resolution, the captured scene
// depth (or a neutral image), camera-derived motion vectors in render-pixel units and the
// Projection jitter offset. Motion vectors are recomputed here with the same depth/projection
// algorithm fsr.cpp uses; fsr.cpp's motion image is private to that file.
//
// Nothing NVIDIA is committed: the plugin and core are optional components loaded from the
// driver and from tools/fetch_vendor_sdk.py's directory. The NGX header itself is
// proprietary (LicenseRef-NvidiaProprietary on v310.9.16), so only the ABI facts this file
// needs are declared, in ngx/mk8_ngx_abi.h.
#include "dlss.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <winreg.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------------------
// Loader probe: paths, dlopen and export checks. No Vulkan object is touched here.
// ---------------------------------------------------------------------------------------

#ifdef _WIN32
typedef HMODULE Mk8Lib;
static Mk8Lib lib_open(const char *path) { const char *plugin = getenv("MK8_DLSS_LIB");
    if (plugin && strcmp(plugin, path) == 0) {
        const wchar_t *wide = _wgetenv(L"MK8_DLSS_LIB");
        return wide ? LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
    }
    DWORD dirs = LOAD_LIBRARY_SEARCH_SYSTEM32;
    if (strchr(path, '\\') || strchr(path, '/')) dirs |= LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
    return LoadLibraryExA(path, NULL, dirs); }
static void *lib_symbol(Mk8Lib library, const char *name) { return (void *)GetProcAddress(library, name); }
static void lib_close(Mk8Lib library) { if (library) FreeLibrary(library); }
static void lib_error_text(char *out, size_t n) {
    DWORD code = GetLastError();
    DWORD written = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                                   out, (DWORD)n, NULL);
    if (!written) snprintf(out, n, "Win32 error %lu", (unsigned long)code);
    else while (written && (out[written - 1] == '\r' || out[written - 1] == '\n')) out[--written] = 0;
}
#define MK8_DLSS_PLUGIN_PREFIX "nvngx_dlss"
#define MK8_DLSSG_PLUGIN_PREFIX "nvngx_dlssg"
#define MK8_DLSS_PLUGIN_SUFFIX ".dll"
#else
typedef void *Mk8Lib;
static Mk8Lib lib_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *lib_symbol(Mk8Lib library, const char *name) { return dlsym(library, name); }
static void lib_close(Mk8Lib library) { if (library) dlclose(library); }
static void lib_error_text(char *out, size_t n) {
    const char *error = dlerror();
    snprintf(out, n, "%s", error && *error ? error : "unknown dlopen error");
}
#define MK8_DLSS_PLUGIN_PREFIX "libnvidia-ngx-dlss.so"
#define MK8_DLSSG_PLUGIN_PREFIX "libnvidia-ngx-dlssg.so"
#define MK8_DLSS_PLUGIN_SUFFIX ""
#endif

#define MK8_DLSS_MAX_CANDIDATES 16

static bool path_join(char *out, size_t size, const char *directory, const char *name) {
    if (!out || !size || !directory || !*directory || !name) return false;
    size_t used = strlen(directory);
    const char *separator = (directory[used - 1] == '/' || directory[used - 1] == '\\') ? "" : "/";
    int written = snprintf(out, size, "%s%s%s", directory, separator, name);
    return written > 0 && (size_t)written < size;
}

static void path_directory(const char *path, char *out, size_t size) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (!slash) {
        snprintf(out, size, ".");
        return;
    }
    size_t length = (size_t)(slash - path);
    if (length >= size) length = size - 1;
    memcpy(out, path, length);
    out[length] = 0;
    if (!length) snprintf(out, size, "/");
}

static bool file_exists(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
#endif
}

#ifndef _WIN32
static uint16_t read_le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t read_le64(const unsigned char *p) {
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

// 1: a complete ELF64 x86-64 ET_DYN file; 0: a regular file that is not (truncated, other
// architecture, not an ELF); -1: not a local regular file, so the loader should search by name.
//
// dlopen mmaps the program segments before it returns, and a file truncated below a PT_LOAD
// segment's p_offset+p_filesz faults with SIGBUS inside the loader. The plugin is a user-
// supplied optional component, so it is validated before being handed to dlopen.
static int elf_file_status(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0) return -1;
    if (!S_ISREG(info.st_mode)) return -1;
    uint64_t file_size = (uint64_t)info.st_size;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char header[64];
    int status = 0;
    if (fread(header, 1, sizeof header, file) == sizeof header && memcmp(header, "\x7f" "ELF", 4) == 0 &&
        header[4] == 2 /* ELFCLASS64 */ && header[5] == 1 /* ELFDATA2LSB */ &&
        read_le16(header + 16) == 3 /* ET_DYN */ && read_le16(header + 18) == 62 /* EM_X86_64 */) {
        uint64_t phoff = read_le64(header + 32);
        uint16_t phentsize = read_le16(header + 54);
        uint16_t phnum = read_le16(header + 56);
        if (phnum && phentsize >= 56 && phoff <= file_size &&
            (uint64_t)phnum * phentsize <= file_size - phoff) {
            status = 1;
            for (uint16_t i = 0; i < phnum && status == 1; i++) {
                unsigned char phdr[56];
                if (fseek(file, (long)(phoff + (uint64_t)i * phentsize), SEEK_SET) != 0 ||
                    fread(phdr, 1, sizeof phdr, file) != sizeof phdr) {
                    status = 0;
                    break;
                }
                if (read_le32(phdr) != 1 /* PT_LOAD */) continue;
                uint64_t offset = read_le64(phdr + 8);
                uint64_t filesz = read_le64(phdr + 32);
                if (offset > file_size || filesz > file_size - offset) status = 0;
            }
        }
    }
    fclose(file);
    return status;
}
#else
static int elf_file_status(const char *path) { return file_exists(path) ? 1 : -1; }
#endif

// The downloader stores the licence beside the artefacts, so it travels with them. A driver
// copy of the plugin has no licence file there; that is reported, not treated as an error.
static void detect_licence(Mk8DlssProbe *probe) {
    char path[MK8_DLSS_PATH_MAX];
    probe->licence_present = false;
    if ((path_join(path, sizeof path, probe->directory, "LICENSE-NVIDIA.txt") && file_exists(path)) ||
        (path_join(path, sizeof path, probe->directory, "LICENSE.txt") && file_exists(path))) {
        probe->licence_present = true;
        return;
    }
    if (path_join(path, sizeof path, probe->directory, "nvngx_dlss.license.txt") && file_exists(path))
        probe->licence_present = true;
}

static void default_plugin_directory(char *out, size_t size) {
    const char *configured = getenv("MK8_DLSS_DIR");
    if (configured && *configured) {
        snprintf(out, size, "%s", configured);
        return;
    }
    const char *base = getenv("XDG_DATA_HOME");
    if (base && *base) {
        snprintf(out, size, "%s/mk8online/nvidia", base);
        return;
    }
    base = getenv("HOME");
#ifdef _WIN32
    if (!base || !*base) base = getenv("USERPROFILE");
#endif
    if (base && *base) {
        snprintf(out, size, "%s/.local/share/mk8online/nvidia", base);
        return;
    }
    snprintf(out, size, "mk8online/nvidia");
}

static int compare_paths(const void *left, const void *right) {
    return strcmp((const char *)left, (const char *)right);
}

// Collect plugin candidates from `directory`, sorted ascending. The caller tries the last
// (highest) version first, so libnvidia-ngx-dlss.so.310.9.16 wins over libnvidia-ngx-dlss.so.
static int collect_plugin_candidates(const char *directory, const char *plugin_prefix,
                                     const char *plugin_suffix,
                                     char paths[][MK8_DLSS_PATH_MAX], int capacity) {
    int count = 0;
    size_t prefix = strlen(plugin_prefix);
#ifdef _WIN32
    char pattern[MK8_DLSS_PATH_MAX];
    if (!path_join(pattern, sizeof pattern, directory, plugin_prefix) ||
        strlen(pattern) + strlen("*") + strlen(plugin_suffix) + 1 > sizeof pattern)
        return 0;
    snprintf(pattern + strlen(pattern), sizeof pattern - strlen(pattern), "*%s", plugin_suffix);
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // The SR prefix is a prefix of the FG/RR names too ("nvngx_dlss" matches
        // nvngx_dlssg.dll); only accept the exact feature or a version suffix.
        const char *rest = entry.cFileName + prefix;
        if (*rest && *rest != '.') continue;
        if (count >= capacity) break;
        if (!path_join(paths[count], MK8_DLSS_PATH_MAX, directory, entry.cFileName)) continue;
        count++;
    } while (FindNextFileA(search, &entry));
    FindClose(search);
#else
    (void)plugin_suffix; // versioned .so names carry no suffix to strip
    DIR *dir = opendir(directory);
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, plugin_prefix, prefix) != 0) continue;
        // Exclude other features (dlssd/dlssg) and unrelated files.
        char next = entry->d_name[prefix];
        if (next != 0 && next != '.') continue;
        if (count >= capacity) break;
        if (!path_join(paths[count], MK8_DLSS_PATH_MAX, directory, entry->d_name)) continue;
        count++;
    }
    closedir(dir);
#endif
    if (count > 1) qsort(paths, (size_t)count, MK8_DLSS_PATH_MAX, compare_paths);
    return count;
}

static bool has_symbol(Mk8Lib library, const char *name) { return library && lib_symbol(library, name) != NULL; }

// Append the names that are absent. Returns true when every required name is present.
static bool check_symbols(Mk8Lib library, const char *const *names, int count, char *missing, size_t size) {
    bool all = true;
    missing[0] = 0;
    for (int i = 0; i < count; i++) {
        if (has_symbol(library, names[i])) continue;
        all = false;
        size_t used = strlen(missing);
        if (used + 1 < size) snprintf(missing + used, size - used, "%s%s", used ? ", " : "", names[i]);
    }
    return all;
}

const char *mk8_dlss_status_name(Mk8DlssLoadStatus status) {
    switch (status) {
    case MK8_DLSS_OK: return "ok";
    case MK8_DLSS_NO_PLUGIN: return "no-plugin";
    case MK8_DLSS_PLUGIN_LOAD_FAILED: return "plugin-load-failed";
    case MK8_DLSS_PLUGIN_EXPORTS_MISSING: return "plugin-exports-missing";
    case MK8_DLSS_NO_CORE: return "no-core";
    case MK8_DLSS_CORE_LOAD_FAILED: return "core-load-failed";
    case MK8_DLSS_CORE_EXPORTS_MISSING: return "core-exports-missing";
    }
    return "unknown";
}

void mk8_dlss_log_unavailable(const Mk8DlssProbe *probe) {
    const char *reason = probe && probe->reason[0] ? probe->reason : "no reason recorded";
    fprintf(stderr, "[dlss] DLSS unavailable: %s\n", reason);
}

static bool probe_plugin_named(Mk8DlssProbe *probe, const char *plugin_prefix, const char *plugin_suffix,
                               const char *label) {
#ifndef _WIN32
    (void)plugin_suffix;  // the ELF plugin name has no suffix; versioned .so files are scanned
#endif
    const char *configured = getenv("MK8_DLSS_LIB");
    char candidates[MK8_DLSS_MAX_CANDIDATES][MK8_DLSS_PATH_MAX];
    int count = 0;
    if (configured && *configured) {
        snprintf(candidates[0], sizeof candidates[0], "%s", configured);
        path_directory(candidates[0], probe->directory, sizeof probe->directory);
        count = 1;
    } else {
        count = collect_plugin_candidates(probe->directory, plugin_prefix, plugin_suffix, candidates,
                                          MK8_DLSS_MAX_CANDIDATES);
        if (!count) {
#ifdef _WIN32
            // The SDK layout puts nvngx_*.dll beside the executable; the downloader layout
            // puts it in the vendor directory. Both are covered by the scan above plus this.
            char executable[MK8_DLSS_PATH_MAX];
            DWORD length = GetModuleFileNameA(NULL, executable, (DWORD)sizeof executable);
            if (length && length < sizeof executable) {
                char name[MK8_DLSS_PATH_MAX];
                path_directory(executable, executable, sizeof executable);
                snprintf(name, sizeof name, "%s%s", plugin_prefix, plugin_suffix);
                if (path_join(candidates[0], sizeof candidates[0], executable, name)) count = 1;
            }
#endif
        }
        if (!count) {
            probe->status = MK8_DLSS_NO_PLUGIN;
            snprintf(probe->reason, sizeof probe->reason,
                     "no %s plugin (%s*) in %s; install the optional component in launcher settings", label, plugin_prefix, probe->directory);
            return false;
        }
    }
    detect_licence(probe);
    char load_error[MK8_DLSS_REASON_MAX] = "not tried";
    for (int i = count - 1; i >= 0; i--) {
        if (elf_file_status(candidates[i]) == 0) {
            snprintf(load_error, sizeof load_error,
                     "%s is truncated or not an ELF64 x86-64 shared object", candidates[i]);
            continue;
        }
        // Clear the platform error so a failure below reports its own cause.
#ifdef _WIN32
        SetLastError(0);
#else
        dlerror();
#endif
        Mk8Lib library = lib_open(candidates[i]);
        if (!library) {
            lib_error_text(load_error, sizeof load_error);
            continue;
        }
        snprintf(probe->plugin, sizeof probe->plugin, "%s", candidates[i]);
        probe->plugin_loaded = true;
        static const char *const required[] = {
            "NVSDK_NGX_VULKAN_EvaluateFeature",
            "NVSDK_NGX_VULKAN_ReleaseFeature",
        };
        char missing[MK8_DLSS_REASON_MAX];
        bool common = check_symbols(library, required, 2, missing, sizeof missing);
        bool create = has_symbol(library, "NVSDK_NGX_VULKAN_CreateFeature1") ||
                      has_symbol(library, "NVSDK_NGX_VULKAN_CreateFeature");
        probe->plugin_exports_ok = common && create;
        if (probe->plugin_exports_ok) {
            lib_close(library);
            return true;
        }
        if (!create) {
            size_t used = strlen(missing);
            snprintf(missing + used, sizeof missing - used, "%sNVSDK_NGX_VULKAN_CreateFeature1",
                     used ? ", " : "");
        }
        lib_close(library);
        probe->status = MK8_DLSS_PLUGIN_EXPORTS_MISSING;
        snprintf(probe->reason, sizeof probe->reason, "%s plugin %s lacks NGX exports: %s", label,
                 probe->plugin, missing);
        return false;
    }
    probe->status = MK8_DLSS_PLUGIN_LOAD_FAILED;
    snprintf(probe->reason, sizeof probe->reason, "%s plugin %s did not load: %s", label,
             candidates[count - 1], load_error);
    return false;
}

static bool probe_plugin(Mk8DlssProbe *probe) {
    return probe_plugin_named(probe, MK8_DLSS_PLUGIN_PREFIX, MK8_DLSS_PLUGIN_SUFFIX, "DLSS");
}

#ifdef _WIN32
// DCH drivers keep the NGX runtime in the DriverStore and register that directory here;
// System32 has no _nvngx.dll. Reading it also lets the plugin's own LoadLibraryA("_nvngx.dll")
// resolve to the same module when the directory is added to the DLL search path.
static bool registry_ngx_directory(char *out, size_t size) {
    HKEY key = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    char buffer[MK8_DLSS_PATH_MAX] = {0};
    DWORD bytes = (DWORD)sizeof buffer;
    DWORD type = 0;
    LONG rc = RegQueryValueExA(key, "FullPath", NULL, &type, (LPBYTE)buffer, &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ || !buffer[0]) return false;
    snprintf(out, size, "%s", buffer);
    return true;
}
#endif

static bool probe_core(Mk8DlssProbe *probe) {
    const char *configured = getenv("MK8_DLSS_CORE");
    char attempts[8][MK8_DLSS_PATH_MAX];
    int count = 0;
    if (configured && *configured) {
        snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", configured);
    } else {
#ifdef _WIN32
        // 1. The driver's registered directory (the only copy on current DCH drivers).
        char driver[MK8_DLSS_PATH_MAX];
        if (registry_ngx_directory(driver, sizeof driver)) {
            static const char *const driver_names[] = {"_nvngx.dll", "nvngx.dll"};
            for (size_t i = 0;
                 i < sizeof driver_names / sizeof driver_names[0] && count < (int)(sizeof attempts / sizeof attempts[0]);
                 i++) {
                char candidate[MK8_DLSS_PATH_MAX];
                if (path_join(candidate, sizeof candidate, driver, driver_names[i]) && file_exists(candidate))
                    snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", candidate);
            }
        }
        // 3. Older drivers copy the core to System32; try the bare names last.
        if (count < (int)(sizeof attempts / sizeof attempts[0]))
            snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", "_nvngx.dll");
        if (count < (int)(sizeof attempts / sizeof attempts[0]))
            snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", "nvngx.dll");
#else
        static const char *const names[] = {"libnvidia-ngx.so.1", "libnvidia-ngx.so"};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", names[i]);
        // A user may keep the driver core beside the downloaded plugin; try it last so the
        // driver's own copy still wins.
        char beside[MK8_DLSS_PATH_MAX];
        if (path_join(beside, sizeof beside, probe->directory, "libnvidia-ngx.so.1") &&
            file_exists(beside) && count < (int)(sizeof attempts / sizeof attempts[0]))
            snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", beside);
#endif
    }

    char load_error[MK8_DLSS_REASON_MAX] = "not found";
    for (int i = 0; i < count; i++) {
        if (elf_file_status(attempts[i]) == 0) {
            snprintf(load_error, sizeof load_error,
                     "%s is truncated or not an ELF64 x86-64 shared object", attempts[i]);
            continue;
        }
#ifdef _WIN32
        SetLastError(0);
#else
        dlerror();
#endif
        Mk8Lib library = lib_open(attempts[i]);
        if (!library) {
            // Keep the first failure: it names the driver's canonical core library.
            if (i == 0) lib_error_text(load_error, sizeof load_error);
            continue;
        }
        snprintf(probe->core, sizeof probe->core, "%s", attempts[i]);
        probe->core_loaded = true;
#ifdef _WIN32
        // The Windows runtime has no NVSDK_NGX_Parameter_* C exports (the parameter object's
        // vtable is the interface), so only Init/parameters/Shutdown are required here.
        static const char *const required[] = {
            "NVSDK_NGX_VULKAN_GetCapabilityParameters",
            "NVSDK_NGX_VULKAN_Shutdown1",
        };
        char missing[MK8_DLSS_REASON_MAX];
        bool common = check_symbols(library, required, (int)(sizeof required / sizeof required[0]), missing,
                                    sizeof missing);
        bool init = has_symbol(library, "NVSDK_NGX_VULKAN_Init_with_ProjectID") ||
                    has_symbol(library, "NVSDK_NGX_VULKAN_Init_ProjectID");
        if (!init) {
            size_t used = strlen(missing);
            snprintf(missing + used, sizeof missing - used, "%sNVSDK_NGX_VULKAN_Init_ProjectID",
                     used ? ", " : "");
        }
        probe->core_exports_ok = common && init;
#else
        static const char *const required[] = {
            "NVSDK_NGX_VULKAN_Init_with_ProjectID",
            "NVSDK_NGX_VULKAN_GetCapabilityParameters",
            "NVSDK_NGX_VULKAN_Shutdown1",
            "NVSDK_NGX_Parameter_SetUI",
            "NVSDK_NGX_Parameter_SetI",
            "NVSDK_NGX_Parameter_SetF",
            "NVSDK_NGX_Parameter_SetVoidPointer",
            "NVSDK_NGX_Parameter_GetUI",
        };
        char missing[MK8_DLSS_REASON_MAX];
        probe->core_exports_ok =
            check_symbols(library, required, (int)(sizeof required / sizeof required[0]), missing, sizeof missing);
#endif
        lib_close(library);
        if (probe->core_exports_ok) return true;
        probe->status = MK8_DLSS_CORE_EXPORTS_MISSING;
        snprintf(probe->reason, sizeof probe->reason, "NGX core %s lacks Vulkan exports: %s",
                 probe->core, missing);
        return false;
    }
    probe->status = MK8_DLSS_NO_CORE;
#ifdef _WIN32
    snprintf(probe->reason, sizeof probe->reason,
             "no NGX core library (_nvngx.dll): %s; checked HKLM\\SOFTWARE\\NVIDIA Corporation\\Global\\"
             "NGXCore, the plugin directory and the DLL search path (the driver installs it, not "
             "this project)", load_error);
#else
    snprintf(probe->reason, sizeof probe->reason,
             "no NGX core library (libnvidia-ngx.so.1): %s; the core is installed by the NVIDIA "
             "driver, not by this project", load_error);
#endif
    return false;
}

bool mk8_dlss_probe(Mk8DlssProbe *out) {
    Mk8DlssProbe local;
    memset(&local, 0, sizeof local);
    local.status = MK8_DLSS_NO_PLUGIN;
    default_plugin_directory(local.directory, sizeof local.directory);

    if (!probe_plugin(&local)) {
        if (out) *out = local;
        return false;
    }
    if (!probe_core(&local)) {
        if (out) *out = local;
        return false;
    }
    local.status = MK8_DLSS_OK;
    local.reason[0] = 0;
    if (out) *out = local;
    return true;
}

const char *mk8_dlss_requirement_text(unsigned bits, char *buffer, size_t size) {
    if (!bits) return "supported";
    if (!buffer || size < 2) return "unsupported";
    buffer[0] = 0;
    struct {
        unsigned bit;
        const char *text;
    } const reasons[] = {
        {MK8_DLSS_SUPPORT_CHECK_NOT_PRESENT, "feature missing"},
        {MK8_DLSS_SUPPORT_DRIVER_VERSION_UNSUPPORTED, "driver too old"},
        {MK8_DLSS_SUPPORT_ADAPTER_UNSUPPORTED, "GPU unsupported"},
        {MK8_DLSS_SUPPORT_OS_VERSION_BELOW_MINIMUM, "OS too old"},
        {MK8_DLSS_SUPPORT_NOT_IMPLEMENTED, "not implemented"},
    };
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
        if (!(bits & reasons[i].bit)) continue;
        size_t used = strlen(buffer);
        if (used + 1 >= size) break;
        snprintf(buffer + used, size - used, "%s%s", used ? ", " : "", reasons[i].text);
    }
    return buffer[0] ? buffer : "unsupported";
}

// ---------------------------------------------------------------------------------------
// Vulkan / NGX feature path.
// ---------------------------------------------------------------------------------------
#if !defined(MK8_DLSS_PROBE_ONLY)

#include <math.h>
#include <time.h>

#include "vulkan/vk.h"
#include "ngx/mk8_ngx_abi.h"

extern "C" {
VkDescriptorBufferInfo fsr_vk_uniform(const float *values, uint32_t size);
VkDescriptorSet fsr_vk_alloc_set(VkDescriptorSetLayout layout);
VkSampler fsr_vk_default_sampler(bool linear);
VkImageView fsr_vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv,
                              uint32_t ly0, uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkShaderModule fsr_vk_compile_glsl(const char *src, int stage, const char *name);

// exec.c: the main-draw camera scan behind the FSR bridge (toolchain/fsr.py).
extern float g_fsr_view[12];
extern float g_fsr_proj[16];
extern int g_fsr_vp_valid;
extern float g_fsr_jitter_x;
extern float g_fsr_jitter_y;
}

typedef struct Dlss {
    bool tried;
    bool ready;
    bool failed;
    Mk8DlssProbe probe;

    Mk8Lib plugin;
    Mk8Lib core;
    Mk8NgxInitWithProjectIdFn init;
    Mk8NgxInitProjectIdFn init9;
    Mk8NgxGetCapabilityParametersFn get_capability_parameters;
    Mk8NgxParameterSetUiFn set_ui;
    Mk8NgxParameterSetIFn set_i;
    Mk8NgxParameterSetFFn set_f;
    Mk8NgxParameterSetVoidPointerFn set_pointer;
    Mk8NgxParameterGetUiFn get_ui;
    Mk8NgxParameterGetUllFn get_ull;
    Mk8NgxCreateFeature1Fn create_feature1;
    Mk8NgxCreateFeatureFn create_feature;
    Mk8NgxEvaluateFeatureFn evaluate;
    Mk8NgxReleaseFeatureFn release;
    Mk8NgxShutdown1Fn shutdown1;
    Mk8NgxGetFeatureRequirementsFn get_requirements;
    Mk8NgxRequiredExtensionsFn required_extensions;

    Mk8NgxParameter *params;
    Mk8NgxHandle *handle;
    bool ngx_initialised;
    bool reset_next;
    uint32_t rw, rh, ow, oh;
    int quality;
    const char *quality_name;

    // The feature search path and data path stay alive for the process: NGX is given
    // pointers to them in FeatureCommonInfo during Init and may consult them later.
    wchar_t wide_plugin_directory[MK8_DLSS_PATH_MAX];
    wchar_t wide_data_directory[MK8_DLSS_PATH_MAX];
    const wchar_t *search_paths[1];

    VkImage output;
    VmaAllocation output_mem;
    VkImageView output_view;
    VkImage neutral_depth;
    VmaAllocation neutral_depth_mem;
    VkImageView neutral_depth_view;
    bool neutral_primed;
    VkImage motion;
    VmaAllocation motion_mem;
    VkImageView motion_view;
    bool motion_primed;

    VkImageView color_view;
    VkImageView depth_view;

    VkDescriptorSetLayout mv_layout;
    VkPipelineLayout mv_pipeline_layout;
    VkPipeline mv_pipeline;
    bool have_prev;
    float prev_vp[16];
} Dlss;

static Dlss dlss;

static const char *result_name(int result) {
    switch ((unsigned)result) {
    case MK8_NGX_RESULT_SUCCESS: return "success";
    case MK8_NGX_RESULT_FAIL_FEATURE_NOT_SUPPORTED: return "feature-not-supported";
    case MK8_NGX_RESULT_FAIL_PLATFORM_ERROR: return "platform-error";
    case MK8_NGX_RESULT_FAIL_INVALID_PARAMETER: return "invalid-parameter";
    case MK8_NGX_RESULT_FAIL_MISSING_INPUT: return "missing-input";
    case MK8_NGX_RESULT_FAIL_NOT_INITIALIZED: return "not-initialized";
    case MK8_NGX_RESULT_FAIL_UNABLE_TO_INITIALIZE_FEATURE: return "unable-to-initialize-feature";
    case MK8_NGX_RESULT_FAIL_UNABLE_TO_WRITE_TO_APP_DATA_PATH: return "unable-to-write-app-data-path";
    default: return "failure";
    }
}

static void log_result(const char *what, int result) {
    fprintf(stderr, "[dlss] %s failed 0x%08x (%s)\n", what, (unsigned)result, result_name(result));
}

// After a fatal NGX error the SDK is left initialised; shut it down once so a second
// device is not sharing a half-created feature. The plugin/core handles stay loaded.
static void shutdown_ngx(void) {
    if (dlss.ngx_initialised && dlss.shutdown1) dlss.shutdown1(g_vk.dev);
    dlss.ngx_initialised = false;
    dlss.params = NULL;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout,
                    VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage,
                    VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkImageView image_view_of(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

static VkImage make_image(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect, VmaAllocation *allocation) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &image, allocation, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    (void)aspect;
    return image;
}

static void destroy_feature_images(void) {
    if (dlss.handle && dlss.release) dlss.release(dlss.handle);
    dlss.handle = NULL;
    if (dlss.output_view) vkDestroyImageView(g_vk.dev, dlss.output_view, NULL);
    if (dlss.output) {
        vkDestroyImage(g_vk.dev, dlss.output, NULL);
        vmaFreeMemory(g_vk.vma, dlss.output_mem);
    }
    dlss.output = VK_NULL_HANDLE;
    dlss.output_mem = VK_NULL_HANDLE;
    dlss.output_view = VK_NULL_HANDLE;
    if (dlss.neutral_depth_view) vkDestroyImageView(g_vk.dev, dlss.neutral_depth_view, NULL);
    if (dlss.neutral_depth) {
        vkDestroyImage(g_vk.dev, dlss.neutral_depth, NULL);
        vmaFreeMemory(g_vk.vma, dlss.neutral_depth_mem);
    }
    dlss.neutral_depth = VK_NULL_HANDLE;
    dlss.neutral_depth_mem = VK_NULL_HANDLE;
    dlss.neutral_depth_view = VK_NULL_HANDLE;
    dlss.neutral_primed = false;
    dlss.color_view = VK_NULL_HANDLE;
    dlss.depth_view = VK_NULL_HANDLE;
    if (dlss.motion_view) vkDestroyImageView(g_vk.dev, dlss.motion_view, NULL);
    if (dlss.motion) {
        vkDestroyImage(g_vk.dev, dlss.motion, NULL);
        vmaFreeMemory(g_vk.vma, dlss.motion_mem);
    }
    dlss.motion = VK_NULL_HANDLE;
    dlss.motion_mem = VK_NULL_HANDLE;
    dlss.motion_view = VK_NULL_HANDLE;
    dlss.motion_primed = false;
    dlss.have_prev = false;
}

static bool mkdir_p(const char *path) {
    char buffer[MK8_DLSS_PATH_MAX];
    size_t length = strlen(path);
    if (!length || length >= sizeof buffer) return false;
    memcpy(buffer, path, length + 1);
    for (size_t i = 1; i < length; i++) {
        if (buffer[i] != '/' && buffer[i] != '\\') continue;
        char saved = buffer[i];
        buffer[i] = 0;
#ifdef _WIN32
        _mkdir(buffer);
#else
        mkdir(buffer, 0770);
#endif
        buffer[i] = saved;
    }
#ifdef _WIN32
    if (_mkdir(buffer) == 0) return true;
    return errno == EEXIST;
#else
    if (mkdir(buffer, 0770) == 0) return true;
    return errno == EEXIST;
#endif
}

static bool to_wide(const char *text, wchar_t *out, size_t capacity) {
    if (!text || !out || !capacity) return false;
#ifdef _WIN32
    return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int)capacity) > 0;
#else
    size_t written = mbstowcs(out, text, capacity);
    return written != (size_t)-1 && written < capacity;
#endif
}

// NGX writes logs/config under this directory; it must be writable by the process.
static bool ngx_data_directory(char *out, size_t size) {
    const char *base = getenv("XDG_DATA_HOME");
    if (base && *base) {
        snprintf(out, size, "%s/mk8online/nvidia/ngx-data", base);
        return true;
    }
    base = getenv("HOME");
#ifdef _WIN32
    if (!base || !*base) base = getenv("USERPROFILE");
#endif
    if (!base || !*base) return false;
    snprintf(out, size, "%s/.local/share/mk8online/nvidia/ngx-data", base);
    return true;
}

#ifdef _WIN32
// The Windows runtime exposes parameters only through the NVSDK_NGX_Parameter C++ object.
// MSVC's overload order differs from nvsdk_ngx_params.h; the verified slots are in
// mk8_ngx_abi.h. These thunks are installed only when the C helpers are absent, so a core
// that still exports them keeps using them.
static void *ngx_parameter_slot(Mk8NgxParameter *params, int index) {
    if (!params) return NULL;
    void **vtable = *(void ***)params;
    return vtable ? vtable[index] : NULL;
}

static void MK8_NGX_CONV win_parameter_set_ui(Mk8NgxParameter *params, const char *name, unsigned value) {
    typedef void(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, unsigned);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_SET_UI);
    if (fn) fn(params, name, value);
}
static void MK8_NGX_CONV win_parameter_set_i(Mk8NgxParameter *params, const char *name, int value) {
    typedef void(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, int);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_SET_I);
    if (fn) fn(params, name, value);
}
static void MK8_NGX_CONV win_parameter_set_f(Mk8NgxParameter *params, const char *name, float value) {
    typedef void(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, float);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_SET_F);
    if (fn) fn(params, name, value);
}
static void MK8_NGX_CONV win_parameter_set_ull(Mk8NgxParameter *params, const char *name,
                                               unsigned long long value) {
    typedef void(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, unsigned long long);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_SET_ULL);
    if (fn) fn(params, name, value);
}
static void MK8_NGX_CONV win_parameter_set_pointer(Mk8NgxParameter *params, const char *name, void *value) {
    typedef void(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, void *);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_SET_POINTER);
    if (fn) fn(params, name, value);
}
static int MK8_NGX_CONV win_parameter_get_ui(Mk8NgxParameter *params, const char *name, unsigned *out) {
    typedef int(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, unsigned *);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_GET_UI);
    return fn ? fn(params, name, out) : (int)MK8_NGX_RESULT_FAIL;
}
static int MK8_NGX_CONV win_parameter_get_ull(Mk8NgxParameter *params, const char *name,
                                              unsigned long long *out) {
    typedef int(MK8_NGX_CONV *Fn)(Mk8NgxParameter *, const char *, unsigned long long *);
    Fn fn = (Fn)ngx_parameter_slot(params, MK8_NGX_WIN_VTABLE_GET_ULL);
    return fn ? fn(params, name, out) : (int)MK8_NGX_RESULT_FAIL;
}
#endif

static bool resolve_runtime_symbols(void) {
#ifdef _WIN32
    // The core owns dispatch: the plugin's own CreateFeature/EvaluateFeature keep a separate
    // state and return FAIL_NotInitialized when called directly (verified on 310.9.16).
    dlss.create_feature1 = (Mk8NgxCreateFeature1Fn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_CreateFeature1");
    dlss.create_feature = (Mk8NgxCreateFeatureFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_CreateFeature");
    dlss.evaluate = (Mk8NgxEvaluateFeatureFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_EvaluateFeature");
    dlss.release = (Mk8NgxReleaseFeatureFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!dlss.create_feature1)
        dlss.create_feature1 = (Mk8NgxCreateFeature1Fn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_CreateFeature1");
    if (!dlss.create_feature)
        dlss.create_feature = (Mk8NgxCreateFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_CreateFeature");
    if (!dlss.evaluate)
        dlss.evaluate = (Mk8NgxEvaluateFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_EvaluateFeature");
    if (!dlss.release)
        dlss.release = (Mk8NgxReleaseFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_ReleaseFeature");
    dlss.init9 = (Mk8NgxInitProjectIdFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_Init_ProjectID");
#else
    dlss.create_feature1 = (Mk8NgxCreateFeature1Fn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_CreateFeature1");
    dlss.create_feature = (Mk8NgxCreateFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_CreateFeature");
    dlss.evaluate = (Mk8NgxEvaluateFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_EvaluateFeature");
    dlss.release = (Mk8NgxReleaseFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_ReleaseFeature");
#endif
    dlss.get_requirements =
        (Mk8NgxGetFeatureRequirementsFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    dlss.init = (Mk8NgxInitWithProjectIdFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_Init_with_ProjectID");
    dlss.get_capability_parameters =
        (Mk8NgxGetCapabilityParametersFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    dlss.set_ui = (Mk8NgxParameterSetUiFn)lib_symbol(dlss.core, "NVSDK_NGX_Parameter_SetUI");
    dlss.set_i = (Mk8NgxParameterSetIFn)lib_symbol(dlss.core, "NVSDK_NGX_Parameter_SetI");
    dlss.set_f = (Mk8NgxParameterSetFFn)lib_symbol(dlss.core, "NVSDK_NGX_Parameter_SetF");
    dlss.set_pointer = (Mk8NgxParameterSetVoidPointerFn)lib_symbol(dlss.core, "NVSDK_NGX_Parameter_SetVoidPointer");
    dlss.get_ui = (Mk8NgxParameterGetUiFn)lib_symbol(dlss.core, "NVSDK_NGX_Parameter_GetUI");
    dlss.get_ull = (Mk8NgxParameterGetUllFn)lib_symbol(dlss.core, "NVSDK_NGX_Parameter_GetULL");
    dlss.shutdown1 = (Mk8NgxShutdown1Fn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_Shutdown1");
    dlss.required_extensions =
        (Mk8NgxRequiredExtensionsFn)lib_symbol(dlss.core, "NVSDK_NGX_VULKAN_RequiredExtensions");
#ifdef _WIN32
    if (!dlss.set_ui) dlss.set_ui = win_parameter_set_ui;
    if (!dlss.set_i) dlss.set_i = win_parameter_set_i;
    if (!dlss.set_f) dlss.set_f = win_parameter_set_f;
    if (!dlss.set_pointer) dlss.set_pointer = win_parameter_set_pointer;
    if (!dlss.get_ui) dlss.get_ui = win_parameter_get_ui;
    if (!dlss.get_ull) dlss.get_ull = win_parameter_get_ull;
#endif
    return (dlss.init || dlss.init9) && dlss.get_capability_parameters && dlss.set_ui && dlss.set_i &&
           dlss.set_f && dlss.set_pointer && dlss.get_ui && dlss.evaluate && dlss.release &&
           (dlss.create_feature1 || dlss.create_feature);
}

static const char *quality_for(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int *quality) {
    // The game fixes the render resolution and the window fixes the display resolution; pick the
    // NGX quality mode that describes that ratio. Equal sizes are DLAA (native-resolution AA).
    double ratio = ow ? (double)rw / (double)ow : 1.0;
    if (rw + 1 >= ow && rh + 1 >= oh) {
        *quality = MK8_NGX_PERFQUALITY_DLAA;
        return "dlaa";
    }
    if (ratio <= 0.40) {
        *quality = MK8_NGX_PERFQUALITY_ULTRA_PERFORMANCE;
        return "ultra-performance";
    }
    if (ratio <= 0.55) {
        *quality = MK8_NGX_PERFQUALITY_MAX_PERF;
        return "performance";
    }
    if (ratio <= 0.65) {
        *quality = MK8_NGX_PERFQUALITY_BALANCED;
        return "balanced";
    }
    if (ratio <= 0.80) {
        *quality = MK8_NGX_PERFQUALITY_MAX_QUALITY;
        return "quality";
    }
    *quality = MK8_NGX_PERFQUALITY_ULTRA_QUALITY;
    return "ultra-quality";
}

static bool load_ngx(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    memset(&dlss.probe, 0, sizeof dlss.probe);
    if (!mk8_dlss_probe(&dlss.probe)) return false;

    dlss.plugin = lib_open(dlss.probe.plugin);
    if (!dlss.plugin) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "DLSS plugin %s did not load now",
                 dlss.probe.plugin);
        return false;
    }
    dlss.core = lib_open(dlss.probe.core);
    if (!dlss.core) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "NGX core %s did not load now",
                 dlss.probe.core);
        return false;
    }
    if (!resolve_runtime_symbols()) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "NGX exports disappeared after the probe");
        return false;
    }

    const char *project_id = getenv("MK8_DLSS_PROJECT_ID");
    if (!project_id || !*project_id) project_id = "6f1d2c3a-8b4e-4d7a-9c15-3e0a5b7c9d21";

    char data_directory[MK8_DLSS_PATH_MAX];
    if (!ngx_data_directory(data_directory, sizeof data_directory)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "no writable NGX data directory: XDG_DATA_HOME and HOME are unset");
        return false;
    }
    if (!mkdir_p(data_directory)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "cannot create NGX data directory %s: %s", data_directory, strerror(errno));
        return false;
    }
    if (!to_wide(dlss.probe.directory, dlss.wide_plugin_directory, MK8_DLSS_PATH_MAX) ||
        !to_wide(data_directory, dlss.wide_data_directory, MK8_DLSS_PATH_MAX)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "cannot convert the plugin path to wchar_t (non-UTF-8 locale?)");
        return false;
    }

    dlss.search_paths[0] = dlss.wide_plugin_directory;
    Mk8NgxFeatureCommonInfo common = {};
    common.PathListInfo.Path = dlss.search_paths;
    common.PathListInfo.Length = 1;

    Mk8NgxFeatureDiscoveryInfo discovery = {};
    discovery.SDKVersion = MK8_NGX_API_VERSION;
    discovery.FeatureID = MK8_NGX_FEATURE_SUPER_SAMPLING;
    discovery.Identifier.IdentifierType = MK8_NGX_IDENTIFIER_PROJECT_ID;
    discovery.Identifier.v.ProjectDesc.ProjectId = project_id;
    discovery.Identifier.v.ProjectDesc.EngineType = MK8_NGX_ENGINE_TYPE_CUSTOM;
    discovery.Identifier.v.ProjectDesc.EngineVersion = "mk8-online";
    discovery.ApplicationDataPath = dlss.wide_data_directory;
    discovery.FeatureInfo = &common;

    if (dlss.get_requirements) {
        Mk8NgxFeatureRequirement requirement = {};
        int rc = dlss.get_requirements(g_vk.inst, g_vk.phys, &discovery, &requirement);
        if (MK8_NGX_SUCCEEDED(rc)) {
            if (requirement.FeatureSupported != 0) {
                char reasons[256];
                unsigned bits = (unsigned)requirement.FeatureSupported;
                mk8_dlss_requirement_text(bits, reasons, sizeof reasons);
                snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                         "NGX reports SuperSampling unsupported (0x%x: %s; min GPU arch 0x%x)", bits,
                         reasons, requirement.MinHWArchitecture);
                return false;
            }
            fprintf(stderr, "[dlss] feature requirements: supported (min GPU arch 0x%x, min OS %s)\n",
                    requirement.MinHWArchitecture, requirement.MinOSVersion);
        } else {
            fprintf(stderr, "[dlss] GetFeatureRequirements failed 0x%08x (%s); trying Init\n", (unsigned)rc,
                    result_name(rc));
        }
    }

    if (dlss.required_extensions) {
        unsigned instance_count = 0, device_count = 0;
        const char **instance_names = NULL;
        const char **device_names = NULL;
        int rc = dlss.required_extensions(&instance_count, &instance_names, &device_count, &device_names);
        if (MK8_NGX_SUCCEEDED(rc)) {
            fprintf(stderr, "[dlss] NGX required extensions: %u instance, %u device\n", instance_count,
                    device_count);
            for (unsigned i = 0; i < device_count && i < 16; i++)
                fprintf(stderr, "[dlss]   device extension: %s\n", device_names[i] ? device_names[i] : "?");
            if (device_count)
                fprintf(stderr, "[dlss] the device must be created with those extensions (device.c); "
                                "Init will fail cleanly if it was not\n");
        }
    }

    fprintf(stderr, "[dlss] NGX init: project %s, plugin %s, data %s\n", project_id, dlss.probe.plugin,
            data_directory);
    const char *init_name = "NVSDK_NGX_VULKAN_Init_with_ProjectID";
    int rc;
#ifdef _WIN32
    if (dlss.init9) {
        init_name = "NVSDK_NGX_VULKAN_Init_ProjectID";
        fprintf(stderr, "[dlss] using the driver Init_ProjectID form (9 arguments, no vkGet*ProcAddr)\n");
        rc = dlss.init9(project_id, MK8_NGX_ENGINE_TYPE_CUSTOM, "mk8-online 4.0.1",
                        dlss.wide_data_directory, g_vk.inst, g_vk.phys, g_vk.dev, MK8_NGX_API_VERSION,
                        &common);
    } else
#endif
    {
        rc = dlss.init(project_id, MK8_NGX_ENGINE_TYPE_CUSTOM, "mk8-online 4.0.1",
                       dlss.wide_data_directory, g_vk.inst, g_vk.phys, g_vk.dev, vkGetInstanceProcAddr,
                       vkGetDeviceProcAddr, &common, MK8_NGX_API_VERSION);
    }
    if (MK8_NGX_SUCCEEDED(rc))
        fprintf(stderr, "[dlss] %s: 0x%08x (success)\n", init_name, (unsigned)rc);
    if (MK8_NGX_FAILED(rc)) {
        log_result(init_name, rc);
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "%s failed 0x%08x (%s)", init_name,
                 (unsigned)rc, result_name(rc));
        if (dlss.shutdown1) dlss.shutdown1(g_vk.dev);
        return false;
    }
    dlss.ngx_initialised = true;
    fprintf(stderr, "[dlss] provider NGX API 0x%x from %s\n", MK8_NGX_API_VERSION, dlss.probe.plugin);

    rc = dlss.get_capability_parameters(&dlss.params);
    if (MK8_NGX_FAILED(rc) || !dlss.params) {
        log_result("NVSDK_NGX_VULKAN_GetCapabilityParameters", rc);
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "NVSDK_NGX_VULKAN_GetCapabilityParameters failed 0x%08x (%s)", (unsigned)rc,
                 result_name(rc));
        shutdown_ngx();
        return false;
    }
    unsigned available = 0;
    rc = dlss.get_ui(dlss.params, MK8_NGX_PARAM_SUPER_SAMPLING_AVAILABLE, &available);
    if (MK8_NGX_FAILED(rc) && dlss.get_ull) {
        unsigned long long value = 0;
        if (MK8_NGX_SUCCEEDED(dlss.get_ull(dlss.params, MK8_NGX_PARAM_SUPER_SAMPLING_AVAILABLE, &value)))
            available = (unsigned)value;
        rc = MK8_NGX_RESULT_SUCCESS;
    }
    if (MK8_NGX_FAILED(rc)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "SuperSampling.Available could not be read (0x%08x)", (unsigned)rc);
        shutdown_ngx();
        return false;
    }
    fprintf(stderr, "[dlss] SuperSampling.Available = %u\n", available);
    if (available != 1) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "SuperSampling.Available = %u: this GPU or driver does not expose DLSS Super Resolution",
                 available);
        shutdown_ngx();
        return false;
    }

    dlss.quality_name = quality_for(rw, rh, ow, oh, &dlss.quality);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_CREATION_NODE_MASK, 1);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_VISIBILITY_NODE_MASK, 1);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_WIDTH, rw);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_HEIGHT, rh);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_OUT_WIDTH, ow);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_OUT_HEIGHT, oh);
    dlss.set_i(dlss.params, MK8_NGX_PARAM_PERF_QUALITY, dlss.quality);
    int create_flags = MK8_NGX_DLSS_FLAG_NONE;
    const char *inverted = getenv("MK8_DLSS_DEPTH_INVERTED");
    if (inverted && *inverted && atoi(inverted) != 0) create_flags |= MK8_NGX_DLSS_FLAG_DEPTH_INVERTED;
    dlss.set_i(dlss.params, MK8_NGX_PARAM_DLSS_CREATE_FLAGS, create_flags);
    dlss.set_i(dlss.params, MK8_NGX_PARAM_DLSS_ENABLE_OUTPUT_SUBRECTS, 0);
    fprintf(stderr, "[dlss] creating %s feature %ux%u -> %ux%u, depth %s\n", dlss.quality_name, rw, rh, ow,
            oh, (create_flags & MK8_NGX_DLSS_FLAG_DEPTH_INVERTED) ? "inverted" : "standard");
    return true;
}

// Camera-derived motion vectors, identical in purpose and maths to fsr.cpp's run_mv: the
// current depth is projected with the inverse view-projection and re-projected with the
// previous frame's, giving pixel-space deltas. Karts have no object vectors; the state is
// logged so a prototype is not mistaken for qualified output.
static void mul4(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

static bool invert4(const float m[16], float out[16]) {
    float a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) a[r][c] = m[r * 4 + c];
        for (int c = 0; c < 4; c++) a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
    }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++)
            if (fabsf(a[r][c]) > fabsf(a[pivot][c])) pivot = r;
        if (fabsf(a[pivot][c]) < 1e-9f) return false;
        if (pivot != c)
            for (int k = 0; k < 8; k++) {
                float t = a[c][k];
                a[c][k] = a[pivot][k];
                a[pivot][k] = t;
            }
        float d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                float f = a[r][c];
                if (f == 0.0f) continue;
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][4 + c];
    return true;
}

static bool create_mv_resources(void) {
    if (dlss.mv_pipeline) return true;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 3;
    layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_vk.dev, &layout, NULL, &dlss.mv_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pipeline_layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &dlss.mv_layout;
    if (vkCreatePipelineLayout(g_vk.dev, &pipeline_layout, NULL, &dlss.mv_pipeline_layout) != VK_SUCCESS)
        return false;
    static const char *source =
        "#version 450\n"
        "layout(local_size_x = 16, local_size_y = 16) in;\n"
        "layout(set = 0, binding = 0) uniform U { mat4 invVP; mat4 prevVP; vec2 size; } u;\n"
        "layout(set = 0, binding = 1) uniform sampler2D depthTex;\n"
        "layout(set = 0, binding = 2, rg16f) uniform image2D mvOut;\n"
        "void main() {\n"
        "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
        "  if (p.x >= int(u.size.x) || p.y >= int(u.size.y)) return;\n"
        "  vec2 uv = (vec2(p) + 0.5) / u.size;\n"
        "  float d = texture(depthTex, uv).r;\n"
        "  if (d >= 0.9999) { imageStore(mvOut, p, vec4(0.0)); return; }\n"
        "  vec4 w = u.invVP * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
        "  if (abs(w.w) < 1e-9) { imageStore(mvOut, p, vec4(0.0)); return; }\n"
        "  w /= w.w;\n"
        "  vec4 pc = u.prevVP * w;\n"
        "  if (pc.w <= 0.0) { imageStore(mvOut, p, vec4(0.0)); return; }\n"
        "  vec2 puv = pc.xy / pc.w * 0.5 + 0.5;\n"
        "  imageStore(mvOut, p, vec4((puv - uv) * u.size, 0.0, 0.0));\n"
        "}\n";
    VkShaderModule module = fsr_vk_compile_glsl(source, VK_SHADER_STAGE_COMPUTE_BIT, "dlss_mv");
    if (!module) return false;
    VkComputePipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = module;
    pipeline.stage.pName = "main";
    pipeline.layout = dlss.mv_pipeline_layout;
    VkResult result = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &pipeline, NULL, &dlss.mv_pipeline);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[dlss] vkCreateComputePipelines(dlss_mv) failed: %d\n", (int)result);
        return false;
    }
    return true;
}

static bool run_mv(VkCommandBuffer cb, TexBackend *depth, uint32_t rw, uint32_t rh) {
    if (!g_fsr_vp_valid || !depth || !depth->image || !dlss.mv_pipeline ||
        !dlss.depth_view)
        return false;
    float v[16] = {g_fsr_view[0], g_fsr_view[1], g_fsr_view[2], g_fsr_view[3],
                   g_fsr_view[4], g_fsr_view[5], g_fsr_view[6], g_fsr_view[7],
                   g_fsr_view[8], g_fsr_view[9], g_fsr_view[10], g_fsr_view[11],
                   0.0f, 0.0f, 0.0f, 1.0f};
    float vp[16], inv[16], prev[16];
    mul4(g_fsr_proj, v, vp);
    if (!invert4(vp, inv)) return false;
    if (dlss.have_prev) memcpy(prev, dlss.prev_vp, sizeof prev);
    else memcpy(prev, vp, sizeof prev);
    float uniform[36] = {};
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            uniform[c * 4 + r] = inv[r * 4 + c];
            uniform[16 + c * 4 + r] = prev[r * 4 + c];
        }
    uniform[32] = (float)rw;
    uniform[33] = (float)rh;
    barrier(cb, dlss.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkDescriptorSet set = fsr_vk_alloc_set(dlss.mv_layout);
    VkDescriptorBufferInfo buffer_info = fsr_vk_uniform(uniform, sizeof uniform);
    VkDescriptorImageInfo depth_info = {
        fsr_vk_default_sampler(true), dlss.depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo output_info = {VK_NULL_HANDLE, dlss.motion_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL,
         &buffer_info, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         &depth_info, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         &output_info, NULL, NULL}};
    vkUpdateDescriptorSets(g_vk.dev, 3, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.mv_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.mv_pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (rw + 15) / 16, (rh + 15) / 16, 1);
    barrier(cb, dlss.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    memcpy(dlss.prev_vp, vp, sizeof dlss.prev_vp);
    dlss.have_prev = true;
    return true;
}

static bool ensure_motion(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (!dlss.motion || dlss.motion_view == VK_NULL_HANDLE) {
        dlss.motion = make_image(VK_FORMAT_R16G16_SFLOAT, w, h,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_STORAGE_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, &dlss.motion_mem);
        dlss.motion_view = dlss.motion ? image_view_of(dlss.motion, VK_FORMAT_R16G16_SFLOAT,
                                                       VK_IMAGE_ASPECT_COLOR_BIT)
                                       : VK_NULL_HANDLE;
        dlss.motion_primed = false;
    }
    if (!dlss.motion || !dlss.motion_view) return false;
    if (!create_mv_resources()) return false;
    if (!dlss.motion_primed) {
        barrier(cb, dlss.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dlss.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(cb, dlss.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        dlss.motion_primed = true;
    }
    return true;
}

static bool ensure_neutral_depth(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (dlss.neutral_depth && dlss.neutral_depth_view) return true;
    dlss.neutral_depth = make_image(VK_FORMAT_R32_SFLOAT, w, h,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT, &dlss.neutral_depth_mem);
    if (!dlss.neutral_depth) return false;
    dlss.neutral_depth_view = image_view_of(dlss.neutral_depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!dlss.neutral_depth_view) return false;
    dlss.neutral_primed = false;
    barrier(cb, dlss.neutral_depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue one = {{1.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, dlss.neutral_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
    barrier(cb, dlss.neutral_depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    dlss.neutral_primed = true;
    return true;
}

static bool create_output(uint32_t ow, uint32_t oh) {
    dlss.output = make_image(VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &dlss.output_mem);
    if (!dlss.output) return false;
    dlss.output_view = image_view_of(dlss.output, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    return dlss.output_view != VK_NULL_HANDLE;
}

static bool ensure_feature(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (dlss.handle && dlss.rw == rw && dlss.rh == rh && dlss.ow == ow && dlss.oh == oh) return true;
    if (dlss.handle || dlss.output) {
        vkDeviceWaitIdle(g_vk.dev);
        destroy_feature_images();
    }
    if (!create_output(ow, oh)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "cannot create the %ux%u DLSS output image", ow,
                 oh);
        return false;
    }
    // The provider writes the output in GENERAL and the present path samples it there.
    barrier(cb, dlss.output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (!ensure_motion(cb, rw, rh)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "cannot create the %ux%u DLSS motion image", rw,
                 rh);
        return false;
    }
    if (!ensure_neutral_depth(cb, rw, rh)) {
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason, "cannot create the %ux%u neutral depth image", rw,
                 rh);
        return false;
    }
    if (!load_ngx(rw, rh, ow, oh)) return false;

    int rc = dlss.create_feature1 ? dlss.create_feature1(g_vk.dev, cb, MK8_NGX_FEATURE_SUPER_SAMPLING,
                                                         dlss.params, &dlss.handle)
                                  : dlss.create_feature(cb, MK8_NGX_FEATURE_SUPER_SAMPLING, dlss.params,
                                                        &dlss.handle);
    if (MK8_NGX_SUCCEEDED(rc) && dlss.handle)
        fprintf(stderr, "[dlss] NVSDK_NGX_VULKAN_CreateFeature: 0x%08x (success)\n", (unsigned)rc);
    if (MK8_NGX_FAILED(rc) || !dlss.handle) {
        log_result("NVSDK_NGX_VULKAN_CreateFeature", rc);
        unsigned init_result = 0;
        if (dlss.params &&
            MK8_NGX_SUCCEEDED(dlss.get_ui(dlss.params, MK8_NGX_PARAM_SUPER_SAMPLING_FEATURE_INIT_RESULT,
                                          &init_result)))
            fprintf(stderr, "[dlss] SuperSampling.FeatureInitResult = %u\n", init_result);
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "NVSDK_NGX_VULKAN_CreateFeature failed 0x%08x (%s)", (unsigned)rc, result_name(rc));
        dlss.handle = NULL;
        // The current command buffer may still reference these images. The feature is
        // disabled for this process; retain them until device teardown.
        return false;
    }
    dlss.rw = rw;
    dlss.rh = rh;
    dlss.ow = ow;
    dlss.oh = oh;
    dlss.reset_next = true;
    return true;
}

extern "C" VkImageView mk8_dlss_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, uint32_t rw,
                                        uint32_t rh, uint32_t ow, uint32_t oh, float frame_ms) {
    if (!getenv("MK8_DLSS")) return VK_NULL_HANDLE;
    if (getenv("MK8_FSR_FG")) {  // frame generation owns the final image; combining is not finished
        static int once;
        if (!once++)
            fprintf(stderr, "[dlss] MK8_FSR_FG is set: DLSS Super Resolution stays off\n");
        return VK_NULL_HANDLE;
    }
    if (!cb || !color || !color->image || !rw || !rh || !ow || !oh) return VK_NULL_HANDLE;
    if (ow < rw || oh < rh) {
        static int once;
        if (!once++)
            fprintf(stderr, "[dlss] display %ux%u is smaller than render %ux%u: not an upscale\n", ow, oh,
                    rw, rh);
        return VK_NULL_HANDLE;
    }

    if (!dlss.tried) {
        dlss.tried = true;
        Mk8DlssProbe probe;
        if (!mk8_dlss_probe(&probe)) {
            mk8_dlss_log_unavailable(&probe);
            fprintf(stderr, "[dlss] falling back to the existing FSR/bilinear path\n");
            dlss.failed = true;
            return VK_NULL_HANDLE;
        }
        dlss.ready = true;
    }
    if (!dlss.ready || dlss.failed) return VK_NULL_HANDLE;

    if (!ensure_feature(cb, rw, rh, ow, oh)) {
        if (dlss.probe.reason[0]) {
            mk8_dlss_log_unavailable(&dlss.probe);
            dlss.probe.reason[0] = 0;
        }
        dlss.failed = true;
        return VK_NULL_HANDLE;
    }

    bool real_depth = depth && depth->image && depth->width == rw && depth->height == rh;
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_R32_SFLOAT;
    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (real_depth) {
        VkImageAspectFlags aspect =
            (depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        dlss.depth_view = fsr_vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1, {}, aspect);
        if (!dlss.depth_view) return VK_NULL_HANDLE;
        depth_image = depth->image;
        depth_view = dlss.depth_view;
        depth_format = depth->format;
        depth_aspect = aspect;
        barrier(cb, depth_image, aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    } else {
        if (!ensure_neutral_depth(cb, rw, rh)) return VK_NULL_HANDLE;
        depth_image = dlss.neutral_depth;
        depth_view = dlss.neutral_depth_view;
    }

    static int depth_state = -1;
    if ((int)real_depth != depth_state) {
        depth_state = (int)real_depth;
        fprintf(stderr, "[dlss] depth source: %s %ux%u fmt %d\n",
                real_depth ? "captured scene depth" : "neutral (no captured scene depth)", rw, rh,
                depth_format);
    }

    if (!ensure_motion(cb, rw, rh)) return VK_NULL_HANDLE;
    bool motion = real_depth && run_mv(cb, depth, rw, rh);
    static int motion_state = -1;
    if ((int)motion != motion_state) {
        motion_state = (int)motion;
        if (motion) fprintf(stderr, "[dlss] motion vectors: camera-derived\n");
        else
            fprintf(stderr, "[dlss] WARNING: motion vectors are zeros (no captured view/projection); "
                            "DLSS will ghost on everything that moves\n");
    }

    VkImageAspectFlags color_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    dlss.color_view = fsr_vk_image_view(color, VK_IMAGE_VIEW_TYPE_2D, color->format, 0, 1, 0, 1, {}, color_aspect);
    if (!dlss.color_view) return VK_NULL_HANDLE;
    barrier(cb, color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    Mk8NgxResourceVk color_resource = {};
    color_resource.Type = 0;
    color_resource.ReadWrite = false;
    color_resource.Resource.ImageViewInfo.ImageView = dlss.color_view;
    color_resource.Resource.ImageViewInfo.Image = color->image;
    color_resource.Resource.ImageViewInfo.SubresourceRange =
        (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    color_resource.Resource.ImageViewInfo.Format = color->format;
    color_resource.Resource.ImageViewInfo.Width = rw;
    color_resource.Resource.ImageViewInfo.Height = rh;

    Mk8NgxResourceVk depth_resource = color_resource;
    depth_resource.Resource.ImageViewInfo.ImageView = depth_view;
    depth_resource.Resource.ImageViewInfo.Image = depth_image;
    depth_resource.Resource.ImageViewInfo.SubresourceRange =
        (VkImageSubresourceRange){depth_aspect, 0, 1, 0, 1};
    depth_resource.Resource.ImageViewInfo.Format = depth_format;

    Mk8NgxResourceVk motion_resource = color_resource;
    motion_resource.Resource.ImageViewInfo.ImageView = dlss.motion_view;
    motion_resource.Resource.ImageViewInfo.Image = dlss.motion;
    motion_resource.Resource.ImageViewInfo.Format = VK_FORMAT_R16G16_SFLOAT;
    motion_resource.Resource.ImageViewInfo.Width = rw;
    motion_resource.Resource.ImageViewInfo.Height = rh;

    Mk8NgxResourceVk output_resource = color_resource;
    output_resource.Type = 0;
    output_resource.ReadWrite = true;
    output_resource.Resource.ImageViewInfo.ImageView = dlss.output_view;
    output_resource.Resource.ImageViewInfo.Image = dlss.output;
    output_resource.Resource.ImageViewInfo.Format = VK_FORMAT_R8G8B8A8_UNORM;
    output_resource.Resource.ImageViewInfo.Width = ow;
    output_resource.Resource.ImageViewInfo.Height = oh;

    float jitter_x = g_fsr_jitter_x, jitter_y = g_fsr_jitter_y;

    dlss.set_ui(dlss.params, MK8_NGX_PARAM_WIDTH, rw);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_HEIGHT, rh);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_OUT_WIDTH, ow);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_OUT_HEIGHT, oh);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_RENDER_SUBRECT_WIDTH, rw);
    dlss.set_ui(dlss.params, MK8_NGX_PARAM_RENDER_SUBRECT_HEIGHT, rh);
    dlss.set_pointer(dlss.params, MK8_NGX_PARAM_COLOR, &color_resource);
    dlss.set_pointer(dlss.params, MK8_NGX_PARAM_OUTPUT, &output_resource);
    dlss.set_pointer(dlss.params, MK8_NGX_PARAM_DEPTH, &depth_resource);
    dlss.set_pointer(dlss.params, MK8_NGX_PARAM_MOTION_VECTORS, &motion_resource);
    dlss.set_f(dlss.params, MK8_NGX_PARAM_JITTER_X, jitter_x);
    dlss.set_f(dlss.params, MK8_NGX_PARAM_JITTER_Y, jitter_y);
    dlss.set_f(dlss.params, MK8_NGX_PARAM_MV_SCALE_X, 1.0f);
    dlss.set_f(dlss.params, MK8_NGX_PARAM_MV_SCALE_Y, 1.0f);
    dlss.set_f(dlss.params, MK8_NGX_PARAM_SHARPNESS, 0.0f);
    dlss.set_f(dlss.params, MK8_NGX_PARAM_PRE_EXPOSURE, 1.0f);
    if (frame_ms > 0) dlss.set_f(dlss.params, MK8_NGX_PARAM_FRAME_TIME_DELTA_MS, frame_ms);
    bool reset = dlss.reset_next || !motion;
    dlss.reset_next = false;
    dlss.set_i(dlss.params, MK8_NGX_PARAM_RESET, reset ? 1 : 0);

    int rc = dlss.evaluate(cb, dlss.handle, dlss.params, NULL);
    // Leave the game image in GENERAL, as fsr.cpp does: the next frame renders into it there.
    barrier(cb, color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (real_depth)
        barrier(cb, depth_image, depth_aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (MK8_NGX_FAILED(rc)) {
        dlss.failed = true;
        static int warned;
        if (warned++ < 3) log_result("NVSDK_NGX_VULKAN_EvaluateFeature", rc);
        return VK_NULL_HANDLE;
    }
    static int logged;
    if (!logged++) {
        fprintf(stderr, "[dlss] NVSDK_NGX_VULKAN_EvaluateFeature: 0x%08x (success)\n", (unsigned)rc);
        fprintf(stderr, "[dlss] upscale active %ux%u -> %ux%u (quality %s, depth %s, motion %s, "
                        "jitter %.3f,%.3f)\n",
                rw, rh, ow, oh, dlss.quality_name, real_depth ? "game" : "neutral",
                motion ? "camera" : "zeros", jitter_x, jitter_y);
    }
    return dlss.output_view;
}

#ifdef MK8_DLSS_TEST_API
// Test-only accessor used by tools/dlss_bridge_harness.cpp to read the feature's output
// back. The runtime consumes the returned view directly and never needs the image handle.
extern "C" VkImage mk8_dlss_test_output_image(void) { return dlss.ready ? dlss.output : VK_NULL_HANDLE; }
extern "C" VkFormat mk8_dlss_test_output_format(void) { return VK_FORMAT_R8G8B8A8_UNORM; }
#endif

#endif  // !MK8_DLSS_PROBE_ONLY

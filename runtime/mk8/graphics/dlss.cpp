// NVIDIA DLSS Super Resolution / DLAA bridge (host side, no guest changes).
//
// Loads the driver-provided NGX core and the DLSS feature plugin at first use, creates a
// SuperSampling feature for the game image and records one EvaluateFeature into the current
// command buffer. MK8_DLSS=1 selects it from fsr.cpp; when anything is missing the bridge
// returns VK_NULL_HANDLE with one reasoned line and the existing FSR/bilinear path runs.
//
// Reused inputs, same as the FSR bridge: colour at render resolution, the captured scene
// depth (or a neutral image), camera-derived motion vectors in render-pixel units and the
// Halton jitter offset. Motion vectors are recomputed here with the same depth/projection
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
static Mk8Lib lib_open(const char *path) { return LoadLibraryA(path); }
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
    if (path_join(path, sizeof path, probe->directory, "LICENSE.txt") && file_exists(path)) {
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
                     "no %s plugin (%s*) in %s; fetch it with tools/fetch_vendor_sdk.py "
                     "--accept-license nvidia", label, plugin_prefix, probe->directory);
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

static bool probe_plugin_fg(Mk8DlssProbe *probe) {
    return probe_plugin_named(probe, MK8_DLSSG_PLUGIN_PREFIX, MK8_DLSS_PLUGIN_SUFFIX, "DLSS-G");
}

static bool probe_core(Mk8DlssProbe *probe) {
    const char *configured = getenv("MK8_DLSS_CORE");
    char attempts[4][MK8_DLSS_PATH_MAX];
    int count = 0;
    if (configured && *configured) {
        snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", configured);
    } else {
#ifdef _WIN32
        static const char *const names[] = {"_nvngx.dll", "nvngx.dll"};
#else
        static const char *const names[] = {"libnvidia-ngx.so.1", "libnvidia-ngx.so"};
#endif
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", names[i]);
        // A user may keep the driver core beside the downloaded plugin; try it last so the
        // driver's own copy still wins.
        char beside[MK8_DLSS_PATH_MAX];
        if (path_join(beside, sizeof beside, probe->directory,
#ifdef _WIN32
                      "_nvngx.dll"
#else
                      "libnvidia-ngx.so.1"
#endif
                      ) &&
            file_exists(beside) && count < (int)(sizeof attempts / sizeof attempts[0]))
            snprintf(attempts[count++], MK8_DLSS_PATH_MAX, "%s", beside);
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
        lib_close(library);
        if (probe->core_exports_ok) return true;
        probe->status = MK8_DLSS_CORE_EXPORTS_MISSING;
        snprintf(probe->reason, sizeof probe->reason, "NGX core %s lacks Vulkan exports: %s",
                 probe->core, missing);
        return false;
    }
    probe->status = MK8_DLSS_NO_CORE;
#ifdef _WIN32
    const char *core_name = "_nvngx.dll";
#else
    const char *core_name = "libnvidia-ngx.so.1";
#endif
    snprintf(probe->reason, sizeof probe->reason,
             "no NGX core library (%s): %s; the core is installed by the NVIDIA driver, not by "
             "this project", core_name, load_error);
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

static bool env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && *value && strcmp(value, "0") != 0;
}

bool mk8_dlss_fg_enabled(void) { return env_enabled("MK8_DLSS_FG"); }

bool mk8_dlss_fg_probe(Mk8DlssProbe *out) {
    Mk8DlssProbe local;
    memset(&local, 0, sizeof local);
    local.status = MK8_DLSS_NO_PLUGIN;
    default_plugin_directory(local.directory, sizeof local.directory);

    if (!probe_plugin_fg(&local)) {
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

const char *mk8_dlss_fg_status_name(Mk8DlssLoadStatus status) { return mk8_dlss_status_name(status); }

void mk8_dlss_fg_log_unavailable(const Mk8DlssProbe *probe) {
    const char *reason = probe && probe->reason[0] ? probe->reason : "no reason recorded";
    fprintf(stderr, "[dlss_fg] DLSS Frame Generation unavailable: %s\n", reason);
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
    VkImage color_view_image;
    VkFormat color_view_format;
    uint32_t color_view_w, color_view_h;
    VkImageView depth_view;
    VkImage depth_view_image;
    VkFormat depth_view_format;
    uint32_t depth_view_w, depth_view_h;

    VkDescriptorSetLayout mv_layout;
    VkPipelineLayout mv_pipeline_layout;
    VkPipeline mv_pipeline;
    VkBuffer mv_ubo;
    VmaAllocation mv_ubo_mem;
    void *mv_ubo_ptr;
    bool have_prev;
    float prev_vp[16];
} Dlss;

static Dlss dlss;

static const char *result_name(int result) {
    switch ((unsigned)result) {
    case MK8_NGX_RESULT_SUCCESS: return "success";
    case MK8_NGX_RESULT_FAIL_FEATURE_NOT_SUPPORTED: return "feature-not-supported";
    case MK8_NGX_RESULT_FAIL_INVALID_PARAMETER: return "invalid-parameter";
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
    if (dlss.color_view) vkDestroyImageView(g_vk.dev, dlss.color_view, NULL);
    dlss.color_view = VK_NULL_HANDLE;
    dlss.color_view_image = VK_NULL_HANDLE;
    if (dlss.depth_view) vkDestroyImageView(g_vk.dev, dlss.depth_view, NULL);
    dlss.depth_view = VK_NULL_HANDLE;
    dlss.depth_view_image = VK_NULL_HANDLE;
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

static bool resolve_runtime_symbols(void) {
    dlss.create_feature1 = (Mk8NgxCreateFeature1Fn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_CreateFeature1");
    dlss.create_feature = (Mk8NgxCreateFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_CreateFeature");
    dlss.evaluate = (Mk8NgxEvaluateFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_EvaluateFeature");
    dlss.release = (Mk8NgxReleaseFeatureFn)lib_symbol(dlss.plugin, "NVSDK_NGX_VULKAN_ReleaseFeature");
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
    return dlss.init && dlss.get_capability_parameters && dlss.set_ui && dlss.set_i && dlss.set_f &&
           dlss.set_pointer && dlss.get_ui && dlss.evaluate && dlss.release &&
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
    int rc = dlss.init(project_id, MK8_NGX_ENGINE_TYPE_CUSTOM, "mk8-online 4.0.1", dlss.wide_data_directory,
                       g_vk.inst, g_vk.phys, g_vk.dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common,
                       MK8_NGX_API_VERSION);
    if (MK8_NGX_FAILED(rc)) {
        log_result("NVSDK_NGX_VULKAN_Init_with_ProjectID", rc);
        snprintf(dlss.probe.reason, sizeof dlss.probe.reason,
                 "NVSDK_NGX_VULKAN_Init_with_ProjectID failed 0x%08x (%s)", (unsigned)rc, result_name(rc));
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
    if (result != VK_SUCCESS) return false;
    VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = 144;
    buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocation = {};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(g_vk.vma, &buffer, &allocation, &dlss.mv_ubo, &dlss.mv_ubo_mem, &info) != VK_SUCCESS)
        return false;
    dlss.mv_ubo_ptr = info.pMappedData;
    return dlss.mv_ubo_ptr != NULL;
}

static bool run_mv(VkCommandBuffer cb, TexBackend *depth, uint32_t rw, uint32_t rh) {
    if (!g_fsr_vp_valid || !depth || !depth->image || !dlss.mv_pipeline || !dlss.mv_ubo_ptr ||
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
    float *uniform = (float *)dlss.mv_ubo_ptr;
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
    VkDescriptorBufferInfo buffer_info = {dlss.mv_ubo, 0, 144};
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

static bool ensure_cached_view(VkImageView *view, VkImage *cached_image, VkFormat *cached_format,
                               uint32_t *cached_w, uint32_t *cached_h, VkImage image, VkFormat format,
                               VkImageAspectFlags aspect, uint32_t w, uint32_t h) {
    if (*view && *cached_image == image && *cached_format == format && *cached_w == w && *cached_h == h)
        return true;
    if (*view) vkDestroyImageView(g_vk.dev, *view, NULL);
    *view = image_view_of(image, format, aspect);
    *cached_image = image;
    *cached_format = format;
    *cached_w = w;
    *cached_h = h;
    return *view != VK_NULL_HANDLE;
}

static bool create_output(uint32_t ow, uint32_t oh) {
    dlss.output = make_image(VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, &dlss.output_mem);
    if (!dlss.output) return false;
    dlss.output_view = image_view_of(dlss.output, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    return dlss.output_view != VK_NULL_HANDLE;
}

static bool ensure_feature(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (dlss.handle && dlss.rw == rw && dlss.rh == rh && dlss.ow == ow && dlss.oh == oh) return true;
    if (dlss.handle || dlss.output) destroy_feature_images();
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
        shutdown_ngx();
        destroy_feature_images();
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
        if (!ensure_cached_view(&dlss.depth_view, &dlss.depth_view_image, &dlss.depth_view_format,
                                &dlss.depth_view_w, &dlss.depth_view_h, depth->image, depth->format, aspect, rw,
                                rh))
            return VK_NULL_HANDLE;
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
    if (!ensure_cached_view(&dlss.color_view, &dlss.color_view_image, &dlss.color_view_format,
                            &dlss.color_view_w, &dlss.color_view_h, color->image, color->format, color_aspect,
                            rw, rh))
        return VK_NULL_HANDLE;
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
    if (getenv("MK8_DLSS_NO_JITTER")) jitter_x = jitter_y = 0.0f;

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
    if (MK8_NGX_FAILED(rc)) {
        static int warned;
        if (warned++ < 3) log_result("NVSDK_NGX_VULKAN_EvaluateFeature", rc);
        return VK_NULL_HANDLE;
    }
    static int logged;
    if (!logged++)
        fprintf(stderr, "[dlss] upscale active %ux%u -> %ux%u (quality %s, depth %s, motion %s, "
                        "jitter %.3f,%.3f)\n",
                rw, rh, ow, oh, dlss.quality_name, real_depth ? "game" : "neutral",
                motion ? "camera" : "zeros", jitter_x, jitter_y);
    return dlss.output_view;
}

// ---------------------------------------------------------------------------------------
// DLSS Frame Generation (MK8_DLSS_FG=1).
//
// NGX DLSS-G does not present and does not replace the swapchain. It produces two display-
// size images (real + interpolated) and the application presents both. This module owns the
// feature and those images; the WSI images, semaphores and queue submission stay with the
// caller. The API is deliberately self-contained so fsr_fg.cpp can call it with a small
// hand-off (docs/DLSS-IMPL.md) and no other runtime file changes.
// ---------------------------------------------------------------------------------------

#if defined(VK_NV_low_latency2)
typedef PFN_vkSetLatencySleepModeNV Mk8FgSetSleepModeFn;
typedef PFN_vkLatencySleepNV Mk8FgLatencySleepFn;
typedef PFN_vkSetLatencyMarkerNV Mk8FgSetMarkerFn;
#else
typedef VkResult (*Mk8FgSetSleepModeFn)(VkDevice, VkSwapchainKHR, const void *);
typedef VkResult (*Mk8FgLatencySleepFn)(VkDevice, VkSwapchainKHR, const void *);
typedef void (*Mk8FgSetMarkerFn)(VkDevice, VkSwapchainKHR, const void *);
#endif

typedef struct DlssFg {
    bool tried;
    bool ready;
    bool failed;
    Mk8DlssProbe probe;

    Mk8Lib plugin;
    Mk8Lib core;
    Mk8NgxInitWithProjectIdFn init;
    Mk8NgxGetCapabilityParametersFn get_capability_parameters;
    Mk8NgxParameterSetUiFn set_ui;
    Mk8NgxParameterSetIFn set_i;
    Mk8NgxParameterSetFFn set_f;
    Mk8NgxParameterSetVoidPointerFn set_pointer;
    Mk8NgxParameterSetUllFn set_ull;
    Mk8NgxParameterGetUiFn get_ui;
    Mk8NgxCreateFeature1Fn create_feature1;
    Mk8NgxCreateFeatureFn create_feature;
    Mk8NgxEvaluateFeatureFn evaluate;
    Mk8NgxReleaseFeatureFn release;
    Mk8NgxShutdown1Fn shutdown1;
    Mk8NgxGetFeatureRequirementsFn get_requirements;

    Mk8NgxParameter *params;
    Mk8NgxHandle *handle;
    bool ngx_initialised;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkFormat format;
    uint32_t width, height;
    uint32_t render_width, render_height;
    bool color_hdr;
    bool depth_inverted;
    wchar_t wide_plugin_directory[MK8_DLSS_PATH_MAX];
    wchar_t wide_data_directory[MK8_DLSS_PATH_MAX];
    const wchar_t *search_paths[1];

    VkImage interpolated_image;
    VmaAllocation interpolated_mem;
    VkImageView interpolated_view;

    float prev_view_proj[16];
    bool have_prev;
    bool reset_next;
    uint64_t frame_id;
    uint64_t present_id;
    bool logged_active;
    int warned;

    Mk8FgSetSleepModeFn set_sleep_mode;
    Mk8FgLatencySleepFn latency_sleep;
    Mk8FgSetMarkerFn set_marker;
} DlssFg;

static DlssFg dlss_fg;

static void fg_destroy_outputs(void) {
    if (dlss_fg.interpolated_view) vkDestroyImageView(g_vk.dev, dlss_fg.interpolated_view, NULL);
    if (dlss_fg.interpolated_image) {
        vkDestroyImage(g_vk.dev, dlss_fg.interpolated_image, NULL);
        vmaFreeMemory(g_vk.vma, dlss_fg.interpolated_mem);
    }
    dlss_fg.interpolated_image = VK_NULL_HANDLE;
    dlss_fg.interpolated_mem = VK_NULL_HANDLE;
    dlss_fg.interpolated_view = VK_NULL_HANDLE;
}

// Releases everything but keeps tried/failed/probe.reason for the one-time failure report.
static void fg_teardown(void) {
    if (dlss_fg.handle && dlss_fg.release) dlss_fg.release(dlss_fg.handle);
    dlss_fg.handle = NULL;
    fg_destroy_outputs();
    if (dlss_fg.ngx_initialised && dlss_fg.shutdown1) dlss_fg.shutdown1(dlss_fg.device);
    dlss_fg.ngx_initialised = false;
    dlss_fg.params = NULL;
    if (dlss_fg.plugin) lib_close(dlss_fg.plugin);
    if (dlss_fg.core) lib_close(dlss_fg.core);
    dlss_fg.plugin = NULL;
    dlss_fg.core = NULL;
    dlss_fg.ready = false;
}

void mk8_dlss_fg_shutdown(void) {
    fg_teardown();
    memset(&dlss_fg, 0, sizeof dlss_fg);
}

static void fg_fail(const char *reason) {
    snprintf(dlss_fg.probe.reason, sizeof dlss_fg.probe.reason, "%s", reason);
    dlss_fg.tried = true;
    dlss_fg.failed = true;
    mk8_dlss_fg_log_unavailable(&dlss_fg.probe);
    fg_teardown();
}

static bool fg_resolve_symbols(void) {
    dlss_fg.create_feature1 =
        (Mk8NgxCreateFeature1Fn)lib_symbol(dlss_fg.plugin, "NVSDK_NGX_VULKAN_CreateFeature1");
    dlss_fg.create_feature =
        (Mk8NgxCreateFeatureFn)lib_symbol(dlss_fg.plugin, "NVSDK_NGX_VULKAN_CreateFeature");
    dlss_fg.evaluate = (Mk8NgxEvaluateFeatureFn)lib_symbol(dlss_fg.plugin, "NVSDK_NGX_VULKAN_EvaluateFeature");
    dlss_fg.release = (Mk8NgxReleaseFeatureFn)lib_symbol(dlss_fg.plugin, "NVSDK_NGX_VULKAN_ReleaseFeature");
    dlss_fg.get_requirements =
        (Mk8NgxGetFeatureRequirementsFn)lib_symbol(dlss_fg.plugin, "NVSDK_NGX_VULKAN_GetFeatureRequirements");
    dlss_fg.init = (Mk8NgxInitWithProjectIdFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_VULKAN_Init_with_ProjectID");
    dlss_fg.get_capability_parameters =
        (Mk8NgxGetCapabilityParametersFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    dlss_fg.set_ui = (Mk8NgxParameterSetUiFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_Parameter_SetUI");
    dlss_fg.set_i = (Mk8NgxParameterSetIFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_Parameter_SetI");
    dlss_fg.set_f = (Mk8NgxParameterSetFFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_Parameter_SetF");
    dlss_fg.set_pointer =
        (Mk8NgxParameterSetVoidPointerFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_Parameter_SetVoidPointer");
    dlss_fg.set_ull = (Mk8NgxParameterSetUllFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_Parameter_SetULL");
    dlss_fg.get_ui = (Mk8NgxParameterGetUiFn)lib_symbol(dlss_fg.core, "NVSDK_NGX_Parameter_GetUI");
    dlss_fg.shutdown1 = (Mk8NgxShutdown1Fn)lib_symbol(dlss_fg.core, "NVSDK_NGX_VULKAN_Shutdown1");
    return dlss_fg.init && dlss_fg.get_capability_parameters && dlss_fg.set_ui && dlss_fg.set_i &&
           dlss_fg.set_f && dlss_fg.set_pointer && dlss_fg.get_ui && dlss_fg.evaluate && dlss_fg.release &&
           (dlss_fg.create_feature1 || dlss_fg.create_feature);
}

static void fg_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void fg_view4x4(const float view[12], float out[16]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = view[r * 4 + c];
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

static void fg_normalize3(float v[3]) {
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1e-6f) return;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
}

// Row-major view matrix: right/up/forward are the first three rows and the translation is
// the fourth column. Position = -R^T * t.
static void fg_camera_from_view(const float view[12], float position[3], float up[3], float right[3],
                                float forward[3]) {
    right[0] = view[0], right[1] = view[1], right[2] = view[2];
    up[0] = view[4], up[1] = view[5], up[2] = view[6];
    forward[0] = -view[8], forward[1] = -view[9], forward[2] = -view[10];
    fg_normalize3(right);
    fg_normalize3(up);
    fg_normalize3(forward);
    position[0] = -(view[0] * view[3] + view[4] * view[7] + view[8] * view[11]);
    position[1] = -(view[1] * view[3] + view[5] * view[7] + view[9] * view[11]);
    position[2] = -(view[2] * view[3] + view[6] * view[7] + view[10] * view[11]);
}

// Source access that is compatible with the image's current layout. Using a write mask for a
// PRESENT_SRC image is a validation error even though it is harmless on many drivers.
static VkAccessFlags fg_layout_source_access(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return VK_ACCESS_MEMORY_READ_BIT;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}

static VkAccessFlags fg_layout_destination_access(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return 0;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_UNDEFINED: return 0;
    default: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
}

static Mk8NgxResourceVk fg_image_resource(VkImage image, VkImageView view, VkFormat format, uint32_t width,
                                          uint32_t height, VkImageAspectFlags aspect, bool read_write) {    Mk8NgxResourceVk resource = {};
    resource.Type = 0;
    resource.ReadWrite = read_write;
    resource.Resource.ImageViewInfo.ImageView = view;
    resource.Resource.ImageViewInfo.Image = image;
    resource.Resource.ImageViewInfo.SubresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    resource.Resource.ImageViewInfo.Format = format;
    resource.Resource.ImageViewInfo.Width = width;
    resource.Resource.ImageViewInfo.Height = height;
    return resource;
}

static void fg_warn(const char *message) {
    if (dlss_fg.warned >= 3) return;
    dlss_fg.warned++;
    fprintf(stderr, "[dlss_fg] WARNING: %s\n", message);
}

// Feature creation and the output-image barrier need a command buffer. When the caller does not
// have one open (the swapchain-creation hook), the bridge records into a one-shot buffer and
// submits it on the game queue. This runs at device-idle points: initial swapchain creation and
// recreate_swapchain after device_wait_idle().
static bool fg_oneshot_begin(VkCommandBuffer *out) {
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = g_vk.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_vk.dev, &allocate, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
        return false;
    }
    *out = cb;
    return true;
}

static void fg_oneshot_free(VkCommandBuffer cb) {
    if (cb) vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
}

static void fg_oneshot_abort(VkCommandBuffer cb) {
    if (!cb) return;
    vkEndCommandBuffer(cb);
    fg_oneshot_free(cb);
}

static bool fg_oneshot_submit(VkCommandBuffer cb) {
    if (!cb) return false;
    VkResult result = vkEndCommandBuffer(cb);
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (result == VK_SUCCESS) result = vkCreateFence(g_vk.dev, &fence_info, NULL, &fence);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (result == VK_SUCCESS) result = vkQueueSubmit(g_vk.queue, 1, &submit, fence);
    if (result == VK_SUCCESS) result = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX);
    if (fence) vkDestroyFence(g_vk.dev, fence, NULL);
    fg_oneshot_free(cb);
    return result == VK_SUCCESS;
}

extern "C" bool mk8_dlss_fg_ready(void) { return dlss_fg.ready; }

bool mk8_dlss_fg_init(const Mk8DlssFgInit *init) {
    if (!mk8_dlss_fg_enabled()) return false;
    if (dlss_fg.failed) return false;
    if (!init || !init->instance || !init->physical_device || !init->device || !init->swapchain ||
        !init->width || !init->height) {
        fg_fail("invalid init: instance, physical device, device, swapchain and sizes are required");
        return false;
    }
    if (env_enabled("MK8_FSR_FG")) {
        fg_fail("MK8_FSR_FG is active; only one frame-generation path can present");
        return false;
    }
    if (env_enabled("MK8_DLSS")) {
        fg_fail("MK8_DLSS (Super Resolution) and MK8_DLSS_FG cannot share the NGX init yet; "
                "run with MK8_DLSS unset");
        return false;
    }
    if (init->device != g_vk.dev) {
        fg_fail("the DLSS-G bridge must use the runtime's VkDevice (g_vk.dev)");
        return false;
    }
    if (dlss_fg.ready && dlss_fg.swapchain == init->swapchain && dlss_fg.format == init->format &&
        dlss_fg.width == init->width && dlss_fg.height == init->height &&
        dlss_fg.render_width == init->render_width && dlss_fg.render_height == init->render_height)
        return true;
    if (dlss_fg.ready) mk8_dlss_fg_shutdown();

    dlss_fg.tried = true;
    dlss_fg.instance = init->instance;
    dlss_fg.physical_device = init->physical_device;
    dlss_fg.device = init->device;
    dlss_fg.queue = init->present_queue;
    dlss_fg.swapchain = init->swapchain;
    if (!mk8_dlss_fg_probe(&dlss_fg.probe)) {
        dlss_fg.failed = true;
        mk8_dlss_fg_log_unavailable(&dlss_fg.probe);
        return false;
    }
    dlss_fg.plugin = lib_open(dlss_fg.probe.plugin);
    if (!dlss_fg.plugin) {
        fg_fail("the DLSS-G plugin did not load now");
        return false;
    }
    dlss_fg.core = lib_open(dlss_fg.probe.core);
    if (!dlss_fg.core) {
        fg_fail("the NGX core did not load now");
        return false;
    }
    if (!fg_resolve_symbols()) {
        fg_fail("the DLSS-G plugin/core lost the exports the probe found");
        return false;
    }

#if !defined(VK_NV_low_latency2)
    fg_fail("this build's Vulkan headers do not define VK_NV_low_latency2");
    return false;
#else
    uint32_t extension_count = 0;
    bool advertised = false;
    if (vkEnumerateDeviceExtensionProperties(init->physical_device, NULL, &extension_count, NULL) == VK_SUCCESS &&
        extension_count) {
        VkExtensionProperties *extensions =
            (VkExtensionProperties *)malloc((size_t)extension_count * sizeof *extensions);
        if (extensions) {
            if (vkEnumerateDeviceExtensionProperties(init->physical_device, NULL, &extension_count, extensions) ==
                VK_SUCCESS) {
                for (uint32_t i = 0; i < extension_count; i++)
                    if (strcmp(extensions[i].extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) == 0)
                        advertised = true;
            }
            free(extensions);
        }
    }
    if (!advertised) {
        fg_fail("VK_NV_low_latency2 is not exposed by this GPU/driver");
        return false;
    }
    dlss_fg.set_sleep_mode = (Mk8FgSetSleepModeFn)vkGetDeviceProcAddr(init->device, "vkSetLatencySleepModeNV");
    dlss_fg.latency_sleep = (Mk8FgLatencySleepFn)vkGetDeviceProcAddr(init->device, "vkLatencySleepNV");
    dlss_fg.set_marker = (Mk8FgSetMarkerFn)vkGetDeviceProcAddr(init->device, "vkSetLatencyMarkerNV");
    if (!dlss_fg.set_sleep_mode || !dlss_fg.latency_sleep || !dlss_fg.set_marker) {
        fg_fail("the device was created without VK_NV_low_latency2; enable the extension before "
                "vkCreateDevice (device.c)");
        return false;
    }
#endif

    char data_directory[MK8_DLSS_PATH_MAX];
    if (!ngx_data_directory(data_directory, sizeof data_directory) || !mkdir_p(data_directory)) {
        fg_fail("no writable NGX data directory (XDG_DATA_HOME/HOME)");
        return false;
    }
    if (!to_wide(dlss_fg.probe.directory, dlss_fg.wide_plugin_directory, MK8_DLSS_PATH_MAX) ||
        !to_wide(data_directory, dlss_fg.wide_data_directory, MK8_DLSS_PATH_MAX)) {
        fg_fail("cannot convert the plugin/data path to wchar_t (non-UTF-8 locale?)");
        return false;
    }
    dlss_fg.search_paths[0] = dlss_fg.wide_plugin_directory;
    Mk8NgxFeatureCommonInfo common = {};
    common.PathListInfo.Path = dlss_fg.search_paths;
    common.PathListInfo.Length = 1;

    const char *project_id = getenv("MK8_DLSS_PROJECT_ID");
    if (!project_id || !*project_id) project_id = "6f1d2c3a-8b4e-4d7a-9c15-3e0a5b7c9d21";
    Mk8NgxFeatureDiscoveryInfo discovery = {};
    discovery.SDKVersion = MK8_NGX_API_VERSION;
    discovery.FeatureID = MK8_NGX_FEATURE_FRAME_GENERATION;
    discovery.Identifier.IdentifierType = MK8_NGX_IDENTIFIER_PROJECT_ID;
    discovery.Identifier.v.ProjectDesc.ProjectId = project_id;
    discovery.Identifier.v.ProjectDesc.EngineType = MK8_NGX_ENGINE_TYPE_CUSTOM;
    discovery.Identifier.v.ProjectDesc.EngineVersion = "mk8-online";
    discovery.ApplicationDataPath = dlss_fg.wide_data_directory;
    discovery.FeatureInfo = &common;

    if (dlss_fg.get_requirements) {
        Mk8NgxFeatureRequirement requirement = {};
        int rc = dlss_fg.get_requirements(init->instance, init->physical_device, &discovery, &requirement);
        if (MK8_NGX_SUCCEEDED(rc)) {
            if (requirement.FeatureSupported != 0) {
                char reasons[256];
                mk8_dlss_requirement_text((unsigned)requirement.FeatureSupported, reasons, sizeof reasons);
                char message[MK8_DLSS_REASON_MAX];
                snprintf(message, sizeof message,
                         "NGX reports FrameGeneration unsupported (0x%x: %s; min GPU arch 0x%x)",
                         (unsigned)requirement.FeatureSupported, reasons, requirement.MinHWArchitecture);
                fg_fail(message);
                return false;
            }
            fprintf(stderr, "[dlss_fg] feature requirements: supported (min GPU arch 0x%x, min OS %s)\n",
                    requirement.MinHWArchitecture, requirement.MinOSVersion);
        } else {
            fprintf(stderr, "[dlss_fg] GetFeatureRequirements failed 0x%08x (%s); trying Init\n", (unsigned)rc,
                    result_name(rc));
        }
    }

    int rc = dlss_fg.init(project_id, MK8_NGX_ENGINE_TYPE_CUSTOM, "mk8-online 4.0.1",
                          dlss_fg.wide_data_directory, init->instance, init->physical_device, init->device,
                          vkGetInstanceProcAddr, vkGetDeviceProcAddr, &common, MK8_NGX_API_VERSION);
    if (MK8_NGX_FAILED(rc)) {
        fprintf(stderr, "[dlss_fg] NVSDK_NGX_VULKAN_Init_with_ProjectID failed 0x%08x (%s)\n", (unsigned)rc,
                result_name(rc));
        if (dlss_fg.shutdown1) dlss_fg.shutdown1(init->device);
        char message[MK8_DLSS_REASON_MAX];
        snprintf(message, sizeof message,
                 "NVSDK_NGX_VULKAN_Init_with_ProjectID failed 0x%08x (%s); an older NGX/driver is the "
                 "usual cause", (unsigned)rc, result_name(rc));
        fg_fail(message);
        return false;
    }
    dlss_fg.ngx_initialised = true;
    fprintf(stderr, "[dlss_fg] provider NGX API 0x%x from %s\n", MK8_NGX_API_VERSION, dlss_fg.probe.plugin);

    rc = dlss_fg.get_capability_parameters(&dlss_fg.params);
    if (MK8_NGX_FAILED(rc) || !dlss_fg.params) {
        char message[MK8_DLSS_REASON_MAX];
        snprintf(message, sizeof message, "NVSDK_NGX_VULKAN_GetCapabilityParameters failed 0x%08x (%s)",
                 (unsigned)rc, result_name(rc));
        fg_fail(message);
        return false;
    }
    unsigned available = 0;
    rc = dlss_fg.get_ui(dlss_fg.params, MK8_NGX_PARAM_FRAME_GENERATION_AVAILABLE, &available);
    if (MK8_NGX_FAILED(rc)) {
        char message[MK8_DLSS_REASON_MAX];
        snprintf(message, sizeof message, "FrameGeneration.Available could not be read (0x%08x)",
                 (unsigned)rc);
        fg_fail(message);
        return false;
    }
    if (available != 1) {
        unsigned needs_driver = 0, min_major = 0, min_minor = 0;
        dlss_fg.get_ui(dlss_fg.params, MK8_NGX_PARAM_FRAME_GENERATION_NEEDS_UPDATED_DRIVER, &needs_driver);
        dlss_fg.get_ui(dlss_fg.params, MK8_NGX_PARAM_FRAME_GENERATION_MIN_DRIVER_MAJOR, &min_major);
        dlss_fg.get_ui(dlss_fg.params, MK8_NGX_PARAM_FRAME_GENERATION_MIN_DRIVER_MINOR, &min_minor);
        char message[MK8_DLSS_REASON_MAX];
        if (min_major || min_minor)
            snprintf(message, sizeof message,
                     "FrameGeneration.Available = %u (needsUpdatedDriver=%u, driver %u.%u or newer)",
                     available, needs_driver, min_major, min_minor);
        else
            snprintf(message, sizeof message,
                     "FrameGeneration.Available = %u (needsUpdatedDriver=%u): this GPU/driver does not "
                     "expose DLSS Frame Generation", available, needs_driver);
        fg_fail(message);
        return false;
    }

    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    dlss_fg.interpolated_image = make_image(init->format, init->width, init->height, usage,
                                            VK_IMAGE_ASPECT_COLOR_BIT, &dlss_fg.interpolated_mem);
    if (!dlss_fg.interpolated_image) {
        fg_fail("cannot create the display-size DLSS-G interpolated output image");
        return false;
    }
    dlss_fg.interpolated_view =
        image_view_of(dlss_fg.interpolated_image, init->format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!dlss_fg.interpolated_view) {
        fg_fail("cannot create the DLSS-G output image view");
        return false;
    }

    VkCommandBuffer owned = VK_NULL_HANDLE;
    VkCommandBuffer cb = init->command_buffer;
    if (!cb) {
        if (!fg_oneshot_begin(&owned)) {
            fg_fail("cannot allocate a one-shot command buffer for DLSS-G feature creation");
            return false;
        }
        cb = owned;
    }

    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_PARAM_CREATION_NODE_MASK, 1);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_PARAM_VISIBILITY_NODE_MASK, 1);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_PARAM_WIDTH, init->width);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_PARAM_HEIGHT, init->height);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_BACKBUFFER_FORMAT, (unsigned)init->format);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_INTERNAL_WIDTH,
                   init->render_width ? init->render_width : init->width);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_INTERNAL_HEIGHT,
                   init->render_height ? init->render_height : init->height);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_DYNAMIC_RESOLUTION, 0);
    rc = dlss_fg.create_feature1
             ? dlss_fg.create_feature1(init->device, cb, MK8_NGX_FEATURE_FRAME_GENERATION, dlss_fg.params,
                                       &dlss_fg.handle)
             : dlss_fg.create_feature(cb, MK8_NGX_FEATURE_FRAME_GENERATION, dlss_fg.params, &dlss_fg.handle);
    if (MK8_NGX_FAILED(rc) || !dlss_fg.handle) {
        unsigned init_result = 0;
        if (dlss_fg.get_ui(dlss_fg.params, MK8_NGX_PARAM_FRAME_GENERATION_FEATURE_INIT_RESULT, &init_result))
            fprintf(stderr, "[dlss_fg] FrameGeneration.FeatureInitResult = %u\n", init_result);
        char message[MK8_DLSS_REASON_MAX];
        snprintf(message, sizeof message, "NVSDK_NGX_VULKAN_CreateFeature(FrameGeneration) failed 0x%08x (%s)",
                 (unsigned)rc, result_name(rc));
        dlss_fg.handle = NULL;
        fg_oneshot_abort(owned);
        fg_fail(message);
        return false;
    }
    barrier(cb, dlss_fg.interpolated_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (owned) {
        if (!fg_oneshot_submit(owned)) {
            fg_fail("submitting the one-shot DLSS-G creation command buffer failed");
            return false;
        }
        fprintf(stderr, "[dlss_fg] feature created and output primed in a one-shot submission\n");
    }

#if defined(VK_NV_low_latency2)
    VkLatencySleepModeInfoNV sleep_mode = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    sleep_mode.lowLatencyMode = VK_TRUE;
    sleep_mode.lowLatencyBoost = VK_FALSE;
    sleep_mode.minimumIntervalUs = 0;
    VkResult latency_result = dlss_fg.set_sleep_mode(init->device, init->swapchain, &sleep_mode);
    if (latency_result != VK_SUCCESS) {
        char message[MK8_DLSS_REASON_MAX];
        snprintf(message, sizeof message, "vkSetLatencySleepModeNV failed (%d); DLSS-G needs working "
                                          "VK_NV_low_latency2 pacing", latency_result);
        fg_fail(message);
        return false;
    }
#endif

    dlss_fg.instance = init->instance;
    dlss_fg.physical_device = init->physical_device;
    dlss_fg.device = init->device;
    dlss_fg.queue = init->present_queue;
    dlss_fg.swapchain = init->swapchain;
    dlss_fg.format = init->format;
    dlss_fg.width = init->width;
    dlss_fg.height = init->height;
    dlss_fg.render_width = init->render_width ? init->render_width : init->width;
    dlss_fg.render_height = init->render_height ? init->render_height : init->height;
    dlss_fg.color_hdr = init->color_hdr;
    dlss_fg.depth_inverted = init->depth_inverted;
    dlss_fg.have_prev = false;
    dlss_fg.reset_next = true;
    dlss_fg.frame_id = 0;
    dlss_fg.present_id = 0;
    dlss_fg.ready = true;
    dlss_fg.failed = false;
    fprintf(stderr,
            "[dlss_fg] active: NGX DLSS-G 2x %ux%u (render %ux%u, format %d), VK_NV_low_latency2 pacing\n",
            init->width, init->height, dlss_fg.render_width, dlss_fg.render_height, init->format);
    fprintf(stderr, "[dlss_fg] mvec scale 1/%u,1/%u (pixel-space MVs); jitter converted to clip space\n",
            dlss_fg.render_width, dlss_fg.render_height);
    dlss_fg.logged_active = true;
    return true;
}

Mk8DlssFgFrame mk8_dlss_fg_evaluate(const Mk8DlssFgInputs *inputs) {
    Mk8DlssFgFrame frame;
    memset(&frame, 0, sizeof frame);
    if (!dlss_fg.ready || !inputs || !inputs->command_buffer || !inputs->backbuffer ||
        !inputs->backbuffer_view || !inputs->depth || !inputs->depth_view || !inputs->depth_width ||
        !inputs->depth_height || !inputs->motion || !inputs->motion_view || !inputs->motion_width ||
        !inputs->motion_height) {
        fg_warn("DLSS-G evaluate needs a command buffer, sized backbuffer, depth and motion-vector images");
        return frame;
    }
    if (inputs->backbuffer_width != dlss_fg.width || inputs->backbuffer_height != dlss_fg.height) {
        char message[256];
        snprintf(message, sizeof message,
                 "backbuffer %ux%u is not the %ux%u display size; DLSS-G interpolates the final "
                 "image (enable the FSR1/DLSS SR upscale)",
                 inputs->backbuffer_width, inputs->backbuffer_height, dlss_fg.width, dlss_fg.height);
        fg_warn(message);
        return frame;
    }
    if (inputs->backbuffer_format != dlss_fg.format) {
        char message[256];
        snprintf(message, sizeof message,
                 "backbuffer format %d does not match the DLSS-G output format %d", inputs->backbuffer_format,
                 dlss_fg.format);
        fg_warn(message);
        return frame;
    }

    float projection[16], view4[16], view_proj[16], inv_view_proj[16], inv_projection[16];
    memcpy(projection, inputs->projection, sizeof projection);
    fg_view4x4(inputs->view, view4);
    mul4(projection, view4, view_proj);
    bool invert_ok = invert4(view_proj, inv_view_proj) && invert4(projection, inv_projection);
    if (!invert_ok) {
        fg_warn("the view/projection matrices are singular; DLSS-G skipped for this frame");
        return frame;
    }

    bool reset = inputs->reset || dlss_fg.reset_next || !dlss_fg.have_prev;
    dlss_fg.reset_next = false;
    float clip_to_prev[16], prev_clip_to_clip[16], clip_to_lens[16];
    fg_identity(clip_to_lens);
    if (reset) {
        fg_identity(clip_to_prev);
        fg_identity(prev_clip_to_clip);
    } else {
        mul4(dlss_fg.prev_view_proj, inv_view_proj, clip_to_prev);
        if (!invert4(clip_to_prev, prev_clip_to_clip)) fg_identity(prev_clip_to_clip);
    }

    float camera_position[3], camera_up[3], camera_right[3], camera_forward[3];
    fg_camera_from_view(inputs->view, camera_position, camera_up, camera_right, camera_forward);
    float camera_near = inputs->camera_near > 0 ? inputs->camera_near : 0.1f;
    float camera_far = inputs->camera_far > 0 ? inputs->camera_far : 1000.0f;
    float camera_fov = inputs->camera_fov > 0
                           ? inputs->camera_fov
                           : (fabsf(projection[5]) > 1e-6f ? 2.0f * atanf(1.0f / fabsf(projection[5]))
                                                         : 1.0472f);
    float camera_aspect = inputs->camera_aspect > 0
                              ? inputs->camera_aspect
                              : (fabsf(projection[0]) > 1e-6f ? fabsf(projection[5] / projection[0])
                                                              : 1.7778f);
    float jitter_x = inputs->jitter_x, jitter_y = inputs->jitter_y;
    if (getenv("MK8_DLSS_FG_NO_JITTER")) jitter_x = jitter_y = 0.0f;
    float jitter_clip_x = 2.0f * jitter_x / (float)dlss_fg.render_width;
    float jitter_clip_y = 2.0f * jitter_y / (float)dlss_fg.render_height;
    float mvec_scale_x = 1.0f / (float)dlss_fg.render_width;
    float mvec_scale_y = 1.0f / (float)dlss_fg.render_height;

    Mk8NgxDlssgOptEvalParams opt = {};
    opt.multiFrameCount = 1;
    opt.multiFrameIndex = 1;
    memcpy(opt.cameraViewToClip, projection, sizeof opt.cameraViewToClip);
    memcpy(opt.clipToCameraView, inv_projection, sizeof opt.clipToCameraView);
    memcpy(opt.clipToLensClip, clip_to_lens, sizeof opt.clipToLensClip);
    memcpy(opt.clipToPrevClip, clip_to_prev, sizeof opt.clipToPrevClip);
    memcpy(opt.prevClipToClip, prev_clip_to_clip, sizeof opt.prevClipToClip);
    opt.jitterOffset[0] = jitter_clip_x;
    opt.jitterOffset[1] = jitter_clip_y;
    opt.mvecScale[0] = mvec_scale_x;
    opt.mvecScale[1] = mvec_scale_y;
    opt.cameraPinholeOffset[0] = 0.0f;
    opt.cameraPinholeOffset[1] = 0.0f;
    memcpy(opt.cameraPos, camera_position, sizeof opt.cameraPos);
    memcpy(opt.cameraUp, camera_up, sizeof opt.cameraUp);
    memcpy(opt.cameraRight, camera_right, sizeof opt.cameraRight);
    memcpy(opt.cameraFwd, camera_forward, sizeof opt.cameraFwd);
    opt.cameraNear = camera_near;
    opt.cameraFar = camera_far;
    opt.cameraFOV = camera_fov;
    opt.cameraAspectRatio = camera_aspect;
    opt.colorBuffersHDR = dlss_fg.color_hdr;
    opt.depthInverted = dlss_fg.depth_inverted;
    opt.cameraMotionIncluded = true;
    opt.reset = reset;
    opt.automodeOverrideReset = false;
    opt.notRenderingGameFrames = inputs->not_rendering;
    opt.orthoProjection = false;
    opt.motionVectorsInvalidValue = 0.0f;
    opt.motionVectorsDilated = false;
    opt.menuDetectionEnabled = false;
    opt.minRelativeLinearDepthObjectSeparation = 40.0f;

    VkImageAspectFlags depth_aspect =
        inputs->depth_aspect ? inputs->depth_aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    Mk8NgxResourceVk backbuffer_resource =
        fg_image_resource(inputs->backbuffer, inputs->backbuffer_view, inputs->backbuffer_format,
                          inputs->backbuffer_width, inputs->backbuffer_height, VK_IMAGE_ASPECT_COLOR_BIT, false);
    Mk8NgxResourceVk depth_resource =
        fg_image_resource(inputs->depth, inputs->depth_view, inputs->depth_format, inputs->depth_width,
                          inputs->depth_height, depth_aspect, false);
    Mk8NgxResourceVk motion_resource =
        fg_image_resource(inputs->motion, inputs->motion_view, inputs->motion_format, inputs->motion_width,
                          inputs->motion_height, VK_IMAGE_ASPECT_COLOR_BIT, false);
    Mk8NgxResourceVk interp_resource = fg_image_resource(dlss_fg.interpolated_image, dlss_fg.interpolated_view,
                                                         dlss_fg.format, dlss_fg.width, dlss_fg.height,
                                                         VK_IMAGE_ASPECT_COLOR_BIT, true);

    VkCommandBuffer cb = inputs->command_buffer;
    VkAccessFlags backbuffer_src = fg_layout_source_access(inputs->backbuffer_layout);
    VkAccessFlags depth_src = fg_layout_source_access(inputs->depth_layout);
    VkAccessFlags motion_src = fg_layout_source_access(inputs->motion_layout);
    barrier(cb, inputs->backbuffer, VK_IMAGE_ASPECT_COLOR_BIT, inputs->backbuffer_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, backbuffer_src, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, inputs->depth, depth_aspect, inputs->depth_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth_src, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, inputs->motion, VK_IMAGE_ASPECT_COLOR_BIT, inputs->motion_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, motion_src, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_PARAM_WIDTH, dlss_fg.width);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_PARAM_HEIGHT, dlss_fg.height);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_MULTI_FRAME_COUNT, opt.multiFrameCount);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_MULTI_FRAME_INDEX, opt.multiFrameIndex);
    if (dlss_fg.set_ull)
        dlss_fg.set_ull(dlss_fg.params, MK8_NGX_DLSSG_BACKBUFFER_FRAME_ID, inputs->frame_id);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_BACKBUFFER, &backbuffer_resource);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_MVECS, &motion_resource);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_DEPTH, &depth_resource);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_OUTPUT_INTERPOLATED, &interp_resource);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_VIEW_TO_CLIP, opt.cameraViewToClip);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_CLIP_TO_CAMERA_VIEW, opt.clipToCameraView);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_CLIP_TO_LENS_CLIP, opt.clipToLensClip);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_CLIP_TO_PREV_CLIP, opt.clipToPrevClip);
    dlss_fg.set_pointer(dlss_fg.params, MK8_NGX_DLSSG_PREV_CLIP_TO_CLIP, opt.prevClipToClip);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_JITTER_OFFSET_X, opt.jitterOffset[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_JITTER_OFFSET_Y, opt.jitterOffset[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_MVEC_SCALE_X, opt.mvecScale[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_MVEC_SCALE_Y, opt.mvecScale[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_PINHOLE_OFFSET_X, opt.cameraPinholeOffset[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_PINHOLE_OFFSET_Y, opt.cameraPinholeOffset[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_POS_X, opt.cameraPos[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_POS_Y, opt.cameraPos[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_POS_Z, opt.cameraPos[2]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_UP_X, opt.cameraUp[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_UP_Y, opt.cameraUp[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_UP_Z, opt.cameraUp[2]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_RIGHT_X, opt.cameraRight[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_RIGHT_Y, opt.cameraRight[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_RIGHT_Z, opt.cameraRight[2]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_FWD_X, opt.cameraFwd[0]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_FWD_Y, opt.cameraFwd[1]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_FWD_Z, opt.cameraFwd[2]);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_NEAR, opt.cameraNear);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_FAR, opt.cameraFar);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_FOV, opt.cameraFOV);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_ASPECT_RATIO, opt.cameraAspectRatio);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_COLOR_BUFFERS_HDR, opt.colorBuffersHDR ? 1 : 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_DEPTH_INVERTED, opt.depthInverted ? 1 : 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_CAMERA_MOTION_INCLUDED, opt.cameraMotionIncluded ? 1 : 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_RESET, opt.reset ? 1 : 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_AUTOMODE_OVERRIDE_RESET, 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_NOT_RENDERING_GAME_FRAMES,
                   opt.notRenderingGameFrames ? 1 : 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_ORTHO_PROJECTION, 0);
    dlss_fg.set_f(dlss_fg.params, MK8_NGX_DLSSG_MVEC_INVALID_VALUE, opt.motionVectorsInvalidValue);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_MVEC_DILATED, 0);
    dlss_fg.set_ui(dlss_fg.params, MK8_NGX_DLSSG_MENU_DETECTION_ENABLED, 0);

    int rc = dlss_fg.evaluate(cb, dlss_fg.handle, dlss_fg.params, NULL);

    barrier(cb, inputs->backbuffer, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            inputs->backbuffer_layout, VK_ACCESS_SHADER_READ_BIT,
            fg_layout_destination_access(inputs->backbuffer_layout), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, inputs->depth, depth_aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, inputs->depth_layout,
            VK_ACCESS_SHADER_READ_BIT, fg_layout_destination_access(inputs->depth_layout),
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, inputs->motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            inputs->motion_layout, VK_ACCESS_SHADER_READ_BIT, fg_layout_destination_access(inputs->motion_layout),
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (MK8_NGX_FAILED(rc)) {
        if (dlss_fg.warned < 3) {
            dlss_fg.warned++;
            fprintf(stderr, "[dlss_fg] NVSDK_NGX_VULKAN_EvaluateFeature(FrameGeneration) failed 0x%08x (%s)\n",
                    (unsigned)rc, result_name(rc));
        }
        dlss_fg.have_prev = false;
        return frame;
    }

    memcpy(dlss_fg.prev_view_proj, view_proj, sizeof dlss_fg.prev_view_proj);
    dlss_fg.have_prev = true;
    dlss_fg.frame_id = inputs->frame_id;
    frame.valid = true;
    frame.interpolated = true;
    frame.real_image = inputs->backbuffer;
    frame.real_view = inputs->backbuffer_view;
    frame.interpolated_image = dlss_fg.interpolated_image;
    frame.interpolated_view = dlss_fg.interpolated_view;
    frame.format = dlss_fg.format;
    frame.width = dlss_fg.width;
    frame.height = dlss_fg.height;
    frame.frame_id = inputs->frame_id;
    return frame;
}

bool mk8_dlss_fg_copy_interpolated(VkCommandBuffer cb, VkImage destination, VkImageLayout destination_layout) {
    if (!dlss_fg.ready || !cb || !destination) return false;
    barrier(cb, dlss_fg.interpolated_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, destination, VK_IMAGE_ASPECT_COLOR_BIT, destination_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region = {};
    region.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = (VkExtent3D){dlss_fg.width, dlss_fg.height, 1};
    vkCmdCopyImage(cb, dlss_fg.interpolated_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cb, destination, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, dlss_fg.interpolated_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    return true;
}

void mk8_dlss_fg_latency_marker(int marker, uint64_t present_id) {
#if defined(VK_NV_low_latency2)
    if (!dlss_fg.ready || !dlss_fg.set_marker || marker < 0 || marker > 7) return;
    VkSetLatencyMarkerInfoNV info = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    info.presentID = present_id;
    info.marker = (VkLatencyMarkerNV)marker;
    dlss_fg.set_marker(dlss_fg.device, dlss_fg.swapchain, &info);
#else
    (void)marker;
    (void)present_id;
#endif
}

bool mk8_dlss_fg_latency_sleep(VkSemaphore signal, uint64_t value) {
#if defined(VK_NV_low_latency2)
    if (!dlss_fg.ready || !dlss_fg.latency_sleep || !signal) return false;
    VkLatencySleepInfoNV info = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
    info.signalSemaphore = signal;
    info.value = value;
    VkResult result = dlss_fg.latency_sleep(dlss_fg.device, dlss_fg.swapchain, &info);
    if (result != VK_SUCCESS) {
        if (dlss_fg.warned < 3) {
            dlss_fg.warned++;
            fprintf(stderr, "[dlss_fg] vkLatencySleepNV failed (%d)\n", result);
        }
        return false;
    }
    return true;
#else
    (void)signal;
    (void)value;
    return false;
#endif
}

static VkResult fg_present_image(VkQueue queue, uint32_t image_index, VkSemaphore ready, int start_marker,
                                 int end_marker, uint64_t present_id) {
#if defined(VK_NV_low_latency2)
    if (dlss_fg.set_marker) {
        VkSetLatencyMarkerInfoNV marker = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
        marker.presentID = present_id;
        marker.marker = (VkLatencyMarkerNV)start_marker;
        dlss_fg.set_marker(dlss_fg.device, dlss_fg.swapchain, &marker);
    }
#else
    (void)start_marker;
    (void)end_marker;
#endif
    VkPresentInfoKHR info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = ready ? 1 : 0;
    info.pWaitSemaphores = ready ? &ready : NULL;
    info.swapchainCount = 1;
    info.pSwapchains = &dlss_fg.swapchain;
    info.pImageIndices = &image_index;
    VkResult result = vkQueuePresentKHR(queue, &info);
#if defined(VK_NV_low_latency2)
    if (dlss_fg.set_marker) {
        VkSetLatencyMarkerInfoNV marker = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
        marker.presentID = present_id;
        marker.marker = (VkLatencyMarkerNV)end_marker;
        dlss_fg.set_marker(dlss_fg.device, dlss_fg.swapchain, &marker);
    }
#endif
    return result;
}

VkResult mk8_dlss_fg_present(VkQueue queue, VkSwapchainKHR swapchain, uint32_t real_index,
                             uint32_t interpolated_index, VkSemaphore real_ready,
                             VkSemaphore interpolated_ready) {
    if (!dlss_fg.ready || !queue || swapchain != dlss_fg.swapchain) {
        if (dlss_fg.warned < 3) {
            dlss_fg.warned++;
            fprintf(stderr, "[dlss_fg] present refused: module is not ready for this swapchain\n");
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (real_index == interpolated_index) {
        if (dlss_fg.warned < 3) {
            dlss_fg.warned++;
            fprintf(stderr, "[dlss_fg] present refused: the real and interpolated frames need two "
                            "different swapchain images\n");
        }
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint64_t real_id = dlss_fg.present_id++;
    VkResult result = fg_present_image(queue, real_index, real_ready, MK8_DLSS_FG_MARKER_PRESENT_START,
                                       MK8_DLSS_FG_MARKER_PRESENT_END, real_id);
    if (result != VK_SUCCESS) return result;
    uint64_t interp_id = dlss_fg.present_id++;
    return fg_present_image(queue, interpolated_index, interpolated_ready, MK8_DLSS_FG_MARKER_PRESENT_START,
                            MK8_DLSS_FG_MARKER_PRESENT_END, interp_id);
}

#endif  // !MK8_DLSS_PROBE_ONLY

// Intel XeSS Super Resolution bridge (host side, no guest changes).
//
// Windows uses libxess.dll's Vulkan API: create a context on the runtime's existing
// device, initialise it for the game image, and record one xessVKExecute per frame
// with the same colour/depth/motion/jitter inputs as the FSR and DLSS bridges.
//
// Linux is a stub: Intel ships only libxess.dll, and this file must not pretend
// otherwise. The probe still parses the PE file so a downloaded DLL can be checked
// (and tested) on Linux, then reports MK8_XESS_UNSUPPORTED_PLATFORM with one line.
//
// Nothing Intel is bundled. The DLL is an optional component from
// tools/fetch_vendor_sdk.py ($XDG_DATA_HOME/mk8online/intel) or MK8_XESS_LIB.
// A missing, truncated or wrong-architecture DLL is rejected before LoadLibraryA;
// every failure returns VK_NULL_HANDLE and leaves the existing path untouched.
#include "xess.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------------------
// PE inspection (portable). XeSS ships a PE32+ x86-64 DLL; parsing it here rejects a
// truncated file before the Windows loader can fault on it, and lets the Linux tests
// check the real export table without Wine.
// ---------------------------------------------------------------------------------------

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static bool file_size(FILE *file, uint64_t *size) {
    if (fseek(file, 0, SEEK_END) != 0) return false;
#ifdef _WIN32
    __int64 end = _ftelli64(file);
#else
    off_t end = ftello(file);
#endif
    if (end < 0) return false;
    *size = (uint64_t)end;
    return fseek(file, 0, SEEK_SET) == 0;
}

static bool read_at(FILE *file, uint64_t offset, void *buffer, size_t size) {
#ifdef _WIN32
    if (_fseeki64(file, (__int64)offset, SEEK_SET) != 0) return false;
#else
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0) return false;
#endif
    return fread(buffer, 1, size, file) == size;
}

typedef struct PeSection {
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
} PeSection;

static bool rva_to_offset(const PeSection *sections, uint32_t count, uint64_t file_size_bytes, uint32_t rva,
                          uint64_t *out) {
    for (uint32_t i = 0; i < count; i++) {
        uint64_t base = sections[i].virtual_address;
        uint64_t span = sections[i].virtual_size > sections[i].raw_size ? sections[i].virtual_size
                                                                         : sections[i].raw_size;
        if (rva < base || rva >= base + span) continue;
        uint64_t delta = rva - base;
        if (delta >= sections[i].raw_size) return false;  // in the zero-filled tail
        uint64_t offset = sections[i].raw_offset + delta;
        if (offset >= file_size_bytes) return false;
        *out = offset;
        return true;
    }
    return false;
}

static void append_missing(char *missing, size_t size, const char *name) {
    size_t used = strlen(missing);
    if (used + 1 >= size) return;
    snprintf(missing + used, size - used, "%s%s", used ? ", " : "", name);
}

bool mk8_xess_inspect_pe(const char *path, Mk8XessPeInfo *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->detail[0] = 0;
    if (!path || !*path) {
        snprintf(out->detail, sizeof out->detail, "no path");
        return false;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(out->detail, sizeof out->detail, "cannot open: %s", strerror(errno));
        return false;
    }
    uint64_t size = 0;
    if (!file_size(file, &size) || size < 64) {
        snprintf(out->detail, sizeof out->detail, "file is shorter than a DOS header");
        fclose(file);
        return false;
    }
    unsigned char dos[64];
    if (!read_at(file, 0, dos, sizeof dos) || memcmp(dos, "MZ", 2) != 0) {
        snprintf(out->detail, sizeof out->detail, "missing MZ header");
        fclose(file);
        return false;
    }
    uint32_t pe = rd32(dos + 0x3c);
    if ((uint64_t)pe + 24 > size) {
        snprintf(out->detail, sizeof out->detail, "e_lfanew %u is outside the file", pe);
        fclose(file);
        return false;
    }
    unsigned char coff[24];
    if (!read_at(file, pe, coff, sizeof coff) || memcmp(coff, "PE\0\0", 4) != 0) {
        snprintf(out->detail, sizeof out->detail, "missing PE signature");
        fclose(file);
        return false;
    }
    out->machine = rd16(coff + 4);
    out->sections = rd16(coff + 6);
    uint16_t optional_size = rd16(coff + 20);
    if (out->machine != 0x8664) {
        snprintf(out->detail, sizeof out->detail, "machine 0x%04x is not x86-64", out->machine);
        fclose(file);
        return false;
    }
    if (out->sections == 0 || out->sections > 96 || optional_size < 128 ||
        (uint64_t)pe + 24 + optional_size > size) {
        snprintf(out->detail, sizeof out->detail, "invalid COFF/optional header");
        fclose(file);
        return false;
    }
    unsigned char optional[128];
    if (!read_at(file, (uint64_t)pe + 24, optional, sizeof optional) || rd16(optional) != 0x20b) {
        snprintf(out->detail, sizeof out->detail, "not a PE32+ optional header");
        fclose(file);
        return false;
    }
    uint32_t directory_count = rd32(optional + 108);
    uint32_t export_rva = rd32(optional + 112);
    uint32_t export_size = rd32(optional + 116);
    if (directory_count < 1 || export_rva == 0 || export_size < 40) {
        snprintf(out->detail, sizeof out->detail, "no export directory");
        fclose(file);
        return false;
    }
    PeSection sections[96];
    memset(sections, 0, sizeof sections);
    uint64_t table = (uint64_t)pe + 24 + optional_size;
    for (uint32_t i = 0; i < out->sections; i++) {
        unsigned char header[40];
        if (!read_at(file, table + (uint64_t)i * 40, header, sizeof header)) {
            snprintf(out->detail, sizeof out->detail, "section table is truncated");
            fclose(file);
            return false;
        }
        sections[i].virtual_size = rd32(header + 8);
        sections[i].virtual_address = rd32(header + 12);
        sections[i].raw_size = rd32(header + 16);
        sections[i].raw_offset = rd32(header + 20);
        if (sections[i].raw_size && (uint64_t)sections[i].raw_offset + sections[i].raw_size > size) {
            snprintf(out->detail, sizeof out->detail, "section %u extends past the file", i);
            fclose(file);
            return false;
        }
    }
    out->valid = true;
    uint64_t export_offset = 0;
    if (!rva_to_offset(sections, out->sections, size, export_rva, &export_offset)) {
        snprintf(out->detail, sizeof out->detail, "export directory RVA 0x%x is unmapped", export_rva);
        fclose(file);
        return false;
    }
    unsigned char directory[40];
    if (!read_at(file, export_offset, directory, sizeof directory)) {
        snprintf(out->detail, sizeof out->detail, "export directory is truncated");
        fclose(file);
        return false;
    }
    uint32_t name_count = rd32(directory + 24);
    uint32_t names_rva = rd32(directory + 32);
    out->exports = name_count;
    if (name_count == 0 || name_count > 65536) {
        snprintf(out->detail, sizeof out->detail, "invalid export name count %u", name_count);
        fclose(file);
        return false;
    }
    uint64_t names_offset = 0;
    if (!rva_to_offset(sections, out->sections, size, names_rva, &names_offset)) {
        snprintf(out->detail, sizeof out->detail, "export name table RVA 0x%x is unmapped", names_rva);
        fclose(file);
        return false;
    }
    static const char *const required[] = {
        MK8_XESS_SYM_CREATE,           MK8_XESS_SYM_INIT,
        MK8_XESS_SYM_EXECUTE,          MK8_XESS_SYM_DESTROY,
        MK8_XESS_SYM_INSTANCE_EXTENSIONS, MK8_XESS_SYM_DEVICE_EXTENSIONS,
        MK8_XESS_SYM_DEVICE_FEATURES,
    };
    bool found[sizeof required / sizeof required[0]] = {false};
    for (uint32_t i = 0; i < name_count; i++) {
        unsigned char name_rva_bytes[4];
        if (!read_at(file, names_offset + (uint64_t)i * 4, name_rva_bytes, sizeof name_rva_bytes)) break;
        uint64_t name_offset = 0;
        if (!rva_to_offset(sections, out->sections, size, rd32(name_rva_bytes), &name_offset)) continue;
        char name[256];
        size_t length = 0;
        if (!read_at(file, name_offset, name, sizeof name - 1)) continue;
        name[sizeof name - 1] = 0;
        while (length < sizeof name && name[length]) length++;
        if (length == sizeof name) continue;
        for (size_t r = 0; r < sizeof required / sizeof required[0]; r++) {
            if (strcmp(name, required[r]) == 0) found[r] = true;
        }
    }
    out->exports_ok = true;
    for (size_t r = 0; r < sizeof required / sizeof required[0]; r++) {
        if (found[r]) continue;
        out->exports_ok = false;
        append_missing(out->missing, sizeof out->missing, required[r]);
    }
    fclose(file);
    return out->valid;
}

// ---------------------------------------------------------------------------------------
// Path resolution and probe.
// ---------------------------------------------------------------------------------------

static bool file_exists(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
#endif
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

static bool path_join(char *out, size_t size, const char *directory, const char *name) {
    if (!out || !size || !directory || !*directory || !name) return false;
    size_t used = strlen(directory);
    const char *separator = (directory[used - 1] == '/' || directory[used - 1] == '\\') ? "" : "/";
    int written = snprintf(out, size, "%s%s%s", directory, separator, name);
    return written > 0 && (size_t)written < size;
}

static void default_directory(char *out, size_t size) {
    const char *configured = getenv("MK8_XESS_DIR");
    if (configured && *configured) {
        snprintf(out, size, "%s", configured);
        return;
    }
    const char *base = getenv("XDG_DATA_HOME");
    if (base && *base) {
        snprintf(out, size, "%s/mk8online/intel", base);
        return;
    }
    base = getenv("HOME");
#ifdef _WIN32
    if (!base || !*base) base = getenv("USERPROFILE");
#endif
    if (base && *base) {
        snprintf(out, size, "%s/.local/share/mk8online/intel", base);
        return;
    }
    snprintf(out, size, "mk8online/intel");
}

static bool find_dll(const char *directory, char *out, size_t size) {
    char candidate[MK8_XESS_PATH_MAX];
    if (path_join(candidate, sizeof candidate, directory, "libxess.dll") && file_exists(candidate)) {
        snprintf(out, size, "%s", candidate);
        return true;
    }
#ifdef _WIN32
    char pattern[MK8_XESS_PATH_MAX];
    if (!path_join(pattern, sizeof pattern, directory, "libxess*.dll")) return false;
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (path_join(candidate, sizeof candidate, directory, entry.cFileName)) {
            snprintf(out, size, "%s", candidate);
            found = true;
            break;
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    return found;
#else
    DIR *directory_handle = opendir(directory);
    if (!directory_handle) return false;
    struct dirent *entry;
    bool found = false;
    while ((entry = readdir(directory_handle)) != NULL) {
        if (strncmp(entry->d_name, "libxess", 7) != 0) continue;
        size_t length = strlen(entry->d_name);
        if (length < 4 || strcmp(entry->d_name + length - 4, ".dll") != 0) continue;
        if (path_join(candidate, sizeof candidate, directory, entry->d_name)) {
            snprintf(out, size, "%s", candidate);
            found = true;
            break;
        }
    }
    closedir(directory_handle);
    return found;
#endif
}

const char *mk8_xess_status_name(Mk8XessLoadStatus status) {
    switch (status) {
    case MK8_XESS_OK: return "ok";
    case MK8_XESS_UNSUPPORTED_PLATFORM: return "unsupported-platform";
    case MK8_XESS_NO_DLL: return "no-dll";
    case MK8_XESS_DLL_LOAD_FAILED: return "dll-load-failed";
    case MK8_XESS_DLL_EXPORTS_MISSING: return "dll-exports-missing";
    }
    return "unknown";
}

void mk8_xess_log_unavailable(const Mk8XessProbe *probe) {
    const char *reason = probe && probe->reason[0] ? probe->reason : "no reason recorded";
    fprintf(stderr, "[xess] XeSS unavailable: %s\n", reason);
}

bool mk8_xess_enabled(void) {
    const char *value = getenv("MK8_XESS");
    return value && *value && strcmp(value, "0") != 0;
}

#ifdef _WIN32
static void win_error_text(char *out, size_t size) {
    DWORD code = GetLastError();
    DWORD written = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                                   out, (DWORD)size, NULL);
    if (!written) snprintf(out, size, "Win32 error %lu", (unsigned long)code);
    else {
        while (written && (out[written - 1] == '\r' || out[written - 1] == '\n')) out[--written] = 0;
    }
}
#endif

bool mk8_xess_probe(Mk8XessProbe *out) {
    Mk8XessProbe local;
    memset(&local, 0, sizeof local);
    local.status = MK8_XESS_NO_DLL;
#ifdef _WIN32
    local.platform_supported = true;
#else
    local.platform_supported = false;
#endif
    default_directory(local.directory, sizeof local.directory);

    const char *configured = getenv("MK8_XESS_LIB");
    if (configured && *configured) {
        snprintf(local.dll, sizeof local.dll, "%s", configured);
        path_directory(local.dll, local.directory, sizeof local.directory);
    } else if (!find_dll(local.directory, local.dll, sizeof local.dll)) {
        snprintf(local.reason, sizeof local.reason,
                 "no libxess.dll in %.400s; fetch the XeSS SDK with tools/fetch_vendor_sdk.py "
                 "--accept-license intel and extract bin/libxess.dll, or set MK8_XESS_LIB",
                 local.directory);
        if (out) *out = local;
        return false;
    }
    local.dll_present = true;

    Mk8XessPeInfo pe;
    if (!mk8_xess_inspect_pe(local.dll, &pe) || !pe.valid) {
        local.status = MK8_XESS_DLL_LOAD_FAILED;
        snprintf(local.reason, sizeof local.reason,
                 "libxess.dll %.400s is not a complete PE32+ x86-64 DLL: %s", local.dll,
                 pe.detail[0] ? pe.detail : "unknown PE error");
        if (out) *out = local;
        return false;
    }
    local.pe_valid = true;
    local.exports_ok = pe.exports_ok;
    if (!pe.exports_ok) {
        local.status = MK8_XESS_DLL_EXPORTS_MISSING;
        snprintf(local.reason, sizeof local.reason, "libxess.dll %.400s lacks XeSS Vulkan exports: %s", local.dll,
                 pe.missing[0] ? pe.missing : (pe.detail[0] ? pe.detail : "unknown"));
        if (out) *out = local;
        return false;
    }

    char licence[MK8_XESS_PATH_MAX];
    local.licence_present =
        path_join(licence, sizeof licence, local.directory, "LICENSE.txt") && file_exists(licence);

#ifndef _WIN32
    local.status = MK8_XESS_UNSUPPORTED_PLATFORM;
    snprintf(local.reason, sizeof local.reason,
             "XeSS is Windows-only (libxess.dll); this Linux build keeps the FSR/bilinear path");
    if (out) *out = local;
    return false;
#else
    SetLastError(0);
    HMODULE library = LoadLibraryA(local.dll);
    if (!library) {
        char error[MK8_XESS_REASON_MAX];
        win_error_text(error, sizeof error);
        local.status = MK8_XESS_DLL_LOAD_FAILED;
        snprintf(local.reason, sizeof local.reason, "libxess.dll %.400s did not load: %s", local.dll, error);
        if (out) *out = local;
        return false;
    }
    local.loaded = true;
    static const char *const required[] = {
        MK8_XESS_SYM_CREATE,           MK8_XESS_SYM_INIT,
        MK8_XESS_SYM_EXECUTE,          MK8_XESS_SYM_DESTROY,
        MK8_XESS_SYM_INSTANCE_EXTENSIONS, MK8_XESS_SYM_DEVICE_EXTENSIONS,
        MK8_XESS_SYM_DEVICE_FEATURES,
    };
    char missing[MK8_XESS_MISSING_MAX] = {0};
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++) {
        if (GetProcAddress(library, required[i])) continue;
        append_missing(missing, sizeof missing, required[i]);
    }
    if (missing[0]) {
        local.status = MK8_XESS_DLL_EXPORTS_MISSING;
        snprintf(local.reason, sizeof local.reason, "libxess.dll %.400s lacks exports: %s", local.dll, missing);
        FreeLibrary(library);
        local.loaded = false;
        if (out) *out = local;
        return false;
    }
    local.status = MK8_XESS_OK;
    local.reason[0] = 0;
    if (out) *out = local;
    return true;
#endif
}

int mk8_xess_query(uint32_t *instance_extension_count, const char *const **instance_extensions,
                   uint32_t *min_vk_api_version, Mk8XessVersion *version, char *reason,
                   size_t reason_size) {
    if (reason && reason_size) reason[0] = 0;
    if (instance_extension_count) *instance_extension_count = 0;
    if (instance_extensions) *instance_extensions = NULL;
    if (min_vk_api_version) *min_vk_api_version = 0;
    if (version) memset(version, 0, sizeof *version);
#ifndef _WIN32
    if (reason && reason_size)
        snprintf(reason, reason_size, "XeSS is Windows-only (libxess.dll); no query on Linux");
    return -1;
#else
    Mk8XessProbe probe;
    if (!mk8_xess_probe(&probe)) {
        if (reason && reason_size) snprintf(reason, reason_size, "%s", probe.reason);
        return -1;
    }
    HMODULE library = LoadLibraryA(probe.dll);
    if (!library) {
        char error[MK8_XESS_REASON_MAX];
        win_error_text(error, sizeof error);
        if (reason && reason_size) snprintf(reason, reason_size, "libxess.dll did not load: %s", error);
        return -1;
    }
    Mk8XessGetVersionFn get_version =
        (Mk8XessGetVersionFn)(void *)GetProcAddress(library, MK8_XESS_SYM_GET_VERSION);
    if (get_version && version) get_version(version);
    Mk8XessVKGetRequiredInstanceExtensionsFn query =
        (Mk8XessVKGetRequiredInstanceExtensionsFn)(void *)GetProcAddress(
            library, MK8_XESS_SYM_INSTANCE_EXTENSIONS);
    if (!query) {
        if (reason && reason_size)
            snprintf(reason, reason_size, "libxess.dll lacks %s", MK8_XESS_SYM_INSTANCE_EXTENSIONS);
        return -1;
    }
    int result = query(instance_extension_count, instance_extensions, min_vk_api_version);
    if (result < 0 && reason && reason_size)
        snprintf(reason, reason_size, "%s returned %d", MK8_XESS_SYM_INSTANCE_EXTENSIONS, result);
    return result;
#endif
}

// ---------------------------------------------------------------------------------------
// Vulkan path (Windows) / stub (Linux).
// ---------------------------------------------------------------------------------------
#if !defined(MK8_XESS_PROBE_ONLY)

#ifdef _WIN32

#include <math.h>

#include "vulkan/vk.h"

extern "C" {
VkDescriptorSet fsr_vk_alloc_set(VkDescriptorSetLayout layout);
VkSampler fsr_vk_default_sampler(bool linear);
VkShaderModule fsr_vk_compile_glsl(const char *src, int stage, const char *name);

// exec.c: the main-draw camera scan behind the FSR bridge (toolchain/fsr.py).
extern float g_fsr_view[12];
extern float g_fsr_proj[16];
extern int g_fsr_vp_valid;
extern float g_fsr_jitter_x;
extern float g_fsr_jitter_y;
}

typedef struct Xess {
    bool tried;
    bool ready;
    bool failed;
    Mk8XessProbe probe;

    HMODULE library;
    Mk8XessVKCreateContextFn create;
    Mk8XessVKInitFn init;
    Mk8XessVKExecuteFn execute;
    Mk8XessDestroyContextFn destroy;
    Mk8XessVKGetRequiredInstanceExtensionsFn instance_extensions;
    Mk8XessVKGetRequiredDeviceExtensionsFn device_extensions;
    Mk8XessGetVersionFn get_version;
    Mk8XessContextHandle context;
    bool initialised;

    uint32_t rw, rh, ow, oh;
    int quality;
    const char *quality_name;
    bool reset_next;

    VkImage output;
    VmaAllocation output_mem;
    VkImageView output_view;
    VkFormat output_format;
    VkImage motion;
    VmaAllocation motion_mem;
    VkImageView motion_view;
    bool motion_primed;
    VkImage neutral_depth;
    VmaAllocation neutral_depth_mem;
    VkImageView neutral_depth_view;
    bool neutral_primed;

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
} Xess;

static Xess xess;

static const char *xess_result_name(int result) {
    switch (result) {
    case MK8_XESS_RESULT_SUCCESS: return "success";
    case MK8_XESS_RESULT_WARNING_NONEXISTING_FOLDER: return "warning-nonexisting-folder";
    case MK8_XESS_RESULT_WARNING_OLD_DRIVER: return "warning-old-driver";
    case MK8_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported-device";
    case MK8_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported-driver";
    case MK8_XESS_RESULT_ERROR_UNINITIALIZED: return "uninitialized";
    case MK8_XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid-argument";
    case MK8_XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "device-out-of-memory";
    case MK8_XESS_RESULT_ERROR_DEVICE: return "device-error";
    case MK8_XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not-implemented";
    case MK8_XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid-context";
    case MK8_XESS_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation-in-progress";
    case MK8_XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case MK8_XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "cant-load-library";
    case MK8_XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong-call-order";
    default: return "unknown";
    }
}

static void log_result(const char *what, int result) {
    fprintf(stderr, "[xess] %s failed %d (%s)\n", what, result, xess_result_name(result));
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
                          VmaAllocation *allocation) {
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
    return image;
}

static void destroy_images(void) {
    if (xess.context && xess.destroy) xess.destroy(xess.context);
    xess.context = NULL;
    xess.initialised = false;
    if (xess.output_view) vkDestroyImageView(g_vk.dev, xess.output_view, NULL);
    if (xess.output) {
        vkDestroyImage(g_vk.dev, xess.output, NULL);
        vmaFreeMemory(g_vk.vma, xess.output_mem);
    }
    xess.output = VK_NULL_HANDLE;
    xess.output_mem = VK_NULL_HANDLE;
    xess.output_view = VK_NULL_HANDLE;
    if (xess.neutral_depth_view) vkDestroyImageView(g_vk.dev, xess.neutral_depth_view, NULL);
    if (xess.neutral_depth) {
        vkDestroyImage(g_vk.dev, xess.neutral_depth, NULL);
        vmaFreeMemory(g_vk.vma, xess.neutral_depth_mem);
    }
    xess.neutral_depth = VK_NULL_HANDLE;
    xess.neutral_depth_mem = VK_NULL_HANDLE;
    xess.neutral_depth_view = VK_NULL_HANDLE;
    xess.neutral_primed = false;
    if (xess.color_view) vkDestroyImageView(g_vk.dev, xess.color_view, NULL);
    xess.color_view = VK_NULL_HANDLE;
    xess.color_view_image = VK_NULL_HANDLE;
    if (xess.depth_view) vkDestroyImageView(g_vk.dev, xess.depth_view, NULL);
    xess.depth_view = VK_NULL_HANDLE;
    xess.depth_view_image = VK_NULL_HANDLE;
    if (xess.motion_view) vkDestroyImageView(g_vk.dev, xess.motion_view, NULL);
    if (xess.motion) {
        vkDestroyImage(g_vk.dev, xess.motion, NULL);
        vmaFreeMemory(g_vk.vma, xess.motion_mem);
    }
    xess.motion = VK_NULL_HANDLE;
    xess.motion_mem = VK_NULL_HANDLE;
    xess.motion_view = VK_NULL_HANDLE;
    xess.motion_primed = false;
    xess.have_prev = false;
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

static bool ensure_motion(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (xess.motion && xess.motion_view && xess.motion_primed) return true;
    if (!xess.motion || !xess.motion_view) {
        if (xess.motion_view) vkDestroyImageView(g_vk.dev, xess.motion_view, NULL);
        if (xess.motion) {
            vkDestroyImage(g_vk.dev, xess.motion, NULL);
            vmaFreeMemory(g_vk.vma, xess.motion_mem);
        }
        xess.motion = make_image(VK_FORMAT_R16G16_SFLOAT, w, h,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 &xess.motion_mem);
        xess.motion_view = xess.motion ? image_view_of(xess.motion, VK_FORMAT_R16G16_SFLOAT,
                                                      VK_IMAGE_ASPECT_COLOR_BIT)
                                       : VK_NULL_HANDLE;
        if (!xess.motion || !xess.motion_view) return false;
    }
    barrier(cb, xess.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, xess.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    barrier(cb, xess.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    xess.motion_primed = true;
    return true;
}

static bool ensure_neutral_depth(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (xess.neutral_depth && xess.neutral_depth_view && xess.neutral_primed) return true;
    if (!xess.neutral_depth || !xess.neutral_depth_view) {
        xess.neutral_depth = make_image(VK_FORMAT_R32_SFLOAT, w, h,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        &xess.neutral_depth_mem);
        xess.neutral_depth_view =
            xess.neutral_depth ? image_view_of(xess.neutral_depth, VK_FORMAT_R32_SFLOAT,
                                               VK_IMAGE_ASPECT_COLOR_BIT)
                               : VK_NULL_HANDLE;
        if (!xess.neutral_depth || !xess.neutral_depth_view) return false;
    }
    barrier(cb, xess.neutral_depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue one = {{1.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, xess.neutral_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
    barrier(cb, xess.neutral_depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    xess.neutral_primed = true;
    return true;
}

static int quality_for(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, const char **name) {
    double ratio = ow ? (double)rw / (double)ow : 1.0;
    if (rw + 1 >= ow && rh + 1 >= oh) {
        *name = "aa";
        return MK8_XESS_QUALITY_AA;
    }
    if (ratio <= 0.40) {
        *name = "ultra-performance";
        return MK8_XESS_QUALITY_ULTRA_PERFORMANCE;
    }
    if (ratio <= 0.55) {
        *name = "performance";
        return MK8_XESS_QUALITY_PERFORMANCE;
    }
    if (ratio <= 0.62) {
        *name = "balanced";
        return MK8_XESS_QUALITY_BALANCED;
    }
    if (ratio <= 0.72) {
        *name = "quality";
        return MK8_XESS_QUALITY_QUALITY;
    }
    if (ratio <= 0.85) {
        *name = "ultra-quality";
        return MK8_XESS_QUALITY_ULTRA_QUALITY;
    }
    *name = "ultra-quality-plus";
    return MK8_XESS_QUALITY_ULTRA_QUALITY_PLUS;
}

static bool resolve_symbols(void) {
    xess.create = (Mk8XessVKCreateContextFn)(void *)GetProcAddress(xess.library, MK8_XESS_SYM_CREATE);
    xess.init = (Mk8XessVKInitFn)(void *)GetProcAddress(xess.library, MK8_XESS_SYM_INIT);
    xess.execute = (Mk8XessVKExecuteFn)(void *)GetProcAddress(xess.library, MK8_XESS_SYM_EXECUTE);
    xess.destroy = (Mk8XessDestroyContextFn)(void *)GetProcAddress(xess.library, MK8_XESS_SYM_DESTROY);
    xess.instance_extensions = (Mk8XessVKGetRequiredInstanceExtensionsFn)(void *)GetProcAddress(
        xess.library, MK8_XESS_SYM_INSTANCE_EXTENSIONS);
    xess.device_extensions = (Mk8XessVKGetRequiredDeviceExtensionsFn)(void *)GetProcAddress(
        xess.library, MK8_XESS_SYM_DEVICE_EXTENSIONS);
    xess.get_version = (Mk8XessGetVersionFn)(void *)GetProcAddress(xess.library, MK8_XESS_SYM_GET_VERSION);
    return xess.create && xess.init && xess.execute && xess.destroy;
}

static bool load_xess(void) {
    memset(&xess.probe, 0, sizeof xess.probe);
    if (!mk8_xess_probe(&xess.probe)) return false;
    xess.library = LoadLibraryA(xess.probe.dll);
    if (!xess.library) {
        snprintf(xess.probe.reason, sizeof xess.probe.reason, "libxess.dll did not load now");
        return false;
    }
    if (!resolve_symbols()) {
        snprintf(xess.probe.reason, sizeof xess.probe.reason,
                 "libxess.dll exports disappeared after the probe");
        return false;
    }
    if (xess.get_version) {
        Mk8XessVersion version = {};
        if (xess.get_version(&version) >= 0)
            fprintf(stderr, "[xess] provider %u.%u.%u from %s\n", version.major, version.minor, version.patch,
                    xess.probe.dll);
    }
    if (xess.instance_extensions) {
        uint32_t count = 0, min_api = 0;
        const char *const *names = NULL;
        int rc = xess.instance_extensions(&count, &names, &min_api);
        if (rc >= 0) {
            fprintf(stderr, "[xess] required instance API 0x%x, %u extension(s)\n", min_api, count);
            for (uint32_t i = 0; i < count && i < 8; i++)
                fprintf(stderr, "[xess]   instance extension: %s\n", names[i] ? names[i] : "?");
        }
    }
    if (xess.device_extensions) {
        uint32_t count = 0;
        const char *const *names = NULL;
        int rc = xess.device_extensions(g_vk.inst, g_vk.phys, &count, &names);
        if (rc >= 0) {
            fprintf(stderr, "[xess] %u required device extension(s)\n", count);
            for (uint32_t i = 0; i < count && i < 16; i++)
                fprintf(stderr, "[xess]   device extension: %s\n", names[i] ? names[i] : "?");
        }
    }

    int rc = xess.create(g_vk.inst, g_vk.phys, g_vk.dev, &xess.context);
    if (rc < 0 || !xess.context) {
        log_result(MK8_XESS_SYM_CREATE, rc);
        snprintf(xess.probe.reason, sizeof xess.probe.reason, "%s failed %d (%s)", MK8_XESS_SYM_CREATE,
                 rc, xess_result_name(rc));
        xess.context = NULL;
        return false;
    }
    Mk8XessInitParams params = {};
    params.outputResolution.x = xess.ow;
    params.outputResolution.y = xess.oh;
    params.qualitySetting = xess.quality;
    params.initFlags = MK8_XESS_INIT_FLAG_LDR_INPUT_COLOR;
    const char *inverted = getenv("MK8_XESS_DEPTH_INVERTED");
    if (inverted && *inverted && atoi(inverted) != 0)
        params.initFlags |= MK8_XESS_INIT_FLAG_INVERTED_DEPTH;
    params.creationNodeMask = 1;
    params.visibleNodeMask = 1;
    rc = xess.init(xess.context, &params);
    if (rc < 0) {
        log_result(MK8_XESS_SYM_INIT, rc);
        snprintf(xess.probe.reason, sizeof xess.probe.reason, "%s failed %d (%s)", MK8_XESS_SYM_INIT, rc,
                 xess_result_name(rc));
        xess.destroy(xess.context);
        xess.context = NULL;
        return false;
    }
    xess.initialised = true;
    xess.reset_next = true;
    fprintf(stderr, "[xess] SR context %ux%u -> %ux%u, quality %s, flags 0x%x\n", xess.rw, xess.rh, xess.ow,
            xess.oh, xess.quality_name, params.initFlags);
    return true;
}

static bool create_output(uint32_t ow, uint32_t oh) {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    xess.output = make_image(format, ow, oh,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             &xess.output_mem);
    if (!xess.output) return false;
    xess.output_format = format;
    xess.output_view = image_view_of(xess.output, format, VK_IMAGE_ASPECT_COLOR_BIT);
    return xess.output_view != VK_NULL_HANDLE;
}

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
    if (xess.mv_pipeline) return true;
    VkDescriptorSetLayoutBinding bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 3;
    layout.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_vk.dev, &layout, NULL, &xess.mv_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pipeline_layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &xess.mv_layout;
    if (vkCreatePipelineLayout(g_vk.dev, &pipeline_layout, NULL, &xess.mv_pipeline_layout) != VK_SUCCESS)
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
    VkShaderModule module = fsr_vk_compile_glsl(source, VK_SHADER_STAGE_COMPUTE_BIT, "xess_mv");
    if (!module) return false;
    VkComputePipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = module;
    pipeline.stage.pName = "main";
    pipeline.layout = xess.mv_pipeline_layout;
    VkResult result = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &pipeline, NULL, &xess.mv_pipeline);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (result != VK_SUCCESS) return false;
    VkBufferCreateInfo buffer = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = 144;
    buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocation = {};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                       VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(g_vk.vma, &buffer, &allocation, &xess.mv_ubo, &xess.mv_ubo_mem, &info) != VK_SUCCESS)
        return false;
    xess.mv_ubo_ptr = info.pMappedData;
    return xess.mv_ubo_ptr != NULL;
}

static bool run_mv(VkCommandBuffer cb, TexBackend *depth, uint32_t rw, uint32_t rh) {
    if (!g_fsr_vp_valid || !depth || !depth->image || !xess.mv_pipeline || !xess.mv_ubo_ptr ||
        !xess.depth_view)
        return false;
    float v[16] = {g_fsr_view[0], g_fsr_view[1], g_fsr_view[2], g_fsr_view[3],
                   g_fsr_view[4], g_fsr_view[5], g_fsr_view[6], g_fsr_view[7],
                   g_fsr_view[8], g_fsr_view[9], g_fsr_view[10], g_fsr_view[11],
                   0.0f, 0.0f, 0.0f, 1.0f};
    float vp[16], inv[16], prev[16];
    mul4(g_fsr_proj, v, vp);
    if (!invert4(vp, inv)) return false;
    if (xess.have_prev) memcpy(prev, xess.prev_vp, sizeof prev);
    else memcpy(prev, vp, sizeof prev);
    float *uniform = (float *)xess.mv_ubo_ptr;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            uniform[c * 4 + r] = inv[r * 4 + c];
            uniform[16 + c * 4 + r] = prev[r * 4 + c];
        }
    uniform[32] = (float)rw;
    uniform[33] = (float)rh;
    memcpy(xess.prev_vp, vp, sizeof vp);
    xess.have_prev = true;
    barrier(cb, xess.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkDescriptorSet set = fsr_vk_alloc_set(xess.mv_layout);
    VkDescriptorBufferInfo buffer_info = {xess.mv_ubo, 0, 144};
    VkDescriptorImageInfo depth_info = {fsr_vk_default_sampler(true), xess.depth_view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo output_info = {VK_NULL_HANDLE, xess.motion_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
         NULL, &buffer_info, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth_info, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         &output_info, NULL, NULL}};
    vkUpdateDescriptorSets(g_vk.dev, 3, writes, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, xess.mv_pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, xess.mv_pipeline);
    vkCmdDispatch(cb, (rw + 15) / 16, (rh + 15) / 16, 1);
    return true;
}

static bool ensure_feature(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (xess.context && xess.rw == rw && xess.rh == rh && xess.ow == ow && xess.oh == oh) return true;
    if (xess.context) destroy_images();
    xess.rw = rw;
    xess.rh = rh;
    xess.ow = ow;
    xess.oh = oh;
    xess.quality = quality_for(rw, rh, ow, oh, &xess.quality_name);
    if (!create_output(ow, oh)) {
        snprintf(xess.probe.reason, sizeof xess.probe.reason, "cannot create the %ux%u XeSS output image", ow,
                 oh);
        return false;
    }
    barrier(cb, xess.output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (!ensure_motion(cb, rw, rh)) {
        snprintf(xess.probe.reason, sizeof xess.probe.reason,
                 "cannot create the %ux%u XeSS motion image", rw, rh);
        return false;
    }
    if (!ensure_neutral_depth(cb, rw, rh)) {
        snprintf(xess.probe.reason, sizeof xess.probe.reason,
                 "cannot create the %ux%u XeSS neutral depth image", rw, rh);
        return false;
    }
    if (!create_mv_resources()) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[xess] camera motion-vector pipeline unavailable; using zero vectors\n");
    }
    return load_xess();
}

extern "C" VkImageView mk8_xess_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, uint32_t rw,
                                        uint32_t rh, uint32_t ow, uint32_t oh, float frame_ms) {
    (void)frame_ms;
    if (!mk8_xess_enabled()) return VK_NULL_HANDLE;
    if (getenv("MK8_FSR_FG")) {
        static int once;
        if (!once++) fprintf(stderr, "[xess] MK8_FSR_FG is set: XeSS Super Resolution stays off\n");
        return VK_NULL_HANDLE;
    }
    if (!cb || !color || !color->image || !rw || !rh || !ow || !oh) return VK_NULL_HANDLE;
    if (ow < rw || oh < rh) {
        static int once;
        if (!once++)
            fprintf(stderr, "[xess] display %ux%u is smaller than render %ux%u: not an upscale\n", ow, oh,
                    rw, rh);
        return VK_NULL_HANDLE;
    }
    if (!xess.tried) {
        xess.tried = true;
        xess.ready = true;
        if (!ensure_feature(cb, rw, rh, ow, oh)) {
            mk8_xess_log_unavailable(&xess.probe);
            fprintf(stderr, "[xess] falling back to the existing FSR/bilinear path\n");
            xess.failed = true;
            return VK_NULL_HANDLE;
        }
    }
    if (!xess.ready || xess.failed || !xess.context) return VK_NULL_HANDLE;
    if (!ensure_feature(cb, rw, rh, ow, oh)) {
        if (xess.probe.reason[0]) {
            mk8_xess_log_unavailable(&xess.probe);
            xess.probe.reason[0] = 0;
        }
        xess.failed = true;
        return VK_NULL_HANDLE;
    }

    bool real_depth = depth && depth->image && depth->width == rw && depth->height == rh;
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_R32_SFLOAT;
    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (real_depth) {
        VkImageAspectFlags aspect =
            (depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                        : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!ensure_cached_view(&xess.depth_view, &xess.depth_view_image, &xess.depth_view_format,
                                &xess.depth_view_w, &xess.depth_view_h, depth->image, depth->format, aspect,
                                rw, rh))
            return VK_NULL_HANDLE;
        depth_image = depth->image;
        depth_view = xess.depth_view;
        depth_format = depth->format;
        depth_aspect = aspect;
        barrier(cb, depth_image, aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    } else {
        if (!ensure_neutral_depth(cb, rw, rh)) return VK_NULL_HANDLE;
        depth_image = xess.neutral_depth;
        depth_view = xess.neutral_depth_view;
    }

    static int depth_state = -1;
    if ((int)real_depth != depth_state) {
        depth_state = (int)real_depth;
        fprintf(stderr, "[xess] depth source: %s %ux%u fmt %d\n",
                real_depth ? "captured scene depth" : "neutral (no captured scene depth)", rw, rh,
                depth_format);
    }

    if (!ensure_motion(cb, rw, rh)) return VK_NULL_HANDLE;
    bool motion = real_depth && run_mv(cb, depth, rw, rh);
    if (motion)
        barrier(cb, xess.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    static int motion_state = -1;
    if ((int)motion != motion_state) {
        motion_state = (int)motion;
        if (motion)
            fprintf(stderr, "[xess] motion vectors: camera-derived\n");
        else
            fprintf(stderr, "[xess] WARNING: motion vectors are zeros (no captured view/projection); "
                            "XeSS will ghost on everything that moves\n");
    }

    if (!ensure_cached_view(&xess.color_view, &xess.color_view_image, &xess.color_view_format,
                            &xess.color_view_w, &xess.color_view_h, color->image, color->format,
                            VK_IMAGE_ASPECT_COLOR_BIT, rw, rh))
        return VK_NULL_HANDLE;
    barrier(cb, color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    Mk8XessExecuteParams params = {};
    params.colorTexture.imageView = xess.color_view;
    params.colorTexture.image = color->image;
    params.colorTexture.subresourceRange =
        (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    params.colorTexture.format = color->format;
    params.colorTexture.width = rw;
    params.colorTexture.height = rh;
    params.velocityTexture.imageView = xess.motion_view;
    params.velocityTexture.image = xess.motion;
    params.velocityTexture.subresourceRange =
        (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    params.velocityTexture.format = VK_FORMAT_R16G16_SFLOAT;
    params.velocityTexture.width = rw;
    params.velocityTexture.height = rh;
    params.depthTexture.imageView = depth_view;
    params.depthTexture.image = depth_image;
    params.depthTexture.subresourceRange = (VkImageSubresourceRange){depth_aspect, 0, 1, 0, 1};
    params.depthTexture.format = depth_format;
    params.depthTexture.width = rw;
    params.depthTexture.height = rh;
    params.outputTexture.imageView = xess.output_view;
    params.outputTexture.image = xess.output;
    params.outputTexture.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    params.outputTexture.format = xess.output_format;
    params.outputTexture.width = ow;
    params.outputTexture.height = oh;
    params.jitterOffsetX = g_fsr_jitter_x;
    params.jitterOffsetY = g_fsr_jitter_y;
    params.exposureScale = 1.0f;
    params.resetHistory = xess.reset_next ? 1 : 0;
    params.inputWidth = rw;
    params.inputHeight = rh;
    xess.reset_next = false;

    int rc = xess.execute(xess.context, cb, &params);
    if (rc < 0) {
        log_result(MK8_XESS_SYM_EXECUTE, rc);
        snprintf(xess.probe.reason, sizeof xess.probe.reason, "%s failed %d (%s)", MK8_XESS_SYM_EXECUTE, rc,
                 xess_result_name(rc));
        mk8_xess_log_unavailable(&xess.probe);
        xess.failed = true;
        return VK_NULL_HANDLE;
    }
    if (rc > 0) {
        static int warned;
        if (!warned++) fprintf(stderr, "[xess] %s warning %d (%s)\n", MK8_XESS_SYM_EXECUTE, rc,
                               xess_result_name(rc));
    }
    static int active;
    if (!active++) {
        fprintf(stderr, "[xess] upscale active %ux%u -> %ux%u (%s)\n", rw, rh, ow, oh, xess.quality_name);
    }
    return xess.output_view;
}

#else  // !_WIN32

extern "C" VkImageView mk8_xess_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, uint32_t rw,
                                        uint32_t rh, uint32_t ow, uint32_t oh, float frame_ms) {
    (void)cb;
    (void)color;
    (void)depth;
    (void)rw;
    (void)rh;
    (void)ow;
    (void)oh;
    (void)frame_ms;
    static int once;
    if (!once++)
        fprintf(stderr, "[xess] XeSS unavailable: XeSS is Windows-only (libxess.dll); this Linux build keeps "
                        "the FSR/bilinear path\n");
    return VK_NULL_HANDLE;
}

#endif  // _WIN32
#endif  // !MK8_XESS_PROBE_ONLY

/* mlfi_replay: replay AMD's FSR 4 MLFI pass graph natively on Vulkan.
 *
 * The pass order, dispatch geometry, descriptor offsets and tensor table come from the Wine
 * capture committed under tools/mlfi-capture/derived/.  The shader modules and the model bank
 * come from the user's own extraction (tools/extract_mlfi.py) and are legalised with
 * tools/radv-mlfi/fix_image_types.py before this program loads them (see prepare.py).
 *
 * What this program does
 *   - creates a headless Vulkan 1.3 device with the feature set the modules require;
 *   - creates the 195 captured pipelines (191 mlfi_4k model passes + 4 preprocess "main");
 *   - allocates the model buffer (the exact uploaded range), the 3,276,800-byte uniform arena,
 *     the scratch buffer and the images the captured descriptor bindings name;
 *   - records 5 preprocess dispatches and pass0..pass190 in the captured order and geometry;
 *   - feeds a synthetic moving pattern, constant depth and zero motion vectors;
 *   - reads the generated frame (rw_prediction, RGBA32F) back, hashes it, writes a PNG and a
 *     raw dump, and prints a summary.
 *
 * It does not contain AMD code or data: module and model paths are arguments and live outside
 * the repository.
 *
 * Build: tools/mlfi-replay/build.sh
 */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define API_VERSION VK_API_VERSION_1_3
#define MAX_BINDINGS 32
#define MAX_MODULES 256
#define MAX_ROWS 512
#define MAX_IMAGES 64

/* ------------------------------------------------------------------ utility ------------------ */

static int g_verbose;

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[mlfi-replay] ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    fflush(stdout);
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[mlfi-replay] error: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

static void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) die("out of memory (%zu x %zu)", n, s);
    return p;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Same offset basis as tools/mlfi-capture/mlfi_capture.c (1469598103934665603, not the canonical
 * FNV-1a 64 basis 14695981039346656037), so the hashes in the capture log can be compared. */
static uint64_t fnv1a64(const void *data, size_t size) {
    const uint8_t *p = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
    long length = ftell(f);
    if (length < 0) die("cannot tell %s", path);
    if (fseek(f, 0, SEEK_SET) != 0) die("cannot seek %s", path);
    uint8_t *data = xmalloc((size_t)length + 1);
    if (length && fread(data, 1, (size_t)length, f) != (size_t)length) die("short read %s", path);
    data[length] = 0; /* lets the TSV parsers use strtok_r safely */
    fclose(f);
    if (size) *size = (size_t)length;
    return data;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

static void write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    if (size && fwrite(data, 1, size, f) != size) die("short write %s", path);
    fclose(f);
}

/* ------------------------------------------------------------------ SPIR-V front end ---------- */

typedef struct {
    uint32_t set, binding;
    uint32_t var_id;
    VkDescriptorType type;
    char name[64];
    uint32_t sampled_kind; /* 0 = int, 1 = float, 2 = unknown */
    uint32_t sampled_bits;
    uint32_t sampled_flag; /* OpTypeImage Sampled operand */
    uint32_t image_format;
    uint32_t dim;
} Binding;

typedef struct {
    char entry[96];
    char path[4096];
    char ubo_name[64]; /* name of the first uniform block, used to identify a "main" module */
    uint8_t *code;
    size_t size;
    uint32_t version;
    uint32_t local[3];
    Binding binds[MAX_BINDINGS];
    uint32_t nbind;
} Module;

enum { K_NONE = 0, K_SAMPLER, K_IMAGE, K_STRUCT, K_POINTER, K_BOOL, K_VOID, K_FLOAT, K_INT, K_VECTOR };

typedef struct {
    uint8_t kind[65536];
    uint8_t block[65536]; /* 2 = Block, 3 = BufferBlock */
    uint16_t pointer_pointee[65536];
    uint8_t pointer_storage[65536];
    uint16_t image_sampled_type[65536];
    uint8_t image_sampled[65536];
    uint32_t image_format[65536];
    uint32_t image_dim[65536];
    uint32_t binding[65536];
    uint32_t set[65536];
    uint32_t var_type[65536];
    uint32_t var_storage[65536];
    uint8_t scalar_kind[65536];
    uint8_t scalar_bits[65536];
    uint8_t in_iface[65536];
    char *names[65536];
} SpirvScan;

static SpirvScan *g_scan; /* one at a time; large enough to keep off the stack */

static const char *spv_image_format_name(uint32_t f) {
    static const char *names[] = {
        "Unknown", "Rgba32f", "Rgba16f", "R32f", "Rgba8", "Rgba8Snorm", "Rg32f", "Rg16f",
        "R11fG11fB10f", "R16f", "Rgba16", "Rgb10A2", "Rg16", "Rg8", "R16", "R8",
        "Rgba16Snorm", "Rg16Snorm", "Rg8Snorm", "R16Snorm", "R8Snorm", "Rgba32i", "Rgba32ui",
        "Rgba16i", "Rgba16ui", "Rgba8i", "Rgba8ui", "R32i", "R32ui", "Rg32i", "Rg32ui",
        "Rg16i", "Rg16ui", "Rg8i", "Rg8ui", "R16i", "R16ui", "R8i", "R8ui", "R64ui", "R64i"};
    return f < sizeof(names) / sizeof(names[0]) ? names[f] : "?";
}

/* Parse the decorations and type graph of a module.  Returns 0 on success. */
static int spv_scan(const uint8_t *code, size_t size, Module *m) {
    if (size < 20 || (size % 4) || rd32(code) != 0x07230203) return -1;
    m->version = rd32(code + 4);
    uint32_t words = (uint32_t)(size / 4);
    SpirvScan *s = g_scan;
    memset(s->kind, 0, sizeof(s->kind));
    memset(s->block, 0, sizeof(s->block));
    memset(s->image_sampled, 0, sizeof(s->image_sampled));
    memset(s->scalar_kind, 0, sizeof(s->scalar_kind));
    memset(s->in_iface, 0, sizeof(s->in_iface));
    for (uint32_t i = 0; i < 65536; ++i) {
        s->binding[i] = 0xffffffffu;
        s->set[i] = 0xffffffffu;
        s->var_type[i] = 0;
        s->var_storage[i] = 0xffffffffu;
        if (s->names[i]) { free(s->names[i]); s->names[i] = NULL; }
    }
    m->entry[0] = 0;
    m->ubo_name[0] = 0;
    m->local[0] = m->local[1] = m->local[2] = 1;

    uint32_t at = 5;
    while (at + 1 <= words) {
        uint32_t word = rd32(code + at * 4);
        uint32_t count = word >> 16, op = word & 0xffff;
        if (!count || at + count > words) return -1;
        switch (op) {
        case 5: { /* OpName */
            uint32_t target = rd32(code + (at + 1) * 4);
            if (count >= 3 && target < 65536) {
                const char *text = (const char *)(code + (at + 2) * 4);
                s->names[target] = xstrdup(text);
            }
            break;
        }
        case 15: { /* OpEntryPoint */
            if (count >= 4 && !m->entry[0]) {
                uint32_t model = rd32(code + (at + 1) * 4);
                if (model != 5) return -2; /* not GLCompute */
                const char *name = (const char *)(code + (at + 3) * 4);
                snprintf(m->entry, sizeof(m->entry), "%s", name);
                /* SPIR-V 1.4+ lists every global variable the entry point uses; the capture only
                 * binds those, so unused declarations (e.g. pass2's binding 256) must not enter
                 * the descriptor set layout. */
                uint32_t name_words = (uint32_t)((strlen(name) + 1 + 3) / 4);
                for (uint32_t k = at + 3 + name_words; k < at + count; ++k)
                    s->in_iface[rd32(code + k * 4) & 0xffff] = 1;
            }
            break;
        }
        case 16: /* OpExecutionMode */
            if (count >= 6 && rd32(code + (at + 2) * 4) == 17) { /* LocalSize */
                m->local[0] = rd32(code + (at + 3) * 4);
                m->local[1] = rd32(code + (at + 4) * 4);
                m->local[2] = rd32(code + (at + 5) * 4);
            }
            break;
        case 71: { /* OpDecorate */
            uint32_t target = rd32(code + (at + 1) * 4) & 0xffff;
            uint32_t dec = rd32(code + (at + 2) * 4);
            uint32_t value = count >= 4 ? rd32(code + (at + 3) * 4) : 0;
            if (dec == 33) s->binding[target] = value;
            else if (dec == 34) s->set[target] = value;
            else if (dec == 2) s->block[target] = 2;
            else if (dec == 3) s->block[target] = 3;
            break;
        }
        case 21: { /* OpTypeInt */
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_INT;
            s->scalar_kind[id] = 0;
            s->scalar_bits[id] = (uint8_t)rd32(code + (at + 2) * 4);
            break;
        }
        case 22: { /* OpTypeFloat */
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_FLOAT;
            s->scalar_kind[id] = 1;
            s->scalar_bits[id] = (uint8_t)rd32(code + (at + 2) * 4);
            break;
        }
        case 23: { /* OpTypeVector */
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_VECTOR;
            break;
        }
        case 25: { /* OpTypeImage */
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_IMAGE;
            s->image_sampled_type[id] = (uint16_t)(rd32(code + (at + 2) * 4) & 0xffff);
            s->image_dim[id] = rd32(code + (at + 3) * 4);
            s->image_sampled[id] = (uint8_t)rd32(code + (at + 7) * 4);
            s->image_format[id] = count >= 9 ? rd32(code + (at + 8) * 4) : 0;
            break;
        }
        case 26: /* OpTypeSampler */
            s->kind[rd32(code + (at + 1) * 4) & 0xffff] = K_SAMPLER;
            break;
        case 30: /* OpTypeStruct */
            s->kind[rd32(code + (at + 1) * 4) & 0xffff] = K_STRUCT;
            break;
        case 32: { /* OpTypePointer */
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_POINTER;
            s->pointer_storage[id] = (uint8_t)rd32(code + (at + 2) * 4);
            s->pointer_pointee[id] = (uint16_t)(rd32(code + (at + 3) * 4) & 0xffff);
            break;
        }
        case 59: { /* OpVariable */
            uint32_t type = rd32(code + (at + 1) * 4) & 0xffff;
            uint32_t id = rd32(code + (at + 2) * 4) & 0xffff;
            s->var_type[id] = type;
            s->var_storage[id] = rd32(code + (at + 3) * 4);
            break;
        }
        default:
            break;
        }
        at += count;
    }

    m->nbind = 0;
    for (uint32_t id = 1; id < 65536; ++id) {
        if (!s->var_type[id] || s->binding[id] == 0xffffffffu) continue;
        /* SPIR-V 1.4+ lists every global a module uses in OpEntryPoint, which is exactly what the
         * captured descriptor sets contain; the one 1.3 module (the undispatched watermark pass)
         * has no interface list and keeps all of its declarations. */
        if (m->version >= 0x00010400u && !s->in_iface[id]) continue;
        uint32_t storage = s->var_storage[id];
        if (storage != 0 && storage != 2 && storage != 12) continue;
        uint32_t ptr = s->var_type[id];
        if (s->kind[ptr] != K_POINTER) continue;
        uint32_t pointee = s->pointer_pointee[ptr];
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (s->kind[pointee] == K_SAMPLER) {
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
        } else if (s->kind[pointee] == K_IMAGE) {
            type = s->image_sampled[pointee] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                  : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        } else if (s->kind[pointee] == K_STRUCT) {
            if (storage == 2 && s->block[pointee] == 2) type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;
        if (m->nbind >= MAX_BINDINGS) die("%s: more than %d bindings", m->path, MAX_BINDINGS);
        Binding *b = &m->binds[m->nbind++];
        memset(b, 0, sizeof(*b));
        b->var_id = id;
        b->set = s->set[id] == 0xffffffffu ? 0 : s->set[id];
        b->binding = s->binding[id];
        b->type = type;
        b->sampled_flag = s->image_sampled[pointee];
        b->image_format = s->image_format[pointee];
        b->dim = s->image_dim[pointee];
        uint32_t st = s->image_sampled_type[pointee];
        if (s->kind[st] == K_FLOAT || s->kind[st] == K_INT) {
            b->sampled_kind = s->scalar_kind[st];
            b->sampled_bits = s->scalar_bits[st];
        } else {
            b->sampled_kind = 2;
            b->sampled_bits = 0;
        }
        if (s->names[id]) {
            snprintf(b->name, sizeof(b->name), "%s", s->names[id]);
            if (g_verbose)
                logmsg("  %-18s set %u binding %3u type %d %s%s", b->name, b->set, b->binding,
                       (int)b->type,
                       type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                               type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                           ? spv_image_format_name(b->image_format)
                           : "",
                       type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? " (storage)" : "");
        }
        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && !m->ubo_name[0] && b->name[0]) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s", b->name);
            memcpy(m->ubo_name, tmp, sizeof(m->ubo_name));
        }
    }
    /* Stable order by binding number makes the log and the descriptor writes deterministic. */
    for (uint32_t i = 0; i + 1 < m->nbind; ++i) {
        for (uint32_t j = i + 1; j < m->nbind; ++j) {
            if (m->binds[j].binding < m->binds[i].binding) {
                Binding t = m->binds[i];
                m->binds[i] = m->binds[j];
                m->binds[j] = t;
            }
        }
    }
    if (!m->entry[0]) return -3;
    return 0;
}

/* ------------------------------------------------------------------ captured tables ---------- */

typedef struct {
    int index;
    char stage[16];
    char entry[64];
    uint32_t gx, gy, gz;
    int substituted;
    uint64_t tensor_offset;
    uint64_t constants_offset;
    int tensor_table;
} Row;

static Row g_rows[MAX_ROWS];
static int g_row_count;

static void parse_passes_tsv(const char *path) {
    size_t size;
    char *text = (char *)read_file(path, &size);
    char *save = NULL;
    int line_no = 0;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        if (line_no == 1) continue; /* header */
        if (!line[0]) continue;
        if (g_row_count >= MAX_ROWS) die("%s: too many rows", path);
        Row *r = &g_rows[g_row_count];
        memset(r, 0, sizeof(*r));
        char *fields[12] = {0};
        int nf = 0;
        char *p = line;
        while (nf < 12) {
            fields[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (nf < 10) die("%s:%d: expected 11 columns, got %d", path, line_no, nf);
        r->index = atoi(fields[0]);
        snprintf(r->stage, sizeof(r->stage), "%s", fields[1]);
        snprintf(r->entry, sizeof(r->entry), "%s", fields[2]);
        {
            unsigned x = 0, y = 0, z = 0;
            if (sscanf(fields[3], "%ux%ux%u", &x, &y, &z) != 3)
                die("%s:%d: bad groups '%s'", path, line_no, fields[3]);
            r->gx = x;
            r->gy = y;
            r->gz = z;
        }
        r->substituted = atoi(fields[4]);
        r->tensor_offset = strtoull(fields[7], NULL, 10);
        r->constants_offset = strtoull(fields[8], NULL, 10);
        r->tensor_table = atoi(fields[9]);
        g_row_count++;
    }
    free(text);
    if (g_row_count != 196) die("%s: expected 5 prepare + 191 framegen rows, got %d", path, g_row_count);
}

static uint32_t g_tensor[92][4];

static void parse_tensor_tsv(const char *path) {
    size_t size;
    char *text = (char *)read_file(path, &size);
    char *save = NULL;
    int line_no = 0, n = 0;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        if (line_no == 1) continue;
        if (!line[0]) continue;
        unsigned idx, x, y, z, w;
        if (sscanf(line, "%u\t%u\t%u\t%u\t%u", &idx, &x, &y, &z, &w) != 5)
            die("%s:%d: bad tensor row", path, line_no);
        if (idx >= 92 || (int)idx != n) die("%s:%d: tensor index %u out of order", path, line_no, idx);
        g_tensor[idx][0] = x;
        g_tensor[idx][1] = y;
        g_tensor[idx][2] = z;
        g_tensor[idx][3] = w;
        n++;
    }
    free(text);
    if (n != 92) die("%s: expected 92 entries, got %d", path, n);
}

/* ------------------------------------------------------------------ Vulkan state -------------- */

static VkInstance g_instance;
static VkPhysicalDevice g_phys;
static VkDevice g_device;
static VkQueue g_queue;
static uint32_t g_queue_family;
static VkCommandPool g_cmd_pool;
static VkCommandBuffer g_cmd;
static VkDebugUtilsMessengerEXT g_messenger;
static int g_validation_errors;
static int g_validation_warnings;
static int g_validation_cm_fconvert;
static int g_validation_int_fetch;
static int g_validation_cm2_spirv;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                     void *user) {
    (void)types;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        /* 90 of AMD's 416 modules use OpFConvert on cooperative-matrix types, which spirv-val
         * rejects with "Expected Use of Matrix type and Result Type to be identical" (recorded in
         * docs/FSR4-MLFI.md §2).  RADV accepts and runs them.  They are counted separately so a
         * real validation error is never hidden. */
        if (strstr(data->pMessage, "SPV_NV_cooperative_matrix2 was declared") ||
            strstr(data->pMessage, "CooperativeMatrixConversionsNV was declared") ||
            strstr(data->pMessage, "CooperativeMatrixPerElementOperationsNV was declared") ||
            strstr(data->pMessage, "CooperativeMatrixReductionsNV was declared")) {
            /* The modules declare the SPIR-V extension SPV_NV_cooperative_matrix2; this driver
             * provides the same features through VK_EXT_cooperative_matrix_maintenance1 (the
             * renamed extension), which the layer does not accept for the NV extension name. */
            g_validation_cm2_spirv++;
            return VK_FALSE;
        }
        if (strstr(data->pMessage, "VUID-vkCmdDispatch-magFilter-04553")) {
            /* The layer pairs every sampler in the set with every sampled image, but the packed
             * splat images are read with OpImageFetch (no sampler is consumed).  Integer formats
             * cannot support linear filtering, so this fires for every dispatch of pass152/190. */
            g_validation_int_fetch++;
            return VK_FALSE;
        }
        if (strstr(data->pMessage, "Expected Use of Matrix type and Result Type to be identical")) {
            g_validation_cm_fconvert++;
            if (g_verbose && g_validation_cm_fconvert <= 3)
                fprintf(stderr, "[validation] known cooperative-matrix OpFConvert rule: %s\n",
                        data->pMessageIdName ? data->pMessageIdName : "?");
            return VK_FALSE;
        }
        g_validation_errors++;
        fprintf(stderr, "[validation] ERROR: %s\n", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        g_validation_warnings++;
        if (g_verbose) fprintf(stderr, "[validation] warning: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static int has_instance_layer(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS) return 0;
    VkLayerProperties *props = xcalloc(count, sizeof(*props));
    vkEnumerateInstanceLayerProperties(&count, props);
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(props[i].layerName, name)) found = 1;
    free(props);
    return found;
}

static int has_device_extension(const char *name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &count, NULL);
    VkExtensionProperties *props = xcalloc(count, sizeof(*props));
    vkEnumerateDeviceExtensionProperties(g_phys, NULL, &count, props);
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (g_verbose) logmsg("  device extension %s", props[i].extensionName);
        if (!strcmp(props[i].extensionName, name)) found = 1;
    }
    free(props);
    return found;
}

static void init_instance(int enable_validation) {
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mlfi-replay";
    app.apiVersion = API_VERSION;
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;

    const char *layers[1];
    const char *extensions[2];
    uint32_t nlayers = 0, next = 0;
    if (enable_validation && has_instance_layer("VK_LAYER_KHRONOS_validation")) {
        layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
        extensions[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    } else {
        logmsg("validation layer not available; continuing without it");
    }
    ci.enabledLayerCount = nlayers;
    ci.ppEnabledLayerNames = layers;
    ci.enabledExtensionCount = next;
    ci.ppEnabledExtensionNames = extensions;
    if (vkCreateInstance(&ci, NULL, &g_instance) != VK_SUCCESS) die("vkCreateInstance failed");

    if (next) {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkCreateDebugUtilsMessengerEXT");
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = debug_callback;
            if (create(g_instance, &mci, NULL, &g_messenger) != VK_SUCCESS) g_messenger = VK_NULL_HANDLE;
        }
    }
}

static void init_device(void) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(g_instance, &count, NULL) != VK_SUCCESS || !count)
        die("no Vulkan physical device");
    VkPhysicalDevice *devices = xcalloc(count, sizeof(*devices));
    vkEnumeratePhysicalDevices(g_instance, &count, devices);
    g_phys = devices[0];
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_phys = devices[i]; break; }
    }
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    logmsg("device: %s (api %u.%u.%u, driver %u)", props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
    if (props.apiVersion < API_VERSION) die("device does not support Vulkan 1.3");

    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT cm2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    /* 8-bit storage and the Vulkan memory model are members of VkPhysicalDeviceVulkan12Features;
     * chaining their promoted structs as well is a spec violation, so only the 1.2 struct is used.
     * Variable pointers and 16-bit storage are Vulkan 1.1 features, safe to chain separately. */
    VkPhysicalDeviceVariablePointersFeatures varptr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDevice16BitStorageFeatures st16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

    void **tail = &f12.pNext;
    *tail = &cm; tail = &cm.pNext;
    *tail = &cm2; tail = &cm2.pNext;
    *tail = &f8; tail = &f8.pNext;
    *tail = &img64; tail = &img64.pNext;
    *tail = &varptr; tail = &varptr.pNext;
    *tail = &st16; tail = &st16.pNext;
    *tail = &f13;
    VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(g_phys, &query);

    VkPhysicalDeviceFeatures core;
    vkGetPhysicalDeviceFeatures(g_phys, &core);
    if (!core.shaderInt64) die("device lacks shaderInt64 (the model passes use 64-bit integers)");
    if (!f12.shaderFloat16) die("device lacks shaderFloat16");
    if (!f12.shaderInt8) die("device lacks shaderInt8");
    if (!cm.cooperativeMatrix) die("device lacks VK_KHR_cooperative_matrix");
    if (!cm2.cooperativeMatrixConversions || !cm2.cooperativeMatrixPerElementOperations ||
        !cm2.cooperativeMatrixReductions)
        die("device lacks the cooperative-matrix maintenance1 features the modules use");
    if (!f8.shaderFloat8CooperativeMatrix) die("device lacks float8 cooperative matrices");
    if (!f12.vulkanMemoryModel) die("device lacks the Vulkan memory model");
    if (!f13.subgroupSizeControl)
        die("device lacks subgroupSizeControl (the cooperative-matrix modules need wave32)");

    int has_cm = has_device_extension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
    /* RADV advertises the cooperative-matrix2 features through the EXT maintenance1 extension
     * (the renamed VK_NV_cooperative_matrix2); accept either spelling. */
    int has_cm2 = has_device_extension(VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME);
    int has_cm1 = has_device_extension(VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME);
    int has_f8 = has_device_extension(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    int has_i64a = has_device_extension(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
    if (!has_cm || !(has_cm2 || has_cm1) || !has_f8)
        die("device is missing a required extension (cooperative_matrix=%d cm2=%d maintenance1=%d float8=%d)",
            has_cm, has_cm2, has_cm1, has_f8);

    f12.vulkanMemoryModelAvailabilityVisibilityChains =
        f12.vulkanMemoryModelAvailabilityVisibilityChains ? VK_TRUE : VK_FALSE;
    f13.subgroupSizeControl = VK_TRUE;
    f13.computeFullSubgroups = VK_FALSE;
    f12.scalarBlockLayout = f12.scalarBlockLayout ? VK_TRUE : VK_FALSE;
    varptr.variablePointers = varptr.variablePointersStorageBuffer;
    if (!varptr.variablePointersStorageBuffer) die("device lacks variablePointersStorageBuffer");
    st16.storageBuffer16BitAccess = st16.storageBuffer16BitAccess ? VK_TRUE : VK_FALSE;
    st16.uniformAndStorageBuffer16BitAccess = st16.uniformAndStorageBuffer16BitAccess ? VK_TRUE : VK_FALSE;
    cm2.cooperativeMatrixProperties2 = VK_FALSE;

    VkPhysicalDeviceFeatures enable = {0};
    enable.shaderInt64 = VK_TRUE;
    enable.shaderStorageImageReadWithoutFormat =
        core.shaderStorageImageReadWithoutFormat ? VK_TRUE : VK_FALSE;
    enable.shaderStorageImageWriteWithoutFormat =
        core.shaderStorageImageWriteWithoutFormat ? VK_TRUE : VK_FALSE;
    if (!enable.shaderStorageImageReadWithoutFormat || !enable.shaderStorageImageWriteWithoutFormat)
        die("device lacks formatless storage-image read/write (pass151/152 need it)");

    float priority = 1.0f;
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &family_count, NULL);
    VkQueueFamilyProperties *family_props = xcalloc(family_count, sizeof(*family_props));
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &family_count, family_props);
    uint32_t compute_family = 0xffffffffu;
    for (uint32_t i = 0; i < family_count; ++i)
        if (family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { compute_family = i; break; }
    free(family_props);
    if (compute_family == 0xffffffffu) die("no compute queue family");
    g_queue_family = compute_family;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char *exts[12];
    uint32_t next = 0;
    exts[next++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    if (has_cm2) exts[next++] = VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME;
    exts[next++] = VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME;
    exts[next++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
    if (has_i64a) exts[next++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;
    if (has_device_extension(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME))
        exts[next++] = VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME;
    if (has_device_extension(VK_KHR_8BIT_STORAGE_EXTENSION_NAME))
        exts[next++] = VK_KHR_8BIT_STORAGE_EXTENSION_NAME;
    if (has_device_extension(VK_KHR_16BIT_STORAGE_EXTENSION_NAME))
        exts[next++] = VK_KHR_16BIT_STORAGE_EXTENSION_NAME;
    if (has_device_extension(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME))
        exts[next++] = VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME;
    if (has_device_extension(VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME))
        exts[next++] = VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME;

    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enable;
    dci.enabledExtensionCount = next;
    dci.ppEnabledExtensionNames = exts;
    dci.pNext = &f12;
    /* Vulkan12Features carries the core 1.2 members; the extension structs above are chained to
     * it, so pNext must start at f12. */
    if (vkCreateDevice(g_phys, &dci, NULL, &g_device) != VK_SUCCESS) die("vkCreateDevice failed");

    vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_queue_family;
    if (vkCreateCommandPool(g_device, &pci, NULL, &g_cmd_pool) != VK_SUCCESS) die("vkCreateCommandPool failed");
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = g_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_device, &cbai, &g_cmd) != VK_SUCCESS) die("vkAllocateCommandBuffers failed");
}

/* ------------------------------------------------------------------ resources ---------------- */

typedef struct {
    const char *name;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkFormat format;
    uint32_t w, h;
    VkImageAspectFlags aspect;
    int depth;
} Image;

static Image g_images[MAX_IMAGES];
static int g_image_count;

static VkBuffer g_model, g_scratch, g_arena, g_staging, g_readback;
static void *g_arena_map;
static VkDeviceMemory g_arena_mem, g_model_mem, g_scratch_mem, g_staging_mem, g_readback_mem;
static VkDeviceSize g_arena_size = 3276800;
static VkDeviceSize g_scratch_size = 58397248;
static VkDeviceSize g_model_size = 10657152;
static VkDeviceSize g_frame_bytes; /* prediction readback size */

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    die("no memory type with flags 0x%x", want);
    return 0;
}

static void create_buffer(const char *name, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags mem_flags, VkBuffer *buffer, VkDeviceMemory *memory,
                          void **map) {
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &ci, NULL, buffer) != VK_SUCCESS) die("vkCreateBuffer %s failed", name);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_device, *buffer, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, mem_flags);
    if (vkAllocateMemory(g_device, &ai, NULL, memory) != VK_SUCCESS) die("vkAllocateMemory %s failed", name);
    if (vkBindBufferMemory(g_device, *buffer, *memory, 0) != VK_SUCCESS) die("vkBindBufferMemory %s failed", name);
    if (map) {
        if (vkMapMemory(g_device, *memory, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS)
            die("vkMapMemory %s failed", name);
        memset(*map, 0, size);
    }
    logmsg("buffer %-10s %9llu bytes", name, (unsigned long long)size);
}

static Image *create_image(const char *name, uint32_t w, uint32_t h, VkFormat format,
                           VkImageUsageFlags usage, VkImageCreateFlags flags, int is_depth) {
    if (g_image_count >= MAX_IMAGES) die("too many images");
    Image *img = &g_images[g_image_count++];
    memset(img, 0, sizeof(*img));
    img->name = name;
    img->format = format;
    img->w = w;
    img->h = h;
    img->depth = is_depth;
    img->aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.flags = flags;
    if (vkCreateImage(g_device, &ci, NULL, &img->image) != VK_SUCCESS)
        die("vkCreateImage %s (%ux%u fmt %d) failed", name, w, h, format);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_device, img->image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_device, &ai, NULL, &img->memory) != VK_SUCCESS)
        die("image memory %s failed", name);
    if (vkBindImageMemory(g_device, img->image, img->memory, 0) != VK_SUCCESS)
        die("bind image %s failed", name);

    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = img->aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_device, &vi, NULL, &img->view) != VK_SUCCESS)
        die("vkCreateImageView %s failed", name);
    logmsg("image  %-10s %4ux%-4u fmt %-2d mem %llu bytes%s", name, w, h, format,
           (unsigned long long)req.size, is_depth ? " (depth)" : "");
    return img;
}

static VkImageView create_view_usage(Image *img, VkFormat format, VkImageUsageFlags usage) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    vu.usage = usage;
    vi.pNext = &vu;
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = img->aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VkImageView view;
    if (vkCreateImageView(g_device, &vi, NULL, &view) != VK_SUCCESS)
        die("vkCreateImageView(%s, fmt %d) failed", img->name, format);
    return view;
}

/* Views of the packed-splat images: an R64_UINT storage view for the preprocess writes and an
 * R32G32_UINT sampled view for the model passes (both are in the 64-bit compatibility class). */
static VkImageView g_splat_storage_view[2];
static VkImageView g_splat_sampled_view[2];

/* ------------------------------------------------------------------ pipelines ----------------- */

static Module g_modules[MAX_MODULES];
static int g_module_count;

static Module *module_by_entry(const char *entry) {
    for (int i = 0; i < g_module_count; ++i)
        if (!strcmp(g_modules[i].entry, entry)) return &g_modules[i];
    return NULL;
}

static Module *module_by_ubo(const char *ubo) {
    for (int i = 0; i < g_module_count; ++i)
        if (!strcmp(g_modules[i].ubo_name, ubo)) return &g_modules[i];
    return NULL;
}

typedef struct {
    Module *module;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkPipeline pipeline;
    VkDescriptorSet set;
} Pipe;

static Pipe g_pipes[256];
static int g_pipe_count;
static int g_bind_counts[512]; /* index by binding number, model passes only */

static VkSampler g_sampler_linear, g_sampler_nearest;

static Pipe *create_pipe(Module *m) {
    Pipe *p = &g_pipes[g_pipe_count++];
    memset(p, 0, sizeof(*p));
    p->module = m;

    VkDescriptorSetLayoutBinding bindings[MAX_BINDINGS];
    for (uint32_t i = 0; i < m->nbind; ++i) {
        bindings[i].binding = m->binds[i].binding;
        bindings[i].descriptorType = m->binds[i].type;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = NULL;
    }
    VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = m->nbind;
    sl.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_device, &sl, NULL, &p->set_layout) != VK_SUCCESS)
        die("%s: vkCreateDescriptorSetLayout failed", m->entry);

    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p->set_layout;
    if (vkCreatePipelineLayout(g_device, &pl, NULL, &p->pipe_layout) != VK_SUCCESS)
        die("%s: vkCreatePipelineLayout failed", m->entry);

    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = m->size;
    sm.pCode = (const uint32_t *)m->code;
    VkShaderModule module;
    if (vkCreateShaderModule(g_device, &sm, NULL, &module) != VK_SUCCESS)
        die("%s: vkCreateShaderModule failed", m->entry);

    /* The 78 cooperative-matrix modules use LocalSize 32x1x1, which is only valid with a wave32
     * subgroup, so every MLFI pipeline requests a subgroup size of 32.  The Wine capture did not
     * record the pipeline create-info pNext, so this is inferred (see docs/MLFI-REPLAY.md). */
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    subgroup.requiredSubgroupSize = 32;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = m->entry;
    ci.stage.pNext = &subgroup;
    ci.layout = p->pipe_layout;
    VkResult r = vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &ci, NULL, &p->pipeline);
    vkDestroyShaderModule(g_device, module, NULL);
    if (r != VK_SUCCESS) die("%s: vkCreateComputePipelines failed (%d)", m->entry, r);
    if (g_verbose)
        logmsg("pipeline %-20s local %ux%ux%u bindings %u (%s)", m->entry, m->local[0], m->local[1],
               m->local[2], m->nbind, m->path);
    return p;
}

/* ------------------------------------------------------------------ descriptor wiring --------- */

typedef enum { RK_IMAGE, RK_BUFFER } ResKind;

typedef struct {
    const char *name;
    ResKind kind;
    VkImageView sampled_view;
    VkImageView storage_view;
} ResMap;

static ResMap g_resmap[64];
static int g_resmap_count;

static void res_image(const char *name, VkImageView sampled, VkImageView storage) {
    g_resmap[g_resmap_count].name = name;
    g_resmap[g_resmap_count].kind = RK_IMAGE;
    g_resmap[g_resmap_count].sampled_view = sampled;
    g_resmap[g_resmap_count].storage_view = storage;
    g_resmap_count++;
}

static const ResMap *res_find(const char *name) {
    for (int i = 0; i < g_resmap_count; ++i)
        if (!strcmp(g_resmap[i].name, name)) return &g_resmap[i];
    return NULL;
}

/* Per-dispatch row information the descriptor writer needs. */
static const Row *g_row;
static int g_prepare_index; /* -1 for model passes */

static void write_pass_descriptors(Pipe *p) {
    VkDescriptorBufferInfo binfo[MAX_BINDINGS];
    VkDescriptorImageInfo iinfo[MAX_BINDINGS];
    VkWriteDescriptorSet writes[MAX_BINDINGS];
    uint32_t nw = 0;
    memset(binfo, 0, sizeof(binfo));
    memset(iinfo, 0, sizeof(iinfo));

    for (uint32_t i = 0; i < p->module->nbind; ++i) {
        Binding *b = &p->module->binds[i];
        VkWriteDescriptorSet *w = &writes[nw++];
        memset(w, 0, sizeof(*w));
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w->dstSet = p->set;
        w->dstBinding = b->binding;
        w->descriptorCount = 1;
        w->descriptorType = b->type;

        if (b->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            iinfo[i].sampler = strstr(b->name, "nearest") ? g_sampler_nearest : g_sampler_linear;
            w->pImageInfo = &iinfo[i];
            continue;
        }
        if (b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            /* The preprocess splat pass writes one packed image per dispatch: row 1 and 4 write
             * splatted0to05, row 3 writes splatted1to05 (see docs/MLFI-REPLAY.md). */
            if (!strcmp(b->name, "rw_splatted")) {
                int which = (g_prepare_index == 3) ? 1 : 0;
                iinfo[i].imageView = g_splat_storage_view[which];
                iinfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                w->pImageInfo = &iinfo[i];
                continue;
            }
            const ResMap *r = res_find(b->name);
            if (!r || r->kind != RK_IMAGE)
                die("%s: binding %u (%s) has no image resource", p->module->entry, b->binding, b->name);
            iinfo[i].imageView =
                b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? r->storage_view : r->sampled_view;
            if (!iinfo[i].imageView)
                die("%s: binding %u (%s) has no %s view", p->module->entry, b->binding, b->name,
                    b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "storage" : "sampled");
            iinfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w->pImageInfo = &iinfo[i];
            continue;
        }

        /* Buffers: the names come from the modules, the offsets from the capture. */
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, range = VK_WHOLE_SIZE;
        if (!strcmp(b->name, "InitializerBuffer")) {
            buffer = g_model;
        } else if (!strcmp(b->name, "ScratchBuffer")) {
            buffer = g_scratch;
        } else if (!strcmp(b->name, "CsTensorSizes")) {
            if (g_row->tensor_offset == 0)
                die("%s: binds CsTensorSizes but the capture has no tensor offset", p->module->entry);
            buffer = g_arena;
            offset = g_row->tensor_offset;
            range = 1472;
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            /* CsFrameInterpolationConstants, CsDilate, CsAMDSplatting64Constants,
             * CsComputeFwdMVUsingMatConstants, WatermarkCB.  The block sizes are the captured
             * constants sizes (80 for the model passes, the prepare sizes otherwise). */
            static const VkDeviceSize prepare_sizes[5] = {80, 64, 144, 64, 64};
            if (g_row->constants_offset == 0 && g_prepare_index < 0)
                die("%s: binds %s but the capture has no constants offset", p->module->entry, b->name);
            buffer = g_arena;
            offset = g_row->constants_offset;
            range = g_prepare_index >= 0 ? prepare_sizes[g_prepare_index] : 80;
        } else {
            die("%s: binding %u (%s) is not a known buffer", p->module->entry, b->binding, b->name);
        }
        binfo[i].buffer = buffer;
        binfo[i].offset = offset;
        binfo[i].range = range;
        w->pBufferInfo = &binfo[i];
    }
    vkUpdateDescriptorSets(g_device, nw, writes, 0, NULL);
}

/* ------------------------------------------------------------------ frame recording ----------- */

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                    VkAccessFlags src_access, VkAccessFlags dst_access) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void transition_to_general(VkCommandBuffer cmd, Image *img) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img->image;
    barrier.subresourceRange.aspectMask = img->aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void clear_color(VkCommandBuffer cmd, Image *img, VkClearColorValue value) {
    VkImageSubresourceRange range = {img->aspect, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

static void clear_depth(VkCommandBuffer cmd, Image *img, float depth) {
    VkClearDepthStencilValue value = {depth, 0};
    VkImageSubresourceRange range = {img->aspect, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

/* Synthetic input: a moving gradient and bar, 8-bit RGBA. */
static void fill_pattern(uint8_t *pixels, uint32_t w, uint32_t h, int phase) {
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *p = pixels + ((size_t)y * w + x) * 4;
            int r = 40 + (int)(170.0 * x / (w - 1));
            int g = 40 + (int)(150.0 * y / (h - 1));
            int b = 60 + (int)(90.0 * (0.5 + 0.5 * sin((x + phase * 11.0) * 0.06)));
            int bar = (int)((phase * 37) % w);
            int dx = (int)x - bar;
            if (dx > -1 && dx < 56) { r = 235; g = 90; b = 60; }
            if (y > (uint32_t)(h * 3 / 4)) { r = (r + 30) / 2; g = (g + 30) / 2; b = 210; }
            p[0] = (uint8_t)(r > 255 ? 255 : r);
            p[1] = (uint8_t)(g > 255 ? 255 : g);
            p[2] = (uint8_t)(b > 255 ? 255 : b);
            p[3] = 255;
        }
    }
}

static uint16_t float_to_half(float value) {
    union { float f; uint32_t u; } bits = {value};
    uint32_t sign = (bits.u >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((bits.u >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits.u & 0x7fffffu;
    if (((bits.u >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0));
    if (exponent <= 0) {
        if (exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exponent);
        uint32_t half_mantissa = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1) half_mantissa++;
        return (uint16_t)(sign | half_mantissa);
    }
    if (exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u) half++; /* round to nearest */
    return (uint16_t)half;
}

/* Fills an R16G16_SFLOAT image with a constant motion vector (UV space). */
static void fill_motion(uint8_t *pixels, uint32_t w, uint32_t h, float vx, float vy) {
    uint16_t hx = float_to_half(vx), hy = float_to_half(vy);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *p = pixels + ((size_t)y * w + x) * 4;
            memcpy(p + 0, &hx, 2);
            memcpy(p + 2, &hy, 2);
        }
    }
}

static void copy_rgb(VkCommandBuffer cmd, Image *dst, VkBuffer staging, uint32_t w, uint32_t h) {
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = w;
    region.imageExtent.height = h;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

/* ------------------------------------------------------------------ PNG writer ---------------- */

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc32_bytes(const uint8_t *data, size_t size) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) c = crc_table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t size) {
    uint8_t header[8], crc_buf[4];
    be32(header, (uint32_t)size);
    memcpy(header + 4, type, 4);
    fwrite(header, 1, 8, f);
    if (size) fwrite(data, 1, size, f);
    uint8_t *all = xmalloc(size + 4);
    memcpy(all, type, 4);
    if (size) memcpy(all + 4, data, size);
    be32(crc_buf, crc32_bytes(all, size + 4));
    fwrite(crc_buf, 1, 4, f);
    free(all);
}

static void write_png_rgba8(const char *path, const uint8_t *pixels, uint32_t w, uint32_t h) {
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    fwrite(signature, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, sizeof(ihdr));

    size_t stride = (size_t)w * 4 + 1;
    size_t raw_size = stride * h;
    uint8_t *raw = xmalloc(raw_size);
    for (uint32_t y = 0; y < h; ++y) {
        raw[y * stride] = 0;
        memcpy(raw + y * stride + 1, pixels + (size_t)y * w * 4, (size_t)w * 4);
    }
    /* zlib stream with stored (uncompressed) deflate blocks. */
    size_t zcap = raw_size + raw_size / 65535 * 5 + 64;
    uint8_t *z = xmalloc(zcap);
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    size_t pos = 0;
    while (pos < raw_size) {
        size_t block = raw_size - pos;
        if (block > 65535) block = 65535;
        z[zn++] = (pos + block == raw_size) ? 1 : 0;
        z[zn++] = (uint8_t)(block & 0xff);
        z[zn++] = (uint8_t)(block >> 8);
        z[zn++] = (uint8_t)(~block & 0xff);
        z[zn++] = (uint8_t)((~block >> 8) & 0xff);
        memcpy(z + zn, raw + pos, block);
        zn += block;
        pos += block;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_size; ++i) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    z[zn++] = (uint8_t)(b >> 8);
    z[zn++] = (uint8_t)b;
    z[zn++] = (uint8_t)(a >> 8);
    z[zn++] = (uint8_t)a;
    png_chunk(f, "IDAT", z, zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(raw);
}

static uint8_t to_u8(float v, float exposure) {
    float x = v * exposure;
    if (!(x > 0.0f)) x = 0.0f; /* NaN -> 0 */
    if (x > 1.0f) x = 1.0f;
    return (uint8_t)(powf(x, 1.0f / 2.2f) * 255.0f + 0.5f);
}

/* ------------------------------------------------------------------ stats -------------------- */

typedef struct {
    uint64_t hash;
    uint64_t nonzero;
    uint64_t total;
    float minv, maxv, mean;
    float stddev;
    double mad_rgb0, mad_rgb1, mad_blend;
} Stats;

static Stats analyze_prediction(const float *rgba, uint32_t w, uint32_t h, const uint8_t *rgb0,
                                const uint8_t *rgb1) {
    Stats s;
    memset(&s, 0, sizeof(s));
    s.total = (uint64_t)w * h;
    s.minv = 1e30f;
    s.maxv = -1e30f;
    double sum = 0, sum2 = 0;
    double d0 = 0, d1 = 0, db = 0;
    uint64_t lum_samples = 0;
    for (uint64_t i = 0; i < s.total; ++i) {
        const float *p = rgba + i * 4;
        if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) s.nonzero++;
        for (int c = 0; c < 3; ++c) {
            float v = p[c];
            if (isnan(v) || isinf(v)) continue;
            if (v < s.minv) s.minv = v;
            if (v > s.maxv) s.maxv = v;
            sum += v;
            sum2 += (double)v * v;
        }
        /* Preview-space differences: the model works in its own colour space, so these are only
         * a rough "is it the input / a plain blend?" indicator, not an image-quality measure. */
        float o0 = to_u8(p[0], 1.0f) / 255.0f;
        float o1 = to_u8(p[1], 1.0f) / 255.0f;
        float o2 = to_u8(p[2], 1.0f) / 255.0f;
        float a0 = rgb0[i * 4 + 0] / 255.0f, a1 = rgb0[i * 4 + 1] / 255.0f, a2 = rgb0[i * 4 + 2] / 255.0f;
        float b0 = rgb1[i * 4 + 0] / 255.0f, b1 = rgb1[i * 4 + 1] / 255.0f, b2 = rgb1[i * 4 + 2] / 255.0f;
        d0 += fabs(o0 - a0) + fabs(o1 - a1) + fabs(o2 - a2);
        d1 += fabs(o0 - b0) + fabs(o1 - b1) + fabs(o2 - b2);
        db += fabs(o0 - 0.5f * (a0 + b0)) + fabs(o1 - 0.5f * (a1 + b1)) +
              fabs(o2 - 0.5f * (a2 + b2));
        lum_samples += 3;
    }
    s.mean = (float)(sum / (double)(s.total * 3));
    double var = sum2 / (double)(s.total * 3) - (double)s.mean * s.mean;
    s.stddev = (float)sqrt(var > 0 ? var : 0);
    if (s.minv > s.maxv) { s.minv = s.maxv = 0; }
    if (lum_samples) {
        s.mad_rgb0 = d0 / (double)lum_samples;
        s.mad_rgb1 = d1 / (double)lum_samples;
        s.mad_blend = db / (double)lum_samples;
    }
    s.hash = fnv1a64(rgba, (size_t)s.total * 16);
    return s;
}

/* ------------------------------------------------------------------ main --------------------- */

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --modules DIR --model PATH [options]\n"
            "  --passes PATH     captured pass table (default tools/mlfi-capture/derived/passes_mlfi_4k_1280x720.tsv)\n"
            "  --tensors PATH    captured tensor table (default .../tensor_4k_1280x720.tsv)\n"
            "  --capture DIR     capture directory with raw/*const* (for the preprocess constants)\n"
            "  --out DIR         output directory for PNG/raw dumps (default .)\n"
            "  --frames N        frames to run (default 2)\n"
            "  --width W --height H   render size (default 1280x720)\n"
            "  --no-validation   do not enable VK_LAYER_KHRONOS_validation\n"
            "  --static-input    use the same input pattern for every frame (determinism check)\n"
            "  --motion PX       constant horizontal motion vector in pixels per frame (default 0)\n"
            "  --verbose         print per-pipeline and per-image detail\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *modules_dir = NULL, *model_path = NULL, *capture_dir = NULL, *out_dir = ".";
    const char *passes_path = "tools/mlfi-capture/derived/passes_mlfi_4k_1280x720.tsv";
    const char *tensors_path = "tools/mlfi-capture/derived/tensor_4k_1280x720.tsv";
    int frames = 2, width = 1280, height = 720, validation = 1, static_input = 0;
    float motion = 0.0f;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--modules") && v) { modules_dir = v; i++; }
        else if (!strcmp(a, "--model") && v) { model_path = v; i++; }
        else if (!strcmp(a, "--capture") && v) { capture_dir = v; i++; }
        else if (!strcmp(a, "--out") && v) { out_dir = v; i++; }
        else if (!strcmp(a, "--passes") && v) { passes_path = v; i++; }
        else if (!strcmp(a, "--tensors") && v) { tensors_path = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = atoi(v); i++; }
        else if (!strcmp(a, "--width") && v) { width = atoi(v); i++; }
        else if (!strcmp(a, "--height") && v) { height = atoi(v); i++; }
        else if (!strcmp(a, "--verbose")) { g_verbose = 1; }
        else if (!strcmp(a, "--no-validation")) { validation = 0; }
        else if (!strcmp(a, "--static-input")) { static_input = 1; }
        else if (!strcmp(a, "--motion") && v) { motion = (float)atof(v); i++; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else { usage(argv[0]); die("unknown argument %s", a); }
    }
    if (!modules_dir || !model_path) { usage(argv[0]); die("--modules and --model are required"); }
    if (frames < 1) die("--frames must be >= 1");
    if (width != 1280 || height != 720)
        die("this milestone replays the captured 1280x720 layout; got %dx%d", width, height);

    g_scan = xcalloc(1, sizeof(*g_scan));
    parse_passes_tsv(passes_path);
    parse_tensor_tsv(tensors_path);

    /* Load every .spv in the directory, classify it by entry point (and, for "main", by its
     * uniform block name). */
    {
        DIR *dir = opendir(modules_dir);
        if (!dir) die("cannot open %s: %s", modules_dir, strerror(errno));
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            const char *name = entry->d_name;
            size_t ll = strlen(name);
            if (ll < 5 || strcmp(name + ll - 4, ".spv")) continue;
            if (g_module_count >= MAX_MODULES) die("too many modules in %s", modules_dir);
            Module *m = &g_modules[g_module_count];
            memset(m, 0, sizeof(*m));
            size_t dl = strlen(modules_dir);
            if (dl + ll + 2 > sizeof(m->path)) die("module path too long: %s/%s", modules_dir, name);
            memcpy(m->path, modules_dir, dl);
            m->path[dl] = '/';
            memcpy(m->path + dl + 1, name, ll + 1);
            m->code = read_file(m->path, &m->size);
            int rc = spv_scan(m->code, m->size, m);
            if (rc != 0) die("%s: SPIR-V parse failed (%d); expected a legalised GLCompute module",
                             m->path, rc);
            g_module_count++;
        }
        closedir(dir);
    }
    if (!g_module_count) die("no .spv modules in %s", modules_dir);
    {
        int mains = 0;
        for (int i = 0; i < g_module_count; ++i) {
            Module *m = &g_modules[i];
            if (!strcmp(m->entry, "main")) {
                mains++;
                if (g_verbose) logmsg("preprocess module %-24s ubo=%s", m->path, m->ubo_name);
            }
        }
        logmsg("loaded %d modules (%d preprocess 'main')", g_module_count, mains);
    }

    /* Model bank: the exact uploaded range model_blob[72 : 72+10,657,152]. */
    {
        size_t model_total;
        uint8_t *blob = read_file(model_path, &model_total);
        const size_t bank_offset = 72;
        const size_t upload_size = 10657152;
        if (model_total < bank_offset + upload_size)
            die("%s is too small for model_blob[%zu : %zu]", model_path, bank_offset,
                bank_offset + upload_size);
        uint64_t h = fnv1a64(blob + bank_offset, upload_size);
        logmsg("model   upload model_blob[%zu : %zu] fnv1a=%016llx%s", bank_offset,
               bank_offset + upload_size, (unsigned long long)h,
               h == 0xb88e755565aa4249ull ? " (matches capture)" : " (DIFFERS from capture b88e755565aa4249)");
        if (h != 0xb88e755565aa4249ull)
            die("model upload does not match the captured range; refusing to run");
        free(blob);
    }

    /* Which module is dispatched by each of the five prepare rows.  MLFI-CAPTURE.md §5 pins the
     * order 9008, 4204, 5004, 4204, 4204; the 8,980-byte module is created but not dispatched. */
    Module *dilate = module_by_ubo("CsDilate");
    Module *splat_module = module_by_ubo("CsAMDSplatting64Constants");
    Module *fwdmv = module_by_ubo("CsComputeFwdMVUsingMatConstants");
    /* The fourth preprocess module (8,980 bytes) is created by the context but not dispatched in
     * the no-swapchain path, so identify it by elimination (it is the only remaining "main"). */
    Module *watermark = NULL;
    for (int i = 0; i < g_module_count; ++i) {
        Module *m = &g_modules[i];
        if (strcmp(m->entry, "main")) continue;
        if (m == dilate || m == splat_module || m == fwdmv) continue;
        if (watermark) die("more than one undispatched preprocess module");
        watermark = m;
    }
    if (!dilate || !splat_module || !fwdmv || !watermark)
        die("could not identify the four preprocess modules");

    init_instance(validation);
    init_device();

    /* Buffers. */
    create_buffer("model", g_model_size,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_model, &g_model_mem, NULL);
    create_buffer("scratch", g_scratch_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_scratch, &g_scratch_mem, NULL);
    create_buffer("arena", g_arena_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &g_arena, &g_arena_mem, &g_arena_map);
    create_buffer("staging", (VkDeviceSize)width * height * 4,
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &g_staging, &g_staging_mem, NULL);
    g_frame_bytes = (VkDeviceSize)width * height * 16;
    create_buffer("readback", g_frame_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &g_readback, &g_readback_mem, NULL);

    /* Images.  Sizes are inferred from the modules and documented in docs/MLFI-REPLAY.md. */
    const uint32_t sw = (uint32_t)(width / 3);           /* 426 at 1280, the captured constant */
    const uint32_t sh = (uint32_t)(height / 3);          /* 240 at 720 */
    const uint32_t fw = (uint32_t)(width / 8);           /* 160 */
    const uint32_t fh = (uint32_t)(height / 8);          /* 90 */

    Image *rgb0 = create_image("rgb0", width, height, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    Image *rgb1 = create_image("rgb1", width, height, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    Image *depth = create_image("depth", width, height, VK_FORMAT_D32_SFLOAT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, 1);
    Image *mv = create_image("mv", width, height, VK_FORMAT_R16G16_SFLOAT,
                             VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    Image *distortion = create_image("distortion", width, height, VK_FORMAT_R16G16_SFLOAT,
                                     VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    Image *dilated = create_image("dilated_mv", sw, sh, VK_FORMAT_R16G16_SFLOAT,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    Image *dsdepth = create_image("downscaled_depth", sw, sh, VK_FORMAT_R32_SFLOAT,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    Image *fwdmv_img = create_image("fwd_mv", sw, sh, VK_FORMAT_R16G16_SFLOAT,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    Image *splat_img[2];
    splat_img[0] = create_image("splat0to05", sw, sh, VK_FORMAT_R32G32_UINT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, 0);
    splat_img[1] = create_image("splat1to05", sw, sh, VK_FORMAT_R32G32_UINT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, 0);
    Image *flow0 = create_image("flow0to05", fw, fh, VK_FORMAT_R16G16_SFLOAT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    Image *flow1 = create_image("flow1to05", fw, fh, VK_FORMAT_R16G16_SFLOAT,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    Image *hudless = create_image("hudless_ui", width, height, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    Image *prediction = create_image("prediction", width, height, VK_FORMAT_R32G32B32A32_SFLOAT,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 0);

    g_splat_storage_view[0] = create_view_usage(splat_img[0], VK_FORMAT_R64_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
    g_splat_storage_view[1] = create_view_usage(splat_img[1], VK_FORMAT_R64_UINT, VK_IMAGE_USAGE_STORAGE_BIT);
    g_splat_sampled_view[0] = splat_img[0]->view;
    g_splat_sampled_view[1] = splat_img[1]->view;
    res_image("r_input_rgb0", rgb0->view, NULL);
    res_image("r_input_rgb1", rgb1->view, NULL);
    res_image("r_input_mv", mv->view, NULL);
    res_image("r_input_depth", depth->view, NULL);
    res_image("r_distortion_field", distortion->view, NULL);
    res_image("rw_output_dilated_mv", dilated->view, dilated->view);
    res_image("rw_output_downscaled_depth", dsdepth->view, dsdepth->view);
    res_image("r_input_flow", dilated->view, NULL);
    res_image("r_depth", dsdepth->view, NULL);
    res_image("rw_splatted", g_splat_sampled_view[0], g_splat_storage_view[0]);
    res_image("r_input_mv0", dilated->view, NULL);
    res_image("r_input_depth0", dsdepth->view, NULL);
    res_image("r_packed_splatted1to0", NULL, g_splat_storage_view[1]);
    res_image("rw_output_mv_0_to_1", fwdmv_img->view, fwdmv_img->view);
    res_image("rw_output_flow_0to05", flow0->view, flow0->view);
    res_image("rw_output_flow_1to05", flow1->view, flow1->view);
    res_image("r_input_packed_splatted0to05", g_splat_sampled_view[0], NULL);
    res_image("r_input_packed_splatted1to05", g_splat_sampled_view[1], NULL);
    res_image("r_hudless_ui", hudless->view, NULL);
    res_image("rw_prediction", NULL, prediction->view);
    res_image("rw_target", NULL, prediction->view);

    VkSamplerCreateInfo smci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smci.magFilter = VK_FILTER_LINEAR;
    smci.minFilter = VK_FILTER_LINEAR;
    smci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(g_device, &smci, NULL, &g_sampler_linear) != VK_SUCCESS) die("sampler linear");
    smci.magFilter = smci.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(g_device, &smci, NULL, &g_sampler_nearest) != VK_SUCCESS) die("sampler nearest");

    /* Pipelines: the 191 model passes in pass order, then the four preprocess modules in the
     * order the capture created them (9008, 4204, 5004, 8980). */
    const int model_rows = g_row_count - 5;
    for (int i = 5; i < g_row_count; ++i) {
        Module *m = module_by_entry(g_rows[i].entry);
        if (!m) die("no module for %s in %s", g_rows[i].entry, modules_dir);
        for (uint32_t b = 0; b < m->nbind; ++b)
            if (m->binds[b].binding < 512) g_bind_counts[m->binds[b].binding]++;
        create_pipe(m);
    }
    Pipe *prep_pipe[4];
    prep_pipe[0] = create_pipe(dilate);
    prep_pipe[1] = create_pipe(splat_module);
    prep_pipe[2] = create_pipe(fwdmv);
    prep_pipe[3] = create_pipe(watermark);
    if (g_pipe_count != 195) die("expected 195 pipelines, created %d", g_pipe_count);
    /* The captured descriptor counts for one 1280x720 frame (MLFI-CAPTURE.md §7). */
    if (g_bind_counts[0] != 78 || g_bind_counts[128] != 175 || g_bind_counts[256] != 3 ||
        g_bind_counts[257] != 175)
        die("binding counts differ from the capture: model0=%d scratch=%d constants=%d tensors=%d "
            "(expected 78/175/3/175)", g_bind_counts[0], g_bind_counts[128], g_bind_counts[256],
            g_bind_counts[257]);
    logmsg("bindings match capture: model0=78 scratch=175 constants=3 tensors=175 sampler=%d",
           g_bind_counts[320]);
    logmsg("pipelines: 195/195 created (%d model + 4 preprocess)", model_rows);

    /* Descriptor pool and sets.  One set per dispatch (196), so the captured per-dispatch buffer
     * offsets can be baked in once. */
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
    };
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 256;
    dpi.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
    dpi.pPoolSizes = pool_sizes;
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(g_device, &dpi, NULL, &pool) != VK_SUCCESS) die("descriptor pool");

    /* One Pipe per prepare dispatch: rows 1, 3 and 4 share the splat module and its pipeline, but
     * the capture gives each dispatch its own descriptor set (the write target differs). */
    static Pipe prep_dispatch[5];
    for (int i = 0; i < 5; ++i) {
        Pipe *src = prep_pipe[i == 0 ? 0 : (i == 2 ? 2 : 1)];
        prep_dispatch[i] = *src;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &src->set_layout;
        if (vkAllocateDescriptorSets(g_device, &ai, &prep_dispatch[i].set) != VK_SUCCESS)
            die("prepare descriptor set %d", i);
        g_row = &g_rows[i];
        g_prepare_index = i;
        write_pass_descriptors(&prep_dispatch[i]);
    }
    for (int i = 5; i < g_row_count; ++i) {
        Pipe *p = &g_pipes[i - 5];
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p->set_layout;
        if (vkAllocateDescriptorSets(g_device, &ai, &p->set) != VK_SUCCESS)
            die("%s: descriptor set", p->module->entry);
        g_row = &g_rows[i];
        g_prepare_index = -1;
        write_pass_descriptors(p);
    }
    logmsg("descriptor sets: 196 written");

    /* Uniform arena: prepare constants, tensor table and the 80-byte model constants. */
    memset(g_arena_map, 0, (size_t)g_arena_size);
    {
        uint8_t *arena = g_arena_map;
        /* Captured prepare constants (exact host bytes for the canonical capture). */
        int found_prepare = 0;
        if (capture_dir) {
            /* The canonical capture produced four *_const_*_main.bin files in dispatch order
             * (seq numbers 429, 434, 439, 444, 449).  Find them by sequence number. */
            static const long seqs[5] = {429, 434, 439, 444, 449};
            static const size_t sizes[5] = {80, 64, 144, 64, 64};
            static const uint64_t expected[5] = {0x6b02f3fb627dbb4eull, 0x496f339a14f42d2eull,
                                                 0xf81c47ef5798a58full, 0xe653ece3e611725full,
                                                 0x6d80a9983c9729aeull};
            for (int i = 0; i < 5; ++i) {
                char path[8192];
                snprintf(path, sizeof(path), "%s/raw/%06ld_const_%06ld_main.bin", capture_dir, seqs[i], seqs[i]);
                size_t size;
                uint8_t *data = NULL;
                FILE *f = fopen(path, "rb");
                if (f) {
                    fclose(f);
                    data = read_file(path, &size);
                    if (size != sizes[i]) die("%s: expected %zu bytes, got %zu", path, sizes[i], size);
                    uint64_t h = fnv1a64(data, size);
                    if (h != expected[i])
                        logmsg("warning: prepare block %d fnv1a=%016llx, capture had %016llx",
                               i, (unsigned long long)h, (unsigned long long)expected[i]);
                    else if (g_verbose)
                        logmsg("prepare block %d fnv1a=%016llx (matches capture)", i,
                               (unsigned long long)h);
                    memcpy(arena + g_rows[i].constants_offset, data, size);
                    free(data);
                    found_prepare++;
                }
            }
        }
        if (found_prepare != 5) {
            if (found_prepare)
                die("only %d/5 prepare constant blocks found under %s/raw", found_prepare, capture_dir);
            logmsg("prepare constants: no capture directory; synthesising them");
            /* CsDilate: 80 bytes (render/display size, dilated size 426x240, scale 1). */
            uint32_t *d = (uint32_t *)(arena + g_rows[0].constants_offset);
            d[0] = 0; d[1] = 0;
            d[2] = (uint32_t)width; d[3] = (uint32_t)height;
            d[4] = (uint32_t)width; d[5] = (uint32_t)height;
            d[6] = sw; d[7] = sh;
            ((float *)d)[8] = 1.0f; ((float *)d)[9] = 1.0f;
            ((float *)d)[12] = 1.0f; ((float *)d)[13] = 1.0f;
            ((float *)d)[18] = 1.0f; ((float *)d)[19] = 1.0f;
            for (int i = 1; i < 5; ++i) {
                uint32_t *s = (uint32_t *)(arena + g_rows[i].constants_offset);
                s[0] = sw; s[1] = sh;
                s[2] = (uint32_t)width; s[3] = (uint32_t)height;
                ((float *)s)[4] = (i == 1) ? 1.0f : 0.5f;
                s[5] = (i == 3) ? 1 : 0;
                ((float *)s)[8] = 1.0f; ((float *)s)[9] = 1.0f;
            }
            if (g_rows[2].constants_offset + 144 <= g_arena_size) {
                float *m = (float *)(arena + g_rows[2].constants_offset);
                for (int i = 0; i < 8; ++i) m[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                for (int i = 0; i < 8; ++i) m[16 + i] = (i % 5 == 0) ? 1.0f : 0.0f;
                uint32_t *tail = (uint32_t *)(arena + g_rows[2].constants_offset + 136);
                tail[0] = (uint32_t)width;
                tail[1] = (uint32_t)height;
            }
        }
        /* Tensor table at every captured table offset, and the 80-byte model constants. */
        int tables = 0, model_consts = 0;
        for (int i = 5; i < g_row_count; ++i) {
            if (g_rows[i].tensor_offset) {
                memcpy(arena + g_rows[i].tensor_offset, g_tensor, sizeof(g_tensor));
                tables++;
            }
            if (g_rows[i].constants_offset) {
                uint32_t *c = (uint32_t *)(arena + g_rows[i].constants_offset);
                c[0] = (uint32_t)width; c[1] = (uint32_t)height;
                c[2] = (uint32_t)width; c[3] = (uint32_t)height;
                c[4] = 1; /* reset/parity, frame 0 */
                c[9] = 0x447a0000u; /* 1000.0f */
                ((float *)c)[12] = 1.0f; ((float *)c)[13] = 1.0f;
                ((float *)c)[17] = 1.0f; ((float *)c)[18] = 1.0f;
                uint64_t h = fnv1a64(c, 80);
                if (h != 0xdb73867b1faff8b4ull)
                    logmsg("warning: model constants at %llu fnv1a=%016llx, capture had db73867b1faff8b4",
                           (unsigned long long)g_rows[i].constants_offset, (unsigned long long)h);
                model_consts++;
            }
        }
        logmsg("arena: %d tensor tables, %d model constant blocks, prepare constants %s",
               tables, model_consts, capture_dir ? "from capture" : "synthesised");
        if (tables != 175) die("expected 175 tensor-table bindings, wrote %d", tables);
        if (model_consts != 3) die("expected 3 model constant blocks, wrote %d", model_consts);
    }

    /* Initial image state: zero (and 0.5 depth), matching a freshly created provider context. */
    {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) die("begin init cmd");
        for (int i = 0; i < g_image_count; ++i) transition_to_general(g_cmd, &g_images[i]);
        VkClearColorValue zero;
        memset(&zero, 0, sizeof(zero));
        clear_color(g_cmd, rgb0, zero);
        clear_color(g_cmd, rgb1, zero);
        clear_color(g_cmd, mv, zero);
        clear_color(g_cmd, distortion, zero);
        clear_color(g_cmd, dilated, zero);
        clear_color(g_cmd, dsdepth, zero);
        clear_color(g_cmd, fwdmv_img, zero);
        clear_color(g_cmd, splat_img[0], zero);
        clear_color(g_cmd, splat_img[1], zero);
        clear_color(g_cmd, flow0, zero);
        clear_color(g_cmd, flow1, zero);
        clear_color(g_cmd, hudless, zero);
        clear_color(g_cmd, prediction, zero);
        clear_depth(g_cmd, depth, 0.5f);
        barrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) die("end init cmd");
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_cmd;
        if (vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) die("init submit");
        if (vkQueueWaitIdle(g_queue) != VK_SUCCESS) die("init wait");
    }

    /* Upload the model bank once (the capture does this during the first PrepareV2). */
    {
        size_t total;
        uint8_t *blob = read_file(model_path, &total);
        VkBuffer staging_model;
        VkDeviceMemory staging_model_mem;
        void *map;
        create_buffer("model_stage", g_model_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &staging_model, &staging_model_mem, &map);
        memcpy(map, blob + 72, (size_t)g_model_size);
        free(blob);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(g_cmd, &bi);
        VkBufferCopy copy = {0, 0, g_model_size};
        vkCmdCopyBuffer(g_cmd, staging_model, g_model, 1, &copy);
        barrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        vkEndCommandBuffer(g_cmd);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_cmd;
        if (vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) die("model submit");
        if (vkQueueWaitIdle(g_queue) != VK_SUCCESS) die("model wait");
        vkDestroyBuffer(g_device, staging_model, NULL);
        vkFreeMemory(g_device, staging_model_mem, NULL);
        logmsg("model uploaded (10,657,152 bytes at offset 0)");
    }

    /* Run the frames. */
    uint8_t *pattern0 = xmalloc((size_t)width * height * 4);
    uint8_t *pattern1 = xmalloc((size_t)width * height * 4);
    uint8_t *mv_host = xmalloc((size_t)width * height * 4);
    float *prediction_host = xmalloc((size_t)g_frame_bytes);
    uint64_t prev_hash = 0;
    int changed = 0;
    int total_dispatches = 0;

    for (int frame = 0; frame < frames; ++frame) {
        /* Inputs: rgb0 is the previous frame, rgb1 the current one; both move.  The motion
         * vector image is the constant displacement between them (zero by default). */
        int phase = static_input ? 0 : frame * 2;
        fill_pattern(pattern0, (uint32_t)width, (uint32_t)height, phase);
        fill_pattern(pattern1, (uint32_t)width, (uint32_t)height, phase + 1);
        fill_motion(mv_host, (uint32_t)width, (uint32_t)height, motion / (float)width, 0.0f);
        void *staging_map;
        if (vkMapMemory(g_device, g_staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_map) != VK_SUCCESS)
            die("map staging");

        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) die("begin frame cmd");
        memcpy(staging_map, pattern0, (size_t)width * height * 4);
        copy_rgb(g_cmd, rgb0, g_staging, (uint32_t)width, (uint32_t)height);
        if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) die("end rgb0 copy");
        {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g_cmd;
            if (vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) die("rgb0 submit");
            if (vkQueueWaitIdle(g_queue) != VK_SUCCESS) die("rgb0 wait");
        }
        /* The mapping stays valid; use the same staging buffer for rgb1 and the MV image. */
        memcpy(staging_map, pattern1, (size_t)width * height * 4);
        vkBeginCommandBuffer(g_cmd, &bi);
        copy_rgb(g_cmd, rgb1, g_staging, (uint32_t)width, (uint32_t)height);
        if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) die("end rgb1 copy");
        {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g_cmd;
            if (vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) die("rgb1 submit");
            if (vkQueueWaitIdle(g_queue) != VK_SUCCESS) die("rgb1 wait");
        }
        memcpy(staging_map, mv_host, (size_t)width * height * 4);
        vkBeginCommandBuffer(g_cmd, &bi);
        copy_rgb(g_cmd, mv, g_staging, (uint32_t)width, (uint32_t)height);

        /* Per-frame model constants (frame 0 differs only in the reset/parity fields). */
        for (int i = 5; i < g_row_count; ++i) {
            if (!g_rows[i].constants_offset) continue;
            uint32_t *c = (uint32_t *)((uint8_t *)g_arena_map + g_rows[i].constants_offset);
            c[4] = frame == 0 ? 1u : 0u;
            ((float *)c)[16] = frame == 0 ? 0.0f : 1.0f;
            ((float *)c)[17] = frame == 0 ? 1.0f : 0.42f;
            ((float *)c)[18] = frame == 0 ? 1.0f : 0.0f;
        }
        barrier(g_cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT);
        barrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

        /* 5 preprocess dispatches. */
        for (int i = 0; i < 5; ++i) {
            Pipe *p = &prep_dispatch[i];
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe_layout, 0, 1,
                                    &p->set, 0, NULL);
            vkCmdDispatch(g_cmd, g_rows[i].gx, g_rows[i].gy, g_rows[i].gz);
            barrier(g_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            total_dispatches++;
        }
        /* 191 model passes in captured order. */
        for (int i = 5; i < g_row_count; ++i) {
            Pipe *p = &g_pipes[i - 5];
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe_layout, 0, 1,
                                    &p->set, 0, NULL);
            vkCmdDispatch(g_cmd, g_rows[i].gx, g_rows[i].gy, g_rows[i].gz);
            barrier(g_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            total_dispatches++;
        }
        /* Read the generated frame back. */
        VkBufferImageCopy region = {0};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = (uint32_t)width;
        region.imageExtent.height = (uint32_t)height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(g_cmd, prediction->image, VK_IMAGE_LAYOUT_GENERAL, g_readback, 1, &region);
        barrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) die("end frame cmd");
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_cmd;
        VkResult submit = vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
        if (submit != VK_SUCCESS) die("vkQueueSubmit frame %d: %d", frame, submit);
        VkResult wait = vkQueueWaitIdle(g_queue);
        if (wait != VK_SUCCESS) die("vkQueueWaitIdle frame %d: %d (device lost?)", frame, wait);

        void *rb;
        if (vkMapMemory(g_device, g_readback_mem, 0, VK_WHOLE_SIZE, 0, &rb) != VK_SUCCESS)
            die("map readback");
        memcpy(prediction_host, rb, (size_t)g_frame_bytes);
        vkUnmapMemory(g_device, g_readback_mem);
        vkUnmapMemory(g_device, g_staging_mem);

        Stats s = analyze_prediction(prediction_host, (uint32_t)width, (uint32_t)height, pattern0, pattern1);
        if (frame && s.hash != prev_hash) changed = 1;
        prev_hash = s.hash;

        char path[8192];
        uint8_t *preview = xmalloc((size_t)width * height * 4);
        for (uint64_t i = 0; i < (uint64_t)width * height; ++i) {
            preview[i * 4 + 0] = to_u8(prediction_host[i * 4 + 0], 1.0f);
            preview[i * 4 + 1] = to_u8(prediction_host[i * 4 + 1], 1.0f);
            preview[i * 4 + 2] = to_u8(prediction_host[i * 4 + 2], 1.0f);
            preview[i * 4 + 3] = 255;
        }
        snprintf(path, sizeof(path), "%s/prediction_f%d.png", out_dir, frame);
        write_png_rgba8(path, preview, (uint32_t)width, (uint32_t)height);
        snprintf(path, sizeof(path), "%s/prediction_f%d.raw", out_dir, frame);
        write_file(path, prediction_host, (size_t)g_frame_bytes);
        snprintf(path, sizeof(path), "%s/rgb0_f%d.png", out_dir, frame);
        write_png_rgba8(path, pattern0, (uint32_t)width, (uint32_t)height);
        snprintf(path, sizeof(path), "%s/rgb1_f%d.png", out_dir, frame);
        write_png_rgba8(path, pattern1, (uint32_t)width, (uint32_t)height);
        free(preview);

        logmsg("frame %d: hash=%016llx nonzero=%llu/%llu min=%.4f max=%.4f mean=%.5f "
               "stddev=%.4f mad(rgb0=%.4f rgb1=%.4f blend=%.4f)",
               frame, (unsigned long long)s.hash, (unsigned long long)s.nonzero,
               (unsigned long long)s.total, s.minv, s.maxv, s.mean, s.stddev, s.mad_rgb0,
               s.mad_rgb1, s.mad_blend);
        printf("MLFI-REPLAY frame %d hash=%016llx nonzero=%llu/%llu min=%.4f max=%.4f "
               "mean=%.5f stddev=%.4f mad_rgb0=%.4f mad_rgb1=%.4f mad_blend=%.4f\n",
               frame, (unsigned long long)s.hash, (unsigned long long)s.nonzero,
               (unsigned long long)s.total, s.minv, s.maxv, s.mean, s.stddev, s.mad_rgb0,
               s.mad_rgb1, s.mad_blend);
        fflush(stdout);
    }

    logmsg("summary: pipelines=195/195 dispatches=%d/frame (%d total) frames=%d "
           "validation_errors=%d validation_warnings=%d known_cm_fconvert=%d "
           "known_int_fetch_linear=%d known_cm2_spirv=%d device=alive changed=%s",
           g_row_count, total_dispatches, frames, g_validation_errors, g_validation_warnings,
           g_validation_cm_fconvert, g_validation_int_fetch, g_validation_cm2_spirv,
           changed ? "yes" : "no");
    printf("MLFI-REPLAY summary pipelines=195 dispatches=%d frames=%d validation_errors=%d "
           "known_cm_fconvert=%d known_int_fetch_linear=%d known_cm2_spirv=%d changed=%s\n",
           g_row_count, frames, g_validation_errors, g_validation_cm_fconvert,
           g_validation_int_fetch, g_validation_cm2_spirv, changed ? "yes" : "no");

    vkDeviceWaitIdle(g_device);
    /* Release every object so the validation layer's leak check is meaningful. */
    vkDestroyDescriptorPool(g_device, pool, NULL);
    for (int i = 0; i < g_pipe_count; ++i) {
        vkDestroyPipeline(g_device, g_pipes[i].pipeline, NULL);
        vkDestroyPipelineLayout(g_device, g_pipes[i].pipe_layout, NULL);
        vkDestroyDescriptorSetLayout(g_device, g_pipes[i].set_layout, NULL);
    }
    for (int i = 0; i < g_image_count; ++i) {
        vkDestroyImageView(g_device, g_images[i].view, NULL);
        vkDestroyImage(g_device, g_images[i].image, NULL);
        vkFreeMemory(g_device, g_images[i].memory, NULL);
    }
    for (int i = 0; i < 2; ++i) vkDestroyImageView(g_device, g_splat_storage_view[i], NULL);
    vkDestroySampler(g_device, g_sampler_linear, NULL);
    vkDestroySampler(g_device, g_sampler_nearest, NULL);
    vkDestroyBuffer(g_device, g_model, NULL);
    vkFreeMemory(g_device, g_model_mem, NULL);
    vkDestroyBuffer(g_device, g_scratch, NULL);
    vkFreeMemory(g_device, g_scratch_mem, NULL);
    vkDestroyBuffer(g_device, g_arena, NULL);
    vkUnmapMemory(g_device, g_arena_mem);
    vkFreeMemory(g_device, g_arena_mem, NULL);
    vkDestroyBuffer(g_device, g_staging, NULL);
    vkFreeMemory(g_device, g_staging_mem, NULL);
    vkDestroyBuffer(g_device, g_readback, NULL);
    vkFreeMemory(g_device, g_readback_mem, NULL);
    if (g_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(g_instance, g_messenger, NULL);
    }
    vkDestroyCommandPool(g_device, g_cmd_pool, NULL);
    vkDestroyDevice(g_device, NULL);
    vkDestroyInstance(g_instance, NULL);

    if (g_validation_errors) {
        printf("MLFI-REPLAY RESULT: FAIL validation_errors=%d\n", g_validation_errors);
        return 1;
    }
    if (!changed && frames > 1 && !static_input) {
        printf("MLFI-REPLAY RESULT: FAIL output did not change between frames\n");
        return 1;
    }
    printf("MLFI-REPLAY RESULT: OK\n");
    return 0;
}

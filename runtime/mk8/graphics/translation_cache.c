// MK8 (toolchain/translation_cache.py): Maxwell -> GLSL translations kept on disk between sessions.
// Only SPIR-V (cache/spirv) and pipelines (cache/vk_pipelines.bin) were kept, so every session translated each shader
// again the first time it showed, and a session translated a shader again for every program and key that used it
// (349 of the first 862 translations of a race). The prefetch scan waits for these on the frame path (one translator
// thread; the pipeline key needs the translated text), and compute shaders translate on the game thread.
//
// The key covers everything maxwell_translate reads: the shader header and the code words its own end scan reads
// (replayed here), the ShaderKey, the translator globals and the runtime executable (size and time). Debug variables
// that change the output bypass the cache. One file, cache/xlate.pack, opened once: a header with the runtime's stamp
// (another runtime starts a new pack) and appended records. Each record carries the key, a second independent hash of
// it, a hash of its contents, the whole ShaderInfo and its strings; a record that does not check out is translated
// again. A file per shader cost 0.2-0.6 ms to open (Windows), most of a translation. A tail cut short (a crash while
// writing) ends the index there and is written over. MK8_TRANSLATION_CACHE=0: off.
#include "rt.h"
#include "platform.h"
#include "maxwell/shader.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define XLATE_PACK "cache/xlate.pack"
#define XLATE_VERSION 3u
#define XLATE_MAX_BYTES (512ull << 20)  // (no more records past this; a full game's shaders are well under it)

typedef struct {
    char magic[8];
    uint32_t version, info_size, key_size, pad;
    uint64_t exe_size, exe_time;
    uint8_t reserved[24];
} PackHeader;

typedef struct {
    char magic[4];
    uint32_t body;   // bytes after this header: ShaderInfo + strings
    uint64_t key, check, sum;  // key hash, second key hash, hash of the body
    uint32_t len[4];           // msl, gs_params, vs_entry_params, vs_call_args; UINT32_MAX: NULL
} RecordHeader;

_Static_assert(sizeof(PackHeader) == 64, "pack header");
_Static_assert(sizeof(RecordHeader) == 48, "record header");

static const char PACK_MAGIC[8] = {'M', 'K', '8', 'X', 'P', 'A', 'C', 'K'};
static const char RECORD_MAGIC[4] = {'X', 'R', 'E', 'C'};

// ------------------------------------------------------------------ hashes
typedef struct { uint64_t a, b; } Hash2;

static void hash_word(Hash2 *h, uint64_t w) {
    h->a = (h->a ^ w) * 0x9E3779B97F4A7C15ull;
    h->a ^= h->a >> 32;
    h->b = (h->b + w) * 0xC2B2AE3D27D4EB4Full;
    h->b = (h->b << 31) | (h->b >> 33);
}

static void hash_bytes(Hash2 *h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        hash_word(h, w);
    }
    uint64_t tail = 0;
    memcpy(&tail, p, n);
    hash_word(h, tail ^ ((uint64_t)n << 56));
}

static uint64_t fmix(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    return k ^ (k >> 33);
}

// Bytes of the blob maxwell_translate reads: the header (graphics stages: NVN header + SPH) and the code words up to
// where its end scan stops, that word included (translate.c; emit_insn only looks back from there).
size_t mk8_translate_extent(const uint8_t *blob, size_t max_size, int stage) {
    size_t start = stage == STAGE_COMPUTE ? 0 : 0x80;
    const uint8_t *code = blob + start;
    size_t n = (max_size - start) / 8;  // (as translate.c, size_t arithmetic included)
    if (n > 65536) n = 65536;
    size_t end = 0, i;
    for (i = 0; i < n; i++) {
        uint64_t w;
        memcpy(&w, code + i * 8, 8);
        if (i % 4 == 0) continue;
        int64_t target = (int64_t)((w >> 20) & 0xFFFFFF);
        if (target & 0x800000) target -= 0x1000000;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && target == -8 && end) break;
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    return start + (i < n ? i + 1 : n) * 8;
}

static Hash2 translate_key(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    Hash2 h = {0x6d6b387861746531ull, 0x7472616e736c6174ull};
    uint64_t fixed[] = {XLATE_VERSION, (uint64_t)stage, (uint64_t)max_size, maxwell_gmem_base, maxwell_gmem_size,
                        (uint64_t)maxwell_glsl_bda | (uint64_t)maxwell_glsl_gmap_gfx << 1 |
                            (uint64_t)maxwell_msl_tex_swizzle << 2 | (uint64_t)(key != NULL) << 3};
    hash_bytes(&h, fixed, sizeof fixed);
    if (key) hash_bytes(&h, key, sizeof *key);
    size_t extent = mk8_translate_extent(blob, max_size, stage);
    hash_word(&h, extent);
    hash_bytes(&h, blob, extent);
    h.a = fmix(h.a);
    h.b = fmix(h.b ^ h.a);
    if (!h.a) h.a = 1;  // (0 marks an empty index slot)
    return h;
}

static uint64_t body_sum(const uint8_t *body, size_t n) {
    Hash2 h = {0x73756d6d61727921ull, 0x626f647968617368ull};
    hash_bytes(&h, body, n);
    return fmix(h.a ^ fmix(h.b));
}

// ------------------------------------------------------------------ the pack
typedef struct { uint64_t key; long offset; } Slot;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static bool g_on, g_writable;
static FILE *g_pack;
static long g_end;  // where the next record goes: after the last good one
static Slot *g_index;
static size_t g_cap, g_count;
static _Atomic uint64_t n_read, ns_read, n_made, ns_made, n_stored;

static void index_put(uint64_t key, long offset) {  // (caller holds g_lock)
    if ((g_count + 1) * 2 > g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 4096;
        Slot *grown = calloc(cap, sizeof *grown);
        if (!grown) return;  // (a lookup that misses translates: correct, only slower)
        for (size_t i = 0; i < g_cap; i++)
            if (g_index[i].key)
                for (size_t j = g_index[i].key & (cap - 1);; j = (j + 1) & (cap - 1))
                    if (!grown[j].key) { grown[j] = g_index[i]; break; }
        free(g_index);
        g_index = grown, g_cap = cap;
    }
    for (size_t j = key & (g_cap - 1);; j = (j + 1) & (g_cap - 1)) {
        if (!g_index[j].key) { g_index[j] = (Slot){key, offset}; g_count++; return; }
        if (g_index[j].key == key) { g_index[j].offset = offset; return; }
    }
}

static long index_get(uint64_t key) {
    if (!g_cap) return -1;
    for (size_t j = key & (g_cap - 1); g_index[j].key; j = (j + 1) & (g_cap - 1))
        if (g_index[j].key == key) return g_index[j].offset;
    return -1;
}

static bool header_matches(const PackHeader *h, uint64_t exe_size, uint64_t exe_time) {
    return !memcmp(h->magic, PACK_MAGIC, 8) && h->version == XLATE_VERSION && h->info_size == sizeof(ShaderInfo) &&
           h->key_size == sizeof(ShaderKey) && h->exe_size == exe_size && h->exe_time == exe_time;
}

static FILE *pack_create(uint64_t exe_size, uint64_t exe_time) {
    mkdir("cache", 0755);
    FILE *f = fopen(XLATE_PACK, "w+b");
    if (!f) return NULL;
    PackHeader h = {.version = XLATE_VERSION, .info_size = sizeof(ShaderInfo), .key_size = sizeof(ShaderKey),
                    .exe_size = exe_size, .exe_time = exe_time};
    memcpy(h.magic, PACK_MAGIC, 8);
    if (fwrite(&h, sizeof h, 1, f) != 1 || fflush(f) != 0) { fclose(f); return NULL; }
    return f;
}

static void init_once(void) {
    const char *e = getenv("MK8_TRANSLATION_CACHE");
    g_on = !(e && !strcmp(e, "0"));
    char exe[1024] = "";
    struct stat sb;
    if (g_on && !(plat_exe_path(exe, sizeof exe) && stat(exe, &sb) == 0)) g_on = false;  // (builds unknown: never reuse)
    if (!g_on) { LOG("[mk8:xlate] translation cache off\n"); return; }
    uint64_t exe_size = (uint64_t)sb.st_size, exe_time = (uint64_t)sb.st_mtime;
    PackHeader h;
    FILE *f = fopen(XLATE_PACK, "r+b");
    bool reuse = f && fread(&h, sizeof h, 1, f) == 1 && header_matches(&h, exe_size, exe_time);
    if (f && !reuse) fclose(f);
    if (!reuse) f = pack_create(exe_size, exe_time);  // (another runtime's translations: none can be served)
    if (!f) { g_on = false; LOG("[mk8:xlate] translation cache off: %s cannot be written\n", XLATE_PACK); return; }
    long offset = sizeof(PackHeader);
    if (reuse) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        RecordHeader r;
        while (offset + (long)sizeof r <= size && !fseek(f, offset, SEEK_SET) && fread(&r, sizeof r, 1, f) == 1 &&
               !memcmp(r.magic, RECORD_MAGIC, 4) && r.key && r.body <= size - offset - (long)sizeof r) {
            index_put(r.key, offset);
            offset += (long)sizeof r + (long)r.body;
        }
        if (offset != size) LOG("[mk8:xlate] %s: %ld bytes after the last whole record are written over\n", XLATE_PACK,
                                size - offset);
    }
    g_pack = f, g_end = offset, g_writable = true;
    LOG("[mk8:xlate] translation cache on: %zu translations in %s\n", g_count, XLATE_PACK);
}

static char *take_string(const uint8_t **p, const uint8_t *end, uint32_t len, bool *ok) {
    if (len == UINT32_MAX) return NULL;
    if ((size_t)(end - *p) < len || memchr(*p, 0, len)) { *ok = false; return NULL; }
    char *s = malloc((size_t)len + 1);
    if (!s) { *ok = false; return NULL; }
    memcpy(s, *p, len);
    s[len] = 0;
    *p += len;
    return s;
}

static ShaderInfo *pack_read(Hash2 h, int stage) {  // (caller holds g_lock)
    long offset = index_get(h.a);
    RecordHeader r;
    if (offset < 0 || fseek(g_pack, offset, SEEK_SET) || fread(&r, sizeof r, 1, g_pack) != 1) return NULL;
    if (memcmp(r.magic, RECORD_MAGIC, 4) || r.key != h.a || r.check != h.b || r.len[0] == UINT32_MAX ||
        r.body < sizeof(ShaderInfo) || r.body > (64u << 20))
        return NULL;
    uint8_t *body = malloc(r.body);
    if (!body) return NULL;
    bool ok = fread(body, 1, r.body, g_pack) == r.body && body_sum(body, r.body) == r.sum;
    ShaderInfo *si = ok ? malloc(sizeof *si) : NULL;
    char *s[4] = {0};
    if (si) {
        memcpy(si, body, sizeof *si);
        ok = si->stage == stage;
        const uint8_t *p = body + sizeof *si, *end = body + r.body;
        for (int i = 0; i < 4 && ok; i++) s[i] = take_string(&p, end, r.len[i], &ok);
        ok = ok && p == end;
    }
    free(body);
    if (!ok) {
        for (int i = 0; i < 4; i++) free(s[i]);
        free(si);
        return NULL;
    }
    si->msl = s[0], si->gs_params = s[1], si->vs_entry_params = s[2], si->vs_call_args = s[3];
    return si;
}

static bool pack_write(Hash2 h, const ShaderInfo *si) {  // (caller holds g_lock)
    const char *s[4] = {si->msl, si->gs_params, si->vs_entry_params, si->vs_call_args};
    RecordHeader r = {.key = h.a, .check = h.b};
    memcpy(r.magic, RECORD_MAGIC, 4);
    size_t body = sizeof *si;
    for (int i = 0; i < 4; i++) {
        size_t n = s[i] ? strlen(s[i]) : 0;
        if (n >= (16u << 20)) return false;
        r.len[i] = s[i] ? (uint32_t)n : UINT32_MAX;
        body += n;
    }
    if ((unsigned long long)g_end + sizeof r + body > XLATE_MAX_BYTES) return false;
    uint8_t *buf = malloc(body);
    if (!buf) return false;
    ShaderInfo copy = *si;  // (pointers mean nothing in another process)
    copy.msl = copy.gs_params = copy.vs_entry_params = copy.vs_call_args = NULL;
    memcpy(buf, &copy, sizeof copy);
    size_t at = sizeof copy;
    for (int i = 0; i < 4; i++)
        if (s[i]) memcpy(buf + at, s[i], r.len[i]), at += r.len[i];
    r.body = (uint32_t)body;
    r.sum = body_sum(buf, body);
    bool ok = !fseek(g_pack, g_end, SEEK_SET) && fwrite(&r, sizeof r, 1, g_pack) == 1 &&
              fwrite(buf, 1, body, g_pack) == body && fflush(g_pack) == 0;
    free(buf);
    if (!ok) { g_writable = false; LOG("[mk8:xlate] %s: write failed; no more translations stored\n", XLATE_PACK); return false; }
    index_put(h.a, g_end);
    g_end += (long)(sizeof r + body);
    return true;
}

// ------------------------------------------------------------------ the translator's stand-in
static bool bypass(void) {  // output that depends on more than the key
    static int env = -1;
    if (env < 0)
        env = getenv("LARECOMP_SAVE_MSL") || getenv("LARECOMP_NO_FMZ") || getenv("LARECOMP_NO_DISCARD") ||
              getenv("LARECOMP_NO_BRA_IF");
    return env || maxwell_dbg_code || maxwell_dbg_vs;
}

static void stats(void) {
    uint64_t r = atomic_load(&n_read), m = atomic_load(&n_made);
    if ((r + m) % 256) return;
    LOG("[mk8:xlate] %llu shaders read from the cache (%.1f ms), %llu translated (%.1f ms), %llu stored\n",
        (unsigned long long)r, atomic_load(&ns_read) / 1e6, (unsigned long long)m, atomic_load(&ns_made) / 1e6,
        (unsigned long long)atomic_load(&n_stored));
}

ShaderInfo *mk8_translate_cached(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    pthread_once(&g_once, init_once);
    if (!g_on || bypass() || !blob) return maxwell_translate(blob, max_size, stage, key);
    uint64_t t0 = plat_now_ns();
    Hash2 h = translate_key(blob, max_size, stage, key);
    pthread_mutex_lock(&g_lock);
    ShaderInfo *si = pack_read(h, stage);
    pthread_mutex_unlock(&g_lock);
    if (si) {
        atomic_fetch_add(&ns_read, plat_now_ns() - t0);
        atomic_fetch_add(&n_read, 1);
        stats();
        return si;
    }
    si = maxwell_translate(blob, max_size, stage, key);
    atomic_fetch_add(&ns_made, plat_now_ns() - t0);
    atomic_fetch_add(&n_made, 1);
    if (si && si->msl) {
        pthread_mutex_lock(&g_lock);
        if (g_writable && pack_write(h, si)) atomic_fetch_add(&n_stored, 1);
        pthread_mutex_unlock(&g_lock);
    }
    stats();
    return si;
}

// (tests) translations indexed, and where the record for these inputs is (-1: none)
size_t mk8_translate_cache_entries(void) {
    pthread_once(&g_once, init_once);
    pthread_mutex_lock(&g_lock);
    size_t n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

long mk8_translate_cache_record(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    pthread_once(&g_once, init_once);
    Hash2 h = translate_key(blob, max_size, stage, key);
    pthread_mutex_lock(&g_lock);
    long offset = index_get(h.a);
    pthread_mutex_unlock(&g_lock);
    return offset;
}

void mk8_translate_cache_counts(uint64_t *read, uint64_t *made) {
    *read = atomic_load(&n_read), *made = atomic_load(&n_made);
}

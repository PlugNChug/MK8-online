"""Texture pool IDs: the cube map a cube sampler still names, and diagnostics for what each ID names.

MK8 4.0.1 alternates its main lighting pass between two environment cube IDs (one per frame parity). At race setup
(Time Trial frame ~2316) it creates its 18 reflection cubes twice; the second creation releases the first one's
descriptor slots, and the game's slot allocator hands those IDs to per-frame render targets from frame ~2371 on.
The lighting pass of every other frame still samples the first ID as a cube: a 2D render target there could only be
bound as an empty view ("[tex] type mismatch: want 3 have 1"), so those frames lost the environment light and the
whole scene alternated between two brightness levels at 30 Hz (automated captures: mean luma 111 vs 99, tunnel 69
vs 43). Why the console keeps the cube there was not found in the game's code (its slots are reference counted).

So a cube sampler whose ID now holds a texture that is not a cube map reads the last cube map registered under that
ID instead of an empty view (logged; MK8_TEXPOOL_CUBE_HISTORY=0 restores the empty view).

Diagnostics: MK8_TRACE_TEXPOOL=<first>:<last> presented frames logs every registration (pool, ID, texture, target,
size, address, guest caller; cube maps with the guest frame-pointer chain) and every bind of an ID that once held a
cube map. With LARECOMP_DEBUG_FRAME the debug draw also logs the handle and ID behind each fragment texture.
MK8_TRACE_RT=<address hex>[,<address>...] (up to 8; MK8_TRACE_RT_FROM=<frame> starts later) logs every pass drawing to
those textures (frame, level, layers) with its draws, their textures and, for CPU-written ones, the hash of the guest
memory against the last upload. LARECOMP_DEBUG_DATA then prints up to 640 floats per uniform buffer.
MK8_TEX_STATS=<address hex>[,...] reads back those textures when a traced or debug-frame draw samples them (once per
frame, before the next pass): mean / peak RGB and non-finite texels per mip, for B10G11R11 and RGBA16F.
"""

REGISTER_OLD = '''    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    NvnTexture *next = mk8_texture_retain(HOST(NvnTexture, A(2)));
'''

SUPPORT = '''
// MK8 (toolchain/texture_pool.py): the last cube map registered under each ID of a texture pool
static struct { NvnDescPool *pool; NvnTexEntry *cubes; } mk8_cube_ids[64];
static pthread_mutex_t mk8_cube_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnTexEntry *mk8_cube_table(NvnDescPool *d, bool create) {  // (mk8_cube_lock held)
    for (int i = 0; i < 64 && mk8_cube_ids[i].pool; i++) if (mk8_cube_ids[i].pool == d) return mk8_cube_ids[i].cubes;
    if (!create) return NULL;
    for (int i = 0; i < 64; i++)
        if (!mk8_cube_ids[i].pool) {
            NvnTexEntry *cubes = calloc((size_t)d->count, sizeof *cubes);
            if (!cubes) return NULL;
            mk8_cube_ids[i].cubes = cubes;
            mk8_cube_ids[i].pool = d;
            return cubes;
        }
    return NULL;
}
static void mk8_cube_note(NvnDescPool *d, int id, const NvnTexEntry *e) {  // a registration of a cube map
    pthread_mutex_lock(&mk8_cube_lock);
    NvnTexEntry *cubes = mk8_cube_table(d, true);
    if (cubes) {
        NvnTexture *keep = mk8_texture_retain(e->tex);
        mk8_texture_release(cubes[id].tex);
        cubes[id] = *e;
        cubes[id].tex = keep;
    }
    pthread_mutex_unlock(&mk8_cube_lock);
}
// The entry a cube sampler reads at an ID that now holds something else; NULL when no cube map was registered there.
const NvnTexEntry *mk8_texpool_cube(NvnDescPool *d, uint32_t id) {
    static int off = -1;
    if (off < 0) { const char *v = getenv("MK8_TEXPOOL_CUBE_HISTORY"); off = v && !strcmp(v, "0"); }
    if (off || !d || id >= (uint32_t)d->count) return NULL;
    pthread_mutex_lock(&mk8_cube_lock);
    NvnTexEntry *cubes = mk8_cube_table(d, false);
    const NvnTexEntry *e = cubes && cubes[id].tex ? &cubes[id] : NULL;
    pthread_mutex_unlock(&mk8_cube_lock);
    return e;
}

// MK8_TRACE_TEXPOOL=<first>:<last> presented frames
uint8_t mk8_texpool_cube_ids[1 << 16];  // pool IDs ever registered with a cube map (their binds are traced)
bool mk8_texpool_trace_on(void) {
    static long first = -2, last;
    if (first == -2) {
        const char *e = getenv("MK8_TRACE_TEXPOOL");
        first = -1;
        if (e && sscanf(e, "%ld:%ld", &first, &last) != 2) first = -1;
    }
    extern uint64_t renderer_present_count(void);
    long f = first >= 0 ? (long)renderer_present_count() : -1;
    return first >= 0 && f >= first && f <= last;
}
void mk8_texpool_callers(Cpu *c) {  // the guest's frame-pointer chain, to tell callers apart
    uint64_t fp = c->x[29];
    LOG("[mk8:texpool]   callers: %llx", (unsigned long long)c->x[30]);
    for (int k = 0; k < 12 && fp >= 0x7000000000ull && fp < 0x8000000000ull && !(fp & 7); k++) {
        LOG(" %llx", (unsigned long long)*(uint64_t *)(uintptr_t)(fp + 8));
        fp = *(uint64_t *)(uintptr_t)fp;
    }
    LOG("\\n");
}
'''

REGISTER_NEW = REGISTER_OLD + '''    if (next && next->target == 8 && id < (1 << 16)) mk8_texpool_cube_ids[id] = 1;
    if (mk8_texpool_trace_on()) {
        extern uint64_t renderer_present_count(void);
        LOG("[mk8:texpool] frame %llu pool %p id %d tex %p (was %p) target %u %ux%u fmt %#x addr %llx view %d lr %llx\\n",
            (unsigned long long)renderer_present_count(), (void *)d, id, (void *)next, (void *)e->tex, next ? next->target : 0,
            next ? next->width : 0, next ? next->height : 0, next ? next->format : 0,
            next ? (unsigned long long)next->addr : 0ull, A(3) != 0, (unsigned long long)c->x[30]);
        if (next && next->target == 8) mk8_texpool_callers(c);
    }
'''

STORE_OLD = '''    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }'''
STORE_NEW = '''    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
    if (e->tex && (e->tex->target == 8 || e->tex->target == 9)) mk8_cube_note(d, id, e);  // (toolchain/texture_pool.py)
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }'''

BIND = '''
// MK8 (toolchain/texture_pool.py): binds of pool IDs that once held a cube map, with their callers (MK8_TRACE_TEXPOOL)
extern uint8_t mk8_texpool_cube_ids[1 << 16];
bool mk8_texpool_trace_on(void);
void mk8_texpool_callers(Cpu *c);
static void mk8_trace_binds(Cpu *c, uint32_t stage, uint32_t first, uint32_t n, const uint64_t *handles) {
    if (!handles || !mk8_texpool_trace_on()) return;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        uint32_t id = (uint32_t)(handles[i] & 0xFFFFF);
        if (id >= (1u << 16) || !mk8_texpool_cube_ids[id]) continue;
        extern uint64_t renderer_present_count(void);
        LOG("[mk8:texpool] frame %llu bind stage %u slot %u handle %llx id %u (handle read from %llx)\\n",
            (unsigned long long)renderer_present_count(), stage, first + i, (unsigned long long)handles[i], id,
            (unsigned long long)(uintptr_t)&handles[i]);
        mk8_texpool_callers(c);
    }
}
'''
BIND_SITES = [
    ('NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }',
     'NVN(nvnCommandBufferBindTextures) {\n    mk8_trace_binds(c, (uint32_t)A(1), (uint32_t)A(2), (uint32_t)A(3), (const uint64_t *)AP(4));\n'
     '    record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8);\n}'),
    ('NVN(nvnCommandBufferBindTexture) {\n    Snap s = snap(c);\n',
     'NVN(nvnCommandBufferBindTexture) {\n    { uint64_t h_ = A(3); mk8_trace_binds(c, (uint32_t)A(1), (uint32_t)A(2), 1, &h_); }\n'
     '    Snap s = snap(c);\n'),
]

LOOKUP = '''
// MK8 (toolchain/texture_pool.py): the entry a sampler reads at a pool ID. A cube sampler whose ID now holds a texture
// that is not a cube map reads the last cube map registered there (vk_view could only give it an empty view).
const NvnTexEntry *mk8_texpool_cube(NvnDescPool *d, uint32_t id);
uint64_t vk_frame_counter(void);
static const NvnTexEntry *mk8_tex_entry(NvnDescPool *p, uint32_t tic, int type) {
    const NvnTexEntry *e = &((NvnTexEntry *)p->entries)[tic];
    if ((type != TEXTYPE_CUBE && type != TEXTYPE_CUBE_ARRAY) || !e->tex || e->tex->target == 8 || e->tex->target == 9) return e;
    const NvnTexEntry *cube = mk8_texpool_cube(p, tic);
    if (!cube) {
        static int nolog;  // (no cube map was ever registered at this ID: what does the sampler get?)
        if (nolog++ < 16)
            LOG("[mk8:texpool] cube sampler at ID %u: %ux%u target %u fmt %#x addr %llx, no cube map ever there (frame %llu)\\n",
                tic, e->tex->width, e->tex->height, e->tex->target, e->tex->format, (unsigned long long)e->tex->addr,
                (unsigned long long)vk_frame_counter());
        return e;
    }
    static uint64_t n;
    if (n++ < 8 || n % 100000 == 0)
        LOG("[mk8:texpool] cube sampler at ID %u: %ux%u target %u registered there now, its last cube map %p used (%llu)\\n", tic,
            e->tex->width, e->tex->height, e->tex->target, (void *)cube->tex, (unsigned long long)n);
    return cube;
}
'''
RT_TRACE = ('static void begin_pass(void) {\n    if (in_pass && !st.rt_dirty) return;\n    vk_end_pass();\n', """// MK8 (toolchain/texture_pool.py): MK8_TRACE_RT=<guest address, hex>[,<address>...] (up to 8): each pass that draws to
// one of those textures, with its draws (the first 3000 such passes from presented frame MK8_TRACE_RT_FROM on)
uint64_t vk_frame_counter(void);
static bool mk8_rtt_pass;
static int mk8_rtt_n = -1, mk8_rtt_logged;
static uint64_t mk8_rtt_from;
static uint64_t mk8_rtt_list[8];
static bool mk8_rtt_match(uint64_t addr) {
    if (mk8_rtt_n < 0) {
        mk8_rtt_n = 0;
        if (getenv("MK8_TRACE_RT_FROM")) mk8_rtt_from = strtoull(getenv("MK8_TRACE_RT_FROM"), NULL, 10);
        for (const char *e = getenv("MK8_TRACE_RT"); e && *e && mk8_rtt_n < 8;) {
            char *end;
            uint64_t a = strtoull(e, &end, 16);
            if (end == e) break;
            mk8_rtt_list[mk8_rtt_n++] = a;
            e = *end == ',' ? end + 1 : end;
        }
    }
    for (int i = 0; i < mk8_rtt_n; i++) if (mk8_rtt_list[i] == addr) return true;
    return false;
}
// MK8_TEX_STATS=<address, hex>[,...] (up to 8): textures at those addresses sampled by a traced or debug-frame draw are
// read back before the next pass, once per frame: mean / peak RGB and non-finite texels per mip (B10G11R11, RGBA16F)
static int mk8_ts_n = -1, mk8_ts_np;
static uint64_t mk8_ts_list[8], mk8_ts_frame[8], mk8_ts_paddr[16];
static TexBackend *mk8_ts_pend[16];
static void mk8_tex_stats_want(TexBackend *b, uint64_t addr) {
    if (mk8_ts_n < 0) {
        mk8_ts_n = 0;
        for (const char *e = getenv("MK8_TEX_STATS"); e && *e && mk8_ts_n < 8;) {
            char *end;
            uint64_t a = strtoull(e, &end, 16);
            if (end == e) break;
            mk8_ts_list[mk8_ts_n++] = a;
            e = *end == ',' ? end + 1 : end;
        }
    }
    for (int i = 0; i < mk8_ts_n; i++)
        if (mk8_ts_list[i] == addr && mk8_ts_frame[i] != vk_frame_counter() + 1 && mk8_ts_np < 16) {
            mk8_ts_frame[i] = vk_frame_counter() + 1;
            mk8_ts_paddr[mk8_ts_np] = addr;
            mk8_ts_pend[mk8_ts_np++] = b;
        }
}
static float mk8_ufloat(uint32_t v, int mbits) {
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 31) return m ? NAN : INFINITY;
    return e ? ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15) : ldexpf((float)m, -14 - mbits);
}
static float mk8_half(uint16_t h) {
    float f = mk8_ufloat(h & 0x7fff, 10);
    return h & 0x8000 ? -f : f;
}
static void mk8_tex_stats_run(void) {
    for (int p = 0; p < mk8_ts_np; p++) {
        TexBackend *b = mk8_ts_pend[p];
        uint32_t bpp = b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? 4 : b->format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 0;
        if (!b->image || !bpp) { LOG("[mk8:texstats] %llx: format %d not read\\n", (unsigned long long)mk8_ts_paddr[p], b->format); continue; }
        for (uint32_t lv = 0; lv < b->levels; lv++) {
            uint32_t w = b->width >> lv ? b->width >> lv : 1, h = b->height >> lv ? b->height >> lv : 1, ly = b->layers ? b->layers : 1;
            uint64_t n = (uint64_t)w * h * ly;
            VkSpan out = vk_readback(n * bpp);
            VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, ly}, .imageExtent = {w, h, 1}};
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
            renderer_flush();
            double sum[3] = {0};
            float peak[3] = {0};
            uint64_t bad = 0;
            for (uint64_t k = 0; k < n; k++) {
                float rgb[3];
                if (bpp == 4) {
                    uint32_t v = ((const uint32_t *)out.ptr)[k];
                    rgb[0] = mk8_ufloat(v & 0x7ff, 6); rgb[1] = mk8_ufloat((v >> 11) & 0x7ff, 6); rgb[2] = mk8_ufloat(v >> 22, 5);
                } else {
                    const uint16_t *q = (const uint16_t *)out.ptr + k * 4;
                    rgb[0] = mk8_half(q[0]); rgb[1] = mk8_half(q[1]); rgb[2] = mk8_half(q[2]);
                }
                if (!isfinite(rgb[0]) || !isfinite(rgb[1]) || !isfinite(rgb[2])) { bad++; continue; }
                for (int ch = 0; ch < 3; ch++) { sum[ch] += rgb[ch]; if (rgb[ch] > peak[ch]) peak[ch] = rgb[ch]; }
            }
            uint64_t good = n - bad ? n - bad : 1;
            LOG("[mk8:texstats] frame %llu %llx %ux%u x%u fmt %d level %u: mean %.4g %.4g %.4g peak %.4g %.4g %.4g bad %llu\\n",
                (unsigned long long)vk_frame_counter(), (unsigned long long)mk8_ts_paddr[p], w, h, ly, b->format, lv,
                sum[0] / good, sum[1] / good, sum[2] / good, peak[0], peak[1], peak[2], (unsigned long long)bad);
        }
    }
    mk8_ts_np = 0;
}
static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (mk8_ts_np) mk8_tex_stats_run();  // (outside any pass: copies)
""")
RT_PASS = ('    if (g_debug_frame) {\n        LOG("[pass] begin %ux%u:", minw, minh);\n',
           """    mk8_rtt_pass = false;
    for (int i = 0; mk8_rtt_logged < 3000 && i < st.rt.ncolor; i++) {
        const NvnTexture *t = st.rt.color[i];
        if (!t || !mk8_rtt_match(t->addr) || vk_frame_counter() < mk8_rtt_from) continue;
        const NvnTextureView *cv = st.rt.has_cview[i] ? &st.rt.cview[i] : NULL;
        mk8_rtt_pass = true;
        mk8_rtt_logged++;
        LOG("[mk8:rt] frame %llu %llx pass %ux%u c%d of %d: level %u layers %u+%u (mask %#x) load %d\\n",
            (unsigned long long)vk_frame_counter(), (unsigned long long)t->addr, minw, minh, i, st.rt.ncolor, cv ? cv->base_level : 0,
            cv ? cv->min_layer : 0, cv ? cv->num_layers : 0, cv ? cv->setmask : 0, ca[i].loadOp);
    }
    if (g_debug_frame) {
        LOG("[pass] begin %ux%u:", minw, minh);
""")
RT_DRAW = ('    if (g_debug_frame) debug_draw(pr, r, indexed);\n',
           '    if (g_debug_frame || mk8_rtt_pass) debug_draw(pr, r, indexed);  // (toolchain/texture_pool.py)\n')
RT_DATA = ('            for (int k = 0; k < 160 && k * 4 < (int)ub.size; k++) LOG(" %.6g", u[k]);\n',
           '            for (int k = 0; k < 640 && k * 4 < (int)ub.size; k++) LOG(" %.6g", u[k]);  // (toolchain/texture_pool.py)\n')
LOOKUP_ANCHOR = 'static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {\n'
USES = [
    ('    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;\n', '    return mk8_tex_entry(st.tpool, tic, st_->type)->tex;\n'),
    ('        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];\n',
     '        const NvnTexEntry *e = mk8_tex_entry(st.tpool, tic, st_->type);\n'),
]

DEBUG_OLD = '''            (unsigned long long)t->addr, b && b->uploaded ? " uploaded" : "", b && b->gpu_owned ? " gpu-owned" : "", b ? b->refs : 0);
    }
    LOG("[draw]   streams:");'''
DEBUG_NEW = '''            (unsigned long long)t->addr, b && b->uploaded ? " uploaded" : "", b && b->gpu_owned ? " gpu-owned" : "", b ? b->refs : 0);
        int mk8_slot = ((int)pr->fi->tex[i].handle_offset - 0x20) / 8;  // (toolchain/texture_pool.py)
        uint64_t mk8_h = mk8_slot >= 0 && mk8_slot < 32 ? st.tex[1][mk8_slot] : 0;
        LOG("[draw]   tex%d handle %llx id %u pool %p\\n", i, (unsigned long long)mk8_h, (unsigned)(mk8_h & 0xFFFFF), (void *)st.tpool);
        if (b) mk8_tex_stats_want((TexBackend *)b, t->addr);  // (MK8_TEX_STATS)
        if (mk8_rtt_pass && b && t->addr && !b->gpu_owned) {  // (MK8_TRACE_RT: what the CPU holds vs what was uploaded)
            uint64_t n = nvn_texture_storage_size(t), fnv = 0xcbf29ce484222325ull;
            for (uint64_t k = 0; k < n; k++) fnv = (fnv ^ ((const uint8_t *)(uintptr_t)t->addr)[k]) * 0x100000001b3ull;
            LOG("[draw]   tex%d memory %llu bytes fnv %016llx, uploaded hash %016llx checked %llu dynamic %d\\n", i,
                (unsigned long long)n, (unsigned long long)fnv, (unsigned long long)b->data_hash,
                (unsigned long long)b->checked_frame, (int)b->dynamic);
        }
    }
    LOG("[draw]   streams:");'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Texture pool boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    objects = runtime/'nvn/objects.c'
    text = replace(objects.read_text(encoding='utf-8'), REGISTER_OLD, REGISTER_NEW)
    text = replace(text, 'static void register_texture(Cpu *c) {\n', SUPPORT + 'static void register_texture(Cpu *c) {\n')
    objects.write_text(replace(text, STORE_OLD, STORE_NEW), encoding='utf-8')
    commands = runtime/'nvn/cmdbuf.c'
    text = replace(commands.read_text(encoding='utf-8'), 'NVN(nvnCommandBufferBindTextures)',
                   BIND + 'NVN(nvnCommandBufferBindTextures)')
    for old, new in BIND_SITES:
        text = replace(text, old, new)
    commands.write_text(text, encoding='utf-8')
    execute = runtime/'nvn/vulkan/exec.c'
    text = replace(execute.read_text(encoding='utf-8'), DEBUG_OLD, DEBUG_NEW)
    text = replace(text, LOOKUP_ANCHOR, LOOKUP + LOOKUP_ANCHOR)
    for old, new in USES:
        text = replace(text, old, new)
    for old, new in (RT_TRACE, RT_PASS, RT_DRAW, RT_DATA):
        text = replace(text, old, new)
    execute.write_text(text, encoding='utf-8')

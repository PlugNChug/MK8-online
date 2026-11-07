"""Wait for compute results only when the CPU is about to copy memory a pending dispatch writes.

Upstream finishes all GPU work after every dispatch that stores to guest memory, because later draws copy their
uniform buffers from guest memory on the CPU while recording. The GPU itself reads guest memory in place (imported
heap), so only those CPU copies need the results. MK8 4.0.1 spent ~7.8 ms per frame in these waits ("compute
results", boot 105). Here a dispatch gets a GPU barrier and marks its storage buffers pending; a draw or dispatch whose
copied ranges (uniform buffers, vertex streams, indices) overlap pending memory finishes the GPU work first, at a
command boundary. Any other CPU copy that touches pending memory is counted and logged.

Deferred results with a submission after each writing dispatch avoid a CPU wait while keeping subsequent uploads
after that dispatch. MK8_DEFER_COMPUTE_FLUSH=0 restores eager waits; MK8_COMPUTE_SUBMIT=0 tests batching again.
"""

TRACK = '''
// MK8 (toolchain/compute_flush.py): guest memory written by compute dispatches whose GPU work may still be running
static uint64_t mk8_cw_bits[(HEAP_MAX >> GPU_READ_PAGE_SHIFT) / 64 + 1];
static bool mk8_cw_any;
static uint64_t mk8_cw_deferred, mk8_cw_flushes, mk8_cw_missed;
static uint64_t mk8_cw_why[3];  // waits by reader: uniform buffer, vertex stream, index buffer
static void mk8_cw_mark(uint64_t addr, uint64_t size) {
    if (!size || addr < HEAP_BASE || addr >= HEAP_BASE + HEAP_MAX) return;
    uint64_t end = addr + size > HEAP_BASE + HEAP_MAX ? HEAP_BASE + HEAP_MAX : addr + size;
    for (uint64_t pg = (addr - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg <= (end - 1 - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg++)
        mk8_cw_bits[pg >> 6] |= 1ull << (pg & 63);
    mk8_cw_any = true;
}
static bool mk8_cw_pending(uint64_t addr, uint64_t size) {
    if (!mk8_cw_any || !size || addr < HEAP_BASE || addr >= HEAP_BASE + HEAP_MAX) return false;
    uint64_t end = addr + size > HEAP_BASE + HEAP_MAX ? HEAP_BASE + HEAP_MAX : addr + size;
    for (uint64_t pg = (addr - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg <= (end - 1 - HEAP_BASE) >> GPU_READ_PAGE_SHIFT; pg++)
        if (mk8_cw_bits[pg >> 6] & (1ull << (pg & 63))) return true;
    return false;
}
static void mk8_cw_clear(void) {
    if (mk8_cw_any) memset(mk8_cw_bits, 0, sizeof mk8_cw_bits);
    mk8_cw_any = false;
}
'''

READERS = '''
// MK8: CPU copies at a command boundary wait for the pending dispatches they read (toolchain/compute_flush.py)
static int mk8_eager_compute(void) {
    static int eager = -1;
    if (eager < 0) { const char *e = getenv("MK8_DEFER_COMPUTE_FLUSH"); eager = e && atoi(e) == 0; }
    return eager;
}
static void mk8_cw_flush(const char *why) {
    wait_why = why;
    mk8_cw_flushes++;
    renderer_flush();  // (clears the pending set: gpu_read_clear)
}
// A uniform buffer inside the imported heap, aligned for a descriptor: the GPU can read it in place.
static bool mk8_ubo_direct_ok(uint64_t addr, uint64_t size) {
    uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment ? g_vk.props.limits.minUniformBufferOffsetAlignment : 256;
    return gmem_buf && size && addr >= HEAP_BASE && addr - HEAP_BASE + size <= gmem_size && (addr - HEAP_BASE) % align == 0 &&
           size <= g_vk.props.limits.maxUniformBufferRange;
}
static bool mk8_ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out) {
    if (!mk8_cw_any || !mk8_cw_pending(addr, size) || !mk8_ubo_direct_ok(addr, size)) return false;
    gpu_read_mark(addr, size);  // (read when the submission runs: later CPU writes to it wait, as for storage buffers)
    *out = (VkDescriptorBufferInfo){gmem_buf, addr - HEAP_BASE, size};
    mk8_cw_direct++;
    return true;
}
static bool mk8_cw_stage_pending(int stage) {  // (a range the GPU will read in place needs no wait)
    for (int i = 0; mk8_cw_any && i < 18; i++) {
        uint64_t a = st.ubuf[stage][i].addr, s = st.ubuf[stage][i].size ? st.ubuf[stage][i].size : 0x10000;
        if (a && mk8_cw_pending(a, s) && !mk8_ubo_direct_ok(a, s)) return true;
    }
    return false;
}
'''

DRAW = '''
// Vertex streams and indices the draw binds in place (guest_direct: sized, inside the import) are read by the GPU after
// the dispatch's barrier; only the ones it copies on the CPU need the dispatch finished.
static bool mk8_vtx_direct_ok(uint64_t addr, uint64_t size) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    return !copy && gmem_buf && size && addr >= HEAP_BASE && addr - HEAP_BASE + size <= gmem_size &&
           addr - HEAP_BASE + size <= gmem_direct_limit();
}
static void mk8_cw_before_draw(const CmdRec *r, bool indexed) {
    if (!mk8_cw_any) return;
    int why = -1;
    for (int s = 0; s < 5 && why < 0; s++) if (mk8_cw_stage_pending(s)) why = 0;
    for (int i = 0; i < 16 && why < 0; i++) {
        Range v = st.vbuf[i];
        if (v.addr && mk8_cw_pending(v.addr, v.size ? v.size : 1u << 20) && !(v.size && mk8_vtx_direct_ok(v.addr, v.size))) why = 1;
    }
    if (why < 0 && indexed) {
        uint32_t type = (uint32_t)r->x[1], mode = (uint32_t)r->x[0];
        uint64_t isz = type == 1 ? 2 : type == 2 ? 4 : 1, bytes = (uint64_t)(uint32_t)r->x[2] * isz;
        bool in_place = isz > 1 && mode != 6 && mode != 7 && mode != 9 && r->x[3] % isz == 0 && mk8_vtx_direct_ok(r->x[3], bytes);
        if (mk8_cw_pending(r->x[3], bytes + 4) && !in_place) why = 2;  // (8-bit, unaligned or expanded on the CPU: copied)
    }
    if (why >= 0) { mk8_cw_why[why]++; mk8_cw_flush("compute results (read by a draw)"); }
}
'''

OLD_FLUSH = '    if (!no_flush && cp->si->uses_gmem_store) { wait_why = "compute results"; renderer_flush(); }\n}\n'
NEW_FLUSH = '''    if (!no_flush && cp->si->uses_gmem_store) {
        if (mk8_eager_compute()) { wait_why = "compute results"; renderer_flush(); }
        else {  // later GPU work sees the results through this barrier; CPU copies wait in mk8_cw_before_draw
            vk_full_barrier(cb);
            for (int i = 0; i < 16; i++) mk8_cw_mark(st.sbuf[5][i].addr, st.sbuf[5][i].size);
            // Keep later uploads after this dispatch without waiting for the GPU on the CPU.
            static int submit = -1;
            if (submit < 0) { const char *e = getenv("MK8_COMPUTE_SUBMIT"); submit = !e || atoi(e) != 0; }
            if (submit) renderer_submit();  // pending CPU-read ranges survive submission until a real wait
            if (++mk8_cw_deferred % 3000 == 0)
                LOG("[mk8] compute results: %llu dispatches deferred, %llu waits for CPU copies, %llu unguarded copies\\n",
                    (unsigned long long)mk8_cw_deferred, (unsigned long long)mk8_cw_flushes, (unsigned long long)mk8_cw_missed);
        }
    }
}
'''

DISPATCH_START = '    vk_end_pass();\n    VkCommandBuffer cb = vk_cmd();\n    vk_full_barrier(cb);\n    gp_mark(cb, "dispatch'
TRANSIENT = 'VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {\n'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Compute result boundary changed: {old[:90]}')
    return text.replace(old, new)


def prepare(runtime):
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    clear = 'static void gpu_read_clear(void) {\n'
    text = replace(text, clear, TRACK + clear + '    mk8_cw_clear();\n')
    text = replace(text, TRANSIENT, TRANSIENT + '    if (mk8_cw_any && mk8_cw_pending((uint64_t)(uintptr_t)data, size) && mk8_cw_missed++ < 8)\n'
                                    '        LOG("[mk8] CPU copy of %llx+%llx while a dispatch may still write it\\n", '
                                    '(unsigned long long)(uintptr_t)data, (unsigned long long)size);\n')
    text = replace(text, 'static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {\n',
                   READERS + 'static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {\n')
    text = replace(text, DISPATCH_START, '    if (mk8_cw_stage_pending(5)) mk8_cw_flush("compute results (read by a dispatch)");\n' + DISPATCH_START)
    text = replace(text, OLD_FLUSH, NEW_FLUSH)
    text = replace(text, 'static void draw(CmdRec *r, bool indexed) {\n', DRAW + 'static void draw(CmdRec *r, bool indexed) {\n')
    text = replace(text, '    if (!st.program) SKIP_DRAW();\n    // Culling both faces',
                   '    if (!st.program) SKIP_DRAW();\n    mk8_cw_before_draw(r, indexed);\n    // Culling both faces')
    text = replace(text, '        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;\n',
                   '        case CMD_COPY_BUFFER_TO_TEXTURE:\n            if (mk8_cw_any) mk8_cw_flush("compute results (buffer -> texture)");\n'
                   '            copy_buffer_to_texture(r->x[0], x);\n            break;\n')
    # A report value (the NVN semaphore release the CPU polls) is written on the CPU when the command is translated:
    # with dispatches still running it would tell the game their results are ready before they are.
    report = '            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];\n            if (out) { out[0] = (uint32_t)r->x[0];'
    text = replace(text, report, '            if (mk8_cw_any) { mk8_cw_reports++; mk8_cw_flush("compute results (report value)"); }\n' + report)
    text = replace(text, 'static uint64_t mk8_cw_deferred, mk8_cw_flushes, mk8_cw_missed;\n',
                   'static uint64_t mk8_cw_deferred, mk8_cw_flushes, mk8_cw_missed, mk8_cw_reports;\n')
    text = replace(text, 'waits for CPU copies, %llu unguarded copies', 'waits for CPU reads (%llu report values), %llu unguarded copies')
    text = replace(text, '(unsigned long long)mk8_cw_flushes, (unsigned long long)mk8_cw_missed);',
                   '(unsigned long long)mk8_cw_flushes, (unsigned long long)mk8_cw_reports, (unsigned long long)mk8_cw_missed);')
    # Uniform buffers written by a pending dispatch are read by the GPU in place from the imported heap (after the
    # dispatch's barrier) instead of copied on the CPU, which had to wait for the dispatch first.
    text = replace(text, '.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |\n',
                   '.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |\n'
                   '                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |\n')
    text = replace(text, '// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.\n',
                   'static bool mk8_ubo_direct(uint64_t addr, uint64_t size, VkDescriptorBufferInfo *out);  // (compute_flush.py)\n'
                   '// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.\n')
    text = replace(text, '    return ubo_span(guest_copy(r.addr, size), size);\n}\n',
                   '    { VkDescriptorBufferInfo direct; if (mk8_ubo_direct(r.addr, size, &direct)) return direct; }\n'
                   '    return ubo_span(guest_copy(r.addr, size), size);\n}\n')
    text = replace(text, 'static uint64_t mk8_cw_deferred, mk8_cw_flushes, mk8_cw_missed, mk8_cw_reports;\n',
                   'static uint64_t mk8_cw_deferred, mk8_cw_flushes, mk8_cw_missed, mk8_cw_reports, mk8_cw_direct;\n')
    text = replace(text, 'waits for CPU reads (%llu report values), %llu unguarded copies',
                   'waits for CPU reads (%llu report values; draws: %llu uniform, %llu vertex, %llu index), %llu uniform buffers read in place, %llu unguarded copies')
    text = replace(text, '(unsigned long long)mk8_cw_reports, (unsigned long long)mk8_cw_missed);',
                   '(unsigned long long)mk8_cw_reports, (unsigned long long)mk8_cw_why[0], (unsigned long long)mk8_cw_why[1], '
                   '(unsigned long long)mk8_cw_why[2], (unsigned long long)mk8_cw_direct, (unsigned long long)mk8_cw_missed);')
    text = replace(text, 'static void mk8_cw_clear(void) {\n',
                   'bool mk8_cw_overlaps(uint64_t addr, uint64_t size) { return mk8_cw_pending(addr, size); }  // (textures.c)\n'
                   'static void mk8_cw_clear(void) {\n')
    execute.write_text(text, encoding='utf-8')
    # Texture uploads copy guest memory on the CPU too (textures.c), from inside draw setup: counted, not waited on.
    textures = runtime/'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    entry = 'static void upload_texture(NvnTexture *t, TexBackend *b) {\n    if (!t->addr || t->target == 10) return;\n'
    text = replace(text, entry, entry + '''    {
        extern bool mk8_cw_overlaps(uint64_t addr, uint64_t size);
        static uint64_t mk8_tex_unguarded;
        if (mk8_cw_overlaps(t->addr, nvn_texture_storage_size(t)) && (mk8_tex_unguarded++ < 8 || mk8_tex_unguarded % 1000 == 0))
            LOG("[mk8] texture upload of %llx (%ux%u fmt %#x) while a dispatch may still write it (%llu)\\n",
                (unsigned long long)t->addr, t->width, t->height, t->format, (unsigned long long)mk8_tex_unguarded);
    }
''')
    textures.write_text(text, encoding='utf-8')

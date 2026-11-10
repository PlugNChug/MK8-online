"""Balance descriptor ownership without invalidating recorded or queued commands.

Keep the existing command-list retirement policy. Finalizing a guest pool drops only
its owner reference; commands and the persistent executor keep it alive as needed.
Reboot cleanup runs after the translator drains and before the backend resets images.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError(f'Descriptor lifetime boundary changed: {old[:90]}')
    return text.replace(old, new)


TRACK = '''
// Track unfinished recordings and calls-only lists too, so reboot can release every owner.
typedef struct Mk8CmdOwner { NvnCmdBuf *cb; struct Mk8CmdOwner *next; } Mk8CmdOwner;
static Mk8CmdOwner *mk8_cmd_owners;
static pthread_mutex_t mk8_cmd_owner_lock = PTHREAD_MUTEX_INITIALIZER;
static void mk8_command_buffer_track(NvnCmdBuf *cb) {
    if (!cb) FATAL("Command buffer allocation failed\\n");
    Mk8CmdOwner *owner = malloc(sizeof *owner);
    if (!owner) FATAL("Command buffer tracking allocation failed\\n");
    owner->cb = cb;
    pthread_mutex_lock(&mk8_cmd_owner_lock);
    owner->next = mk8_cmd_owners; mk8_cmd_owners = owner;
    pthread_mutex_unlock(&mk8_cmd_owner_lock);
}
static void mk8_command_buffer_untrack(NvnCmdBuf *cb) {
    if (!cb) return;
    pthread_mutex_lock(&mk8_cmd_owner_lock);
    Mk8CmdOwner **p = &mk8_cmd_owners;
    while (*p && (*p)->cb != cb) p = &(*p)->next;
    Mk8CmdOwner *owner = *p;
    if (owner) *p = owner->next;
    pthread_mutex_unlock(&mk8_cmd_owner_lock);
    free(owner);
}
'''

RESET_COMMANDS = '''
// Reboot only: no guest threads, no translation in flight, GPU idle. List categories are disjoint owners.
void mk8_commands_reset_owned(void) {
    pthread_mutex_lock(&mk8_cmd_owner_lock);
    Mk8CmdOwner *owner = mk8_cmd_owners;
    mk8_cmd_owners = NULL;
    pthread_mutex_unlock(&mk8_cmd_owner_lock);
    while (owner) {
        Mk8CmdOwner *next = owner->next;
        NvnCmdBuf *cb = owner->cb;
        mk8_release_records(cb->buf, cb->buf_used);
        free(cb->buf);
        for (int i = 0; i < 64; i++) if (cb->calls_only[i]) mk8_free_list(cb->calls_only[i]);
        free(cb);
        free(owner);
        owner = next;
    }
    pthread_mutex_lock(&list_lock);
    for (unsigned i = 0; i < LIST_BUCKETS; i++) {
        ListInfo *l = list_buckets[i];
        list_buckets[i] = NULL;
        while (l) {
            ListInfo *next = l->next;
            mk8_free_list(l->mem);
            free(l);
            l = next;
        }
    }
    while (grave_tail < grave_head) mk8_free_list(graveyard[grave_tail++ % NGRAVE].mem);
    memset(graveyard, 0, sizeof graveyard);
    grave_head = grave_tail = list_clock = 0;
    pthread_mutex_unlock(&list_lock);
}
'''


def prepare(runtime):
    header = runtime/'nvn/nvn.h'
    text = replace(header.read_text(encoding='utf-8'), '} NvnDescPool;', '''    unsigned mk8_pool_refs;
    bool mk8_texture, mk8_finalized;
    struct NvnDescPool *mk8_next;
} NvnDescPool;
NvnDescPool *mk8_descriptor_retain(NvnDescPool *d);
void mk8_descriptor_release(NvnDescPool *d);''')
    header.write_text(text, encoding='utf-8')

    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    anchor = 'NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];'
    text = replace(text, anchor, anchor + '\n' + (ROOT/'runtime/mk8/graphics/descriptor_refs.inc').read_text(encoding='utf-8'))
    text = replace(text, 'RT_ON_RESET(desc_pools_reset) { memset(g_texture_pools, 0, sizeof g_texture_pools); memset(g_sampler_pools, 0, sizeof g_sampler_pools); }',
                   '// Descriptor cleanup runs in backend_reset before texture backends are invalidated.')
    text = replace(text, '    NvnDescPool *d = calloc(1, sizeof *d);', '''    if (count <= 0 || (size_t)count > SIZE_MAX / entry) FATAL("Invalid descriptor pool size\\n");
    NvnDescPool *d = calloc(1, sizeof *d);
    if (!d) FATAL("Descriptor pool allocation failed\\n");''')
    text = replace(text, '    d->entries = calloc(count, entry);', '''    d->entries = calloc(count, entry);
    if (!d->entries) FATAL("Descriptor entries allocation failed\\n");
    mk8_descriptor_created(d, reg == g_texture_pools);''')
    text = replace(text, '    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }', '''    pthread_mutex_lock(&mk8_desc_lock);
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    pthread_mutex_unlock(&mk8_desc_lock);''')
    for kind in ('Texture', 'Sampler'):
        text = replace(text, f'NVN(nvn{kind}PoolFinalize) {{ (void)c; }}', f'''NVN(nvn{kind}PoolFinalize) {{
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    SET_HOST(A(0), 0);
    mk8_descriptor_finalized(d);
}}''')
    text = replace(text, '        mk8_textures_drain();  //', '''        extern void mk8_descriptors_drain(void);
        mk8_descriptors_drain();
        mk8_textures_drain();  //''')
    text = replace(text, 'bool mk8_async_submit_hook(int n, const uint64_t *handles) { return mk8_async_submit(n, handles); }', '''bool mk8_async_submit_hook(int n, const uint64_t *handles) {
    if (!mk8_async_on()) {
        extern void mk8_descriptors_drain(void);
        mk8_descriptors_drain();
    }
    return mk8_async_submit(n, handles);
}''')
    objects.write_text(text, encoding='utf-8')

    commands = runtime/'nvn/cmdbuf.c'
    text = commands.read_text(encoding='utf-8')
    text = replace(text, 'static NvnCmdBuf *cb_of(', TRACK + '\nstatic NvnCmdBuf *cb_of(')
    text = replace(text, '    cb->guest = A(0);', '    mk8_command_buffer_track(cb);\n    cb->guest = A(0);')
    text = replace(text, '    free(cb);  // its remaining lists were transferred to the graveyard',
                   '    mk8_command_buffer_untrack(cb);\n    free(cb);  // its remaining lists were transferred to the graveyard')
    bind = '    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);'
    text = replace(text, bind, bind + '\n    mk8_command_pool(r, (NvnDescPool *)(uintptr_t)r->x[0]);', 2)
    commands.write_text(text + RESET_COMMANDS, encoding='utf-8')

    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    text = replace(text, 'static void st_defaults(void) {\n    mk8_rt_release(&st.rt);', '''// The shadow scan borrows command/state references; only the persistent executor owns bindings.
void mk8_descriptor_bindings_reset(void) {
    mk8_descriptor_release(st.tpool);
    mk8_descriptor_release(st.spool);
    st.tpool = st.spool = NULL;
}
void mk8_executor_owners_reset(void) {
    mk8_descriptor_bindings_reset();
    mk8_rt_release(&st.rt);
    memset(&st.rt, 0, sizeof st.rt);
}
static void st_defaults(void) {
    mk8_executor_owners_reset();''')
    for command, field in [('TEXTURE', 'tpool'), ('SAMPLER', 'spool')]:
        text = replace(text, f'        case CMD_SET_{command}_POOL: s->{field} = (NvnDescPool *)(uintptr_t)r->x[0]; return true;',
                       f'''        case CMD_SET_{command}_POOL: {{
            NvnDescPool *next = (NvnDescPool *)(uintptr_t)r->x[0];
            if (s == &st) {{ mk8_descriptor_retain(next); mk8_descriptor_release(s->{field}); }}
            s->{field} = next;
            return true;
        }}''')
    execute.write_text(text, encoding='utf-8')

    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    text = replace(text, '    g_last_presented = t;', '''    NvnTexture *previous_presented = g_last_presented;
    g_last_presented = mk8_texture_retain(t);
    mk8_texture_release(previous_presented);''')
    text = replace(text, '''void backend_reset(void) {
    if (!g_vk.dev) return;
    device_wait_idle();''', '''void backend_reset(void) {
    if (!g_vk.dev) return;
    // Guest threads have stopped. Drain translation before taking its backend state on this thread.
    extern void mk8_async_drain_hook(void);
    extern void mk8_commands_reset_owned(void);
    extern void mk8_executor_owners_reset(void);
    extern void mk8_descriptors_reset(void);
    extern void mk8_textures_drain(void);
    mk8_async_drain_hook();
    device_wait_idle();
    mk8_commands_reset_owned();
    mk8_executor_owners_reset();
    mk8_texture_release(g_last_presented);
    g_last_presented = NULL;
    mk8_descriptors_reset();
    mk8_textures_drain();''')
    device.write_text(text, encoding='utf-8')
    main = runtime/'main.c'
    text = main.read_text(encoding='utf-8')
    text = replace(text, '    kernel_stop_threads(reboot_idle);', '''    kernel_stop_threads(reboot_idle);
    extern void mk8_async_drain_hook(void);
    mk8_async_drain_hook();  // no translation may overlap the main-thread reboot renderer''')
    main.write_text(text, encoding='utf-8')

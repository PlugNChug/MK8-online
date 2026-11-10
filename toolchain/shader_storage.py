"""Prevent saturated shader lookup loops and orphaned prefetch work in long sessions."""


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise ValueError(f'Shader storage boundary changed: {old[:90]}')
    return text.replace(old, new)


def table(name, value_type, field, initial, shift):
    # Only the translator accesses these lookup tables. Rehash pointers, never the jobs/objects themselves.
    return f'''// MK8 shader storage: {name} (translator-owned, grow before 75% occupancy).
typedef struct {{ uint64_t key; {value_type} *{field}; }} Mk8_{name}_entry;
static Mk8_{name}_entry *{name};
static uint32_t {name}_capacity, {name}_count;
static uint32_t mk8_{name}_find(Mk8_{name}_entry *entries, uint32_t capacity, uint64_t key) {{
    uint32_t slot = (uint32_t)(key ^ (key >> {shift})) & (capacity - 1);
    for (uint32_t probe = 0; probe < capacity; probe++, slot = (slot + 1) & (capacity - 1))
        if (!entries[slot].key || entries[slot].key == key) return slot;
    FATAL("Shader table unexpectedly full: {name}\\n");
}}
static uint32_t mk8_{name}_slot(uint64_t key) {{
    if (!key) FATAL("Zero shader table key\\n");
    if ({name}_capacity) {{
        uint32_t slot = mk8_{name}_find({name}, {name}_capacity, key);
        if ({name}[slot].key || {name}_count < {name}_capacity - {name}_capacity / 4) return slot;
    }}
    if ({name}_capacity > UINT32_MAX / 2) FATAL("Shader table size overflow\\n");
    uint32_t capacity = {name}_capacity ? {name}_capacity * 2 : {initial};
    if ((size_t)capacity > SIZE_MAX / sizeof *{name}) FATAL("Shader table allocation overflow\\n");
    Mk8_{name}_entry *grown = calloc(capacity, sizeof *grown);
    if (!grown) FATAL("Shader table allocation failed: {name}\\n");
    for (uint32_t i = 0; i < {name}_capacity; i++)
        if ({name}[i].key) grown[mk8_{name}_find(grown, capacity, {name}[i].key)] = {name}[i];
    free({name});
    {name} = grown;
    {name}_capacity = capacity;
    return mk8_{name}_find({name}, capacity, key);
}}
'''


def prepare(runtime):
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    for name, typ, field, initial, shift, key, slot, occurrences in (
        ('prog_tab', 'Prog', 'prog', 4096, 31, 'h', 'slot', 1),
        ('pipe_tab', 'Pipe', 'pipe', 8192, 29, 'ph', 'pslot', 2),
    ):
        constant = 'PROG_TAB' if name == 'prog_tab' else 'PIPE_TAB'
        old = f'#define {constant} {initial}\nstatic struct {{ uint64_t key; {typ} *{field}; }} {name}[{constant}];'
        text = replace(text, old, table(name, typ, field, initial, shift))
        old = (f'    uint32_t {slot} = (uint32_t)({key} ^ ({key} >> {shift})) & ({constant} - 1);\n'
               f'    while ({name}[{slot}].key && {name}[{slot}].key != {key}) {slot} = ({slot} + 1) & ({constant} - 1);')
        text = replace(text, old, f'    uint32_t {slot} = mk8_{name}_slot({key});', occurrences)
        text = replace(text, f'{name}[{slot}].key = {key};', f'{name}_count++;\n    {name}[{slot}].key = {key};', occurrences)
        text = replace(text, f'    memset({name}, 0, sizeof {name});',
                       f'    if ({name}) memset({name}, 0, {name}_capacity * sizeof *{name});\n    {name}_count = 0;')
    boundary = '    if (atomic_load(&pr->state) == BUILD_PENDING) {  // compiling: requested when it\'s done (prog_state)\n'
    text = replace(text, boundary, boundary + '''        // Repeated draws may request the same pipeline while a slow shader is still compiling.
        for (PipeReq *pending = pr->waiting; pending; pending = pending->next)
            if (pending->hash == ph) { pthread_mutex_unlock(&g_wait_lock); return; }
''')
    text = replace(text, '        PipeReq *q = malloc(sizeof *q);',
                   '        PipeReq *q = malloc(sizeof *q);\n        if (!q) FATAL("Pipeline request allocation failed\\n");')
    execute.write_text(text, encoding='utf-8')
    buildsync = runtime/'nvn/buildsync.c'
    text = buildsync.read_text(encoding='utf-8')
    text = replace(text, 'else if (npend >= PEND_TAB - 1) r = fresh;  // (full: not kept; the draw builds it on demand)',
                   'else if (npend >= PEND_TAB - 1) r = NULL;  // full: caller releases fresh; draw builds on demand')
    # The worker must not keep a dequeued, still-stealable pointer after unlocking: an inline waiter can finish and
    # let its caller free the containing object before the worker tries the CAS.
    text = replace(text, '''        unlink_job(q, j);
        pthread_mutex_unlock(&q->lock);
        int want = JOB_QUEUED;
        if (atomic_compare_exchange_strong(&j->state, &want, JOB_RUNNING)) run_job(j);  // (else a waiter took it)''',
                   '''        unlink_job(q, j);
        int want = JOB_QUEUED;
        bool claimed = atomic_compare_exchange_strong(&j->state, &want, JOB_RUNNING);
        pthread_mutex_unlock(&q->lock);
        if (claimed) run_job(j);''')
    text = replace(text, '''    int want = JOB_QUEUED;
    if (atomic_compare_exchange_strong(&j->state, &want, JOB_RUNNING)) {
        pthread_mutex_lock(&q->lock);
        unlink_job(q, j);
        pthread_mutex_unlock(&q->lock);
        run_job(j);''', '''    int want = JOB_QUEUED;
    pthread_mutex_lock(&q->lock);
    bool claimed = atomic_compare_exchange_strong(&j->state, &want, JOB_RUNNING);
    if (claimed) unlink_job(q, j);
    pthread_mutex_unlock(&q->lock);
    if (claimed) {
        run_job(j);''')
    buildsync.write_text(text, encoding='utf-8')
    header = runtime/'nvn/buildsync.h'
    text = header.read_text(encoding='utf-8')
    text = replace(text, '// the entry there, or fresh (now there)', '// existing entry, fresh (inserted), or NULL when full (caller frees fresh)')
    header.write_text(text, encoding='utf-8')

"""Shader compile-ahead off by default for MK8 (LARECOMP_AHEAD=1 or =force turns it on).

Automated Time Trial runs at ~53 fps: with compile-ahead, 8 of 12 runs (171-184) came up blown out from the first 3D
scene on; with LARECOMP_AHEAD=0, 1 of 8 (185-192). Both switches the variable controls are inverted the same way:
the per-submission shader prediction (exec.c) and the RomFS shader compile-ahead (ahead.c).
"""

SWITCHES = [
    ('vulkan/exec.c',
     '{ const char *m = getenv("LARECOMP_AHEAD"); shader_ahead_set_enable(!m || strcmp(m, "0")); }',
     '{ const char *m = getenv("LARECOMP_AHEAD"); shader_ahead_set_enable(m && strcmp(m, "0")); /* MK8: opt-in */ }'),
    ('vulkan/ahead.c',
     'if (atomic_exchange(&vah.started, 1) || (mode && !strcmp(mode, "0")) || !g_romfs_dir) return;',
     'if (atomic_exchange(&vah.started, 1) || !mode || !strcmp(mode, "0") || !g_romfs_dir) return;  /* MK8: opt-in */'),
]


def prepare(runtime):
    for relative, old, new in SWITCHES:
        path = runtime/'nvn'/relative
        text = path.read_text(encoding='utf-8')
        if text.count(old) != 1:
            raise ValueError(f'Shader compile-ahead boundary changed: {relative}')
        path.write_text(text.replace(old, new), encoding='utf-8')
    device = runtime/'nvn/vulkan/device.c'
    text = device.read_text(encoding='utf-8')
    old = '{ fflush(stderr); _exit(0); }'
    if text.count(old) != 2:
        raise ValueError('Screenshot exit boundaries changed')
    # Use the existing exit handler, which saves the Vulkan pipeline cache before terminating.
    device.write_text(text.replace(old, '{ fflush(stderr); exit(0); }'), encoding='utf-8')
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    old = 'atomic_load(&stat_compiles));'
    if text.count(old) != 1:
        raise ValueError('Hitch compilation counter boundary changed')
    text = text.replace(old, 'build_inflight());')
    changes = [
        ('static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;',
         'static uint64_t cum_prefetch;\nstatic uint64_t cum_exec, cum_wait, cum_draws, cum_submits;'),
        ('    g_prefetch.scan_ns += SDL_GetTicksNS() - t0;',
         '    uint64_t elapsed = SDL_GetTicksNS() - t0;\n    g_prefetch.scan_ns += elapsed;\n    cum_prefetch += elapsed;'),
        ('static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;',
         'static uint64_t last_t, l_exec, l_wait, l_draws, l_subs, l_prefetch;'),
        ('    if (last_t && now - last_t > 50000000) {', '''    static int frame_times = -1;
    if (frame_times < 0) frame_times = getenv("MK8_FRAME_TIMES") != NULL;
    uint64_t pre = cum_prefetch - l_prefetch;
    if (last_t && frame_times)
        LOG("[frame] %llu %.3f %.3f %.3f %.3f %.3f\\n", (unsigned long long)frame,
            (now - last_t) / 1e6, (cum_exec - l_exec) / 1e6, pre / 1e6, present_ns / 1e6,
            ((double)(now - last_t) - (cum_exec - l_exec) - pre - present_ns) / 1e6);
    if (last_t && now - last_t > 50000000) {'''),
        ('+ present %.1f + other %.1f; %llu draws, ', '+ prefetch %.1f + present %.1f + other %.1f; %llu draws, '),
        ('ex / 1e6, wt / 1e6, present_ns / 1e6,', 'ex / 1e6, wt / 1e6, pre / 1e6, present_ns / 1e6,'),
        ('((double)dt - ex - present_ns) / 1e6,', '((double)dt - ex - pre - present_ns) / 1e6,'),
        ('    last_t = now; l_exec = cum_exec;', '    l_prefetch = cum_prefetch;\n    last_t = now; l_exec = cum_exec;'),
    ]
    for old, new in changes:
        if text.count(old) != 1:
            raise ValueError(f'Frame timing boundary changed: {old[:80]}')
        text = text.replace(old, new)
    execute.write_text(text, encoding='utf-8')

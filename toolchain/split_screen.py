"""Local multiplayer (split screen): diagnostics for the controllers of players 2-4 as MK8 4.0.1 reads them.

MK8_TRACE_PADS=1 counts the controller state reads per Npad id and state type (FullKey, JoyDual, JoyLeft/Right,
Handheld, plural states) and the style set queries, and logs the buttons player 2 was given, every 1200 samples.
MK8_TRACE_IMPORTS=1 lists the game's nn::hid imports and whether the host or the guest SDK answers each.

Findings (08/11/2026): after the controller applet ("2 player(s)"), the game keeps reading FullKey states of ids 0
and 1 once per frame and gets both players' buttons, yet its two-player character select ignores every press of
both players (cursor, confirm, back). Signalling the style set events after the applet and writing the player count
as a 32-bit int did not change that; both were removed. The cause is still in the game's state after the applet.
"""

TRACE = '''    *(int64_t *)s = ++sampling;
    {   // MK8 (toolchain/split_screen.py): MK8_TRACE_PADS=1, state reads per Npad id and state type
        static int on = -1;
        static uint64_t calls[5][6], last, p2;
        if (on < 0) on = getenv("MK8_TRACE_PADS") != NULL;
        if (on) {
            calls[id < 4 ? id : 4][mk8_pad_kind < 6 ? mk8_pad_kind : 5]++;
            if (id == 1) p2 |= extra_buttons[0] | script2_buttons;
            if (sampling - last >= 1200) {
                last = sampling;
                for (int i = 0; i < 5; i++)
                    if (calls[i][0] + calls[i][1] + calls[i][2] + calls[i][3] + calls[i][4] + calls[i][5])
                        LOG("[mk8:pads] id %s: fullkey %llu dual %llu left %llu right %llu handheld %llu states %llu, style queries %llu\\n",
                            i < 4 ? (const char *[]){"0", "1", "2", "3"}[i] : "other", (unsigned long long)calls[i][0],
                            (unsigned long long)calls[i][1], (unsigned long long)calls[i][2], (unsigned long long)calls[i][3],
                            (unsigned long long)calls[i][4], (unsigned long long)calls[i][5], (unsigned long long)mk8_style_queries[i]);
                LOG("[mk8:pads] player 2 buttons seen %#llx\\n", (unsigned long long)p2);
                p2 = 0;
            }
        }
    }
'''
KINDS = [
    ('HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { FILL1; }',
     'HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { mk8_pad_kind = 0; FILL1; }'),
    ('HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { FILL1; }',
     'HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { mk8_pad_kind = 1; FILL1; }'),
    ('HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { FILL1; }',
     'HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { mk8_pad_kind = 2; FILL1; }'),
    ('HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { FILL1; }',
     'HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { mk8_pad_kind = 3; FILL1; }'),
    ('HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { FILL1; }',
     'HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { mk8_pad_kind = 4; FILL1; }'),
    ('static void fill_states(Cpu *c) {\n', 'static void fill_states(Cpu *c) {\n    mk8_pad_kind = 5;\n'),
    ('    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];\n    static int nlog;\n    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\\n", id);\n',
     '    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];\n    mk8_style_queries[id < 4 ? id : 4]++;  // (toolchain/split_screen.py)\n    static int nlog;\n    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\\n", id);\n'),
    ('static void fill_state(Cpu *c, uint8_t *s, uint32_t id, bool single) {\n',
     'static int mk8_pad_kind;  // (toolchain/split_screen.py: which GetNpadState the game called)\nstatic uint64_t mk8_style_queries[5];\nstatic void fill_state(Cpu *c, uint8_t *s, uint32_t id, bool single) {\n'),
]


IMPORTS = ('    HostFn impl = hle_lookup_impl(name);\n',
           '''    HostFn impl = hle_lookup_impl(name);
    if (getenv("MK8_TRACE_IMPORTS") && !strncmp(name, "_ZN2nn3hid", 10))  // (toolchain/split_screen.py)
        LOG("[mk8:import] %s %s: %s\\n", m->name, impl ? "host" : "guest SDK", name);
''')


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Controller input boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    source = runtime/'hle/input.c'
    text = source.read_text(encoding='utf-8')
    text = replace(text, '    *(int64_t *)s = ++sampling;\n', TRACE)
    for old, new in KINDS:
        text = replace(text, old, new)
    source.write_text(text, encoding='utf-8')
    loader = runtime/'loader.c'
    loader.write_text(replace(loader.read_text(encoding='utf-8'), *IMPORTS), encoding='utf-8')

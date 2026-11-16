"""Run the loop of the game's Yaz0 decoder natively (runtime/mk8/support/yaz0.c).

f_7000a88a44 (main, MK8 4.0.1) decompresses the next screen's archives (two of ~92 MB) on a worker thread of priority
52 at the end of every race. As recompiled code its byte loop took ~0.5 s, and with no preemption inside guest code the
worker held its emulated core (core 2) meanwhile: threads of better priority there waited ~260 ms for it (14/11/2026;
MK8_SAMPLE put all of that worker's CPU in this function). Natively each archive takes ~0.2 s. The main thread's wait
on core 0 at the same moment is another worker (toolchain/core_preempt.py). The guest keeps the header (magic and size
through its own byte-order helpers, the -1 / -2 returns, the `size < 1` exit); one line before its loop hands the
loop to mk8_yaz0_loop, which returns 0 with MK8_YAZ0_GUEST=1 so the guest loop runs. The generated unit is verified by
its SHA-256 and changed only in the build copy.
"""
import hashlib

UNIT = 'main_230.c'
SHA256 = '01e1a6019c1191f1708156e3c434fbe3c328cc11e4ff16e1d46e2b80b7d9e3cc'
ENTRY = 'void f_7000a88a44(Cpu *c) {'
LOOP = ('    if (COND_LT(c)) { goto L_7000a88b90; }\n'
        '    c->x[11] = (uint32_t)(0u | 0u);\n'
        '    c->x[10] = (uint32_t)(0u | 0u);\n'
        '    c->x[9] = (uint64_t)(c->x[20] - 0x1ull);\n'
        '    c->x[13] = (uint32_t)(0u | 0x10ull);\n'
        'L_7000a88af0:;\n')
HOOK = '    if (mk8_yaz0_loop(c)) goto L_7000a88b90;  // MK8 (toolchain/yaz0_native.py)\n'
DECLARATION = 'int mk8_yaz0_loop(Cpu *c);  // MK8 (toolchain/yaz0_native.py): runtime/mk8/support/yaz0.c\n'


def patch(text):
    if text.count(ENTRY) != 1 or text.count(LOOP) != 1 or 'mk8_yaz0_loop' in text:
        raise ValueError('Yaz0 decoder boundary changed')
    head, loop_rest = LOOP.split('    c->x[11]', 1)
    text = text.replace(LOOP, head + HOOK + '    c->x[11]' + loop_rest)
    return text.replace(ENTRY, DECLARATION + ENTRY)


def patch_unit(src, output):
    """The build copy of main_230.c with the hook; any other unit unchanged."""
    if src.name != UNIT or src.parent.name != 'main':
        return src
    raw = src.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise ValueError('Native Yaz0 loop requires the verified MK8 4.0.1 generated unit')
    target = output/'yaz0'/src.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(patch(raw.decode('utf-8').replace('\r\n', '\n')), encoding='utf-8')  # (generated with CRLF)
    return target

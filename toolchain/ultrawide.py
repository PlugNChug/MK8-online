"""Opt-in solo camera hook in the verified MK8 4.0.1 render-layer update.

The build copy calls the runtime after the game's viewport/aspect updates, while its
manager mutex is held, before projection consumers and frustum construction.
"""
from guest_source import verified_bytes

UNIT = 'main_077.c'
SHA256 = 'e2acde446b23167a4ed1bb4f211d5888773d245e4e2e9d60352d8b0f0bd5dce4'
ENTRY = 'void f_70003cc218(Cpu *c) {'
BOUNDARY = ('    c->x[30] = 0x70003cc4e0ull; f_70003cc8ac(c);\n'
            '    c->x[0] = (uint64_t)(0ull | c->x[20]);\n'
            '    c->x[30] = 0x70003cc4e8ull; f_70003ccb1c(c);\n')
HOOK = '    mk8_ultrawide_camera(c->x[20]);  // MK8 (toolchain/ultrawide.py)\n'
DECLARATION = 'void mk8_ultrawide_camera(uint64_t manager);\n'


def patch(text):
    if text.count(ENTRY) != 1 or 'mk8_ultrawide_camera' in text:
        raise ValueError('Ultrawide camera function boundary changed')
    start = text.index(ENTRY)
    end = text.find('\nvoid ', start + len(ENTRY))
    if end < 0:
        end = len(text)
    function = text[start:end]
    if function.count(BOUNDARY) != 1 or text.count(BOUNDARY) != 1:
        raise ValueError('Ultrawide camera update boundary changed')
    first, rest = BOUNDARY.split('\n', 1)
    function = function.replace(BOUNDARY, first + '\n' + HOOK + rest)
    return text[:start] + DECLARATION + function + text[end:]


def patch_unit(src, output):
    if src.name != UNIT or src.parent.name != 'main':
        return src
    raw = verified_bytes(src, SHA256, 'Ultrawide camera requires the verified MK8 4.0.1 generated unit')
    text = patch(raw.decode('utf-8').replace('\r\n', '\n'))
    target = output/'ultrawide'/src.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8', newline='\r\n')
    return target

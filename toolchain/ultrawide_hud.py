"""Wrap the verified 4.0.1 layout calculation after animation updates."""
import hashlib

UNIT = 'main_164.c'
SHA256 = '6794319ad61606e5a2910676d06eb26f3ac3f8b748f61a2302b798f4016b4f00'
ENTRY = 'void f_7000732600(Cpu *c) {'
BOUNDARY = '    c->pc = c->x[8]; c->x[30] = 0x7000732668ull; rt_dispatch(c);\n'
HOOK = BOUNDARY.replace('rt_dispatch(c)', 'mk8_ultrawide_hud(c, c->x[21])')
DECLARATION = 'void mk8_ultrawide_hud(Cpu *c, uint64_t layout);\n'


def patch(text):
    if text.count(ENTRY) != 1 or 'mk8_ultrawide_hud' in text:
        raise ValueError('Ultrawide HUD function boundary changed')
    start = text.index(ENTRY)
    end = text.find('\nvoid ', start + len(ENTRY))
    if end < 0:
        end = len(text)
    function = text[start:end]
    if function.count(BOUNDARY) != 1 or text.count(BOUNDARY) != 1:
        raise ValueError('Ultrawide HUD traversal boundary changed')
    return text[:start] + DECLARATION + function.replace(BOUNDARY, HOOK) + text[end:]


def patch_unit(src, output):
    if src.name != UNIT or src.parent.name != 'main':
        return src
    raw = src.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise ValueError('Ultrawide HUD requires the verified MK8 4.0.1 generated unit')
    text = patch(raw.decode('utf-8').replace('\r\n', '\n'))
    target = output/'ultrawide_hud'/src.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8')
    return target

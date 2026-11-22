"""Opt-in solo camera hook in the verified MK8 4.0.1 render-layer update.

The build copy calls the runtime after the game's viewport/aspect updates, while its
manager mutex is held, before projection consumers and frustum construction.
"""
from guest_source import verified_bytes

UNIT = 'main_077.c'
SHA256 = 'e2acde446b23167a4ed1bb4f211d5888773d245e4e2e9d60352d8b0f0bd5dce4'
DISTANCE_UNIT = 'main_038.c'
DISTANCE_SHA256 = '76be2df04f02533aeb2db346acecd1a9fa2ff10d52535f0a422f4901bbad0e30'
DISTANCE_SITES = (
    ('f_70001189f8', '    c->x[30] = 0x7000118f4cull; f_700061ebf4(c);\n'
     '    { uint64_t ea = c->sp + 0xcull; c->v[0] = LD32(ea); }\n', 0, 'c->x[30] == 0x7000119838ull || c->x[30] == 0x70001188b0ull'),
    ('f_700011bc08', 'L_700011bde8:;\n', 8, 'c->x[30] == 0x70001193acull'),
)
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


def patch_distance(text):
    # Each solver still chooses its direction, target and collision-limited eye position.
    declaration = 'float mk8_driver_camera_distance(uint64_t owner, float length);\n'
    if 'mk8_driver_camera_distance' in text:
        raise ValueError('Driver camera distance already patched')
    for function, boundary, register, caller in DISTANCE_SITES:
        entry = f'void {function}(Cpu *c) {{'
        if text.count(entry) != 1 or text.count(boundary) != 1:
            raise ValueError('Driver camera solver boundary changed')
        start = text.index(entry)
        end = text.find('\n}', start)
        if end < 0 or boundary not in text[start:end+1]:
            raise ValueError('Driver camera distance outside expected solver')
        # The rear solver is shared with other collision queries. Only the registered camera writer scales it.
        scalar = f'VSF32({register})'
        text = text.replace(entry, declaration + entry + f'\n    const int driver_camera = {caller};', 1)
        declaration = ''
        text = text.replace(boundary, boundary + f'    if (driver_camera) {scalar} = mk8_driver_camera_distance(c->x[21], {scalar});\n')
    return text


def patch_unit(src, output):
    if src.name not in (UNIT, DISTANCE_UNIT) or src.parent.name != 'main':
        return src
    expected, adapt = (SHA256, patch) if src.name == UNIT else (DISTANCE_SHA256, patch_distance)
    raw = verified_bytes(src, expected, 'Ultrawide camera requires the verified MK8 4.0.1 generated unit')
    text = adapt(raw.decode('utf-8').replace('\r\n', '\n'))
    target = output/'ultrawide'/src.name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8', newline='\r\n')
    return target

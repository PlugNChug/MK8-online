"""Select MK8 4.0.1's full-rate views and stabilize bloom depth during split races."""
from guest_source import verified_bytes

UNIT = 'main_078.c'
SHA256 = 'bf737d1f150f5a61d442c88b2b2b24abd11e3d5e5d8225648c59503db99f703e'
ENTRY = 'void f_70003cd5c8(Cpu *c) {\n'
BOUNDARY = ('    if (COND_NE(c)) { goto L_70003cd688; }\n'
            '    { uint64_t ea = c->x[8] + 0x4230ull; uint64_t v = LD64(ea); c->x[9] = (uint64_t)(v); }\n')
HOOK = '    if (COND_NE(c) || mk8_split_race_full_speed()) { goto L_70003cd688; }\n'
BLOOM_UNIT = 'main_154.c'
BLOOM_SHA256 = 'c8617aec96557549cebc7abb879ce5a10eaa2c7fe2075dcb4e432ba0ee5f1449'
BLOOM_ENTRY = 'void f_70006b5a20(Cpu *c) {\n'
BLOOM_BOUNDARY = ('    if (((c->x[10] >> 11) & 1) != 0) { goto L_70006b5a70; }\n'
                  '    if (((c->x[10] >> 0) & 1) != 0) { goto L_70006b5a78; }\n')
# Only bloom's normalized-depth sampler visits this selector. Bits 11 and 0 mark
# the current view's available half/full maps. Preserve authored half-light scenes.
BLOOM_HOOK = ('    if (COND_HI(c) && (c->x[10] & 0x801) == 0x801 && mk8_split_race_full_speed()) {\n'
              '        uint64_t config = LD64(bloom_owner + 8);\n'
              '        if (config && !LD8(config + 0x16f0)) { goto L_70006b5a78; }\n'
              '    }\n')


def patch(text, unit=UNIT):
    entry, boundary, replacement, label = ENTRY, BOUNDARY, HOOK + BOUNDARY.split('\n', 1)[1], 'L_70003cd688:;'
    if unit == BLOOM_UNIT:
        entry, boundary = BLOOM_ENTRY, BLOOM_BOUNDARY
        replacement, label = BLOOM_HOOK + BLOOM_BOUNDARY, 'L_70006b5a78:;'
    if text.count(entry) != 1 or text.count(boundary) != 1 or 'mk8_split_race_full_speed' in text:
        raise ValueError('Split-screen race setup boundary changed')
    start = text.index(entry)
    end = text.find('\nvoid ', start + len(entry))
    if end < 0:
        end = len(text)
    function = text[start:end]
    if boundary not in function or function.count(label) != 1:
        raise ValueError('Split-screen native full-rate branch changed')
    function = function.replace(boundary, replacement)
    if unit == BLOOM_UNIT:
        function = function.replace(entry, entry + '    const uint64_t bloom_owner = c->x[0];\n')
    return text[:start] + '_Bool mk8_split_race_full_speed(void);\n' + function + text[end:]


def patch_unit(src, output, original=None):
    original = src if original is None else original
    if original.name not in (UNIT, BLOOM_UNIT) or original.parent.name != 'main':
        return src
    expected = SHA256 if original.name == UNIT else BLOOM_SHA256
    if src.name != original.name:
        raise ValueError('Split-screen cadence received an unexpected generated unit')
    verified_bytes(original, expected, 'Split-screen cadence requires the verified MK8 4.0.1 generated unit')
    target = output / 'split_race_cadence' / src.name
    # main_078 also owns the Mii model heap guard. Patch the preceding adapter's output.
    text = patch(src.read_text(encoding='utf-8'), original.name)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8', newline='\r\n')
    return target

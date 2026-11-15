"""Read MK8 4.0.1's cups from a larger area (runtime/mk8/support/cups.c; CTGP phase B, docs/STATUS.md).

The cups are 0x80-byte entries at the start of one object with room for 25 (f_70003f96b0 builds 24 race cups and the
battle cup). Code reads an entry as `object + (index << 7)` after clamping the index below 25 (`subs32(index, 0x19)` and
a conditional select). In the build copies of the verified units:
- the builder (main_081.c) keeps its code and, at its one return, hands its object to mk8_cups_built, which copies the
  entries into a runtime area with room for 41;
- in the 9 functions that read cups, each clamp allows 41 (0x29) and its `base + (index << 7)` becomes
  mk8_cup_entry(base, index): that area for the builder's object, the original arithmetic (and limit) otherwise.
Each unit is checked by its SHA-256 and each function by its exact number of clamps, every one paired with one entry
computation; anything else is refused. main_082.c arrives as toolchain/experimental.py's copy (its Mii dialog hook) and
is accepted by that copy's SHA-256. The game's behaviour does not change until cups are added.
"""
import hashlib
import re

# main_082.c as toolchain/experimental.py leaves it (the Mii dialog hook, in f_70004017b8, not a cup reader)
AFTER_EXPERIMENTAL = {'main_082.c': 'f861292e6a6cb546924ca6f555bd28dce080afebdf251a7c8c9e3d9b8da7c814'}

BUILDER = '70003f96b0'
# unit -> (SHA-256 of the generated unit, {function: clamps that read a cup entry})
UNITS = {
    'main_081.c': ('70f5290b6efe074f0d7ecdca6ba8ee5d3342339563dd0732bb6c21e590122a19', {}),
    'main_082.c': ('c6d46af3e71b558fa38894d3b26de9569671c93a026f7a0cce99eb2e16707082',
                   {'70003fb2b4': 5, '70003fb638': 9, '70003fc0c0': 10}),
    'main_099.c': ('eecbee2a09ecc4bc86992e72e9d00b2723a4e5c082f2aa11e76591d0971c5322',
                   {'7000489610': 4, '700048a5e4': 2, '700048b5d0': 2, '7000494f08': 8}),
    'main_100.c': ('187122fd53c501e91020efb776088154977181fd647ba02cf084d1e9d4c518cc', {'700049890c': 1}),
    'main_119.c': ('2611d632acc04d898350952779d61c0407deee2029f1b4d1cba03f97669b9247', {'70004fb290': 1}),
}
DECLARATIONS = ('uint64_t mk8_cup_entry(uint64_t base, uint64_t index);  // MK8 (toolchain/cup_relocation.py)\n'
                'void mk8_cups_built(uint64_t object);  // MK8 (toolchain/cup_relocation.py): runtime/mk8/support/cups.c\n')
CLAMP = re.compile(r'\(void\)\(subs32\(c, \(uint32_t\)c->x\[(\d+)\], 0x19ull\)\);\n')
SELECT = 'c->x[{d}] = (uint64_t)((COND_CC(c)) ? c->x[{i}] : (uint64_t)(0ull));'
ENTRY = re.compile(r'c->x\[(\d+)\] = \(uint64_t\)\(c->x\[(\d+)\] \+ \(c->x\[(\d+)\] << 7\)\);')


def _functions(text):
    return list(re.finditer(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{\n(.*?)^\}\n', text, re.S | re.M))


def patch_function(body, expected):
    """One reader's body with its clamps widened and entry computations redirected; exactly `expected` of them."""
    lines = body.split('\n')
    sites = 0
    i = 0
    while i < len(lines):
        match = CLAMP.fullmatch(lines[i].strip() + '\n') if 'subs32' in lines[i] else None
        if not match:
            i += 1
            continue
        index = match.group(1)
        lines[i] = lines[i].replace('0x19ull', '0x29ull')
        # the select comes next (labels aside), the entry computation within the next 3 lines
        j = i + 1
        while j < len(lines) and lines[j].startswith('L_'):
            j += 1
        select = re.fullmatch(r'\s+c->x\[(\d+)\] = \(uint64_t\)\(\(COND_CC\(c\)\) \? c->x\[%s\] : \(uint64_t\)\(0ull\)\);' % index,
                              lines[j]) if j < len(lines) else None
        if not select:
            raise ValueError('Cup clamp without its select')
        dest = select.group(1)
        found = None
        for k in range(j + 1, min(j + 5, len(lines))):
            entry = ENTRY.search(lines[k])
            if entry and entry.group(3) == dest:
                found = k
                break
        if found is None:
            raise ValueError('Cup clamp without its entry computation')
        entry = ENTRY.search(lines[found])
        lines[found] = lines[found].replace(entry.group(0), 'c->x[%s] = (uint64_t)(mk8_cup_entry(c->x[%s], c->x[%s]));'
                                            % (entry.group(1), entry.group(2), entry.group(3)))
        sites += 1
        i = found + 1
    if sites != expected:
        raise ValueError(f'Cup readers changed: {sites} sites, {expected} expected')
    return '\n'.join(lines)


def patch(text, unit):
    sha, readers = UNITS[unit]
    if 'mk8_cup_entry' in text or 'mk8_cups_built' in text:
        raise ValueError('Cup relocation already applied')
    out, pos, seen = [], 0, set()
    for match in _functions(text):
        name, body = match.group(1), match.group(2)
        if name == BUILDER:
            returns = body.count('\n    return;\n')
            if returns != 1 or body.count('return') != 1:
                raise ValueError('Cup builder changed: its single return')
            entry = '    FN_ENTER(0x%sull);\n' % BUILDER
            if not body.startswith(entry):
                raise ValueError('Cup builder changed: its entry')
            body = (entry + '    const uint64_t mk8_cups_object = c->x[0];  // MK8 (toolchain/cup_relocation.py)\n'
                    + body[len(entry):].replace('\n    return;\n', '\n    mk8_cups_built(mk8_cups_object);\n    return;\n'))
            seen.add(name)
        elif name in readers:
            body = patch_function(body, readers[name])
            seen.add(name)
        else:
            continue
        out.append(text[pos:match.start(2)])
        out.append(body)
        pos = match.end(2)
    out.append(text[pos:])
    wanted = set(readers) | ({BUILDER} if unit == 'main_081.c' else set())
    if seen != wanted:
        raise ValueError(f'Cup functions missing in {unit}: {sorted(wanted - seen)}')
    result = ''.join(out)
    first = result.index('void f_')
    return result[:first] + DECLARATIONS + result[first:]


def patch_unit(src, output):
    """The build copy of a cup unit with the relocation; any other unit unchanged."""
    if src.name not in UNITS or src.parent.name not in ('main', 'experimental'):
        return src
    expected = UNITS[src.name][0] if src.parent.name == 'main' else AFTER_EXPERIMENTAL.get(src.name)
    raw = src.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError(f'Cup relocation requires the verified MK8 4.0.1 generated unit {src.name}')
    text = re.sub(r'\r+\n', '\n', raw.decode('utf-8'))  # (generated with CRLF; the experimental copy with CR CR LF)
    target = output/'cups'/src.name
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(target, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(patch(text, src.name))
    return target

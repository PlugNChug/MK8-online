"""Read MK8 4.0.1's cups from a larger area (runtime/mk8/support/cups.c; CTGP phase B, docs/STATUS.md).

The cups are 0x80-byte entries at the start of one object with room for 25 (f_70003f96b0 builds 24 race cups and the
battle cup). Code reads an entry as `object + (index << 7)` after clamping the index below 25: `subs32(index, 0x19)`, a
conditional select of that index or 0, and then, possibly after other instructions, labels, calls (the index in a
register they preserve) or a `goto` within the function, the `base + (selected << 7)` computation. In the build copies
of the verified units:
- the builder (main_081.c) keeps its code and, at its one return, hands its object to mk8_cups_built, which copies the
  entries into a runtime area with room for 41;
- in the 19 functions that read cups, each clamp allows 41 (0x29) and the computation it feeds becomes
  mk8_cup_entry(base, index): that area for the builder's object, the original arithmetic (and limit) otherwise.
Each unit is checked by its SHA-256 and each function by its exact numbers of clamps and of computations; a clamp that
cannot be followed to its computation is refused. main_082.c and main_115.c arrive as toolchain/experimental.py's copies
(its Mii dialog and LAN vote hooks, in other functions) and are accepted by those copies' SHA-256. The game's behaviour
does not change until cups are added.
"""
import hashlib
import re

BUILDER = '70003f96b0'
# unit -> (SHA-256 of the generated unit, {function: (clamps, entry computations)})
UNITS = {
    'main_081.c': ('70f5290b6efe074f0d7ecdca6ba8ee5d3342339563dd0732bb6c21e590122a19', {}),
    'main_082.c': ('c6d46af3e71b558fa38894d3b26de9569671c93a026f7a0cce99eb2e16707082',
                   {'70003fb134': (1, 1), '70003fb2b4': (5, 5), '70003fb4b8': (1, 1), '70003fb638': (9, 9),
                    '70003fc0c0': (10, 10)}),
    'main_089.c': ('a5d232ba6edd081fec6bcf15200e18d8c4507f9bb5741730a84ab1db82e2e95a', {'7000446068': (1, 1)}),
    'main_090.c': ('d0348aa87c8cacd0bf7ec3fdd023f1fdbb391751d1b3e6655d07532ecab7c6f7', {'700045b51c': (1, 1)}),
    'main_093.c': ('3c6bc7b8efeb8d3180f3ae1d334f0e5b37db6c142544d85a6d4f728274d2ddb4', {'7000470d78': (1, 1)}),
    'main_099.c': ('eecbee2a09ecc4bc86992e72e9d00b2723a4e5c082f2aa11e76591d0971c5322',
                   {'7000489610': (4, 4), '700048a5e4': (2, 2), '700048b5d0': (2, 2), '700048cef0': (1, 1),
                    '700048e190': (2, 1), '7000494f08': (8, 8)}),
    'main_100.c': ('187122fd53c501e91020efb776088154977181fd647ba02cf084d1e9d4c518cc',
                   {'70004980a0': (1, 1), '700049890c': (1, 1), '7000498e90': (1, 1), '70004a133c': (1, 1),
                    '70004a3940': (1, 1)}),
    'main_115.c': ('e7f979c9bae346fca7da9f76349d27eeec5fcc4b23c87b43d396c39b605ef048', {'70004e5bb0': (1, 1)}),
    'main_119.c': ('2611d632acc04d898350952779d61c0407deee2029f1b4d1cba03f97669b9247', {'70004fb290': (1, 1)}),
    'main_121.c': ('0ba51132a85db27e2f2bbfa04b0f24e75f432ade1142e12869f6f77873e96fe0', {'7000501778': (1, 1)}),
    'main_123.c': ('3aa03c28e54f7fbcb64f8a1159bbbe5fcd3b501cde1ba404b0ffed8363968b2f', {'700050dc6c': (1, 1)}),
}
# Units as toolchain/experimental.py leaves them (its hooks are in other functions than the cup readers)
AFTER_EXPERIMENTAL = {'main_082.c': 'f861292e6a6cb546924ca6f555bd28dce080afebdf251a7c8c9e3d9b8da7c814',
                      'main_115.c': 'cddddcba68768597f16f60322f4753f4c3e7c7ace2d9966708c5ae02d4fb758e'}
DECLARATIONS = ('uint64_t mk8_cup_entry(uint64_t base, uint64_t index);  // MK8 (toolchain/cup_relocation.py)\n'
                'void mk8_cups_built(uint64_t object);  // MK8 (toolchain/cup_relocation.py): runtime/mk8/support/cups.c\n')
CLAMP = re.compile(r'\(void\)\(subs32\(c, \(uint32_t\)c->x\[(\d+)\], 0x19ull\)\);')
ENTRY = re.compile(r'c->x\[(\d+)\] = \(uint64_t\)\(c->x\[(\d+)\] \+ \(c->x\[(\d+)\] << 7\)\);')
FLAGS = re.compile(r'subs32|subs64|adds32|adds64|ands32|ands64|nzcv|ccmp|COND_')
LABEL = re.compile(r'L_\w+:;')
CALL = re.compile(r'\bf_[0-9a-f]+\(c\)|rt_dispatch|rt_svc')
FOLLOW = 120  # lines (labels aside) from a select to its computation


def _functions(text):
    return list(re.finditer(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{\n(.*?)^\}\n', text, re.S | re.M))


def _entry_of(lines, i, labels):
    """Line of the `base + (selected << 7)` computation the clamp at line i feeds; None when it cannot be followed."""
    index = CLAMP.search(lines[i]).group(1)
    select = re.compile(r'\s+c->x\[(\d+)\] = \(uint64_t\)\(\(COND_CC\(c\)\) \? c->x\[%s\] : \(uint64_t\)\(0ull\)\);'
                        % index)
    j, dest = i + 1, None
    for j in range(i + 1, min(i + 12, len(lines))):
        if LABEL.fullmatch(lines[j]):
            continue
        found = select.fullmatch(lines[j])
        if found:
            dest = found.group(1)
            break
        if 'goto' in lines[j] or FLAGS.search(lines[j]) or CALL.search(lines[j]) or 'c->x[%s] =' % index in lines[j]:
            return None
    if dest is None:
        return None
    k, steps, seen = j + 1, 0, set()
    written = re.compile(r'c->x\[%s\] = ' % dest)
    while k < len(lines) and steps < FOLLOW:
        line = lines[k]
        if LABEL.fullmatch(line):
            k += 1
            continue
        steps += 1
        entry = ENTRY.search(line)
        if entry and entry.group(3) == dest:
            return k
        if written.search(line) or 'return;' in line or (CALL.search(line) and not 19 <= int(dest) <= 28):
            return None
        jump = re.fullmatch(r'\s+goto (L_\w+);', line)
        if jump:
            if jump.group(1) in seen or jump.group(1) not in labels:
                return None
            seen.add(jump.group(1))
            k = labels[jump.group(1)] + 1
            continue
        k += 1
    return None


def patch_function(body, expected):
    """One reader's body with its clamps widened and entry computations redirected; exactly `expected` of them."""
    lines = body.split('\n')
    labels = {line[:-2]: n for n, line in enumerate(lines) if LABEL.fullmatch(line)}
    clamps, entries = [], set()
    for i, line in enumerate(lines):
        if not CLAMP.search(line):
            continue
        k = _entry_of(lines, i, labels)
        if k is None:
            raise ValueError('Cup clamp without its entry computation')
        clamps.append(i)
        entries.add(k)
    if (len(clamps), len(entries)) != expected:
        raise ValueError(f'Cup readers changed: {len(clamps)} clamps and {len(entries)} computations, {expected} expected')
    for i in clamps:
        lines[i] = lines[i].replace('0x19ull', '0x29ull')
    for k in entries:
        entry = ENTRY.search(lines[k])
        lines[k] = lines[k].replace(entry.group(0), 'c->x[%s] = (uint64_t)(mk8_cup_entry(c->x[%s], c->x[%s]));'
                                    % (entry.group(1), entry.group(2), entry.group(3)))
    return '\n'.join(lines)


def patch(text, unit):
    readers = UNITS[unit][1]
    if 'mk8_cup_entry' in text or 'mk8_cups_built' in text:
        raise ValueError('Cup relocation already applied')
    out, pos, seen = [], 0, set()
    for match in _functions(text):
        name, body = match.group(1), match.group(2)
        if name == BUILDER:
            if body.count('\n    return;\n') != 1 or body.count('return') != 1:
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

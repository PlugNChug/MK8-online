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
    'main_182.c': ('9937927a02dc942790b2c4a07ea51d5b941b16732864a99d08b7d934ec7f6286', {}),  # (the DLC check only)
    'main_084.c': ('5951e724de88abddbbfa88d33550b6d214816134ac1000411f8b2f0937f9a5bd', {}),  # (the cup name lookup)
}
# Units as toolchain/experimental.py leaves them (its hooks are in other functions than the cup readers)
AFTER_EXPERIMENTAL = {'main_082.c': 'f861292e6a6cb546924ca6f555bd28dce080afebdf251a7c8c9e3d9b8da7c814',
                      'main_115.c': 'cddddcba68768597f16f60322f4753f4c3e7c7ace2d9966708c5ae02d4fb758e',
                      'main_084.c': '51b673f39245c204f92b188f53c7347d8d70269e07fd7aa9434764a048d4d2a1'}
# The readers that copy the chosen course into the selection (+0x2088), and the Grand Prix's (its 4 courses into its
# list, f_70004fb290): they also tell which cup the race is from. f_70004a3940 takes a course ID (x25, stored at
# sp+0x18 just before) and finds its cup and position in the reverse map (always a game cup): with the ID as well, the
# runtime keeps an added cup chosen that has the course and gives its record instead.
SELECTORS = {'70004980a0': 'mk8_cup_select', '7000498e90': 'mk8_cup_select', '70004a3940': 'mk8_cup_by_course',
             '70004fb290': 'mk8_cup_select'}
# The readers that find a course's cup and position in the reverse map (+0xC80/+0xC84; some also the battle map at
# +0x1058) to show it, the race's course name among them: while an added cup is chosen, its courses' records (their
# name labels) take the borrowed courses' places in the game cup they read.
for _name in ('7000446068', '700045b51c', '7000470d78', '700048cef0', '70004a133c', '70004e5bb0', '7000501778',
              '700050dc6c'):
    SELECTORS[_name] = 'mk8_cup_by_slot'
SELECTOR_ARGS = {'70004a3940': (', c->x[25]', '    { uint64_t ea = c->sp + 0x18ull; ST32(ea, (uint32_t)c->x[25]); }')}
# The course path builders (Course/%s/%s and Course/%s): once they have the course's name (from the course enum, or for
# ID 1 the settings' own), they ask mk8_course_folder for the folder of an added cup's course. unit -> (SHA-256,
# [(label where the name is ready, the line to add)])
PATHS = {
    'main_186.c': ('9584bae10537278bb103dd2c287dadc687a07b40a83deec4ee5f4e1e1b85d7e6', [
        ('L_700089ece0:;', '    c->x[2] = (uint64_t)(mk8_course_folder(c->x[20], c->x[2]));  // MK8 (toolchain/cup_relocation.py)'),
        ('L_700089ed48:;', '    c->x[20] = (uint64_t)(mk8_course_folder(c->x[21], c->x[20]));  // MK8 (toolchain/cup_relocation.py)'),
    ]),
}
# The cup select screen (runtime/mk8/support/cups.c: a third page with the added cups): function -> [(line found once
# in it, 'before' or 'after', line to add)]. The button setups of both pages map each button's cup; the navigation
# reports which way a page turn goes; the per-cup DLC check (main_182.c) and the cup name lookup (main_084.c)
# answer for the added cups.
_MARK = '  // MK8 (toolchain/cup_relocation.py)'
UI = {
    '7000494b44': [('    { uint64_t ea = c->sp + 0x38ull; ST32(ea, (uint32_t)c->x[24]); }', 'before',
                    '    c->x[24] = (uint64_t)(uint32_t)mk8_cup_page_id(c->x[24]);' + _MARK)],
    '7000495564': [('    { uint64_t ea = c->sp + 0x38ull; ST32(ea, (uint32_t)c->x[25]); }', 'before',
                    '    c->x[25] = (uint64_t)(uint32_t)mk8_cup_page_id(c->x[25]);' + _MARK)],
    '70004966c8': [('    { uint64_t ea = c->x[19] + 0x5a3ull; ST8(ea, (uint32_t)c->x[8]); }', 'after',
                    '    mk8_cup_page_turn(c->x[20]);' + _MARK),
                   ('    { uint64_t ea = c->x[19] + 0x5a3ull; ST8(ea, 0u); }', 'after',
                    '    mk8_cup_page_turn(c->x[20]);' + _MARK)],
    '7000863428': [('    FN_ENTER(0x7000863428ull);', 'after',
                    '    { int64_t mk8_owned = mk8_cup_owned(LD32(c->x[1])); if (mk8_owned >= 0) '
                    '{ ST32(c->x[8], (uint32_t)mk8_owned); return; } }' + _MARK)],
    '700041fd6c': [('    FN_ENTER(0x700041fd6cull);', 'after',
                    '    { int64_t mk8_label = mk8_cup_label(LD32(c->x[0])); if (mk8_label >= 0) '
                    '{ c->x[0] = (uint32_t)mk8_label; return; } }' + _MARK)],
}
UI_UNITS = {'main_099.c': {'7000494b44'}, 'main_100.c': {'7000495564', '70004966c8'}, 'main_182.c': {'7000863428'}, 'main_084.c': {'700041fd6c'}}


# nn::fs::OpenFile(FileHandle *, const char *path, int mode) in the SDK (sdk+0x50080, by its exported name): the
# course music's path is asked for first (runtime/mk8/support/cups.c mk8_stream_path).
SDK_UNITS = {'sdk_008.c': ('67f6d5f3e751361429f4caae53ea0481e0897e0ea2da03c7b5638e81981b2f13', {'7004050080'})}
UI['7004050080'] = [('    FN_ENTER(0x7004050080ull);', 'after', '    c->x[1] = mk8_stream_path(c->x[1]);' + _MARK)]
# The course pictures (main_084.c): f_7000418ed0 asks for the suffix once it has the game's (x21: the record or ID it
# was given, x19: 1 to load, 0 to find, both still from its entry); the menu's loading (f_7000418e3c) loads the added
# courses' pictures after its own, with IDs from 123 on.
UI['7000418ed0'] = [('    { uint64_t ea = c->x[8] + 0x0ull; uint64_t v = LD64(ea); c->x[20] = (uint64_t)(v); }', 'after',
                     '    c->x[20] = mk8_course_picture(c->x[21], c->x[19], c->x[20]);' + _MARK)]
UI['7000418e3c'] = [('    c->x[30] = 0x7000418ec0ull; f_7000418ed0(c);', 'after',
                     '    for (int64_t mk8_k = 0, mk8_n = mk8_course_pictures(); mk8_k < mk8_n; mk8_k++) {' + _MARK + '\n'
                     '        { uint64_t ea = c->sp + 0x0ull; ST32(ea, (uint32_t)(0x7bull + (uint64_t)mk8_k)); }\n'
                     '        c->x[0] = c->x[19]; c->x[1] = c->sp + 0x0ull; c->x[2] = 1u;\n'
                     '        c->x[30] = 0x7000418ec0ull; f_7000418ed0(c);\n'
                     '    }')]
UI_UNITS['main_084.c'] = UI_UNITS['main_084.c'] | {'7000418ed0', '7000418e3c'}
# ... and the screens that show a course's picture by a copy of its ID (main_089.c): the cup panel remembers the
# record it is set to and gives the added picture's ID for it; the time trial screen gives it for the chosen cup's.
UI['700043c2e8'] = [('    FN_ENTER(0x700043c2e8ull);', 'after', '    mk8_panel_record(c->x[0], c->x[1]);' + _MARK),
                    ('    { uint64_t ea = c->x[19] + 0x3ccull; uint64_t v = LD32(ea); c->x[8] = (uint64_t)(v); }\n'
                     '    c->x[1] = (uint64_t)(c->sp + 0x0ull);', 'after',
                     '    c->x[8] = (uint64_t)(uint32_t)mk8_panel_picture(c->x[19], c->x[8]);' + _MARK)]
UI['7000446068'] = [('    { uint64_t ea = c->x[19] + 0xb4ull; uint64_t v = LD32(ea); c->x[8] = (uint64_t)(v); }', 'after',
                     '    c->x[8] = (uint64_t)(uint32_t)mk8_course_picture_id(c->x[8]);' + _MARK)]
UI_UNITS['main_089.c'] = {'700043c2e8', '7000446068'}


def patch_ui(body, name):
    for anchor, where, line in UI[name]:
        if body.count(anchor + '\n') != 1:
            raise ValueError(f'Cup screen changed: f_{name}')
        body = body.replace(anchor + '\n', (line + '\n' + anchor + '\n') if where == 'before' else (anchor + '\n' + line + '\n'))
    return body


DECLARATIONS = ('uint64_t mk8_cup_entry(uint64_t base, uint64_t index);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_cup_page_id(uint64_t id);  // MK8 (toolchain/cup_relocation.py)\n'
                'void mk8_cup_page_turn(uint64_t input);  // MK8 (toolchain/cup_relocation.py)\n'
                'int64_t mk8_cup_owned(uint64_t id);  // MK8 (toolchain/cup_relocation.py)\n'
                'int64_t mk8_cup_label(uint64_t id);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_stream_path(uint64_t path);  // MK8 (toolchain/cup_relocation.py)\n'
                'int64_t mk8_course_pictures(void);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_course_picture(uint64_t record, uint64_t load, uint64_t name);  // MK8 (toolchain/cup_relocation.py)\n'
                'void mk8_panel_record(uint64_t panel, uint64_t record);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_panel_picture(uint64_t panel, uint64_t id);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_course_picture_id(uint64_t id);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_cup_select(uint64_t base, uint64_t index);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_cup_by_course(uint64_t base, uint64_t index, uint64_t course);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_cup_by_slot(uint64_t base, uint64_t index);  // MK8 (toolchain/cup_relocation.py)\n'
                'uint64_t mk8_course_folder(uint64_t settings, uint64_t name);  // MK8 (toolchain/cup_relocation.py)\n'
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


def patch_function(body, expected, call='mk8_cup_entry', extra=''):
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
        lines[k] = lines[k].replace(entry.group(0), 'c->x[%s] = (uint64_t)(%s(c->x[%s], c->x[%s]%s));'
                                    % (entry.group(1), call, entry.group(2), entry.group(3), extra))
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
            extra, anchor = SELECTOR_ARGS.get(name, ('', None))
            if anchor is not None and body.count(anchor + '\n') != 1:
                raise ValueError(f'Cup reader changed: f_{name}')
            body = patch_function(body, readers[name], SELECTORS.get(name, 'mk8_cup_entry'), extra)
            if name in UI_UNITS.get(unit, ()):
                body = patch_ui(body, name)
            seen.add(name)
        elif name in UI_UNITS.get(unit, ()):
            body = patch_ui(body, name)
            seen.add(name)
        else:
            continue
        out.append(text[pos:match.start(2)])
        out.append(body)
        pos = match.end(2)
    out.append(text[pos:])
    wanted = set(readers) | set(UI_UNITS.get(unit, ())) | ({BUILDER} if unit == 'main_081.c' else set())
    if seen != wanted:
        raise ValueError(f'Cup functions missing in {unit}: {sorted(wanted - seen)}')
    result = ''.join(out)
    first = result.index('void f_')
    return result[:first] + DECLARATIONS + result[first:]


def patch_paths(text, unit):
    """The path builders' unit with the folder hook after each label where the course's name is ready."""
    if 'mk8_course_folder' in text:
        raise ValueError('Course folder hook already applied')
    for label, line in PATHS[unit][1]:
        if text.count('\n' + label + '\n') != 1:
            raise ValueError(f'Course path builder changed: {label}')
        text = text.replace('\n' + label + '\n', '\n' + label + '\n' + line + '\n')
    first = text.index('void f_')
    return text[:first] + DECLARATIONS + text[first:]


def patch_sdk(text, unit):
    """The SDK unit with its hooks (each function once)."""
    if 'mk8_stream_path' in text:
        raise ValueError('SDK hook already applied')
    out, pos, seen = [], 0, set()
    for match in _functions(text):
        if match.group(1) not in SDK_UNITS[unit][1]:
            continue
        out += [text[pos:match.start(2)], patch_ui(match.group(2), match.group(1))]
        pos = match.end(2)
        seen.add(match.group(1))
    if seen != SDK_UNITS[unit][1]:
        raise ValueError(f'SDK functions missing in {unit}')
    out.append(text[pos:])
    result = ''.join(out)
    first = result.index('void f_')
    return result[:first] + DECLARATIONS + result[first:]


def patch_unit(src, output):
    """The build copy of a cup or path unit with the relocation; any other unit unchanged."""
    if src.name in SDK_UNITS and src.parent.name == 'sdk':
        raw = src.read_bytes()
        if hashlib.sha256(raw).hexdigest() != SDK_UNITS[src.name][0]:
            raise ValueError(f'Course music hook requires the verified MK8 4.0.1 generated unit {src.name}')
        target = output/'cups'/'sdk'/src.name
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(patch_sdk(re.sub(r'\r+\n', '\n', raw.decode('utf-8')), src.name))
        return target
    if src.name in PATHS and src.parent.name == 'main':
        raw = src.read_bytes()
        if hashlib.sha256(raw).hexdigest() != PATHS[src.name][0]:
            raise ValueError(f'Course folder hook requires the verified MK8 4.0.1 generated unit {src.name}')
        target = output/'cups'/src.name
        target.parent.mkdir(parents=True, exist_ok=True)
        with open(target, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(patch_paths(re.sub(r'\r+\n', '\n', raw.decode('utf-8')), src.name))
        return target
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

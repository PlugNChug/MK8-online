"""Lift sized SDK assembly exports omitted by the STT_FUNC-only discovery pass."""
import hashlib
import re
import sys

SDK_HASH = '427404f3218eaa790f611acd9e45fe0336efb9504f02949054b14de3578bd1c2'


def supplement(source, generated, output):
    nso = source.parent/'inputs/exefs/sdk'
    if not nso.is_file():
        return None
    if hashlib.sha256(nso.read_bytes()).hexdigest() != SDK_HASH:
        raise ValueError('SDK assembly supplement requires verified MK8 4.0.1')
    sys.path.insert(0, str(source/'tools'))
    from gen import Module
    m = Module('sdk', str(nso), 0x7004000000, simd='c')
    table = (generated/'sdk/sdk_table.c').read_text(encoding='utf-8')
    addresses = set(int(a, 16) for a in re.findall(r'void f_([0-9a-f]+)\(Cpu', table))
    bodies = []
    for name, info, section, offset, size in m.nso.exports():
        a = m.base + offset
        if info & 15 or not size or not name.startswith('nndetailCryptoBignum') or a in addresses:
            continue
        if size % 4 or not m.in_text(a) or not m.in_text(a + size - 4):
            raise ValueError('Invalid sized SDK assembly export')
        labels, refs, tables = m.scan(a, a + size)
        if refs or tables:
            raise ValueError('SDK assembly export needs dependency review')
        lines = m.emit_function(a, a + size, labels, tables, {})
        if any('rt_udf' in line for line in lines):
            raise ValueError('Unsupported SDK assembly instruction')
        bodies.extend(lines)
        addresses.add(a)
    if not bodies:
        return None
    folder = output/'experimental/sdk'
    folder.mkdir(parents=True, exist_ok=True)
    code = folder/'sdk_assembly.c'
    code.write_text('#include "cpu.h"\n#include "simd.h"\n'+'\n'.join(bodies), encoding='utf-8')
    order = sorted(addresses)
    table = '#include "cpu.h"\n'+''.join(f'void f_{a:x}(Cpu *c);\n' for a in order)
    table += 'const uint64_t sdk_fn_addrs[] = {\n'+',\n'.join(f'0x{a:x}ull' for a in order)+'};\n'
    table += 'const GuestFn sdk_fn_ptrs[] = {\n'+',\n'.join(f'f_{a:x}' for a in order)+'};\n'
    table += f'const uint64_t sdk_fn_count = {len(order)};\n'
    target = folder/'sdk_table.c'
    target.write_text(table, encoding='utf-8')
    return code, target

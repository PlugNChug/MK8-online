"""Bake a static Mii head from the calibrated, private intermediate mesh export.

Input positions are scaled by 7.14, X-up/Z-forward, not world-space GLB nodes.
This stage deliberately supports only the tested six-part export and 256px maps.
It does not load arbitrary GLBs, firmware resources, or animated expressions.
"""
import argparse
import json
import math
from pathlib import Path
import re
import shutil
import struct

def head_vector(v):
    """GLB head-local axes -> MK8 Head bone-local axes; determinant +1."""
    return [v[0], v[2], -v[1]]

def validate(data, root):
    if not isinstance(data, dict) or len(data.get('parts', [])) != 6 or len(data.get('textures', [])) > 8:
        raise ValueError('Expected the calibrated six-part export')
    expected = {'Faceline', 'Nose', 'Forehead', 'Hair', 'Mask', 'Noseline'}
    textures = {}
    for t in data['textures']:
        name = t.get('name', '')
        if not re.fullmatch('[a-z0-9_]{1,32}', name) or name in textures:
            raise ValueError('Invalid texture name')
        w, h = (t['width'], t['height'])
        if type(w) != int or type(h) != int or (not 2 <= w <= 256) or (not 2 <= h <= 256):
            raise ValueError('Invalid texture dimensions')
        if (root / (name + '.rgba')).stat().st_size != w * h * 4:
            raise ValueError('RGBA length mismatch')
        textures[name] = (w, h)
    if textures.get('head0') != (128, 256) or textures.get('head1') != (256, 256) or textures.get('head2') != (256, 256):
        raise ValueError('Unsupported calibrated texture layout')
    for p in data['parts']:
        if p['name'] not in expected or p['texture'] not in textures:
            raise ValueError('Unexpected part or texture')
        expected.remove(p['name'])
        if p['transparent'] != (p['name'] in {'Mask', 'Noseline'}):
            raise ValueError('Unsupported opacity layout')
        expected_texture = {'Faceline': 'head0', 'Mask': 'head1', 'Noseline': 'head2'}
        if p['texture'] != expected_texture.get(p['name'], 'white'):
            raise ValueError('Unsupported texture assignment')
        n = len(p['positions'])
        if not 3 <= n <= 16000 or len(p['normals']) != n or len(p['uv']) != n:
            raise ValueError('Invalid vertex count')
        for field, width, limit in [('positions', 3, 32), ('normals', 3, 1.01), ('uv', 2, 2)]:
            for row in p[field]:
                if len(row) != width or any((type(v) not in (int, float) or not math.isfinite(v) or abs(v) > limit for v in row)):
                    raise ValueError('Invalid vertex value')
        indices = p['indices']
        if not 3 <= len(indices) <= 96000 or len(indices) % 3 or any((type(i) != int or not 0 <= i < n for i in indices)):
            raise ValueError('Invalid triangle indices')
        if len(p['color']) != 4 or any((not math.isfinite(v) or not 0 <= v <= 1 for v in p['color'])):
            raise ValueError('Invalid color')
    face = next((p for p in data['parts'] if p['name'] == 'Faceline'))
    if any((not 0 <= u <= 2 or not 0 <= v <= 1 for u, v in face['uv'])):
        raise ValueError('Unsupported face UV range')
    nose = next(p for p in data['parts'] if p['name'] == 'Nose')
    if any(max(v[k] for v in nose['positions']) - min(v[k] for v in nose['positions']) < 1e-6
           for k in (0, 1)):
        raise ValueError('Degenerate nose projection')

def expand_mirrored_skin(rgba, width, height):
    """Expand U=0..2 mirrored-repeat into one ordinary U=0..1 texture."""
    if len(rgba) != width * height * 4:
        raise ValueError('RGBA length mismatch')
    out = bytearray()
    for y in range(height):
        row = rgba[y * width * 4:(y + 1) * width * 4]
        out.extend(row)
        for x in range(width - 1, -1, -1):
            out.extend(row[x * 4:x * 4 + 4])
    return out

def pack_yaz0(source, output):
    """Literal Yaz0 for the private overlay; never overwrite the original asset."""
    source, output = (Path(source), Path(output))
    if not 16 <= source.stat().st_size <= 4 * 1024 * 1024:
        raise ValueError('BFRES exceeds the tested resource budget')
    data = source.read_bytes()
    if data[:4] != b'FRES':
        raise ValueError('Expected BFRES')
    if shutil.disk_usage(output.parent).free < 4 * 1024 ** 3:
        raise ValueError('At least 4 GiB free required')
    packed = b'Yaz0' + struct.pack('>I', len(data)) + bytes(8) + b''.join((b'\xff' + data[i:i + 8] for i in range(0, len(data), 8)))
    with output.open('xb') as stream:
        stream.write(packed)

def bary(p, a, b, c):
    den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(den) < 1e-10:
        return None
    u = ((b[1] - c[1]) * (p[0] - c[0]) + (c[0] - b[0]) * (p[1] - c[1])) / den
    v = ((c[1] - a[1]) * (p[0] - c[0]) + (a[0] - c[0]) * (p[1] - c[1])) / den
    return (u, v, 1 - u - v) if min(u, v, 1 - u - v) >= -1e-05 else None

def blend(target, mask, size, base, root, data):
    out = bytearray(base)
    tex = next((t for t in data['textures'] if t['name'] == mask['texture']))
    rgba = (root / (mask['texture'] + '.rgba')).read_bytes()
    tris = []
    for i in range(0, len(mask['indices']), 3):
        ix = mask['indices'][i:i + 3]
        tris.append(([(mask['positions'][j][0], mask['positions'][j][2]) for j in ix], [mask['uv'][j] for j in ix]))
    for i in range(0, len(target['indices']), 3):
        ix = target['indices'][i:i + 3]
        uv = [target['uv'][j] for j in ix]
        if max((target['normals'][j][1] for j in ix)) < 0.05:
            continue
        lo = [max(0, int(min((v[k] for v in uv)) * size[k])) for k in range(2)]
        hi = [min(size[k] - 1, int(max((v[k] for v in uv)) * size[k]) + 1) for k in range(2)]
        for y in range(lo[1], hi[1] + 1):
            for x in range(lo[0], hi[0] + 1):
                w = bary(((x + 0.5) / size[0], (y + 0.5) / size[1]), *uv)
                if w is None:
                    continue
                pos = [sum((w[k] * target['positions'][ix[k]][j] for k in range(3))) for j in range(3)]
                if pos[1] < 0:
                    continue
                for tri, coords in tris:
                    z = bary((pos[0], pos[2]), *tri)
                    if z is None:
                        continue
                    t = [sum((z[k] * coords[k][j] for k in range(3))) for j in range(2)]
                    sx = max(0, min(tex['width'] - 1, int(t[0] * tex['width'])))
                    sy = max(0, min(tex['height'] - 1, int(t[1] * tex['height'])))
                    si = 4 * (sy * tex['width'] + sx)
                    oi = 4 * (y * size[0] + x)
                    alpha = rgba[si + 3] / 255
                    for c in range(3):
                        out[oi + c] = round(rgba[si + c] * alpha + out[oi + c] * (1 - alpha))
                    break
    return out

def bake(source, output):
    source, output = (Path(source), Path(output))
    if source.stat().st_size > 8 * 1024 * 1024:
        raise ValueError('Head JSON too large')
    d = json.loads(source.read_text(encoding='utf-8'))
    root = source.parent
    validate(d, root)
    if shutil.disk_usage(output.parent).free < 4 * 1024 ** 3:
        raise ValueError('At least 4 GiB free required')
    output.mkdir(exist_ok=False)
    for p in d['parts']:
        p['positions'] = [head_vector(v) for v in p['positions']]
        p['normals'] = [head_vector(v) for v in p['normals']]
    parts = {p['name']: p for p in d['parts']}
    face = parts['Faceline']
    size = (256, 256)
    face['uv'] = [[u / 2, v] for u, v in face['uv']]
    base = expand_mirrored_skin((root / 'head0.rgba').read_bytes(), 128, 256)
    (output / 'bakedface.rgba').write_bytes(blend(face, parts['Mask'], size, base, root, d))
    face['texture'] = 'bakedface'
    nose = parts['Nose']
    bounds = [(min((v[k] for v in nose['positions'])), max((v[k] for v in nose['positions']))) for k in (2, 0)]
    nose['uv'] = [[(v[k] - bounds[j][0]) / (bounds[j][1] - bounds[j][0]) for j, k in enumerate((2, 0))] for v in nose['positions']]
    (output / 'bakednose.rgba').write_bytes(blend(nose, parts['Noseline'], (128, 128), bytes([255, 255, 255, 255]) * 128 * 128, root, d))
    nose['texture'] = 'bakednose'
    d['parts'] = [p for p in d['parts'] if not p['transparent']]
    d['textures'] = [dict(name='white', width=2, height=2), dict(name='flatnormal', width=2, height=2), dict(name='black', width=2, height=2), dict(name='bakedface', width=256, height=256), dict(name='bakednose', width=128, height=128)]
    (output / 'white.rgba').write_bytes(bytes([255] * 16))
    (output / 'black.rgba').write_bytes(bytes([0, 0, 0, 255]) * 4)
    (output / 'flatnormal.rgba').write_bytes(bytes([0, 0, 127, 127]) * 4)
    (output / 'head.json').write_text(json.dumps(d), encoding='utf-8')
    return d


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('head', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--pack', action='store_true', help='Pack derived BFRES as a new Yaz0 overlay file')
    args = parser.parse_args()
    (pack_yaz0 if args.pack else bake)(args.head, args.output)

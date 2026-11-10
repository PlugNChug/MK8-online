"""Convert the launcher's FFL GLB to a private MK8 4.0.1 male Normal Suit overlay.

Static face/nose/hair only, optional forehead; unsupported parts fail explicitly.
Requires Pillow, the verified original BFRES and the compiled mii_head_builder.cs.
"""
import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess

from PIL import Image

if __package__:
    from .mii_head_bake import blend, expand_mirrored_skin, pack_yaz0, srgb_decode, srgb_encode
else:
    from mii_head_bake import blend, expand_mirrored_skin, pack_yaz0, srgb_decode, srgb_encode

ORIGINAL_SHA256 = 'd2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a'
PARTS = {0: 'Faceline', 2: 'Nose', 3: 'Forehead', 4: 'Hair', 6: 'Mask', 7: 'Noseline'}
# Same serialized Switch contract as crates/installer/src/mii_data.rs.
CHARINFO_MAX = (3, 11, 1, 127, 127, 1, 3, 11, 9, 11, 11, 131, 99, 1, 59, 99, 7, 6, 7, 12, 18,
                23, 99, 8, 6, 11, 12, 18, 17, 8, 18, 35, 99, 8, 6, 18, 99, 5, 5, 8, 16, 19, 99,
                7, 20, 1, 8, 16, 30, 0)


def native_vector(v, position=False):
    # FFL: Y up / Z front. Head bone: X up / Y front. Scale derives from the calibrated export.
    scale = .1 if position else 1
    return [v[1]*scale, v[2]*scale, v[0]*scale]


def bounded_int(value, low, high):
    if type(value) != int or not low <= value <= high:
        raise ValueError('Integer outside supported range')
    return value


class Glb:
    def __init__(self, raw):
        if not 28 <= len(raw) <= 2*1024**2 or raw[:4] != b'glTF':
            raise ValueError('Expected a launcher GLB, at most 2 MiB')
        version, total, size = struct.unpack_from('<III', raw, 4)
        end = 20+size
        if version != 2 or total != len(raw) or size % 4 or end+8 > total or raw[16:20] != b'JSON':
            raise ValueError('Invalid GLB envelope')
        if raw[end+4:end+8] != b'BIN\0' or struct.unpack_from('<I', raw, end)[0] != total-end-8:
            raise ValueError('Invalid binary chunk')
        self.doc = json.loads(raw[20:end])
        self.binary = raw[end+8:]
        d = self.doc
        if d.get('asset', {}).get('version') != '2.0' or any(d.get(k) for k in ('skins', 'animations', 'extensionsUsed', 'extensionsRequired')):
            raise ValueError('Only static unextended GLB is supported')
        buffers = d.get('buffers', [])
        if len(buffers) != 1 or 'uri' in buffers[0] or buffers[0].get('byteLength') != len(self.binary):
            raise ValueError('External or inconsistent buffer')
        self.charinfo = d['asset'].get('extras', {}).get('mii_charinfo')
        if not isinstance(self.charinfo, list) or len(self.charinfo) != 88 or any(type(v) != int or not 0 <= v <= 255 for v in self.charinfo):
            raise ValueError('Missing launcher CharInfo binding')
        if self.charinfo[8] & 0xc0 != 0x80 or self.charinfo[40] != 0:
            raise ValueError('Only a valid male Mii identity is supported by this overlay')
        chars = self.charinfo
        name = bytes(chars[16:36]).decode('utf-16le').split('\0')[0]
        if not name.strip() or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in name) or chars[36:38] != [0, 0] or chars[65] < 3 or any(v > cap for v, cap in zip(chars[38:], CHARINFO_MAX)):
            raise ValueError('Invalid CharInfo fields or name')

    def view(self, index):
        views = self.doc['bufferViews']
        view = views[bounded_int(index, 0, len(views)-1)]
        start = bounded_int(view.get('byteOffset', 0), 0, len(self.binary))
        size = bounded_int(view['byteLength'], 1, len(self.binary)-start)
        if view.get('buffer') != 0 or view.get('extensions'):
            raise ValueError('Unsupported buffer view')
        return self.binary[start:start+size], view

    def accessor(self, index, kind):
        accessors = self.doc['accessors']
        a = accessors[bounded_int(index, 0, len(accessors)-1)]
        component = a.get('componentType')
        if a.get('type') != kind or a.get('sparse') or a.get('normalized') or a.get('extensions'):
            raise ValueError('Unsupported accessor')
        if component not in ((5123, 5125) if kind == 'SCALAR' else (5126,)):
            raise ValueError('Unsupported component type')
        count = bounded_int(a['count'], 3, 96000 if kind == 'SCALAR' else 16000)
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3}[kind]
        fmt = '<'+{5123: 'H', 5125: 'I', 5126: 'f'}[component]*width
        length = struct.calcsize(fmt)
        data, view = self.view(a['bufferView'])
        stride = bounded_int(view.get('byteStride', length), length, 256)
        offset = bounded_int(a.get('byteOffset', 0), 0, len(data))
        if offset + (count-1)*stride + length > len(data):
            raise ValueError('Accessor exceeds buffer view')
        rows = [list(struct.unpack_from(fmt, data, offset+i*stride)) for i in range(count)]
        if any(not math.isfinite(v) for row in rows for v in row):
            raise ValueError('Nonfinite vertex')
        return rows

    def image(self, texture):
        textures = self.doc['textures']
        t = textures[bounded_int(texture['index'], 0, len(textures)-1)]
        if texture.get('texCoord', 0) != 0 or texture.get('extensions') or t.get('extensions'):
            raise ValueError('Unsupported texture coordinates')
        images = self.doc['images']
        image = images[bounded_int(t['source'], 0, len(images)-1)]
        if image.get('mimeType') != 'image/png' or 'uri' in image:
            raise ValueError('Only embedded PNG textures are supported')
        data, _ = self.view(image['bufferView'])
        with Image.open(io.BytesIO(data)) as source:
            if source.format != 'PNG' or not all(1 <= n <= 1024 for n in source.size):
                raise ValueError('Texture exceeds supported dimensions')
            return source.convert('RGBA')

    def parts(self):
        d = self.doc
        nodes, meshes, materials = d['nodes'], d['meshes'], d['materials']
        if not 1 <= len(nodes) <= 16 or not 5 <= len(meshes) <= 6:
            raise ValueError('Unsupported number of head parts')
        if len(d['scenes']) != 1 or d.get('scene', 0) != 0:
            raise ValueError('Expected one scene')
        result, seen, used = {}, set(), set()

        def visit(index, parent_scale=1, parent_offset=(0, 0, 0)):
            bounded_int(index, 0, len(nodes)-1)
            if index in seen:
                raise ValueError('Cycle or shared node')
            seen.add(index)
            node = nodes[index]
            if any(k in node for k in ('skin', 'weights', 'rotation', 'translation', 'scale', 'extensions')):
                raise ValueError('Unsupported node transform')
            matrix = node.get('matrix', [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
            if len(matrix) != 16 or any(type(v) not in (int, float) or not math.isfinite(v) for v in matrix):
                raise ValueError('Invalid transform')
            if any(matrix[i] != 0 for i in (1, 2, 3, 4, 6, 7, 8, 9, 11)) or matrix[15] != 1 or not 0 < matrix[0] == matrix[5] == matrix[10] <= 4:
                raise ValueError('Only positive uniform scale and translation are supported')
            scale = parent_scale * matrix[0]
            offset = [parent_offset[k]+matrix[12+k]*parent_scale for k in range(3)]
            if 'mesh' in node:
                mi = bounded_int(node['mesh'], 0, len(meshes)-1)
                if mi in used or len(meshes[mi]['primitives']) != 1:
                    raise ValueError('Duplicate or multipart mesh')
                used.add(mi)
                prim = meshes[mi]['primitives'][0]
                kind = prim.get('extras', {}).get('modulateType')
                if kind not in PARTS or PARTS[kind] in result or prim.get('mode', 4) != 4 or prim.get('targets') or prim.get('extensions'):
                    raise ValueError('Unsupported FFL part; glasses/beards/expressions require separate qualification')
                mat = materials[bounded_int(prim['material'], 0, len(materials)-1)]
                transparent = kind in (6, 7)
                if mat.get('alphaMode', 'OPAQUE') != ('BLEND' if transparent else 'OPAQUE') or mat.get('extensions'):
                    raise ValueError('Unsupported material')
                pbr = mat['pbrMetallicRoughness']
                color = pbr.get('baseColorFactor', [1]*4)
                if len(color) != 4 or any(type(v) not in (float, int) or not math.isfinite(v) or not 0 <= v <= 1 for v in color) or color[3] != 1:
                    raise ValueError('Unsupported color')
                a = prim['attributes']
                positions = [native_vector([v[k]*scale+offset[k] for k in range(3)], True) for v in self.accessor(a['POSITION'], 'VEC3')]
                normals = [native_vector(v) for v in self.accessor(a['NORMAL'], 'VEC3')]
                uv = self.accessor(a['TEXCOORD_0'], 'VEC2') if 'TEXCOORD_0' in a else [[0, 0]]*len(positions)
                indices = [v[0] for v in self.accessor(prim['indices'], 'SCALAR')]
                if len(normals) != len(positions) or len(uv) != len(positions) or len(indices) % 3 or max(indices) >= len(positions):
                    raise ValueError('Inconsistent vertex/triangle count')
                if any(abs(v) > 32 for row in positions for v in row) or any(not .9 < sum(v*v for v in row) < 1.1 for row in normals):
                    raise ValueError('Invalid position or normal')
                if any(not 0 <= v <= (2 if kind == 0 and k == 0 else 1) for row in uv for k, v in enumerate(row)):
                    raise ValueError('Unsupported UV range')
                image = self.image(pbr['baseColorTexture']) if 'baseColorTexture' in pbr else None
                if transparent and image is None or image is not None and kind not in (0, 6, 7):
                    raise ValueError('Unsupported texture assignment')
                result[PARTS[kind]] = dict(name=PARTS[kind], positions=positions, normals=normals, uv=uv,
                                         indices=indices, color=color, transparent=transparent, image=image)
            for child in node.get('children', []):
                visit(child, scale, offset)

        for root in d['scenes'][0]['nodes']:
            visit(root)
        if len(seen) != len(nodes) or len(used) != len(meshes) or not {'Faceline', 'Nose', 'Hair', 'Mask', 'Noseline'} <= result.keys():
            raise ValueError('Incomplete head scene')
        return result


def colored_pixels(image, size, color):
    pixels = bytearray(image.resize(size, Image.Resampling.LANCZOS).tobytes() if image else bytes([255])*math.prod(size)*4)
    for i in range(0, len(pixels), 4):
        for c in range(3):
            pixels[i+c] = round(255*srgb_encode(srgb_decode(pixels[i+c]/255)*color[c]))
    return pixels


def prepare(source, output):
    source, output = Path(source), Path(output)
    if source.stat().st_size > 2*1024**2:
        raise ValueError('GLB exceeds size limit')
    raw = source.read_bytes()
    glb = Glb(raw)
    parts = glb.parts()  # Validate before creating any output.
    if shutil.disk_usage(output.parent).free < 4*1024**3:
        raise ValueError('At least 4 GiB free required')
    output.mkdir(exist_ok=False)
    (output/'source.glb').write_bytes(raw)
    textures = []

    def texture(name, size, data):
        (output/(name+'.rgba')).write_bytes(data)
        textures.append(dict(name=name, width=size[0], height=size[1]))

    for name in ('Mask', 'Noseline'):
        p = parts[name]
        p['texture'] = name.lower()
        texture(p['texture'], (256, 256), colored_pixels(p['image'], (256, 256), p['color']))
    face = parts['Faceline']
    base = colored_pixels(face['image'], (128, 256), face['color'])
    if any(base[i] != 255 for i in range(3, len(base), 4)):
        raise ValueError('Faceline must be opaque')
    base = expand_mirrored_skin(base, 128, 256)
    face['uv'] = [[u/2, v] for u, v in face['uv']]
    texture('bakedface', (256, 256), blend(face, parts['Mask'], (256, 256), base, output, dict(textures=textures), linear=True))
    face.update(texture='bakedface', color=[1]*4)
    nose = parts['Nose']
    bounds = [(min(v[k] for v in nose['positions']), max(v[k] for v in nose['positions'])) for k in (2, 0)]
    if any(hi-lo < 1e-6 for lo, hi in bounds):
        raise ValueError('Degenerate nose projection')
    nose['uv'] = [[(v[k]-bounds[j][0])/(bounds[j][1]-bounds[j][0]) for j, k in enumerate((2, 0))] for v in nose['positions']]
    base = colored_pixels(None, (128, 128), nose['color'])
    texture('bakednose', (128, 128), blend(nose, parts['Noseline'], (128, 128), base, output, dict(textures=textures), linear=True))
    nose.update(texture='bakednose', color=[1]*4)
    for name, pixel in [('white', [255]*4), ('black', [0, 0, 0, 255]), ('flatnormal', [0, 0, 127, 127])]:
        texture(name, (2, 2), bytes(pixel)*4)
    opaque = []
    for p in parts.values():
        p.pop('image')
        if not p['transparent']:
            p.setdefault('texture', 'white')
            opaque.append(p)
    data = dict(parts=opaque, textures=[t for t in textures if t['name'] not in ('mask', 'noseline')])
    (output/'head.json').write_text(json.dumps(data), encoding='utf-8')
    chars = glb.charinfo
    name = bytes(chars[16:38]).decode('utf-16le').split('\0')[0]
    profile = dict(schema=1, id=chars[:16], charinfo=chars, appearance=dict(name=name, color=chars[39], gender=chars[40]))
    (output/'mii-profile.json').write_text(json.dumps(profile), encoding='utf-8')
    return dict(schema=1, glb_sha256=hashlib.sha256(raw).hexdigest(), charinfo=chars,
                transform='FFL (x,y,z) -> Head (y,z,x) * 0.1', parts=[p['name'] for p in opaque],
                original_bfres_sha256=ORIGINAL_SHA256, scope='static male Normal Suit; all users of MiiM share this head',
                gameplay_verified=False, multiplayer_verified=False)


def convert(source, output, original, builder):
    original, builder, output = Path(original).resolve(), Path(builder).resolve(), Path(output).resolve()
    with original.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != ORIGINAL_SHA256:
            raise ValueError('Unsupported original MK8 4.0.1 BFRES')
    if not builder.is_file():
        raise ValueError('Compile mii_head_builder.cs first')
    manifest = prepare(source, output)
    subprocess.run([str(builder), str(original), str(output/'head.json'), str(output/'MiiM.bfres')], check=True)
    driver = output/'romfs/Driver'
    driver.mkdir(parents=True)
    pack_yaz0(output/'MiiM.bfres', driver/'MiiM.szs')
    manifest['overlay_sha256'] = hashlib.sha256((driver/'MiiM.szs').read_bytes()).hexdigest()
    manifest['builder_sha256'] = hashlib.sha256(builder.read_bytes()).hexdigest()
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('glb', type=Path)
    parser.add_argument('output', type=Path, help='New private overlay directory')
    parser.add_argument('--original', required=True, type=Path)
    parser.add_argument('--builder', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(convert(args.glb, args.output, args.original, args.builder), indent=2))

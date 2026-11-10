"""Authored triangles and pixels only; no game or firmware fixtures."""
import io
import json
import struct
from pathlib import Path
import tempfile
import unittest

from PIL import Image
from toolchain.mii_native_head import Glb, native_vector, colored_pixels, prepare
from toolchain.mii_head_bake import srgb_decode, srgb_encode


def fixture():
    binary = bytearray()
    views, accessors = [], []

    def view(data):
        while len(binary) % 4:
            binary.append(0)
        views.append(dict(buffer=0, byteOffset=len(binary), byteLength=len(data)))
        binary.extend(data)
        return len(views)-1

    def accessor(rows, kind, component=5126):
        data = struct.pack('<'+('H' if component == 5123 else 'f')*sum(map(len, rows)), *(v for row in rows for v in row))
        accessors.append(dict(bufferView=view(data), componentType=component, type=kind, count=len(rows)))
        return len(accessors)-1

    position = accessor([[0, 0, 0], [1, 0, 0], [0, 1, 0]], 'VEC3')
    normal = accessor([[0, 0, 1]]*3, 'VEC3')
    uv = accessor([[0, 0], [1, 0], [0, 1]], 'VEC2')
    indices = accessor([[0], [1], [2]], 'SCALAR', 5123)
    png = io.BytesIO()
    Image.new('RGBA', (2, 2), (128, 128, 128, 128)).save(png, format='PNG')
    png_view = view(png.getvalue())
    while len(binary) % 4:
        binary.append(0)
    chars = [0]*88
    chars[8], chars[16], chars[65] = 128, 65, 3
    d = dict(asset=dict(version='2.0', extras=dict(mii_charinfo=chars)),
             buffers=[dict(byteLength=len(binary))], bufferViews=views, accessors=accessors,
             images=[dict(mimeType='image/png', bufferView=png_view)], textures=[dict(source=0)],
             materials=[dict(pbrMetallicRoughness={}), dict(alphaMode='BLEND', pbrMetallicRoughness=dict(baseColorTexture=dict(index=0)))],
             meshes=[], nodes=[], scenes=[dict(nodes=[0])], scene=0)
    d['nodes'].append(dict(children=list(range(1, 6))))
    for i, kind in enumerate((0, 4, 6, 7, 2)):
        d['meshes'].append(dict(primitives=[dict(attributes=dict(POSITION=position, NORMAL=normal, TEXCOORD_0=uv),
                           indices=indices, material=int(kind in (6, 7)), extras=dict(modulateType=kind))]))
        d['nodes'].append(dict(mesh=i))
    d['nodes'][-1]['matrix'] = [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 2, 3, 4, 1]
    return d, binary


def encode(doc, binary):
    data = json.dumps(doc).encode()
    data += b' '*(-len(data) % 4)
    return b'glTF'+struct.pack('<III', 2, 28+len(data)+len(binary), len(data))+b'JSON'+data+struct.pack('<I', len(binary))+b'BIN\0'+binary


class NativeHeadTests(unittest.TestCase):
    def test_uniform_node_transform_precedes_bone_axes_and_scale(self):
        doc, binary = fixture()
        parts = Glb(encode(doc, binary)).parts()
        self.assertEqual(set(parts), {'Faceline', 'Hair', 'Mask', 'Noseline', 'Nose'})
        for actual, expected in zip(parts['Nose']['positions'][0], [.3, .4, .2]):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(parts['Nose']['normals'][0], [0, 1, 0])
        self.assertEqual(native_vector([1, 2, 3]), [2, 3, 1])

    def test_rejects_corrupt_geometry_transforms_and_unsupported_parts(self):
        for mutation in (
            lambda d: d['accessors'][0].update(count=16001),
            lambda d: d['accessors'][0].update(byteOffset=999999),
            lambda d: d['meshes'][0]['primitives'][0]['extras'].update(modulateType=8),
            lambda d: d['nodes'][0]['children'].append(0),
            lambda d: d['nodes'][-1]['matrix'].__setitem__(0, -2),
            lambda d: d['buffers'][0].update(uri='private.dat'),
            lambda d: d['images'][0].update(uri='https://invalid.example/image.png'),
            lambda d: d['asset']['extras']['mii_charinfo'].__setitem__(49, 132),
            lambda d: d['asset']['extras']['mii_charinfo'].__setitem__(40, 1),
        ):
            doc, binary = fixture()
            mutation(doc)
            with self.assertRaises(ValueError):
                Glb(encode(doc, binary)).parts()

    def test_rejects_truncated_envelope(self):
        doc, binary = fixture()
        raw = encode(doc, binary)
        for cut in (0, 4, 20, len(raw)-1):
            with self.assertRaises(ValueError):
                Glb(raw[:cut])

    def test_color_factors_multiply_in_linear_space_and_keep_alpha(self):
        image = Image.new('RGBA', (2, 2), (128, 128, 128, 127))
        pixels = colored_pixels(image, (2, 2), [.5, 1, 0, 1])
        self.assertEqual(pixels[:4], bytes([92, 128, 0, 127]))
        for value in (0, .001, .03, .3, 1):
            self.assertAlmostEqual(srgb_encode(srgb_decode(value)), value)

    def test_prepares_three_opaque_parts_and_bound_profile_without_overwriting(self):
        doc, binary = fixture()
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root/'source.glb'
            source.write_bytes(encode(doc, binary))
            output = root/'derived'
            manifest = prepare(source, output)
            head = json.loads((output/'head.json').read_text())
            self.assertEqual(set(manifest['parts']), {'Faceline', 'Hair', 'Nose'})
            self.assertTrue(all(not p['transparent'] for p in head['parts']))
            for texture in head['textures']:
                self.assertEqual((output/(texture['name']+'.rgba')).stat().st_size,
                                 texture['width']*texture['height']*4)
            profile = json.loads((output/'mii-profile.json').read_text())
            self.assertEqual(profile['charinfo'], doc['asset']['extras']['mii_charinfo'])
            self.assertEqual(profile['appearance']['name'], 'A')
            self.assertFalse(manifest['gameplay_verified'])
            with self.assertRaises(FileExistsError):
                prepare(source, output)
            self.assertEqual((output/'source.glb').read_bytes(), source.read_bytes())


if __name__ == '__main__':
    unittest.main()

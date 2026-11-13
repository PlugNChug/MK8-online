import json
from pathlib import Path
import struct
import tempfile
import unittest

from toolchain.mii_library import inspect_glb, extract, SOURCE_HASHES


def triangle(index=2):
    binary = struct.pack('<9f3H', 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, index)
    binary += bytes((-len(binary)) % 4)
    doc = dict(buffers=[dict(byteLength=len(binary))],
               bufferViews=[dict(byteOffset=0, byteLength=36),
                            dict(byteOffset=36, byteLength=6)],
               accessors=[dict(bufferView=0, componentType=5126, count=3, type='VEC3'),
                          dict(bufferView=1, componentType=5123, count=3, type='SCALAR')],
               meshes=[dict(primitives=[dict(attributes={'POSITION': 0}, indices=1)])])
    raw = json.dumps(doc).encode()
    raw += b' '*((-len(raw)) % 4)
    return (struct.pack('<4sII', b'glTF', 2, 28+len(raw)+len(binary))
            + struct.pack('<I4s', len(raw), b'JSON') + raw
            + struct.pack('<I4s', len(binary), b'BIN\0') + binary)


class MiiLibraryTests(unittest.TestCase):
    def test_real_triangle_bounds_and_counts(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'triangle.glb'
            path.write_bytes(triangle())
            info = inspect_glb(path)
            self.assertEqual((info['vertices'], info['triangles']), (3, 1))
            self.assertEqual(info['bounds'], [[0, 1], [0, 1], [0, 0]])

    def test_bad_index_and_truncation_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'bad.glb'
            for data in (triangle(3), triangle()[:-1]):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    inspect_glb(path)

    def test_unknown_resource_does_not_create_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            resource = root/'unknown.dat'
            resource.write_bytes(b'not a supported resource')
            with self.assertRaisesRegex(ValueError, 'Unsupported resource'):
                extract(resource, root/'extractor', root/'output')
            self.assertFalse((root/'output').exists())

    def test_source_pins_are_complete_sha256_values(self):
        self.assertEqual(len(SOURCE_HASHES), 6)
        for value in SOURCE_HASHES.values():
            self.assertEqual(len(bytes.fromhex(value)), 32)


if __name__ == '__main__':
    unittest.main()

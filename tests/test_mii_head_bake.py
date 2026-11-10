"""Synthetic geometry and pixels only; no commercial or personal assets."""
from pathlib import Path
import struct
import tempfile
import unittest

from toolchain.mii_head_bake import (
    bary, blend, expand_mirrored_skin, head_vector, pack_yaz0, validate,
)


class MiiHeadBakeTests(unittest.TestCase):
    def test_head_axes_preserve_up_and_rotate_front_without_reflection(self):
        self.assertEqual(head_vector([1, 0, 0]), [1, 0, 0])
        self.assertEqual(head_vector([0, 0, 1]), [0, 1, 0])
        self.assertEqual(head_vector([0, 1, 0]), [0, 0, -1])
        vector = [0.2, -0.3, 0.4]
        self.assertAlmostEqual(sum(x*x for x in vector),
                               sum(x*x for x in head_vector(vector)))

    def test_mirrored_skin_keeps_both_halves_and_row_order(self):
        pixels = bytes(range(16))
        self.assertEqual(expand_mirrored_skin(pixels, 2, 2),
                         pixels[:8] + pixels[4:8] + pixels[:4]
                         + pixels[8:] + pixels[12:] + pixels[8:12])
        with self.assertRaises(ValueError):
            expand_mirrored_skin(pixels[:-1], 2, 2)

    def test_projection_rejects_outside_and_degenerate_triangles(self):
        triangle = [(0, 0), (1, 0), (0, 1)]
        self.assertEqual(bary((0.25, 0.25), *triangle), (0.5, 0.25, 0.25))
        self.assertIsNone(bary((2, 2), *triangle))
        self.assertIsNone(bary((0, 0), (0, 0), (0, 0), (0, 0)))

    def test_mask_blends_pixels_on_front_surface_and_preserves_outside(self):
        target = dict(positions=[[0, 1, 0], [1, 1, 0], [0, 1, 1]],
                      normals=[[0, 1, 0]]*3,
                      uv=[[0, 0], [1, 0], [0, 1]], indices=[0, 1, 2])
        mask = dict(target, texture='mask')
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'mask.rgba').write_bytes(bytes([255, 0, 0, 128])*4)
            data = dict(textures=[dict(name='mask', width=2, height=2)])
            out = blend(target, mask, (2, 2), bytes([255]*16), root, data)
            self.assertEqual(out[:4], bytes([255, 127, 127, 255]))
            self.assertEqual(out[-4:], bytes([255]*4))
            target['normals'] = [[0, -1, 0]]*3
            self.assertEqual(blend(target, mask, (2, 2), bytes([255]*16), root, data),
                             bytes([255]*16))

    def test_rejects_texture_path_before_reading_it(self):
        with self.assertRaisesRegex(ValueError, 'texture name'):
            validate(dict(parts=[{}]*6, textures=[dict(name='../private')]), Path('.'))

    def test_literal_yaz0_roundtrip_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source, output = root/'input.bfres', root/'MiiM.szs'
            original = b'FRES' + bytes(range(25))
            source.write_bytes(original)
            pack_yaz0(source, output)
            packed = output.read_bytes()
            self.assertEqual(packed[:4], b'Yaz0')
            self.assertEqual(struct.unpack_from('>I', packed, 4)[0], len(original))
            restored = bytearray()
            for offset in range(16, len(packed), 9):
                self.assertEqual(packed[offset], 255)
                restored.extend(packed[offset+1:offset+9])
            self.assertEqual(restored, original)
            with self.assertRaises(FileExistsError):
                pack_yaz0(source, output)
            self.assertEqual(source.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()

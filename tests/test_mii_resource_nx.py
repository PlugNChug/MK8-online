"""Authored AFL-shaped tables and pixels only; no game, firmware or AFL data."""
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from toolchain import mii_resource_nx as nx


def gob_offset(xb, y, width_gobs, bh):
    # Independent restatement of the runtime's gob_offset (upstream nvn/tegra.c) for one slice.
    gob_x, gob_y = xb >> 6, y >> 3
    block = 512 << bh
    off = (gob_y >> bh) * width_gobs * block + gob_x * block + (gob_y & ((1 << bh) - 1)) * 512
    xi, yi = xb & 63, y & 7
    return off + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 15))


def afl_texture(width, height, fmt, seed):
    bpp = nx.FORMAT_BPP[fmt]
    pixels = bytes((seed + i * 7) & 0xff for i in range(width * height * bpp))
    return pixels + struct.pack('>IHHBBBB', 0, width, height, 1, fmt, 0, 0)


def afl_shape(extra=0, vertices=3, hair=False):
    header = 0x48 + extra
    position = struct.pack(f'>{vertices * 4}f', *[float(i) for i in range(vertices * 4)])
    normal = struct.pack(f'>{vertices}I', *[0x1ff | (i << 10) for i in range(vertices)])
    uv = b'' if hair else struct.pack(f'>{vertices * 2}f', *[i / 8 for i in range(vertices * 2)])
    tangent = bytes(range(vertices * 4)) if hair else b''
    color = bytes(vertices * 4) if hair else b'\xff\xff\xff\xff'
    index = struct.pack('>3H', 0, 1, 2)
    parts, offsets, cursor = [position, normal, uv, tangent, color, index], [], header
    for p in parts:
        offsets.append(cursor if p else 0)
        cursor += len(p)
    sizes = [len(p) for p in parts]
    sizes[5] = 3  # u16 count
    head = struct.pack('>6I6I6f', *offsets, *sizes, -1, -2, -3, 1, 2, 3)
    head += struct.pack(f'>{extra // 4}f', *[0.5 * i for i in range(extra // 4)])
    return head + b''.join(parts)


def build_afl():
    """AFL envelope with every table present and a few parts filled."""
    textures = {('eye', 0): afl_texture(152, 128, 2, 1), ('noseline', 3): afl_texture(64, 16, 0, 2),
                ('beard', 1): afl_texture(8, 8, 1, 3)}
    shapes = {('faceline', 2): afl_shape(0x24), ('hair_normal', 5): afl_shape(0x48, hair=True),
              ('hair_cap', 5): afl_shape(hair=True), ('nose', 1): afl_shape()}
    table = bytearray(b'FFRA' + bytes(0x10) + bytes(11 * 4))
    blobs, entries = [], []
    for kinds, parts, prefix in ((nx.AFL_TEXTURES, textures, None), (nx.AFL_SHAPES, shapes, 'shape')):
        if prefix:
            table += bytes(12 * 4)
        for name, count in kinds:
            for i in range(count):
                entries.append((len(table), parts.get((name, i))))
                table += bytes(0x10)
    assert len(table) == 0x4cd0
    data = bytearray(table)
    for offset, raw in entries:
        if raw:
            packed = zlib.compress(raw)
            struct.pack_into('>III', data, offset, len(data), len(raw), len(packed))
            data += packed
    return bytes(data + bytes(max(0, 0x5000 - len(data))))


class MiiResourceNxTests(unittest.TestCase):
    def test_swizzle_matches_runtime_gob_layout_and_roundtrips(self):
        for width, height, bpp in ((152, 128, 4), (64, 16, 1), (8, 8, 2), (256, 40, 1)):
            bh = nx.default_block_height_log2(height)
            linear = bytes((i * 31 + 7) & 0xff for i in range(width * height * bpp))
            tiled = nx.swizzle(linear, width, height, bpp, bh)
            row = width * bpp
            for y in (0, height // 2, height - 1):
                for x in (0, row // 3, row - 1):
                    self.assertEqual(tiled[gob_offset(x, y, (row + 63) // 64, bh)], linear[y * row + x])
            self.assertEqual(nx.deswizzle(tiled, width, height, bpp, bh), linear)
        self.assertEqual([nx.default_block_height_log2(h) for h in (8, 16, 64, 128, 512)], [0, 1, 3, 4, 4])

    def test_texture_keeps_level_zero_and_footer_fields(self):
        texture, (width, height, fmt) = nx.convert_texture(afl_texture(64, 16, 0, 9))
        self.assertEqual((width, height, fmt), (64, 16, 0))
        self.assertEqual(nx.deswizzle(texture, 64, 16, 1, 1), afl_texture(64, 16, 0, 9)[:1024])
        with self.assertRaises(ValueError):
            nx.convert_texture(afl_texture(64, 16, 0, 9)[:-14])

    def test_shape_converts_positions_to_half_and_keeps_indices(self):
        blob, offsets, sizes, (index_offset, index_size), bbox, transform = nx.convert_shape(afl_shape(0x24), 'faceline')
        self.assertEqual(sizes[:3], [24, 12, 12])
        self.assertEqual(list(struct.unpack_from('<12e', blob, offsets[0])), [float(i) for i in range(12)])
        self.assertEqual(struct.unpack_from('<3H', blob, index_offset), (0, 1, 2))
        self.assertEqual((index_size, len(transform), bbox[0]), (6, 9, -1))
        hair = nx.convert_shape(afl_shape(0x48, hair=True), 'hair_normal')
        self.assertEqual((hair[2][2], hair[2][3], hair[2][4], len(hair[5])), (0, 12, 12, 18))
        self.assertIsNone(nx.convert_shape(bytes(0x48), 'hair_normal'))

    def test_rejects_bad_shapes(self):
        bad_index = bytearray(afl_shape())
        struct.pack_into('>3H', bad_index, len(bad_index) - 6, 0, 1, 9)
        bad_header = bytearray(afl_shape())
        struct.pack_into('>I', bad_header, 0, 0x40)
        for data in (bytes(bad_index), bytes(bad_header), afl_shape()[:0x40]):
            with self.assertRaises(ValueError):
                nx.convert_shape(data, 'nose')

    def test_tables_follow_game_offsets(self):
        afl = nx.Afl(build_afl())
        textures, report = nx.build_textures(afl)
        self.assertEqual((report['eye'], report['beard'], report['noseline']), (1, 1, 1))
        # Eye 0 is NX texture type 1, entry 0; beard AFL index 1 becomes NX beard 0 (type 3).
        eye = nx.TEXTURE_TABLE + 132 * nx.TEXTURE_ENTRY
        pos, size, packed = struct.unpack_from('<III', textures, eye)
        self.assertEqual(struct.unpack_from('<HHBBB', textures, eye + 0x14), (152, 128, 2, 1, 0))
        self.assertEqual(len(zlib.decompress(textures[pos:pos + packed])), size)
        self.assertEqual(struct.unpack_from('<I', textures, nx.TEXTURE_TABLE + (132 + 62 + 24) * nx.TEXTURE_ENTRY + 4)[0], 512)  # one GOB
        self.assertEqual(struct.unpack_from('<I', textures, 0x0c + 4)[0], size)
        shapes, report = nx.build_shapes(afl)
        self.assertEqual((report['faceline'], report['hair_normal'], report['nose']), (1, 1, 1))
        faceline = nx.SHAPE_TABLE + (4 + 2) * nx.SHAPE_ENTRY
        pos, size, packed = struct.unpack_from('<III', shapes, faceline)
        self.assertGreaterEqual(pos, nx.SHAPE_DATA)
        blob = zlib.decompress(shapes[pos:pos + packed])
        offsets = struct.unpack_from('<5I', shapes, faceline + 0x10)
        index_offset, index_size = struct.unpack_from('<II', shapes, faceline + 0x38)
        self.assertEqual(struct.unpack_from('<3H', blob, index_offset), (0, 1, 2))
        self.assertEqual(struct.unpack_from('<e', blob, offsets[0] + 2)[0], 1.0)
        self.assertEqual(struct.unpack_from('<9f', shapes, nx.FACELINE_TRANSFORMS + 2 * 0x24)[1], 0.5)
        self.assertEqual(struct.unpack_from('<18f', shapes, nx.HAIR_TRANSFORMS + 5 * 0x48)[2], 1.0)
        self.assertEqual(struct.unpack_from('<6f', shapes, faceline + 0x40), (-1, -2, -3, 1, 2, 3))

    def test_requires_verified_afl_and_new_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'AFL.dat'
            source.write_bytes(build_afl())
            with self.assertRaises(ValueError):
                nx.convert(source, Path(folder) / 'out')
            self.assertFalse((Path(folder) / 'out').exists())


if __name__ == '__main__':
    unittest.main()

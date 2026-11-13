"""Derive private MiiModel-layout resources (ShapeMid/High.dat, NXTexture*.dat) from the verified local AFL.

The layout follows what the MK8 4.0.1 Mii library reads (docs/STATUS.md, "Provedor substituto"):
fixed per-type entry tables, zlib parts, nn::gfx vertex formats and Tegra block-linear textures.
Output stays private, like the AFL itself. Rendering in the game is not qualified by this tool.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zlib

AFL_SHA256 = '4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722'
AFL_MAX = 8 * 1024 * 1024
OUTPUT_FILES = ('ShapeMid.dat', 'ShapeHigh.dat', 'NXTextureLowLinear.dat', 'NXTextureLowSRGB.dat',
                'NXTextureMidLinear.dat', 'NXTextureMidSRGB.dat')

# AFL (FFL order, big-endian): 0x14 header, then texture maxSize[11] + 0x10-byte entries,
# then shape maxSize[12] + entries.
AFL_TEXTURES = (('beard', 3), ('hat', 132), ('eye', 80), ('eyebrow', 28), ('faceline', 12), ('makeup', 12),
                ('glass', 20), ('mole', 2), ('mouth', 52), ('mustache', 6), ('noseline', 18))
AFL_SHAPES = (('beard', 4), ('hat_normal', 132), ('hat_cap', 132), ('faceline', 12), ('glass', 1), ('mask', 12),
              ('noseline', 18), ('nose', 18), ('hair_normal', 132), ('hair_cap', 132), ('forehead_normal', 132),
              ('forehead_cap', 132))

# NX tables as read by f_70008eabc8 / f_70008ea9d4. Counts come from the executable's tables
# (0x7000f766b0 / 0x7000f76630). (NX name, AFL name, first AFL index) per type, in NX type order.
NX_TEXTURES = (('hat', 132, 'hat', 0), ('eye', 62, 'eye', 0), ('eyebrow', 24, 'eyebrow', 0), ('beard', 2, 'beard', 1),
               ('faceline', 12, 'faceline', 0), ('makeup', 12, 'makeup', 0), ('glass', 20, 'glass', 0),
               ('mole', 2, 'mole', 0), ('mouth', 37, 'mouth', 0), ('mustache', 6, 'mustache', 0),
               ('noseline', 18, 'noseline', 0))
# Types 0-2 and 9 are fixed by code (GetBeardType/GetFacelineType, single glass). The hat/forehead/hair
# grouping of 3-8 and nose/noseline of 10-11 are inferred from draw slots; confirm in game.
NX_SHAPES = (('beard', 4), ('faceline', 12), ('mask', 12), ('hat_normal', 132), ('hat_cap', 132),
             ('forehead_normal', 132), ('forehead_cap', 132), ('hair_normal', 132), ('hair_cap', 132),
             ('glass', 1), ('nose', 18), ('noseline', 18))
TEXTURE_TABLE, TEXTURE_ENTRY = 0x64, 0x1c
SHAPE_TABLE, SHAPE_ENTRY = 0x6c, 0x58
HAIR_TRANSFORMS = 0x12704   # 132 x 0x48, f_70008ea904
FACELINE_TRANSFORMS = 0x14c24  # 12 x 0x24, f_70008ea920
SHAPE_DATA = 0x14dd4
ALIGNMENT = 0x1000
FORMAT_BPP = {0: 1, 1: 2, 2: 4}  # AFL/NX index: R8, R8G8, R8G8B8A8 (nn::gfx 0x201/0x901/0xb01)


def _bounded(value, low, high, what):
    if not low <= value <= high:
        raise ValueError(f'{what} outside supported range')
    return value


class Afl:
    def __init__(self, raw):
        if not 0x5000 <= len(raw) <= AFL_MAX or raw[:4] != b'FFRA':
            raise ValueError('Expected the verified AFL resource')
        self.raw = raw
        offset = 0x14 + 11 * 4
        self.textures, offset = self._table(offset, AFL_TEXTURES)
        offset += 12 * 4
        self.shapes, end = self._table(offset, AFL_SHAPES)
        if end != 0x4cd0:
            raise ValueError('Unexpected AFL table layout')

    def _table(self, offset, kinds):
        result = {}
        for name, count in kinds:
            entries = []
            for _ in range(count):
                pos, size, packed = struct.unpack_from('>III', self.raw, offset)
                offset += 0x10
                if size == 0:
                    entries.append(None)
                    continue
                _bounded(pos, 0x4cd0, len(self.raw) - 1, 'Part position')
                _bounded(packed, 1, len(self.raw) - pos, 'Compressed part')
                _bounded(size, 1, 0x200000, 'Part size')
                entries.append((pos, size, packed))
            result[name] = entries
        return result, offset

    def part(self, info):
        pos, size, packed = info
        data = zlib.decompressobj().decompress(self.raw[pos:pos + packed], size + 1)
        if len(data) != size:
            raise ValueError('AFL part size mismatch')
        return data


def _div(a, b):
    return (a + b - 1) // b


def default_block_height_log2(height):
    """Mirrors the runtime's tegra_default_block_height_log2 (upstream nvn/tegra.c)."""
    gobs, log2 = _div(height, 8), 0
    while (1 << log2) < gobs and log2 < 4:
        log2 += 1
    return log2


def swizzle(linear, width, height, bpp, bh_log2):
    """Pitch-linear rows -> Tegra block-linear (GOB 64x8 bytes), the inverse of the runtime's deswizzle."""
    row = width * bpp
    width_gobs = _div(row, 64)
    block = 512 << bh_log2
    out = bytearray(width_gobs * block * _div(height, 8 << bh_log2))
    for y in range(height):
        gob_y = y >> 3
        base = (gob_y >> bh_log2) * width_gobs * block + (gob_y & ((1 << bh_log2) - 1)) * 512
        yi = y & 7
        for x in range(0, row, 16):
            xi = x & 63
            o = base + (x >> 6) * block + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4))
            n = min(16, row - x)
            out[o:o + n] = linear[y * row + x:y * row + x + n]
    return bytes(out)


def deswizzle(data, width, height, bpp, bh_log2):
    row = width * bpp
    width_gobs = _div(row, 64)
    block = 512 << bh_log2
    out = bytearray(row * height)
    for y in range(height):
        gob_y = y >> 3
        base = (gob_y >> bh_log2) * width_gobs * block + (gob_y & ((1 << bh_log2) - 1)) * 512
        yi = y & 7
        for x in range(0, row, 16):
            xi = x & 63
            o = base + (x >> 6) * block + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4))
            n = min(16, row - x)
            out[y * row + x:y * row + x + n] = data[o:o + n]
    return bytes(out)


def convert_texture(data):
    """AFL texture (linear level 0 + mips, 12-byte BE footer) -> (block-linear level 0, footer fields)."""
    if len(data) < 12:
        raise ValueError('Truncated AFL texture')
    _mip_offset, width, height, _levels, fmt = struct.unpack('>IHHBB', data[-12:-2])
    if fmt not in FORMAT_BPP or not (1 <= width <= 1024 and 1 <= height <= 1024):
        raise ValueError('Unsupported AFL texture format or size')
    bpp = FORMAT_BPP[fmt]
    level0 = width * height * bpp
    if level0 > len(data) - 12:
        raise ValueError('AFL texture level exceeds data')
    bh = default_block_height_log2(height)
    # One level: the AFL mip layout is not yet decoded, and nn::gfx accepts a single-level texture.
    return swizzle(data[:level0], width, height, bpp, bh), (width, height, fmt)


def _half(values):
    return struct.pack(f'<{len(values)}e', *values)


def convert_shape(data, kind):
    """AFL shape (BE header + elements) -> (NX blob, attribute offsets/sizes, index, bbox, transform)."""
    # Faceline carries hair/nose/beard positions (0x24); normal hair carries front/side/top (0x48).
    extra = {'faceline': 0x24, 'hair_normal': 0x48}.get(kind, 0)
    if len(data) < 0x48:
        raise ValueError('Truncated AFL shape')
    pos = struct.unpack_from('>6I', data, 0)
    size = list(struct.unpack_from('>6I', data, 0x18))
    bbox = struct.unpack_from('>6f', data, 0x30)
    if not any(size):
        return None  # Present but empty (for example a bald hair style): the NX entry stays absent.
    if len(data) < 0x48 + extra:
        raise ValueError('Truncated AFL shape')
    if pos[0] != 0x48 + extra:
        raise ValueError('Unexpected AFL shape header')
    size[5] *= 2  # The index element stores a u16 count, not bytes.
    for p, s in zip(pos, size):
        if s and (p < pos[0] or p + s > len(data)):
            raise ValueError('AFL shape element outside part')
    transform = struct.unpack_from(f'>{extra // 4}f', data, 0x48) if extra else ()
    if size[0] % 16 or not size[0]:
        raise ValueError('Unexpected AFL position stride')
    vertices = size[0] // 16
    if size[1] not in (0, vertices * 4) or size[2] not in (0, vertices * 8) or size[3] not in (0, vertices * 4) \
            or size[4] not in (0, 4, vertices * 4) or not size[5] or size[5] % 6:
        raise ValueError('Inconsistent AFL vertex attributes')
    position = struct.unpack_from(f'>{vertices * 4}f', data, pos[0])
    normal = struct.unpack_from(f'>{size[1] // 4}I', data, pos[1]) if size[1] else ()
    uv = struct.unpack_from(f'>{size[2] // 4}f', data, pos[2]) if size[2] else ()
    indices = struct.unpack_from(f'>{size[5] // 2}H', data, pos[5])
    if indices and max(indices) >= vertices or len(indices) % 3:
        raise ValueError('AFL index outside vertex buffer')
    elements = [
        _half(position),                                   # 0x1505 R16G16B16A16 float
        struct.pack(f'<{len(normal)}I', *normal),          # 0x0e02 R10G10B10A2 snorm (same bits)
        _half(uv),                                         # 0x1205 R16G16 float
        data[pos[3]:pos[3] + size[3]],                     # 0x0b02 R8G8B8A8 snorm, byte order kept
        data[pos[4]:pos[4] + size[4]],                     # 0x0b01 R8G8B8A8 unorm
    ]
    blob, offsets, sizes = bytearray(), [], []
    for element in elements:
        while len(blob) % 0x10:
            blob.append(0)
        offsets.append(len(blob) if element else 0)
        sizes.append(len(element))
        blob += element
    while len(blob) % 0x10:
        blob.append(0)
    index_offset = len(blob)
    blob += struct.pack(f'<{len(indices)}H', *indices)
    return bytes(blob), offsets, sizes, (index_offset, len(indices) * 2), bbox, transform


def _pack(data):
    return zlib.compress(data, 9)


def build_textures(afl):
    header = bytearray(TEXTURE_TABLE + sum(c for _, c, _, _ in NX_TEXTURES) * TEXTURE_ENTRY)
    header[:4] = b'NFTR'
    payload, entry, report = bytearray(), TEXTURE_TABLE, {}
    max_size = [0] * len(NX_TEXTURES)
    for kind, (name, count, source, first) in enumerate(NX_TEXTURES):
        written = 0
        for i in range(count):
            index = first + i
            info = afl.textures[source][index] if index < len(afl.textures[source]) else None
            if info:
                texture, (width, height, fmt) = convert_texture(afl.part(info))
                packed = _pack(texture)
                pos = len(header) + len(payload)
                struct.pack_into('<IIIBBBB', header, entry, pos, len(texture), len(packed), 9, 15, 8, 0)
                # +0x10: level-0 size, width, height, format, levels, tile mode (0 = optimal/block-linear).
                struct.pack_into('<IHHBBBB', header, entry + 0x10, len(texture), width, height, fmt, 1, 0, 0)
                payload += packed
                while len(payload) % 4:
                    payload.append(0)
                max_size[kind] = max(max_size[kind], len(texture))
                written += 1
            entry += TEXTURE_ENTRY
        report[name] = written
    struct.pack_into('<11I', header, 0x0c, *max_size)
    struct.pack_into('<11I', header, 0x38, *[ALIGNMENT if s else 0 for s in max_size])
    return bytes(header + payload), report


def build_shapes(afl):
    header = bytearray(SHAPE_DATA)
    header[:4] = b'NFSR'
    payload, entry, report = bytearray(), SHAPE_TABLE, {}
    max_size = [0] * len(NX_SHAPES)
    for kind, (name, count) in enumerate(NX_SHAPES):
        written = 0
        for index in range(count):
            info = afl.shapes[name][index] if index < len(afl.shapes[name]) else None
            if info:
                shape = convert_shape(afl.part(info), name)
            if info and shape:
                blob, offsets, sizes, (index_offset, index_size), bbox, transform = shape
                packed = _pack(blob)
                pos = SHAPE_DATA + len(payload)
                struct.pack_into('<IIIBBBB', header, entry, pos, len(blob), len(packed), 9, 15, 8, 0)
                struct.pack_into('<5I5III6f', header, entry + 0x10, *offsets, *sizes, index_offset, index_size, *bbox)
                if name == 'hair_normal':
                    struct.pack_into('<18f', header, HAIR_TRANSFORMS + index * 0x48, *transform)
                elif name == 'faceline':
                    struct.pack_into('<9f', header, FACELINE_TRANSFORMS + index * 0x24, *transform)
                payload += packed
                while len(payload) % 4:
                    payload.append(0)
                max_size[kind] = max(max_size[kind], len(blob))
                written += 1
            entry += SHAPE_ENTRY
        report[name] = written
    if entry != HAIR_TRANSFORMS:
        raise ValueError('NX shape table layout mismatch')
    struct.pack_into('<12I', header, 0x0c, *max_size)
    struct.pack_into('<12I', header, 0x3c, *[ALIGNMENT if s else 0 for s in max_size])
    return bytes(header + payload), report


def convert(afl_path, output):
    afl_path, output = Path(afl_path), Path(output)
    if afl_path.stat().st_size > AFL_MAX:
        raise ValueError('AFL exceeds size limit')
    raw = afl_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != AFL_SHA256:
        raise ValueError('Unsupported AFL resource; expected the verified AFLResHigh_2_3.dat')
    afl = Afl(raw)
    textures, texture_report = build_textures(afl)
    shapes, shape_report = build_shapes(afl)  # Build and validate everything before writing.
    if shutil.disk_usage(output.parent).free < 4 * 1024**3:
        raise ValueError('At least 4 GiB free required')
    output.mkdir(exist_ok=False)
    files = {}
    for name in OUTPUT_FILES:
        data = shapes if name.startswith('Shape') else textures
        (output / name).write_bytes(data)
        files[name] = hashlib.sha256(data).hexdigest()
    manifest = dict(schema=1, afl_sha256=AFL_SHA256, files=files, textures=texture_report, shapes=shape_report,
                    texture_levels=1, inferred_types=['shape 3-8 hat/forehead/hair grouping', 'shape 10-11 nose/noseline'],
                    rendering_verified=False, gameplay_verified=False, multiplayer_verified=False)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('afl', type=Path)
    parser.add_argument('output', type=Path, help='New private folder for the six MiiModel-layout files')
    args = parser.parse_args()
    print(json.dumps(convert(args.afl, args.output), indent=2))

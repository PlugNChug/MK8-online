"""extract_mlfi fails closed on layout changes and extracts a synthetic provider layout correctly.

The extractor runs against AMD's signed DLL, which is never committed, so these tests build a synthetic
PE32+ image that mirrors the parts the tool relies on: a .rdata section holding descriptor records,
back-to-back SPIR-V modules, the 191+191 pass-name pool and a dense fp16 model bank. They exercise the
real code paths (PE parsing, module boundaries, pool validation, model discovery, output guard) rather
than looking for text in the source.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import extract_mlfi


RESOURCES = list(extract_mlfi.RESOURCE_NAMES)


def instr(op: int, *operands: int) -> bytes:
    return struct.pack('<I', ((len(operands) + 1) << 16) | op) + struct.pack(f'<{len(operands)}I', *operands)


def instr_str(op: int, operands, text: bytes) -> bytes:
    padded = text + b'\0' * (-len(text) % 4)
    count = 1 + len(operands) + len(padded) // 4
    return struct.pack('<I', (count << 16) | op) + struct.pack(f'<{len(operands)}I', *operands) + padded


def spv_module(entry: str, bindings=(), extra_globals=b'') -> bytes:
    """Minimal valid compute module: one function, optional named descriptor variables."""
    words = [0x07230203, 0x00010500, 0, 3, 0]  # magic, version 1.5, generator, bound (fixed below), schema
    body = b''
    body += instr(17, 1)                              # OpCapability Shader
    body += instr(14, 0, 1)                           # OpMemoryModel Logical GLSL450
    body += instr_str(15, (5, 2), entry.encode() + b'\0')  # OpEntryPoint GLCompute %2 "entry"
    body += instr(16, 2, 17, 1, 1, 1)                    # OpExecutionMode LocalSize 1 1 1
    body += instr_str(5, (2,), entry.encode() + b'\0')    # OpName %2
    for index, name in enumerate(bindings):
        ident = 100 + index
        body += instr_str(5, (ident,), name.encode() + b'\0')
        body += instr(71, ident, 34, 0)               # DescriptorSet 0
        body += instr(71, ident, 33, 0)               # Binding 0
    body += extra_globals
    body += instr(19, 1)                              # %1 = OpTypeVoid
    body += instr(33, 3, 1)                           # %3 = OpTypeFunction %1
    body += instr(54, 1, 2, 0, 3)                     # %2 = OpFunction
    body += instr(248, 4)                             # %4 = OpLabel
    body += instr(253)                                # OpReturn
    body += instr(56)                                 # OpFunctionEnd
    words[3] = 200                                    # bound
    return struct.pack('<5I', *words) + body


def pass_pool_block() -> bytes:
    names = ['mlfi_4k_pass0'] + RESOURCES + [f'mlfi_4k_pass{i}' for i in range(1, 191)]
    names += ['r_hudless_ui', 'rw_prediction']
    names += [f'mlfi_8k_pass{i}' for i in range(191)]
    names += ['4.0.2']
    out = b''
    for name in names:
        raw = name.encode() + b'\0'
        out += raw + b'\0' * (-len(raw) % 16)
    return out


def model_block(size: int = 4 << 20) -> bytes:
    """Dense fp16 values with a small exponent (weight-like), plus a zero table in the middle."""
    pattern = struct.pack('<4H', 0x3123, 0xB2CD, 0x2E1F, 0x9A87)
    block = bytearray(pattern * (size // len(pattern)))
    middle = len(block) // 2
    block[middle:middle + 4096] = b'\0' * 4096
    return bytes(block)


def build_image() -> bytes:
    rdata = b''
    rdata += b'\0' * 8
    rdata += spv_module('mlfi_4k_pass0')            # module A
    rdata += b'\x28\x34\x2e\x82\x01\0\0\0'           # descriptor-looking gap
    rdata += spv_module('mlfi_4k_pass1')            # module B, back to back
    rdata += b'\0' * 16
    pool_at = len(rdata)
    rdata += pass_pool_block()
    model_at = len(rdata)
    rdata += model_block()
    rdata += b'\0' * 8
    rdata += spv_module('mlfi_4k_pass2')            # trailing module bounds the bank

    section_raw = 0x200
    header = bytearray(section_raw)
    header[0:2] = b'MZ'
    struct.pack_into('<I', header, 0x3C, 0x40)
    header[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', header, 0x44, 0x8664, 1, 0, 0, 0, 0xF0, 0x22)
    struct.pack_into('<H', header, 0x58, 0x20B)      # PE32+ magic in the optional header
    table = 0x40 + 24 + 0xF0
    header[table:table + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', header, table + 8, len(rdata), 0x1000, len(rdata), section_raw)
    image = bytes(header) + rdata
    image = image[:section_raw] + rdata
    return image, pool_at, model_at


class SyntheticLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mlfi-test-', dir=Path.home() / '.cache' / 'mk8online')
        cls.image, cls.pool_at, cls.model_at = build_image()
        cls.dll = Path(cls.tmp.name) / 'synthetic_framegeneration_vk.dll'
        cls.dll.write_bytes(cls.image)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_pe_sections(self):
        fmt, sections = extract_mlfi.parse_pe(self.image)
        self.assertEqual(fmt, 'pei-x86-64')
        self.assertEqual([section.name for section in sections], ['.rdata'])
        self.assertEqual(sections[0].raw_size, len(self.image) - 0x200)

    def test_module_boundaries_do_not_swallow_neighbours(self):
        modules = extract_mlfi.scan_spirv(self.image)
        self.assertEqual([module.entry for module in modules],
                         ['mlfi_4k_pass0', 'mlfi_4k_pass1', 'mlfi_4k_pass2'])
        self.assertTrue(all(module.functions == 1 for module in modules))
        self.assertLess(modules[0].offset + modules[0].size, modules[1].offset)

    def test_pass_pool_counts_and_version(self):
        _fmt, sections = extract_mlfi.parse_pe(self.image)
        pool = extract_mlfi.find_pass_pool(self.image, sections)
        self.assertEqual(pool.version, '4.0.2')
        self.assertEqual(pool.count('mlfi_4k_pass'), 191)
        self.assertEqual(pool.count('mlfi_8k_pass'), 191)
        self.assertGreater(pool.end, pool.offset)

    def test_pass_pool_rejects_reordered_or_unknown_names(self):
        _fmt, sections = extract_mlfi.parse_pe(self.image)
        good = pass_pool_block()
        broken = good.replace(b'mlfi_4k_pass2\0', b'mlfi_4k_passX\0')
        image = self.image.replace(good, broken)
        with self.assertRaises(extract_mlfi.LayoutError):
            extract_mlfi.find_pass_pool(image, sections)

    def test_model_segments_split_on_the_zero_table(self):
        start = 0x200 + self.model_at
        segments = extract_mlfi.model_segments(self.image, start, len(self.image))
        self.assertEqual(len(segments), 3)
        self.assertGreater(segments[0][1], segments[0][0])
        histogram = extract_mlfi.fp16_histogram(self.image, start, len(self.image) - start)
        self.assertEqual(histogram['words'], (len(self.image) - start) // 2)
        self.assertGreaterEqual(histogram['zero_words'], 2048)

    def test_heuristic_model_region_finds_the_bank(self):
        modules = extract_mlfi.scan_spirv(self.image)
        start, end = extract_mlfi.find_model_region_heuristic(self.image, modules)
        self.assertGreaterEqual(start, 0x200 + self.model_at)
        self.assertLess(start, 0x200 + self.model_at + 0x10000)
        self.assertGreater(end - start, extract_mlfi.MIN_HEURISTIC_BYTES)

    def test_output_guard_refuses_the_repository(self):
        with self.assertRaises(extract_mlfi.OutputGuardError):
            extract_mlfi.check_output_path(ROOT / 'local' / 'mlfi')
        outside = Path(self.tmp.name) / 'out'
        self.assertEqual(extract_mlfi.check_output_path(outside), outside.resolve())

    def test_unpinned_extraction_writes_a_manifest_and_refuses_replacement(self):
        out = Path(self.tmp.name) / 'extract'
        manifest = extract_mlfi.extract(self.dll, out, allow_unknown=True, dry_run=False,
                                        validate_spirv=False, force=False)
        self.assertFalse(manifest['pinned'])
        self.assertEqual(manifest['provider_version'], '4.0.2')
        written = json.loads((out / 'manifest.json').read_text())
        self.assertEqual(written['content_root'], manifest['content_root'])
        self.assertEqual(len(written['modules']), 3)
        blob = (out / 'model' / 'model_blob.bin').read_bytes()
        self.assertEqual(hashlib.sha256(blob).hexdigest(), written['model']['sha256'])
        with self.assertRaises(extract_mlfi.OutputGuardError):
            extract_mlfi.extract(self.dll, out, allow_unknown=True, dry_run=False,
                                 validate_spirv=False, force=False)
        replaced = extract_mlfi.extract(self.dll, out, allow_unknown=True, dry_run=False,
                                        validate_spirv=False, force=True)
        self.assertEqual(replaced['content_root'], manifest['content_root'])

    def test_unknown_dll_is_rejected_without_the_override(self):
        with self.assertRaises(extract_mlfi.UnpinnedError):
            extract_mlfi.extract(self.dll, Path(self.tmp.name) / 'nope', allow_unknown=False,
                                 dry_run=True, validate_spirv=False, force=False)


class PinnedLayoutTests(unittest.TestCase):
    """Only runs when the real SDK 2.4.0 DLL is available; never in the default test run."""

    dll = os.environ.get('MK8_MLFI_DLL')

    @unittest.skipUnless(dll and Path(dll).is_file(), 'MK8_MLFI_DLL not set')
    def test_pinned_build_counts(self):
        manifest = extract_mlfi.extract(Path(self.dll), Path.home() / '.cache' / 'mk8online' / 'mlfi-test',
                                        allow_unknown=False, dry_run=True, validate_spirv=False, force=False)
        self.assertTrue(manifest['pinned'])
        self.assertEqual(manifest['module_counts'], extract_mlfi.PINNED['modules'])
        self.assertEqual(manifest['pass_pool']['counts']['mlfi_4k'], 191)
        self.assertEqual(manifest['pass_pool']['counts']['mlfi_8k'], 191)
        self.assertEqual(manifest['model']['sha256'], extract_mlfi.PINNED['model']['sha256'])


if __name__ == '__main__':
    unittest.main()

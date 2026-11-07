"""Small authored containers exercise both NCZ encodings without console data."""
from io import BytesIO
from pathlib import Path
import struct
import sys
import unittest

from Crypto.Cipher import AES
from zstandard import ZstdCompressor

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolchain"))
from import_mk8 import Entry, HEADER, entries, ncz_chunks


class PackageTests(unittest.TestCase):
    def test_ncz_stream_and_blocks_restore_identical_nca(self):
        block = 1 << 14
        plain = b"a" * block + bytes(range(256))
        key, nonce = b"k" * 16, b"n" * 8
        body = AES.new(key, AES.MODE_CTR, nonce=nonce, initial_value=HEADER // 16).encrypt(plain)
        header = b"h" * HEADER
        sections = b"NCZSECTN" + struct.pack("<Q", 1)
        sections += struct.pack("<QQQQ16s16s", HEADER, len(plain), 3, 0, key, nonce + b"\0" * 8)
        compressor = ZstdCompressor()
        first, last = compressor.compress(plain[:block]), plain[block:]
        blocks = struct.pack("<8sBBBBIQII", b"NCZBLOCK", 2, 1, 0, 14, 2, len(plain), len(first), len(last))
        for encoding in (compressor.compress(plain), blocks + first + last):
            data = header + sections + encoding
            self.assertEqual(b"".join(ncz_chunks(Entry(BytesIO(data), 0, len(data)), HEADER + len(plain))), header + body)
            with self.assertRaises(ValueError):
                list(ncz_chunks(Entry(BytesIO(data), 0, len(data)), HEADER + len(plain) + 1))

    def test_pfs_members_cannot_read_past_container(self):
        name = b"program.ncz\0"
        prefix = struct.pack("<4sIII", b"PFS0", 1, len(name), 0)
        data = prefix + struct.pack("<QQII", 0, 4, 0, 0) + name + b"abcd"
        index = entries(BytesIO(data), len(data))
        self.assertEqual(Entry(BytesIO(data + b"secret"), *index[b"program.ncz"]).read(99), b"abcd")
        with self.assertRaises(ValueError):
            entries(BytesIO(data[:-1]), len(data) - 1)


if __name__ == "__main__":
    unittest.main()

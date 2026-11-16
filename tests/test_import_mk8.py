"""Small authored containers exercise both NCZ encodings without console data."""
from io import BytesIO
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

from Crypto.Cipher import AES
from zstandard import ZstdCompressor

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolchain"))
from import_mk8 import Entry, HEADER, entries, ncz_chunks
import import_mk8 as importer


def package(contents):
    names = b""
    table = b""
    body = b""
    for name, payload in contents.items():
        table += struct.pack("<QQII", len(body), len(payload), len(names), 0)
        names += name + b"\0"
        body += payload
    return struct.pack("<4sIII", b"PFS0", len(contents), len(names), 0) + table + names + body


class PackageTests(unittest.TestCase):
    def test_base_variants_require_one_known_content_id(self):
        for digest in importer.BASE_VARIANTS:
            for extension in (b".nca", b".ncz"):
                with self.subTest(digest=digest, extension=extension):
                    index = {digest[:32].encode() + extension: (0, 1)}
                    self.assertEqual(importer.base_content(index), digest)
        for index in ({}, {b"0" * 32 + b".nca": (0, 1)},
                      {digest[:32].encode() + b".nca": (0, 1) for digest in importer.BASE_VARIANTS}):
            with self.subTest(index=index), self.assertRaises(ValueError):
                importer.base_content(index)

    def test_selected_base_still_checks_size_and_full_hash(self):
        payload = b"conteudo sintetico"
        digest = hashlib.sha256(payload).hexdigest()
        name = digest[:32].encode() + b".nca"
        for body in (payload, payload[:-1], b"x" * len(payload)):
            with self.subTest(body=body), tempfile.TemporaryDirectory() as folder:
                target = Path(folder) / "base.nca"
                with patch.dict(importer.SIZES, {digest: len(payload)}):
                    if body == payload:
                        importer.write_nca(BytesIO(body), {name: (0, len(body))}, digest, target)
                        self.assertEqual(target.read_bytes(), payload)
                    else:
                        with self.assertRaises(ValueError):
                            importer.write_nca(BytesIO(body), {name: (0, len(body))}, digest, target)

    def test_prepare_selects_each_base_variant_without_doubling_disk_budget(self):
        """Contêineres autorais exercitam seleção, cópia e hashes; LibHac é validado separadamente."""
        bases = [b"h" * HEADER + b"base A", b"h" * HEADER + b"base B"]
        update, control = b"update sintetico", b"controle sintetico"
        digests = [hashlib.sha256(payload).hexdigest() for payload in (*bases, update, control)]
        for base_index, base_payload in enumerate(bases):
            for compressed in (False, True):
                with self.subTest(base=base_index, compressed=compressed), tempfile.TemporaryDirectory() as folder:
                    root = Path(folder)
                    tools = root / "tools/mk8-import"
                    tools.mkdir(parents=True)
                    (tools / "mk8-import.dll").touch()
                    encoded = base_payload
                    if compressed:
                        section = struct.pack("<QQQQ16s16s", HEADER, len(base_payload) - HEADER, 1, 0, b"\0" * 16, b"\0" * 16)
                        encoded = base_payload[:HEADER] + b"NCZSECTN" + struct.pack("<Q", 1) + section
                        encoded += ZstdCompressor().compress(base_payload[HEADER:])
                    base_name = digests[base_index][:32].encode() + (b".ncz" if compressed else b".nca")
                    (root / "base.nsp").write_bytes(package({base_name: encoded}))
                    (root / "update.nsp").write_bytes(package({digests[2][:32].encode() + b".nca": update,
                                                               digests[3][:32].encode() + b".nca": control}))
                    (root / "fixture.keys").write_bytes(b"fixture sem chaves reais")
                    config = {key: str(root / name) for key, name in
                              (("base_package", "base.nsp"), ("update_package", "update.nsp"), ("prod_keys", "fixture.keys"))}

                    def extract(base, patch_nca, control_nca, keys, title_keys, scratch, output, tool, workers):
                        self.assertEqual([path.read_bytes() for path in (base, patch_nca, control_nca)],
                                         [base_payload, update, control])
                        (output / "fixture").write_bytes(b"extraido")

                    sizes = {digest: len(payload) for digest, payload in zip(digests, (*bases, update, control))}
                    with patch.multiple(importer, ROOT=root, BASE=digests[0], BASE_VARIANTS=tuple(digests[:2]),
                                        PATCH=digests[2], CONTROL=digests[3], SIZES=sizes), \
                            patch.object(importer, "require_space") as space, \
                            patch.object(importer, "version", side_effect=lambda name: {"pycryptodome": "3.23.0", "zstandard": "0.25.0"}[name]), \
                            patch.object(importer, "io_workers", return_value=1), \
                            patch.object(importer, "extract", side_effect=extract) as extracted, \
                            patch.object(importer, "validate", side_effect=lambda output, validate: {"exefs": str(output / "exefs")}):
                        local = root / "local"
                        result = importer.prepare(config, local, lambda path: None)
                        extracted.assert_called_once()
                        expected_reserve = 16 * 1024**3 + 128 * 1024**2 + len(base_payload) + len(update) + len(control)
                        space.assert_called_once_with(local, expected_reserve)
                        self.assertEqual(Path(result["exefs"]).parent, local / "imports/mk8-4.0.1")
                        self.assertEqual((local / "imports/mk8-4.0.1/fixture").read_bytes(), b"extraido")

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

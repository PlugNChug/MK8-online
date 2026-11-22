"""Generated sources can differ in line endings; every other byte remains pinned."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'toolchain'))
from guest_source import verified_bytes


class VerifiedGuestSourceTests(unittest.TestCase):
    def test_windows_and_unix_sources_produce_the_exact_pinned_bytes(self):
        original = b'first\r\nsecond\r\n'
        expected = hashlib.sha256(original).hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'unit.c'
            for source in (original, b'first\nsecond\n'):
                with self.subTest(source=source):
                    path.write_bytes(source)
                    self.assertEqual(verified_bytes(path, expected, 'wrong unit'), original)
                    self.assertEqual(path.read_bytes(), source)
            for changed in (b'first\nsecond\r\n', b'first\r\r\nsecond\r\n',
                            b'first\nsecond', b'first\nother\n'):
                with self.subTest(source=changed):
                    path.write_bytes(changed)
                    with self.assertRaisesRegex(ValueError, 'wrong unit'):
                        verified_bytes(path, expected, 'wrong unit')

    def test_existing_patch_output_with_mixed_endings_needs_an_exact_pin(self):
        patched = b"new\r\noriginal\r\r\n"
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "unit.c"
            expected = hashlib.sha256(patched).hexdigest()
            source.write_bytes(patched)
            self.assertEqual(verified_bytes(source, expected, "bad pin"), patched)
            source.write_bytes(b"new\r\noriginal\r\n")
            with self.assertRaisesRegex(ValueError, "bad pin"):
                verified_bytes(source, expected, "bad pin")

    def test_only_exact_pins_and_pure_line_ending_conversion(self):
        lf = b"one\ntwo\n"
        crlf = b"one\r\ntwo\r\n"
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "unit.c"
            for pinned in (lf, crlf):
                expected = hashlib.sha256(pinned).hexdigest()
                for raw in (lf, crlf):
                    source.write_bytes(raw)
                    self.assertEqual(verified_bytes(source, expected, "bad pin"), pinned)
                for raw in (b"one\r\ntwo\n", b"one\rtwo\r", b"one\nTWO\n"):
                    source.write_bytes(raw)
                    with self.assertRaisesRegex(ValueError, "bad pin"):
                        verified_bytes(source, expected, "bad pin")

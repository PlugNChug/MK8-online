import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolchain"))
from guest_source import verified_bytes


class GuestSourceTests(unittest.TestCase):
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

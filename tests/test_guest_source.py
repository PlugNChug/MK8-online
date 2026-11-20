import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "toolchain"))
from guest_source import verified_bytes


class GuestSourceTests(unittest.TestCase):
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

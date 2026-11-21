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

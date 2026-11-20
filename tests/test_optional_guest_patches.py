"""The optional guest hooks must match the current 4.0.1 generated units.

The generated AArch64 translation is private game data and is not in the
repository. This test uses a local generated tree when one exists (for example
the builder's `local/builds/<id>/generated/main`) and otherwise skips, so CI
without game files stays green. When the tree is present it checks the pinned
hashes and that every patch anchor still applies.
"""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))

import settings_menu  # noqa: E402
import ultrawide  # noqa: E402
import ultrawide_hud  # noqa: E402


def generated_main():
    explicit = os.environ.get('MK8_GENERATED_MAIN')
    if explicit and Path(explicit).is_dir():
        return Path(explicit)
    candidates = [ROOT / 'local/windows-rtx2/generated-new/main']
    candidates += sorted(ROOT.glob('local/builds/*/generated/main'))
    candidates += sorted(ROOT.glob('local/qualification/*/builds/*/generated/main'))
    candidates += sorted(ROOT.glob('local/qualification/builds/*/generated/main'))
    for candidate in candidates:
        if (candidate / 'main_077.c').is_file():
            return candidate
    return None


class OptionalGuestPatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = generated_main()
        if cls.main is None:
            raise unittest.SkipTest('no generated 4.0.1 tree; set MK8_GENERATED_MAIN')

    def digest(self, name):
        return hashlib.sha256((self.main / name).read_bytes()).hexdigest()

    def test_pinned_ultrawide_hashes_match_generated_units(self):
        self.assertEqual(self.digest(ultrawide.UNIT), ultrawide.SHA256)
        self.assertEqual(self.digest(ultrawide_hud.UNIT), ultrawide_hud.SHA256)

    def test_pinned_settings_hashes_match_generated_units(self):
        self.assertEqual(self.digest(settings_menu.UNIT), settings_menu.SOURCE_SHA256)
        self.assertEqual(self.digest('main_113.c'), settings_menu.BUILD_SHA256)
        self.assertEqual(self.digest('main_082.c'), settings_menu.NAV_SHA256)
        self.assertEqual(self.digest('main_115.c'), settings_menu.TICK_SHA256)
        for name, expected in settings_menu.SHARED_HASHES.items():
            self.assertEqual(self.digest(name), expected, name)

    def test_patch_anchors_apply_to_the_generated_units(self):
        text = (self.main / ultrawide.UNIT).read_text(encoding='utf-8')
        self.assertIn('mk8_ultrawide_camera', ultrawide.patch(text))
        text = (self.main / ultrawide_hud.UNIT).read_text(encoding='utf-8')
        self.assertIn('mk8_ultrawide_hud', ultrawide_hud.patch(text))
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            for name in (settings_menu.UNIT, 'main_113.c', 'main_082.c', 'main_115.c',
                         *settings_menu.SHARED_HASHES):
                source = self.main / name
                target = settings_menu.patch_guest(source, source, output)
                self.assertTrue(target.is_file(), name)
                self.assertIn('mk8_settings_', target.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()

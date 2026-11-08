"""The pipeline redoes its own folders instead of stopping, and never deletes through a link."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolchain"))
import import_mk8
from repair import cleanup_stale, remove_owned, unlink_links_on_the_way


def junction(link, target):
    made = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
    if made.returncode:
        raise unittest.SkipTest("mklink /J indisponível")


@unittest.skipUnless(os.name == "nt", "Windows junctions")
class RepairTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix="mk8-repair-")
        self.root = Path(self.folder.name)
        self.data = self.root / "data"
        # The user's own extracted game, outside the data folder: must survive everything below.
        self.outside = self.root / "user-game"
        (self.outside / "romfs/Course").mkdir(parents=True)
        (self.outside / "romfs/Course/track.bin").write_bytes(b"keep")

    def tearDown(self):
        self.folder.cleanup()

    def test_removal_never_follows_a_junction_or_leaves_the_data_folder(self):
        owned = self.data / "game-data/abc/extracted"
        owned.mkdir(parents=True)
        (owned / "file.bin").write_bytes(b"x")
        junction(owned / "romfs", self.outside / "romfs")
        remove_owned(self.data / "game-data/abc", self.data)
        self.assertFalse((self.data / "game-data/abc").exists())
        self.assertEqual((self.outside / "romfs/Course/track.bin").read_bytes(), b"keep")
        with self.assertRaises(ValueError):
            remove_owned(self.outside, self.data)
        with self.assertRaises(ValueError):
            remove_owned(self.data, self.data)

    def test_old_junctions_on_the_way_to_game_data_are_unlinked_only(self):
        extracted = self.data / "game-data/abc/extracted"
        extracted.mkdir(parents=True)
        junction(extracted / "romfs", self.outside / "romfs")
        removed = unlink_links_on_the_way(extracted / "romfs", self.data)
        self.assertEqual(removed, [extracted / "romfs"])
        self.assertFalse((extracted / "romfs").exists())
        self.assertTrue((self.outside / "romfs/Course/track.bin").is_file())
        self.assertEqual(unlink_links_on_the_way(extracted / "romfs", self.data), [])

    def test_leftovers_of_cut_off_steps_are_removed(self):
        imports = self.data / "imports"
        (imports / "mk8-stage-abc/output").mkdir(parents=True)
        junction(imports / "mk8-stage-abc/output/romfs", self.outside / "romfs")
        (imports / "mk8-4.0.1").mkdir()
        cleanup_stale(imports, "mk8-stage-*")
        self.assertFalse((imports / "mk8-stage-abc").exists())
        self.assertTrue((imports / "mk8-4.0.1").is_dir())
        self.assertTrue((self.outside / "romfs/Course/track.bin").is_file())

    def test_an_incomplete_import_is_extracted_again_or_explained(self):
        target = self.data / "imports/mk8-4.0.1"
        (target / "exefs").mkdir(parents=True)  # cut off: no main.npdm, no RomFS
        (self.data / "imports/mk8-stage-old").mkdir()
        never = lambda path: None  # noqa: E731
        # Without the packages there is nothing to extract from: the message says what to select.
        with self.assertRaises(ValueError) as caught:
            import_mk8.prepare({}, self.data, never)
        self.assertIn("Selecione o jogo base", str(caught.exception))
        self.assertTrue(target.is_dir())
        self.assertFalse((self.data / "imports/mk8-stage-old").exists())
        # With them, the broken extraction is removed and the import starts over (here it stops at the fake package).
        packages = self.root / "packages"
        packages.mkdir()
        config = {}
        for name in ("base_package", "update_package", "prod_keys"):
            (packages / name).write_bytes(b"")
            config[name] = str(packages / name)
        space = import_mk8.require_space
        import_mk8.require_space = lambda *args: None
        try:
            with self.assertRaises(ValueError) as caught:
                import_mk8.prepare(config, self.data, never)
        finally:
            import_mk8.require_space = space
        self.assertFalse(target.exists())
        self.assertNotIn("Selecione", str(caught.exception))


if __name__ == "__main__":
    unittest.main()

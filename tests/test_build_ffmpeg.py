"""Verify FFmpeg inputs before reusing an artifact cache."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build-ffmpeg.py"
SPEC = importlib.util.spec_from_file_location("build_ffmpeg", SCRIPT)
FFMPEG = importlib.util.module_from_spec(SPEC)
with patch.object(sys, "path", sys.path.copy()):
    SPEC.loader.exec_module(FFMPEG)


class BuildFfmpegTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.output = root / "ffmpeg"
        self.source = root / "prebuilt-ffmpeg"
        self.output.mkdir()
        self.source.mkdir()
        self.manifest = self.output / "manifest.json"
        paths = patch.multiple(FFMPEG, OUTPUT=self.output, SOURCE=self.source)
        paths.start()
        self.addCleanup(paths.stop)
        for name in (*FFMPEG.DLLS, *FFMPEG.NOTICES):
            (self.output / name).write_bytes(name.encode("ascii"))
        (self.source / FFMPEG.SCRIPT.name).write_bytes(FFMPEG.SCRIPT.read_bytes().replace(b"\r\n", b"\n"))
        (self.source / FFMPEG.SOURCE_ARCHIVE).write_bytes(b"source archive fixture")
        self.manifest.write_text(json.dumps({
            "script": FFMPEG.script_digest(),
            "source_sha256": FFMPEG.sha256(self.source / FFMPEG.SOURCE_ARCHIVE),
            "files": {name: FFMPEG.sha256(self.output / name) for name in FFMPEG.DLLS},
        }), encoding="utf-8")

    def test_complete_cache_skips_the_build(self):
        with (
            patch.object(sys, "argv", [str(SCRIPT), "--require-source"]),
            patch.object(subprocess, "run", side_effect=AssertionError("unexpected FFmpeg build")),
        ):
            FFMPEG.main()

    def test_ci_requires_both_source_files(self):
        for name in (FFMPEG.SOURCE_ARCHIVE, FFMPEG.SCRIPT.name):
            with self.subTest(name=name):
                path = self.source / name
                contents = path.read_bytes()
                path.unlink()
                self.assertFalse(FFMPEG.current(require_source=True))
                self.assertTrue(FFMPEG.current())
                path.write_bytes(b"bad cache entry")
                self.assertFalse(FFMPEG.current(require_source=True))
                path.write_bytes(contents)

    def test_cli_rebuilds_when_the_source_archive_is_missing(self):
        (self.source / FFMPEG.SOURCE_ARCHIVE).unlink()
        with (
            patch.object(sys, "argv", [str(SCRIPT), "--require-source"]),
            patch.object(subprocess, "run", side_effect=RuntimeError("start build")) as run,
            self.assertRaisesRegex(RuntimeError, "start build"),
        ):
            FFMPEG.main()
        run.assert_called_once()

    def test_changed_dll_or_bad_manifest_invalidates_the_cache(self):
        path = self.output / FFMPEG.DLLS[0]
        contents = path.read_bytes()
        path.write_bytes(b"corrupt")
        self.assertFalse(FFMPEG.current(require_source=True))
        path.write_bytes(contents)
        self.manifest.write_text("{", encoding="utf-8")
        self.assertFalse(FFMPEG.current(require_source=True))

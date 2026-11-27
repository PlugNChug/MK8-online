"""Regression tests for tool setup without downloads or an installed game."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch


SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    with patch.object(sys, "path", [str(SCRIPTS), *sys.path]):
        spec.loader.exec_module(module)
    return module


SETUP = load_script("setup-linux")


class LinuxSetupTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.python = self.root / "local/python/bin/python"

    def prepare(self, run):
        with (
            patch.multiple(SETUP, ROOT=self.root, check_dependencies=Mock(), setup_vma=Mock(),
                           sdk_ready=Mock(return_value=True)),
            patch.object(SETUP, "sys", SimpleNamespace(argv=[], executable=sys.executable, version_info=(3, 12))),
            patch.object(SETUP.shutil, "disk_usage", return_value=SimpleNamespace(free=8 * 1024**3)),
            patch.object(SETUP.subprocess, "run", side_effect=run),
        ):
            SETUP.main()

    def test_retry_rebuilds_a_partial_venv_with_symlinks(self):
        creations = []

        def run(command, **kwargs):
            if "venv" in command:
                creations.append(command)
                self.python.parent.mkdir(parents=True)
                if len(creations) == 1:
                    self.python.write_text("incomplete", encoding="ascii")
                    raise subprocess.CalledProcessError(1, command)
                self.python.write_text("working", encoding="ascii")
            elif "-c" in command:
                return subprocess.CompletedProcess(command, 1)
            elif "pip" in command:
                self.assertEqual(len(creations), 2, "retry tried pip in the incomplete environment")
            return subprocess.CompletedProcess(command, 0)

        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare(run)
        self.prepare(run)
        self.assertEqual(self.python.read_text(encoding="ascii"), "working")
        backups = list((self.root / "local").glob("python-incomplete-*/python/bin/python"))
        self.assertEqual([path.read_text(encoding="ascii") for path in backups], ["incomplete"])
        self.assertEqual(len(creations), 2)
        for command in creations:
            self.assertIn("--symlinks", command)

    def test_existing_working_venv_is_probed_and_reused(self):
        self.python.parent.mkdir(parents=True)
        self.python.touch()
        commands = []

        def run(command, **kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command, 0)

        self.prepare(run)
        self.assertEqual(commands[0][:3], [self.python, "-I", "-c"])
        self.assertTrue(any("pip" in command for command in commands))
        self.assertFalse(any("venv" in command for command in commands))

    @unittest.skipUnless(sys.platform == "linux" and sys.version_info[:2] == (3, 12), "Requires Linux Python 3.12")
    def test_repairs_and_reuses_a_real_linux_venv(self):
        self.python.parent.mkdir(parents=True)
        self.python.write_bytes(b"not an executable")
        with patch.object(SETUP, "ROOT", self.root):
            self.assertEqual(SETUP.setup_python(), self.python)
            self.assertTrue(self.python.is_symlink())
            original_config = self.python.parent.parent / "pyvenv.cfg"
            created_at = original_config.stat().st_mtime_ns
            self.assertEqual(SETUP.setup_python(), self.python)
            self.assertEqual(original_config.stat().st_mtime_ns, created_at)

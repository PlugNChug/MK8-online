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
if sys.platform == "win32":
    with patch.dict(sys.modules, {"fcntl": SimpleNamespace(flock=Mock(), LOCK_EX=2)}):
        SDK = load_script("setup-linux-sdk")
else:
    SDK = load_script("setup-linux-sdk")


class LinuxSdkSetupTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def prepare(self, run):
        with (
            patch.multiple(SETUP, ROOT=self.root, check_dependencies=Mock(), setup_vma=Mock(),
                           sdk_ready=Mock(return_value=False)),
            patch.object(SETUP, "sys", SimpleNamespace(argv=[], executable=sys.executable, version_info=(3, 12))),
            patch.object(SETUP.shutil, "disk_usage", return_value=SimpleNamespace(free=8 * 1024**3)),
            patch.object(SETUP.subprocess, "run", side_effect=run),
        ):
            SETUP.main()

    def test_missing_cmake_fails_before_downloading_sdk(self):
        with (
            patch.multiple(SDK, TOOLS=self.root, SDK=self.root / "linux-sdk"),
            patch.object(SDK.shutil, "which", return_value=None),
            patch.object(SDK.shutil, "disk_usage", return_value=SimpleNamespace(free=8 * 1024**3)),
            patch.object(SDK, "fetch", side_effect=AssertionError("unexpected SDK download")) as fetch,
        ):
            with self.assertRaisesRegex(ValueError, "CMake"):
                SDK.setup()
            fetch.assert_not_called()

    def test_an_installed_sdk_can_be_reused_without_cmake(self):
        installed = self.root / "linux-sdk"
        for name in SDK.FILES:
            file = installed / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.touch()
        (installed / "sources.sha256").write_text(f"{SDK.SDL_SHA256}\n{SDK.VULKAN_SHA256}\n", encoding="ascii")
        with (
            patch.multiple(SDK, TOOLS=self.root, SDK=installed),
            patch.object(SDK.shutil, "which", return_value=None),
        ):
            self.assertEqual(SDK.setup(), installed)

    def test_sdk_setup_failure_exits_without_a_second_exception(self):
        def run(command, *, check=False):
            if check:
                raise subprocess.CalledProcessError(23, command)
            return subprocess.CompletedProcess(command, 23)

        with self.assertRaises(SystemExit) as failure:
            self.prepare(run)
        self.assertEqual(failure.exception.code, 23)

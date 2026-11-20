"""Native validation of the optional FSR frame-generation provider (tools/build_fg_provider.py).

The provider is a user-installed component derived from AMD's MIT-licensed SDK, so this test skips cleanly when it is
not present. When it is present, the test compiles and runs two real Vulkan programs:

  * test_query   - ffxQuery must report the swapchain provider version 3.1.7;
  * test_context - ffxCreateContext for a 1080p frame-generation context must return OK on a real Vulkan device,
                   which builds every optical-flow/frame-interpolation pipeline from the generated shader blobs.

A provider that is broken (wrong version, missing export, bad blob binding) fails here instead of at game start.

Locations searched:
  $MK8_FG_LIB, $XDG_DATA_HOME/mk8online/fsr/, local/fg-provider/
SDK headers for the test programs:
  $MK8_FG_SDK, ~/.cache/mk8online/fsr-build/sdk-copy, ~/.cache/mk8online/fg-sdk
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FG_SRC = ROOT / "tools" / "fg-linux"


def _first(paths):
    for path in paths:
        if path and path.exists():
            return path
    return None


def provider_library():
    home = Path.home()
    return _first([
        Path(os.environ["MK8_FG_LIB"]) if os.environ.get("MK8_FG_LIB") else None,
        Path(os.environ.get("XDG_DATA_HOME", home / ".local" / "share")) / "mk8online" / "fsr"
        / "libamd_fidelityfx_framegeneration_vk.so",
        ROOT / "local" / "fg-provider" / "libamd_fidelityfx_framegeneration_vk.so",
    ])


def sdk_headers():
    home = Path.home()
    return _first([
        Path(os.environ["MK8_FG_SDK"]) if os.environ.get("MK8_FG_SDK") else None,
        home / ".cache" / "mk8online" / "fsr-build" / "sdk-copy",
        home / ".cache" / "mk8online" / "fg-sdk",
    ])


class FrameGenerationProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = provider_library()
        cls.sdk = sdk_headers()
        if cls.library is None:
            raise unittest.SkipTest("FSR frame-generation provider not installed (run tools/build_fg_provider.py)")
        if cls.sdk is None:
            raise unittest.SkipTest("FidelityFX SDK headers not found (set MK8_FG_SDK)")
        cls.tmp = Path(tempfile.mkdtemp(prefix="mk8-fg-test-",
                                        dir=os.environ.get("MK8_TEST_TMP") or None))

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "tmp", None):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def _build(self, source, name):
        target = self.tmp / name
        cmd = [
            "g++", "-std=c++20", "-DFFX_API_ENTRY=",
            "-I", str(self.sdk / "Kits" / "FidelityFX" / "api" / "include"),
            "-I", str(self.sdk / "Kits" / "FidelityFX" / "framegeneration" / "include"),
            str(FG_SRC / source), "-o", str(target), "-lvulkan", "-ldl", "-include", "cstdint",
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, f"compile {source}:\n{proc.stdout}\n{proc.stderr}")
        return target

    def test_query_reports_3_1_7(self):
        binary = self._build("test_query.cpp", "test_query")
        proc = subprocess.run([str(binary), str(self.library)], capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("name=3.1.7", output, output)

    def test_context_is_created_on_this_device(self):
        binary = self._build("test_context.cpp", "test_context")
        proc = subprocess.run([str(binary), str(self.library)], capture_output=True, text=True, timeout=300)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("ffxCreateContext(FG 1920x1080) rc=0 (OK)", output, output)
        self.assertIn("ffxDestroyContext rc=0 (OK)", output, output)


if __name__ == "__main__":
    unittest.main(verbosity=2)

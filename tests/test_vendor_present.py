"""Compile and exercise the runtime present paths with synthetic Vulkan calls."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "linux" and shutil.which("clang++"), "Linux Vulkan compiler required")
class VendorPresentTests(unittest.TestCase):
    def run_fixture(self, fixture):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / fixture
            includes = ("runtime/mk8/graphics", "tools/dlss_bridge_include", "tools/fsr4-linux/src/provider")
            subprocess.run(
                ["clang++", "-std=gnu++17", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 *[f"-I{ROOT / name}" for name in includes], ROOT / f"tests/fixtures/{fixture}.cpp",
                 "-lvulkan", "-ldl", "-o", binary],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([binary], check=True)

    def test_fsr_slot_reuse_fresh_depth_and_fallback_layouts(self):
        self.run_fixture("fsr_present")

    def test_dlss_intermediate_present_precedes_the_real_image(self):
        self.run_fixture("dlss_present")

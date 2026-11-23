"""Contract and native tests for the optional FSR 4.1.2 provider.

The provider is the project's GPL-3.0-only port in tools/fsr4-linux, built by
tools/build_fsr4_provider.py from the user's own signed AMD DLL. The native tests
skip cleanly when the provider or the SDK headers are absent; the generator and
extractor tests only need the vendored sources.

Native checks (when installed):
  * selftest --query-only: ffxQuery(GET_VERSIONS) must report exactly FSR 4.1.2;
  * selftest 1920x1080 -> 2560x1440: a legacy-ABI context creation, dispatch,
    readback and destroy must all succeed on a real Vulkan device.

Locations searched for the provider:
  $MK8_FSR4_LIB, $XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_upscaler_vk.so,
  local/fsr4-provider/
Assets: $MK8_FSR4_ASSETS, <provider-dir>/fsr4-assets, local/fsr4-provider/fsr4-assets
SDK headers: $MK8_FSR4_SDK, ~/.cache/mk8online/fsr4-build/sdk-extract/*
"""
import contextlib
import ctypes
import hashlib
import importlib.util
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / "tools" / "fsr4-linux"
PROVIDER_NAME = "libamd_fidelityfx_upscaler_vk.so"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


build = _load("build_fsr4_provider", ROOT / "tools" / "build_fsr4_provider.py")
extract_shaders = _load("fsr4_extract_shaders", PORT / "tools" / "extract_shaders.py")
trim_assets = _load("fsr4_trim_assets", PORT / "tools" / "trim_assets.py")
verify_assets = _load("fsr4_verify_assets", PORT / "tools" / "verify_assets.py")


def _first(paths):
    for path in paths:
        if path and Path(path).exists():
            return Path(path)
    return None


def provider_library():
    home = Path.home()
    return _first([
        Path(os.environ["MK8_FSR4_LIB"]) if os.environ.get("MK8_FSR4_LIB") else None,
        Path(os.environ.get("XDG_DATA_HOME", home / ".local" / "share"))
        / "mk8online" / "fsr" / PROVIDER_NAME,
        ROOT / "local" / "fsr4-provider" / PROVIDER_NAME,
    ])


def provider_assets():
    home = Path.home()
    library = provider_library()
    return _first([
        Path(os.environ["MK8_FSR4_ASSETS"]) if os.environ.get("MK8_FSR4_ASSETS") else None,
        library.parent / "fsr4-assets" if library else None,
        Path(os.environ.get("XDG_DATA_HOME", home / ".local" / "share"))
        / "mk8online" / "fsr" / "fsr4-assets",
        ROOT / "local" / "fsr4-provider" / "fsr4-assets",
    ])


def sdk_headers():
    home = Path.home()
    work = home / ".cache" / "mk8online" / "fsr4-build"
    candidates = [
        Path(os.environ["MK8_FSR4_SDK"]) if os.environ.get("MK8_FSR4_SDK") else None,
        work / "sdk-extract" / "FidelityFX-SDK-2.4.0",
    ]
    extract = work / "sdk-extract"
    if extract.is_dir():
        candidates.extend(sorted(extract.glob("*")))
    for candidate in candidates:
        if candidate and (candidate / "Kits" / "FidelityFX" / "api" / "include"
                          / "ffx_api.h").is_file():
            return candidate
    return None


class PinnedInputTests(unittest.TestCase):
    """The build must stay pinned to the documented SDK, DLL and asset counts."""

    def test_pins_match_the_port_and_the_documentation(self):
        self.assertEqual(build.SDK_ARCHIVE_SIZE, 253_592_010)
        self.assertEqual(build.SDK_ARCHIVE_SHA256,
                         "111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a")
        self.assertEqual(build.DLL_SIZE, 36_135_696)
        self.assertEqual(build.DLL_SHA256,
                         "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7")
        phase0 = (PORT / "PHASE0.md").read_text(encoding="utf-8")
        self.assertIn(build.DLL_SHA256, phase0)
        docs = (ROOT / "docs" / "en" / "reports" / "FSR4-UPSCALER.md").read_text(encoding="utf-8")
        self.assertIn(build.SDK_ARCHIVE_SHA256, docs)
        self.assertIn(build.DLL_SHA256, docs)

    def test_extraction_contract_matches_the_ports_verifier(self):
        self.assertEqual(build.EXPECTED_SHADERS, verify_assets.EXPECTED_SHADERS)
        self.assertEqual(build.EXPECTED_SHADER_BYTES, verify_assets.EXPECTED_SHADER_BYTES)
        self.assertEqual(build.FP8_STANDARD_SHA256,
                         verify_assets.KNOWN_MODEL_HASHES["FP8_standard.bin"])
        self.assertEqual(build.EXPECTED_MODEL_CLASSES, 4)
        self.assertEqual(build.EXPECTED_MODEL_BANKS, 8)

    def test_trim_selects_the_installed_runtime_blob_set(self):
        ids = trim_assets.collect_ids(PORT / "tools" / "data" / "fsr4_412_selector_map.json",
                                      catalog=None)
        # 191 modules + models/FP8_standard.bin is what the provider can request.
        self.assertEqual(len(ids), build.EXPECTED_RUNTIME_SHADERS)
        self.assertIn(329, ids)  # SPD pyramid

    def test_verify_pinned_fails_closed_with_expected_and_actual(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.bin"
            path.write_bytes(b"x" * 16)

            errors = io.StringIO()
            with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                build.verify_pinned(path, 32, None, "test input")
            message = errors.getvalue()
            self.assertIn("expected 32", message)
            self.assertIn("got 16", message)

            errors = io.StringIO()
            with contextlib.redirect_stderr(errors), self.assertRaises(SystemExit):
                build.verify_pinned(path, 16, "0" * 64, "test input")
            message = errors.getvalue()
            self.assertIn("expected " + "0" * 64, message)
            self.assertIn(hashlib.sha256(b"x" * 16).hexdigest(), message)


class ExtractorContractTests(unittest.TestCase):
    """The FFXZ scanner must accept real modules and reject malformed candidates."""

    @staticmethod
    def _pe(data):
        return {
            "image_base": 0x180000000,
            "sections": [{
                "name": ".text",
                "rva": 0x1000,
                "vsize": len(data),
                "raw": 0,
                "rawsize": len(data),
            }],
        }

    @staticmethod
    def _spirv(size=64):
        # A structurally valid module header; the scanner only needs the magic.
        return struct.pack("<5I", 0x07230203, 0x00010000, 0, 1, 0) + b"\0" * (size - 20)

    def test_scanner_extracts_a_compressed_module(self):
        spv = self._spirv()
        payload = zlib.compress(spv)
        data = b"\0" * 0x400 + struct.pack("<II", 0x465A5846, len(spv)) + payload
        blobs = extract_shaders.extract(data, self._pe(data))
        self.assertEqual(len(blobs), 1)
        blob = blobs[0]
        self.assertEqual(blob["uncompressed_size"], len(spv))
        self.assertEqual(blob["sha256"], hashlib.sha256(spv).hexdigest())
        self.assertEqual(blob["rva"], 0x1000 + 0x400)
        self.assertEqual(blob["section"], ".text")

    def test_scanner_rejects_corrupt_and_non_spirv_payloads(self):
        spv = self._spirv()
        compressed = zlib.compress(spv)
        corrupt = b"\0" * 0x400 + struct.pack("<II", 0x465A5846, len(spv)) + compressed[:-4]
        self.assertEqual(extract_shaders.extract(corrupt, self._pe(corrupt)), [])

        not_spirv = b"\x11" * len(spv)
        payload = zlib.compress(not_spirv)
        data = b"\0" * 0x400 + struct.pack("<II", 0x465A5846, len(not_spirv)) + payload
        self.assertEqual(extract_shaders.extract(data, self._pe(data)), [])

    def test_scanner_rejects_impossible_sizes(self):
        spv = self._spirv()
        payload = zlib.compress(spv)
        for size in (0, 1, extract_shaders.MAX_SIZE + 1):
            data = b"\0" * 0x400 + struct.pack("<II", 0x465A5846, size) + payload
            self.assertEqual(extract_shaders.extract(data, self._pe(data)), [],
                             f"size {size} should be rejected")


class InstalledAssetTests(unittest.TestCase):
    """The installed runtime set must be the trimmed, hash-checked one."""

    @classmethod
    def setUpClass(cls):
        cls.assets = provider_assets()
        if cls.assets is None or not (cls.assets / "shaders").is_dir():
            raise unittest.SkipTest("FSR 4.1.2 assets not installed")

    def test_runtime_set_has_191_modules_and_the_fp8_bank(self):
        shaders = sorted((self.assets / "shaders").glob("*.spv"))
        self.assertEqual(len(shaders), build.EXPECTED_RUNTIME_SHADERS)
        self.assertTrue(all(p.read_bytes()[:4] == b"\x03\x02\x23\x07" for p in shaders))
        model = self.assets / "models" / "FP8_standard.bin"
        self.assertTrue(model.is_file())
        self.assertEqual(hashlib.sha256(model.read_bytes()).hexdigest(),
                         build.FP8_STANDARD_SHA256)


class NativeProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = provider_library()
        cls.assets = provider_assets()
        cls.sdk = sdk_headers()
        if cls.library is None:
            raise unittest.SkipTest("FSR 4.1.2 provider not installed "
                                    "(run tools/build_fsr4_provider.py)")
        if cls.sdk is None:
            raise unittest.SkipTest("FidelityFX SDK headers not found (set MK8_FSR4_SDK)")
        cls.tmp = Path(tempfile.mkdtemp(prefix="mk8-fsr4-test-",
                                        dir=os.environ.get("MK8_TEST_TMP") or None))

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "tmp", None):
            shutil.rmtree(cls.tmp, ignore_errors=True)

    def _build(self):
        target = self.tmp / "fsr4_selftest"
        cmd = [
            "g++", "-std=c++17", "-O2",
            "-I", str(self.sdk / "Kits" / "FidelityFX" / "api" / "include"),
            "-I", str(self.sdk / "Kits" / "FidelityFX" / "upscalers" / "include"),
            str(PORT / "tests" / "provider" / "selftest.cpp"),
            "-o", str(target), "-lvulkan", "-ldl", "-include", "cstdint",
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, f"compile selftest:\n{proc.stdout}\n{proc.stderr}")
        return target

    def test_library_exports_the_five_entry_points(self):
        handle = ctypes.CDLL(str(self.library))
        for name in ("ffxCreateContext", "ffxDestroyContext", "ffxConfigure",
                     "ffxQuery", "ffxDispatch"):
            self.assertTrue(hasattr(handle, name), f"{self.library} lacks {name}")

    def test_query_reports_only_4_1_2(self):
        binary = self._build()
        proc = subprocess.run([str(binary), str(self.library), "--query-only"],
                              capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("version[0]: name=4.1.2 id=0x1001002", output, output)
        self.assertIn("RESULT: OK", output, output)

    def test_1080p_context_dispatches_and_destroys(self):
        if self.assets is None:
            self.skipTest("FSR 4.1.2 assets not installed")
        binary = self._build()
        proc = subprocess.run(
            [str(binary), str(self.library), "--assets", str(self.assets),
             "1920", "1080", "2560", "1440"],
            capture_output=True, text=True, timeout=600)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("ffxCreateContext(UPSCALE 1920x1080 -> 2560x1440 legacy ABI", output)
        self.assertIn("-> 0x0 (OK)", output, output)
        self.assertIn("ffxQuery(provider) -> 0x0", output, output)
        self.assertIn("name=FSR 4.1.2", output, output)
        self.assertIn("ffxDispatch(UPSCALE) -> 0x0 (OK)", output, output)
        self.assertIn("ffxDestroyContext -> 0x0 (OK)", output, output)
        self.assertIn("RESULT: OK", output)
        self.assertNotIn("degenerate", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)

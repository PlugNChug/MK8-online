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

The generator tests in this file do not need a provider or the SDK: they exercise
tools/gen_fg_blobs.py on SPIR-V disassembly text (descriptor classification across the
UniformConstant/Uniform/StorageBuffer storage classes, storage-image format rewriting and the
zero-length binding array emission).
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

sys.path.insert(0, str(ROOT / "tools"))
import gen_fg_blobs  # noqa: E402


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


class BlobGeneratorTests(unittest.TestCase):
    """The generator must bind buffers and keep storage-image formats dispatch-valid."""

    def test_classification_covers_uniform_and_storage_buffer_classes(self):
        # The shape DXC emits for the SDK's shaders: cbuffers and StructuredBuffer in Uniform,
        # RWStructuredBuffer in StorageBuffer, textures in UniformConstant. The read-only buffer
        # carries NonWritable on its runtime-array member, not on the variable.
        text = """
               OpCapability Shader
               OpName %r_counters "r_counters"
               OpName %rw_counters "rw_counters"
               OpName %cbFI "cbFI"
               OpName %r_color "r_color"
               OpName %rw_output "rw_output"
               OpDecorate %r_counters Binding 8
               OpDecorate %rw_counters Binding 0
               OpDecorate %cbFI Binding 1
               OpDecorate %r_color Binding 3
               OpDecorate %rw_output Binding 4
               OpMemberDecorate %type_readonly 0 Offset 0
               OpMemberDecorate %type_readonly 0 NonWritable
       %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
%_runtimearr_uint = OpTypeRuntimeArray %uint
%type_readonly = OpTypeStruct %_runtimearr_uint
%type_readwrite = OpTypeStruct %_runtimearr_uint
%type_cbFI = OpTypeStruct %uint %uint
%type_srv = OpTypeImage %float 2D 2 0 0 1 Unknown
%type_uav = OpTypeImage %float 2D 2 0 0 2 Rgba32f
%_ptr_Uniform_type_readonly = OpTypePointer Uniform %type_readonly
%_ptr_StorageBuffer_type_readwrite = OpTypePointer StorageBuffer %type_readwrite
%_ptr_Uniform_type_cbFI = OpTypePointer Uniform %type_cbFI
%_ptr_UniformConstant_type_srv = OpTypePointer UniformConstant %type_srv
%_ptr_UniformConstant_type_uav = OpTypePointer UniformConstant %type_uav
%r_counters = OpVariable %_ptr_Uniform_type_readonly Uniform
%rw_counters = OpVariable %_ptr_StorageBuffer_type_readwrite StorageBuffer
%cbFI = OpVariable %_ptr_Uniform_type_cbFI Uniform
%r_color = OpVariable %_ptr_UniformConstant_type_srv UniformConstant
%rw_output = OpVariable %_ptr_UniformConstant_type_uav UniformConstant
"""
        names = gen_fg_blobs.name_map(text)
        kinds = {name: kind for _, name, kind, _ in gen_fg_blobs.classify(text, names)}
        self.assertEqual(kinds, {
            "r_counters": "srv_buf",
            "rw_counters": "uav_buf",
            "cbFI": "cbv",
            "r_color": "srv_tex",
            "rw_output": "uav_tex",
        })
        bindings = {name: binding for _, name, _, binding in gen_fg_blobs.classify(text, names)}
        self.assertEqual(bindings, {
            "r_counters": gen_fg_blobs.BIND_SRV_BUF,
            "rw_counters": gen_fg_blobs.BIND_UAV_BUF,
            "cbFI": gen_fg_blobs.BIND_CBV,
            "r_color": gen_fg_blobs.BIND_SRV_TEX,
            "rw_output": gen_fg_blobs.BIND_UAV_TEX,
        })

    def test_storage_image_formats_become_unknown_and_duplicates_merge(self):
        # Two float storage images that only differ by DXC's inferred format (Rg32f/Rgba32f) must
        # merge into one Unknown type; an image used by an atomic keeps its explicit R32ui format.
        text = ("               OpCapability Shader\n"
                "               OpCapability StorageImageExtendedFormats\n"
                "       %type_a = OpTypeImage %float 2D 2 0 0 2 Rg32f\n"
                "       %type_b = OpTypeImage %float 2D 2 0 0 2 Rgba32f\n"
                "       %type_c = OpTypeImage %uint 2D 2 0 0 2 R32ui\n"
                "       %ptr_a = OpTypePointer UniformConstant %type_a\n"
                "       %ptr_b = OpTypePointer UniformConstant %type_b\n"
                "       %ptr_c = OpTypePointer UniformConstant %type_c\n"
                "       %rw_a = OpVariable %ptr_a UniformConstant\n"
                "       %rw_b = OpVariable %ptr_b UniformConstant\n"
                "       %rw_c = OpVariable %ptr_c UniformConstant\n"
                "       %value = OpImageTexelPointer %ptr_uint %rw_c %coord %zero\n"
                "               OpImageWrite %rw_a %coord %texel\n"
                "               OpImageWrite %rw_b %coord %texel\n")
        rewritten = gen_fg_blobs.rewrite_storage_image_formats(text)
        self.assertEqual(rewritten.count("OpTypeImage %float 2D 2 0 0 2 Unknown"), 1, rewritten)
        self.assertEqual(rewritten.count("OpTypeImage %uint 2D 2 0 0 2 R32ui"), 1, rewritten)
        self.assertIn("OpCapability StorageImageWriteWithoutFormat", rewritten)
        self.assertNotIn("StorageImageExtendedFormats", rewritten)
        # Both former float images now use the surviving type id.
        self.assertEqual(rewritten.count("%type_a"), 3, rewritten)
        self.assertNotIn("%type_b", rewritten)

    def test_empty_binding_arrays_are_valid_c_plus_plus(self):
        # static const char* x[] = {}; is not valid C++ (MSVC rejects it); the blob's count is 0.
        self.assertEqual(gen_fg_blobs.c_names([]), "{nullptr}")
        self.assertEqual(gen_fg_blobs.c_u32([]), "{0}")
        self.assertEqual(gen_fg_blobs.c_names(["a"]), '{"a"}')
        self.assertEqual(gen_fg_blobs.c_u32([224]), "{224}")


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
        self.assertIn("ffxCreateContext(FG 1920x1080 flags=0x2) rc=0 (OK)", output, output)
        self.assertIn("ffxDestroyContext rc=0 (OK)", output, output)

    def test_inverted_depth_permutation_is_created(self):
        # Flags bit 3 is FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED; this selects the separately compiled d1 blobs.
        binary = self._build("test_context.cpp", "test_context_inverted")
        proc = subprocess.run([str(binary), str(self.library), "1920", "1080", "8"],
                              capture_output=True, text=True, timeout=300)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("flags=0xa", output, output)
        self.assertIn("rc=0 (OK)", output, output)


if __name__ == "__main__":
    unittest.main(verbosity=2)

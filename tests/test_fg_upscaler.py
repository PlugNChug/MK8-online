"""Native validation of the optional FSR 3.1.6 upscaler provider (tools/build_upscaler_provider.py).

The provider is a user-installed component derived from AMD's MIT-licensed SDK, so this test skips cleanly
when it is not present. When it is present, the test compiles and runs two real Vulkan programs:

  * test_query   - ffxQuery must report exactly the 3.1.6 upscaler provider version;
  * test_context - ffxCreateContext for a 1920x1080 -> 2560x1440 upscale context must return OK on a real
                   Vulkan device (building every upscaler pipeline from the generated shader blobs), a
                   dispatch must return OK, the readback must contain a varied image, and the context must
                   be destroyed cleanly.

A provider that is broken (wrong version, missing export, bad blob binding, wrong storage-image format)
fails here instead of at game start.

Locations searched:
  $MK8_UPSCALER_LIB, $XDG_DATA_HOME/mk8online/fsr/libamd_fidelityfx_upscaler_vk.so,
  local/upscaler-provider/
SDK headers for the test programs:
  $MK8_UPSCALER_SDK, ~/.cache/mk8online/fsr-upscaler-build/sdk-copy, ~/.cache/mk8online/fg-sdk

The generator tests in this file do not need a provider or the SDK: they exercise
tools/gen_upscaler_blobs.py on SPIR-V disassembly text (descriptor classification across the
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
UPSCALER_SRC = ROOT / "tools" / "fg-linux-upscaler"
PROVIDER_NAME = "libamd_fidelityfx_upscaler_vk.so"

sys.path.insert(0, str(ROOT / "tools"))
import gen_upscaler_blobs  # noqa: E402


def _first(paths):
    for path in paths:
        if path and path.exists():
            return path
    return None


def provider_library():
    home = Path.home()
    return _first([
        Path(os.environ["MK8_UPSCALER_LIB"]) if os.environ.get("MK8_UPSCALER_LIB") else None,
        Path(os.environ.get("XDG_DATA_HOME", home / ".local" / "share")) / "mk8online" / "fsr"
        / PROVIDER_NAME,
        ROOT / "local" / "upscaler-provider" / PROVIDER_NAME,
    ])


def sdk_headers():
    home = Path.home()
    return _first([
        Path(os.environ["MK8_UPSCALER_SDK"]) if os.environ.get("MK8_UPSCALER_SDK") else None,
        home / ".cache" / "mk8online" / "fsr-upscaler-build" / "sdk-copy",
        home / ".cache" / "mk8online" / "fg-sdk",
    ])


class BlobGeneratorTests(unittest.TestCase):
    """The generator must classify every descriptor class and keep storage images dispatch-valid."""

    def test_classification_covers_uniform_and_storage_buffer_classes(self):
        # The shape DXC emits for the SDK's shaders: cbuffers and StructuredBuffer in Uniform,
        # RWStructuredBuffer in StorageBuffer, textures in UniformConstant. The read-only buffer
        # carries NonWritable on its runtime-array member, not on the variable.
        text = """
               OpCapability Shader
               OpName %r_counters "r_counters"
               OpName %rw_counters "rw_counters"
               OpName %cbFSR3Upscaler "cbFSR3Upscaler"
               OpName %r_input_color "r_input_color"
               OpName %rw_upscaled_output "rw_upscaled_output"
               OpDecorate %r_counters Binding 8
               OpDecorate %rw_counters Binding 0
               OpDecorate %cbFSR3Upscaler Binding 1
               OpDecorate %r_input_color Binding 3
               OpDecorate %rw_upscaled_output Binding 4
               OpMemberDecorate %type_readonly 0 Offset 0
               OpMemberDecorate %type_readonly 0 NonWritable
        %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
%_runtimearr_uint = OpTypeRuntimeArray %uint
%type_readonly = OpTypeStruct %_runtimearr_uint
%type_readwrite = OpTypeStruct %_runtimearr_uint
%type_cbFSR3Upscaler = OpTypeStruct %uint %uint
%type_srv = OpTypeImage %float 2D 2 0 0 1 Unknown
%type_uav = OpTypeImage %float 2D 2 0 0 2 Rgba32f
%_ptr_Uniform_type_readonly = OpTypePointer Uniform %type_readonly
%_ptr_StorageBuffer_type_readwrite = OpTypePointer StorageBuffer %type_readwrite
%_ptr_Uniform_type_cbFSR3Upscaler = OpTypePointer Uniform %type_cbFSR3Upscaler
%_ptr_UniformConstant_type_srv = OpTypePointer UniformConstant %type_srv
%_ptr_UniformConstant_type_uav = OpTypePointer UniformConstant %type_uav
%r_counters = OpVariable %_ptr_Uniform_type_readonly Uniform
%rw_counters = OpVariable %_ptr_StorageBuffer_type_readwrite StorageBuffer
%cbFSR3Upscaler = OpVariable %_ptr_Uniform_type_cbFSR3Upscaler Uniform
%r_input_color = OpVariable %_ptr_UniformConstant_type_srv UniformConstant
%rw_upscaled_output = OpVariable %_ptr_UniformConstant_type_uav UniformConstant
"""
        names = gen_upscaler_blobs.name_map(text)
        kinds = {name: kind for _, name, kind, _ in gen_upscaler_blobs.classify(text, names)}
        self.assertEqual(kinds, {
            "r_counters": "srv_buf",
            "rw_counters": "uav_buf",
            "cbFSR3Upscaler": "cbv",
            "r_input_color": "srv_tex",
            "rw_upscaled_output": "uav_tex",
        })
        bindings = {name: binding for _, name, _, binding in gen_upscaler_blobs.classify(text, names)}
        self.assertEqual(bindings, {
            "r_counters": gen_upscaler_blobs.BIND_SRV_BUF,
            "rw_counters": gen_upscaler_blobs.BIND_UAV_BUF,
            "cbFSR3Upscaler": gen_upscaler_blobs.BIND_CBV,
            "r_input_color": gen_upscaler_blobs.BIND_SRV_TEX,
            "rw_upscaled_output": gen_upscaler_blobs.BIND_UAV_TEX,
        })

    def test_storage_image_formats_become_unknown_and_atomics_keep_format(self):
        # Two float storage images that only differ by DXC's inferred format (Rg32f/Rgba32f) must
        # merge into one Unknown type; an image used by an atomic keeps its explicit R32ui format.
        # This is what makes the R8_UNORM/R16_FLOAT SDK resources legal for the same pipeline.
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
        rewritten = gen_upscaler_blobs.rewrite_storage_image_formats(text)
        self.assertEqual(rewritten.count("OpTypeImage %float 2D 2 0 0 2 Unknown"), 1, rewritten)
        self.assertEqual(rewritten.count("OpTypeImage %uint 2D 2 0 0 2 R32ui"), 1, rewritten)
        self.assertIn("OpCapability StorageImageWriteWithoutFormat", rewritten)
        self.assertNotIn("StorageImageExtendedFormats", rewritten)
        # Both former float images now use the surviving type id.
        self.assertEqual(rewritten.count("%type_a"), 3, rewritten)
        self.assertNotIn("%type_b", rewritten)

    def test_empty_binding_arrays_are_valid_c_plus_plus(self):
        # static const char* x[] = {}; is not valid C++ (MSVC rejects it); the blob's count is 0.
        self.assertEqual(gen_upscaler_blobs.c_names([]), "{nullptr}")
        self.assertEqual(gen_upscaler_blobs.c_u32([]), "{0}")
        self.assertEqual(gen_upscaler_blobs.c_names(["a"]), '{"a"}')
        self.assertEqual(gen_upscaler_blobs.c_u32([224]), "{224}")

    def test_permutation_contract_matches_the_sdk_key(self):
        # The order is the bit order of POPULATE_PERMUTATION_KEY in ffx_fsr3upscaler_shaderblobs.cpp.
        self.assertEqual(gen_upscaler_blobs.KEY_ORDER, [
            "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE",
            "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
            "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS",
            "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
            "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH",
            "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING",
        ])
        # The bridge creates the context with NON_LINEAR only: low-resolution MVs, no HDR/jitter/inverted.
        self.assertEqual(gen_upscaler_blobs.BASE_OPTIONS[
            "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS"], 1)
        for option in ("FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT",
                       "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS",
                       "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH"):
            self.assertEqual(gen_upscaler_blobs.BASE_OPTIONS[option], 0)
        self.assertEqual(len(gen_upscaler_blobs.PASSES), 11)
        enums = [enum for enum, _, _ in gen_upscaler_blobs.PASSES]
        self.assertEqual(len(enums), len(set(enums)))
        self.assertTrue(all(enum.startswith("FFX_FSR3UPSCALER_PASS_") for enum in enums))
        # The effect always creates the accumulate-sharpen pipeline; its separately compiled module
        # is selected by the pass id, not a key bit guessed from the base options.
        self.assertIn("FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN", enums)
        self.assertIn("FFX_FSR3UPSCALER_PASS_ACCUMULATE", enums)


class UpscalerProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = provider_library()
        cls.sdk = sdk_headers()
        if cls.library is None:
            raise unittest.SkipTest("FSR upscaler provider not installed (run tools/build_upscaler_provider.py)")
        if cls.sdk is None:
            raise unittest.SkipTest("FidelityFX SDK headers not found (set MK8_UPSCALER_SDK)")
        cls.tmp = Path(tempfile.mkdtemp(prefix="mk8-upscaler-test-",
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
            "-I", str(self.sdk / "Kits" / "FidelityFX" / "upscalers" / "include"),
            str(UPSCALER_SRC / source), "-o", str(target), "-lvulkan", "-ldl", "-include", "cstdint",
        ]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, f"compile {source}:\n{proc.stdout}\n{proc.stderr}")
        return target

    def test_query_reports_only_3_1_6(self):
        binary = self._build("test_query.cpp", "test_query")
        proc = subprocess.run([str(binary), str(self.library)], capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("name=3.1.6", output, output)
        self.assertIn("id=0xf5a5ca1e00c01006", output, output)

    def test_unsupported_context_flags_are_rejected(self):
        # The generated blobs cover low-resolution motion vectors, no HDR/jitter/inverted depth. A context
        # asking for inverted depth (0x8) must fail with a parameter error, not run the wrong shader or crash.
        binary = self._build("test_context.cpp", "test_context_inverted")
        proc = subprocess.run([str(binary), str(self.library), "1920", "1080", "2560", "1440", "0x108"],
                              capture_output=True, text=True, timeout=600)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 2, output)
        self.assertIn("flags=0x108", output, output)
        self.assertIn("rc=6 (PARAMETER) ctx=(nil)", output, output)

    def test_context_creates_dispatches_and_destroys(self):
        binary = self._build("test_context.cpp", "test_context")
        proc = subprocess.run([str(binary), str(self.library), "1920", "1080", "2560", "1440"],
                              capture_output=True, text=True, timeout=600)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, output)
        self.assertIn("ffxCreateContext(UPSCALE 1920x1080 -> 2560x1440 flags=0x100) rc=0 (OK)", output, output)
        self.assertIn("ffxQuery(provider) rc=0 id=0xf5a5ca1e00c01006 name=3.1.6", output, output)
        self.assertIn("ffxDispatch(UPSCALE) rc=0 (OK)", output, output)
        self.assertIn("varied=1", output, output)
        self.assertIn("ffxDestroyContext rc=0 (OK)", output, output)
        self.assertIn("RESULT: OK", output, output)


if __name__ == "__main__":
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""Build the native Linux FidelityFX frame-generation provider from AMD's MIT-licensed SDK sources.

Why this exists: AMD publishes the FidelityFX SDK sources under the MIT licence, but ships the frame-generation
provider only as a signed Windows DLL and does not include the SDK's generated shader-permutation tables in the
source tree. This script derives the Linux provider locally from the user's own downloaded SDK:

  1. verify the pinned SDK source archive (size + SHA-256) if given an archive;
  2. apply the small, explicit Linux portability edits (Win32 shims, GCC 16 fixes, wchar_t context sizing);
  3. compile the frame-interpolation/optical-flow shaders with DXC and generate the blob tables;
  4. build libamd_fidelityfx_framegeneration_vk.so and run the version/context self-tests.

Nothing from AMD's signed binaries is used or redistributed. The output is installed beside the user's data with
AMD's licence text, and the game loads it only when the user enables FSR in the launcher.

Usage:
  build_fg_provider.py --sdk-src <SDK v2.4.0 archive or extracted tree> --dxc <dxc binary> --install-dir <dir>
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FG_DIR = REPO / "tools" / "fg-linux"
GEN_BLOBS = REPO / "tools" / "gen_fg_blobs.py"

SDK_TAG = "v2.4.0"

# Textual portability edits. Each entry is (relative path, old text, new text). The build fails closed if the SDK
# does not contain the expected text, so a different SDK version is rejected instead of silently miscompiled.
API_H = "Kits/FidelityFX/api/include/ffx_api.h"
PROVIDER_H = "Kits/FidelityFX/api/internal/ffx_provider.h"
VK_CPP = "Kits/FidelityFX/backend/vk/ffx_vk.cpp"
FI_H = "Kits/FidelityFX/framegeneration/fsr3/include/ffx_frameinterpolation.h"
SWAP_CPP = "Kits/FidelityFX/framegeneration/fsr3/vk/FrameInterpolationSwapchainVK.cpp"
PROV_SWAP_CPP = "Kits/FidelityFX/framegeneration/fsr3/vk/ffx_provider_framegenerationswapchain_vk.cpp"
PROV_FG_CPP = "Kits/FidelityFX/framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp"

PROVIDER_EXTERNAL_STUB = '''
// Linux provider build: the SDK source does not include the loader-internal external-provider type, and this module
// never selects a driver-side provider. A minimal definition keeps the shared API object compiling.
class ffxProviderExternal : public ffxProvider
{
public:
    ffxProviderExternal() : ffxProvider(0, 0, "") {}
    ffxProviderExternal(void*, uint64_t) : ffxProvider(0, 0, "") {}
    bool Valid() const { return false; }
    ffxReturnCode_t CreateContext(ffxContext*, ffxCreateContextDescHeader*, Allocator&) override { return FFX_API_RETURN_ERROR; }
    ffxReturnCode_t DestroyContext(ffxContext*, Allocator&) override { return FFX_API_RETURN_ERROR; }
    ffxReturnCode_t Configure(ffxContext*, const ffxConfigureDescHeader*) const override { return FFX_API_RETURN_ERROR; }
    ffxReturnCode_t Query(ffxContext*, ffxQueryDescHeader*) const override { return FFX_API_RETURN_ERROR; }
    ffxReturnCode_t Dispatch(ffxContext*, const ffxDispatchDescHeader*) const override { return FFX_API_RETURN_ERROR; }
};
'''

PATCHES = [
    (
        API_H,
        "#define FFX_API_ENTRY __declspec(dllexport)",
        "#ifndef FFX_API_ENTRY\n#define FFX_API_ENTRY __declspec(dllexport)\n#endif",
    ),
    (
        PROVIDER_H,
        "    virtual ffxReturnCode_t Dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const = 0;\n\n"
        "protected:\n    size_t RefCount = 0;\n    uint64_t Id;\n    uint64_t EffectId;\n"
        "    const char* VersionName;\n};\n",
        "    virtual ffxReturnCode_t Dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const = 0;\n\n"
        "protected:\n    size_t RefCount = 0;\n    uint64_t Id;\n    uint64_t EffectId;\n"
        "    const char* VersionName;\n};\n" + PROVIDER_EXTERNAL_STUB,
    ),
    (
        VK_CPP,
        "    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;\n"
        "    wcscpy_s(outputBuffer, outputLen, converter.from_bytes(inputName).c_str());",
        "    mk8_utf8_to_utf16(inputName, outputBuffer, outputLen);",
    ),
    (
        VK_CPP,
        "    std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> converter;\n"
        "    strcpy_s(outputBuffer, outputLen, converter.to_bytes(inputName).c_str());",
        "    mk8_utf16_to_utf8(inputName, outputBuffer, outputLen);",
    ),
    (
        FI_H,
        "#define FFX_FRAMEINTERPOLATION_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE)",
        "#if defined(_WIN32)\n"
        "#define FFX_FRAMEINTERPOLATION_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE)\n"
        "#else\n"
        "// Linux: wchar_t is 4 bytes, so FfxPipelineState (259 bindings, each with a wchar_t name) is about twice the\n"
        "// Win32 size. The context is private to this provider build; callers only hold an opaque ffxContext.\n"
        "#define FFX_FRAMEINTERPOLATION_CONTEXT_SIZE (FFX_SDK_DEFAULT_CONTEXT_SIZE * 2)\n"
        "#endif",
    ),
    (
        SWAP_CPP,
        "    VkSurfaceFullScreenExclusiveInfoEXT      surfaceFullScreenExclusive;\n"
        "    VkSurfaceFullScreenExclusiveWin32InfoEXT surfaceFullScreenExclusiveWin32;\n",
        "#if defined(_WIN32)\n"
        "    VkSurfaceFullScreenExclusiveInfoEXT      surfaceFullScreenExclusive;\n"
        "    VkSurfaceFullScreenExclusiveWin32InfoEXT surfaceFullScreenExclusiveWin32;\n"
        "#endif\n",
    ),
    (
        SWAP_CPP,
        "        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT:\n"
        "            realSwapchainCreateInfo.surfaceFullScreenExclusive       = *reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(pCurrent);\n"
        "            realSwapchainCreateInfo.surfaceFullScreenExclusive.pNext = const_cast<void*>(realSwapchainCreateInfo.swapchain.pNext);  // because pNext is void* instead of const void* in vulkan header\n"
        "            realSwapchainCreateInfo.swapchain.pNext                  = &realSwapchainCreateInfo.surfaceFullScreenExclusive;\n"
        "            break;\n"
        "        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT:\n"
        "            FFX_USE_PNEXT_AS_IS(surfaceFullScreenExclusiveWin32, VkSurfaceFullScreenExclusiveWin32InfoEXT);\n"
        "            break;\n",
        "#if defined(_WIN32)\n"
        "        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT:\n"
        "            realSwapchainCreateInfo.surfaceFullScreenExclusive       = *reinterpret_cast<const VkSurfaceFullScreenExclusiveInfoEXT*>(pCurrent);\n"
        "            realSwapchainCreateInfo.surfaceFullScreenExclusive.pNext = const_cast<void*>(realSwapchainCreateInfo.swapchain.pNext);  // because pNext is void* instead of const void* in vulkan header\n"
        "            realSwapchainCreateInfo.swapchain.pNext                  = &realSwapchainCreateInfo.surfaceFullScreenExclusive;\n"
        "            break;\n"
        "        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT:\n"
        "            FFX_USE_PNEXT_AS_IS(surfaceFullScreenExclusiveWin32, VkSurfaceFullScreenExclusiveWin32InfoEXT);\n"
        "            break;\n"
        "#else\n"
        "        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT:\n"
        "        case VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT:\n"
        "            // Win32-only extensions; a Linux swapchain never offers them. Keep the chain intact.\n"
        "            FFX_USE_PNEXT_AS_IS(imageFormatList, VkImageFormatListCreateInfo);\n"
        "            break;\n"
        "#endif\n",
    ),
    (
        SWAP_CPP,
        "setWaitCallback(static_cast<FfxWaitCallbackFunc>(ptr));",
        "setWaitCallback(reinterpret_cast<FfxWaitCallbackFunc>(ptr));",
    ),
    (
        PROV_SWAP_CPP,
        "0xF65D'564Bui64",
        "0xF65D564Bull",
    ),
    (
        PROV_FG_CPP,
        "0xF600'0000ui64",
        "0xF6000000ull",
    ),
]

def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unpack_sdk(source: Path, work: Path) -> Path:
    if source.is_dir():
        candidates = list(source.glob("*/Kits")) + list(source.glob("Kits"))
        for candidate in candidates:
            if (candidate / "FidelityFX" / "api" / "include" / "ffx_api.h").is_file():
                # Patch a copy, never the user's tree.
                copy = work / "sdk-copy"
                if copy.exists():
                    shutil.rmtree(copy)
                shutil.copytree(candidate.parent, copy)
                return copy
        fail(f"{source} does not look like an extracted FidelityFX SDK tree")

    extract = work / "sdk-extract"
    if extract.exists():
        shutil.rmtree(extract)
    extract.mkdir(parents=True)
    with tarfile.open(source, "r:*") as archive:
        archive.extractall(extract)
    for candidate in sorted(extract.glob("*/Kits")):
        if (candidate / "FidelityFX" / "api" / "include" / "ffx_api.h").is_file():
            return candidate.parent
    fail("the archive does not contain the expected FidelityFX SDK layout")


def apply_patches(sdk_root: Path):
    for rel, old, new in PATCHES:
        path = sdk_root / rel
        if not path.is_file():
            fail(f"missing SDK file: {rel}")
        text = path.read_text(encoding="utf-8")
        # A previously patched tree is acceptable if the replacement is already present. This check must come first:
        # several edits append after an anchor, so the original text remains and would otherwise be applied twice.
        if new in text:
            continue
        count = text.count(old)
        if count == 0:
            fail(f"cannot apply Linux portability edit to {rel}: expected text not found (not SDK {SDK_TAG}?)")
        if count > 1:
            fail(f"ambiguous Linux portability edit in {rel}: {count} matches")
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    # Normalise Windows-style include paths anywhere in the SDK (some Vulkan headers use ..\\..\\..\\backend\\...).
    for path in sdk_root.rglob("*"):
        if path.suffix.lower() not in (".h", ".hpp", ".cpp", ".c"):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        fixed = re.sub(r'#include\s+"([^"]*)"', lambda m: '#include "%s"' % m.group(1).replace("\\", "/"), text)
        if fixed != text:
            path.write_text(fixed, encoding="utf-8")


def run(cmd, cwd=None, env=None):
    print("+", " ".join(str(c) for c in cmd))
    proc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env)
    if proc.returncode != 0:
        fail(f"command failed with exit code {proc.returncode}: {' '.join(str(c) for c in cmd)}")


def build(args):
    work = args.work_dir
    work.mkdir(parents=True, exist_ok=True)

    if args.sdk_archive:
        archive = args.sdk_archive
        if not archive.is_file():
            fail(f"SDK archive not found: {archive}")
        if args.sdk_size and archive.stat().st_size != args.sdk_size:
            fail(f"SDK archive size mismatch: expected {args.sdk_size}, got {archive.stat().st_size}")
        if args.sdk_sha256:
            actual = sha256_of(archive)
            if actual.lower() != args.sdk_sha256.lower():
                fail(f"SDK archive SHA-256 mismatch: expected {args.sdk_sha256}, got {actual}")
        print(f"sdk archive ok: {archive.stat().st_size} bytes sha256={sha256_of(archive)}")
        sdk_src = archive
    else:
        sdk_src = args.sdk_src

    sdk_root = unpack_sdk(sdk_src, work)
    print(f"sdk tree: {sdk_root}")
    apply_patches(sdk_root)

    src = work / "src"
    if src.exists():
        shutil.rmtree(src)
    src.mkdir()
    for name in ("CMakeLists.txt", "fg_compat.h", "fg_provider_list.cpp"):
        shutil.copy2(FG_DIR / name, src / name)
    shutil.copytree(FG_DIR / "win32compat", src / "win32compat")
    shutil.copytree(FG_DIR / "miniz", src / "miniz")

    dxc = Path(args.dxc)
    if not dxc.is_file():
        fail(f"dxc not found: {dxc} (pass --dxc /path/to/dxc)")

    run([sys.executable, GEN_BLOBS, "--sdk", sdk_root, "--dxc", str(dxc),
         "--out", src / "fg_blobs.cpp", "--tmp", work / "blobgen"])

    build_dir = work / "build"
    env_note = os.environ.copy()
    env_note["FG_SDK"] = str(sdk_root)
    print(f"FG_SDK={sdk_root}")
    run(["cmake", "-S", src, "-B", build_dir, "-G", "Ninja",
         f"-DCMAKE_BUILD_TYPE=Release"], cwd=work, env=env_note)
    run(["cmake", "--build", build_dir, "-j", str(args.jobs)], env=env_note)

    produced = build_dir / "libamd_fidelityfx_framegeneration_vk.so"
    if not produced.is_file():
        fail("build produced no provider library")

    install = args.install_dir
    install.mkdir(parents=True, exist_ok=True)
    shutil.copy2(produced, install / produced.name)

    license_candidates = [sdk_root / "Kits" / "FidelityFX" / "docs" / "license.md",
                          sdk_root / "Kits" / "FidelityFX" / "LICENSE.md",
                          sdk_root / "license.md"]
    for license_path in license_candidates:
        if license_path.is_file():
            shutil.copy2(license_path, install / "LICENSE-AMD-FSR-SDK.md")
            break

    print(f"installed {install / produced.name} ({produced.stat().st_size} bytes)")

    if args.self_test:
        test_src = FG_DIR
        test_bin = work / "test_context"
        run(["g++", "-std=c++20", "-DFFX_API_ENTRY=",
             "-I", sdk_root / "Kits" / "FidelityFX" / "api" / "include",
             "-I", sdk_root / "Kits" / "FidelityFX" / "framegeneration" / "include",
             test_src / "test_context.cpp", "-o", test_bin, "-lvulkan", "-ldl", "-include", "cstdint",
             str(install / produced.name)])
        run([test_bin, str(install / produced.name)])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--sdk-archive", type=Path, help="official FidelityFX SDK v2.4.0 source archive")
    source.add_argument("--sdk-src", type=Path, help="already extracted SDK tree")
    parser.add_argument("--sdk-size", type=int, help="expected SDK archive size in bytes")
    parser.add_argument("--sdk-sha256", help="expected SDK archive SHA-256")
    parser.add_argument("--dxc", required=True, help="path to the DXC compiler binary")
    parser.add_argument("--work-dir", type=Path, default=Path.home() / ".cache" / "mk8online" / "fsr-build")
    parser.add_argument("--install-dir", type=Path,
                        default=Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
                        / "mk8online" / "fsr")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    parser.add_argument("--self-test", action="store_true", help="build and run the headless context test")
    args = parser.parse_args()
    build(args)


if __name__ == "__main__":
    main()

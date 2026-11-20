#!/usr/bin/env python3
"""Build the project's native Linux FSR 4.1.2 upscaler provider.

The provider itself is the project's GPL-3.0-only port in tools/fsr4-linux. Its
shader SPIR-V and ML model banks are not distributed: they are extracted at build
time from the user's own signed AMD DLL, which ships with AMD's FidelityFX SDK
2.4.0 for Windows. This script

  1. verifies the pinned SDK source archive and the signed upscaler DLL inside it;
  2. extracts the 1,778 SPIR-V modules and four model dispatchers (eight banks)
     with the port's own tools, and runs the port's asset verifier;
  3. configures and builds libamd_fidelityfx_upscaler_vk.so with CMake, passing
     the SDK headers through FSR4_SDK_ROOT;
  4. installs the library, the runtime subset of the generated assets, the GPL
     and AMD notices and a source-offer note under
     $XDG_DATA_HOME/mk8online/fsr/;
  5. optionally compiles and runs a headless self-test (version query, a
     1920x1080 -> 2560x1440 context, dispatch and readback).

Nothing from AMD's signed binaries is committed or packaged. The extracted
assets stay in the user's build/install directories.

Usage:
  build_fsr4_provider.py --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz --self-test
  build_fsr4_provider.py --sdk-src /path/to/FidelityFX-SDK-2.4.0 --self-test
"""

import argparse
import ctypes
import datetime
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PORT = REPO / "tools" / "fsr4-linux"
EXTRACT_ALL = PORT / "tools" / "extract_all.py"
TRIM_ASSETS = PORT / "tools" / "trim_assets.py"
SELFTEST = PORT / "tests" / "provider" / "selftest.cpp"

PROVIDER_NAME = "libamd_fidelityfx_upscaler_vk.so"
ASSET_DIR_NAME = "fsr4-assets"
DLL_RELATIVE = Path("Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll")

# Pinned FidelityFX SDK 2.4.0 source archive. These are the only inputs the
# launcher download step needs to fetch; a different size or hash is rejected.
SDK_ARCHIVE_SIZE = 253_592_010
SDK_ARCHIVE_SHA256 = "111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a"

# The signed Vulkan upscaler DLL inside that SDK. The port's PHASE0.md records
# this hash; verify_assets.py pins the model-bank hashes derived from it.
DLL_SIZE = 36_135_696
DLL_SHA256 = "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"

# Extraction contract from tools/fsr4-linux/tools/verify_assets.py.
EXPECTED_SHADERS = 1_778
EXPECTED_SHADER_BYTES = 102_181_900
EXPECTED_MODEL_CLASSES = 4
EXPECTED_MODEL_BANKS = 8
FP8_STANDARD_SHA256 = "ce1bd5f19d4fc14f857c5fc810def7f45d5a935a62aa2ca5b5a59829a1d6c868"

# trim_assets.py selects the blobs the provider can request: 191 modules plus
# models/FP8_standard.bin (the RDX 9070 XT / RADV FP8-WMMA family).
EXPECTED_RUNTIME_SHADERS = 191
MIN_FREE_BYTES = 4 * 1024 ** 3


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_pinned(path: Path, expected_size, expected_sha256, label: str):
    """Fail closed on a size or hash mismatch, printing expected and actual."""
    path = Path(path)
    if not path.is_file():
        fail(f"{label} not found: {path}")
    actual_size = path.stat().st_size
    if expected_size is not None and actual_size != expected_size:
        fail(f"{label} size mismatch: expected {expected_size}, got {actual_size} ({path})")
    actual_sha256 = sha256_of(path)
    if expected_sha256 and actual_sha256.lower() != expected_sha256.lower():
        fail(f"{label} SHA-256 mismatch: expected {expected_sha256}, got {actual_sha256} ({path})")
    print(f"{label} ok: {actual_size} bytes sha256={actual_sha256}")
    return actual_sha256


def check_python():
    if sys.version_info < (3, 10):
        fail(f"Python 3.10+ is required (this is {sys.version.split()[0]})")


def check_disk(path: Path):
    path.mkdir(parents=True, exist_ok=True)
    free = shutil.disk_usage(path).free
    if free < MIN_FREE_BYTES:
        fail(f"only {free / 1024 ** 3:.1f} GiB free under {path}; "
             f"the build needs a {MIN_FREE_BYTES / 1024 ** 3:.0f} GiB reserve")
    print(f"disk reserve ok: {free / 1024 ** 3:.1f} GiB free under {path}")


def find_sdk_root(candidate: Path):
    """Return the directory that contains Kits/FidelityFX from an extracted tree."""
    for base in (candidate, *sorted(p for p in candidate.iterdir() if p.is_dir())):
        if (base / "Kits" / "FidelityFX" / "api" / "include" / "ffx_api.h").is_file():
            return base
    return None


def unpack_sdk(source: Path, work: Path) -> Path:
    if source.is_dir():
        sdk_root = find_sdk_root(source)
        if sdk_root is None:
            fail(f"{source} does not look like an extracted FidelityFX SDK tree")
        return sdk_root

    if not source.is_file():
        fail(f"SDK archive not found: {source}")
    extract = work / "sdk-extract"
    if extract.is_symlink():
        fail(f"refusing to replace a symlink: {extract}")
    if extract.is_dir():
        existing = find_sdk_root(extract)
        dll = existing / DLL_RELATIVE if existing else None
        if (dll and dll.is_file() and dll.stat().st_size == DLL_SIZE
                and sha256_of(dll) == DLL_SHA256):
            print(f"reusing extracted SDK: {existing}")
            return existing
    if extract.exists():
        shutil.rmtree(extract)
    extract.mkdir(parents=True)
    try:
        with tarfile.open(source, "r:*") as archive:
            archive.extractall(extract)
    except tarfile.TarError as exc:
        fail(f"cannot read SDK archive {source}: {exc}")
    sdk_root = find_sdk_root(extract)
    if sdk_root is None:
        fail("the archive does not contain the expected FidelityFX SDK layout")
    print(f"sdk tree: {sdk_root}")
    return sdk_root


def locate_dll(sdk_root: Path) -> Path:
    dll = sdk_root / DLL_RELATIVE
    if not dll.is_file():
        fail(f"signed upscaler DLL not found inside the SDK: {dll}")
    return dll


def run(cmd, cwd=None, env=None):
    print("+", " ".join(str(c) for c in cmd))
    proc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env)
    if proc.returncode != 0:
        fail(f"command failed with exit code {proc.returncode}: {' '.join(str(c) for c in cmd)}")


def validate_assets(assets: Path, dll_sha256: str):
    """Check the extractor's own manifest against the documented contract."""
    manifest_path = assets / "manifest.json"
    index_path = assets / "index.json"
    models_path = assets / "models" / "manifest.json"
    for path in (manifest_path, index_path, models_path):
        if not path.is_file():
            fail(f"asset extraction is incomplete: {path} is missing")
    import json
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    index = json.loads(index_path.read_text(encoding="utf-8"))
    models = json.loads(models_path.read_text(encoding="utf-8"))

    if manifest.get("source_sha256") != dll_sha256:
        fail("asset manifest source hash does not match the verified DLL: "
             f"expected {dll_sha256}, got {manifest.get('source_sha256')}")
    if index.get("blob_count") != EXPECTED_SHADERS:
        fail(f"shader count mismatch: expected {EXPECTED_SHADERS}, "
             f"got {index.get('blob_count')}")
    if index.get("total_uncompressed") != EXPECTED_SHADER_BYTES:
        fail(f"shader byte count mismatch: expected {EXPECTED_SHADER_BYTES}, "
             f"got {index.get('total_uncompressed')}")
    classes = models.get("models", {})
    if len(classes) != EXPECTED_MODEL_CLASSES:
        fail(f"model class count mismatch: expected {EXPECTED_MODEL_CLASSES}, got {len(classes)}")
    banks = [(name, bank) for name, data in classes.items() for bank in data.get("banks", {}).values()]
    if len(banks) != EXPECTED_MODEL_BANKS:
        fail(f"model bank count mismatch: expected {EXPECTED_MODEL_BANKS}, got {len(banks)}")
    fp8 = classes.get("FP8", {}).get("banks", {}).get("standard")
    if not fp8 or fp8.get("sha256") != FP8_STANDARD_SHA256:
        fail("FP8 standard model bank hash mismatch: "
             f"expected {FP8_STANDARD_SHA256}, got {fp8 and fp8.get('sha256')}")
    if not manifest.get("catalog_present") or not manifest.get("selectors_present"):
        fail("asset extraction is incomplete: catalog/selector tables are missing")
    print(f"assets ok: {index['blob_count']} modules, {index['total_uncompressed']} bytes, "
          f"{len(classes)} model classes / {len(banks)} banks")


def extract_assets(dll: Path, assets: Path, reuse: bool) -> str:
    if reuse:
        manifest = assets / "manifest.json"
        if manifest.is_file():
            try:
                import json
                source_sha = json.loads(manifest.read_text(encoding="utf-8")).get("source_sha256")
            except (OSError, ValueError):
                source_sha = None
            if source_sha == sha256_of(dll):
                print(f"reusing extracted assets: {assets}")
                return source_sha
            print("existing asset extraction is from another DLL; extracting again")
    if assets.is_symlink():
        fail(f"refusing to replace a symlink: {assets}")
    if assets.exists():
        shutil.rmtree(assets)
    run([sys.executable, EXTRACT_ALL, "--dll", dll, "--out", assets])
    import json
    manifest = json.loads((assets / "manifest.json").read_text(encoding="utf-8"))
    return manifest["source_sha256"]


def check_exports(library: Path):
    """Load the built library and resolve the five FFX entry points."""
    names = ("ffxCreateContext", "ffxDestroyContext", "ffxConfigure", "ffxQuery", "ffxDispatch")
    try:
        handle = ctypes.CDLL(str(library))
    except OSError as exc:
        fail(f"cannot load {library}: {exc}")
    missing = []
    for name in names:
        try:
            getattr(handle, name)
        except AttributeError:
            missing.append(name)
    if missing:
        fail(f"{library} does not export: {', '.join(missing)}")
    print(f"exports ok: {', '.join(names)}")


def install_assets(assets: Path, install: Path):
    target = install / ASSET_DIR_NAME
    if target.is_symlink():
        fail(f"refusing to replace a symlinked asset directory: {target}")
    temporary = install / (ASSET_DIR_NAME + ".new")
    if temporary.is_symlink():
        fail(f"refusing to replace a symlinked temporary directory: {temporary}")
    if temporary.exists():
        shutil.rmtree(temporary)
    run([sys.executable, TRIM_ASSETS, assets, temporary])

    shaders = temporary / "shaders"
    count = len(list(shaders.glob("*.spv"))) if shaders.is_dir() else 0
    if count != EXPECTED_RUNTIME_SHADERS:
        fail(f"runtime shader set mismatch: expected {EXPECTED_RUNTIME_SHADERS}, got {count}")
    model = temporary / "models" / "FP8_standard.bin"
    if not model.is_file():
        fail(f"runtime model bank is missing: {model}")
    actual = sha256_of(model)
    if actual != FP8_STANDARD_SHA256:
        fail(f"runtime model bank hash mismatch: expected {FP8_STANDARD_SHA256}, got {actual}")

    if target.exists():
        shutil.rmtree(target)
    temporary.rename(target)
    size = sum(p.stat().st_size for p in target.rglob("*") if p.is_file())
    print(f"installed runtime assets: {count} shader modules + FP8_standard.bin, "
          f"{size / 1024 / 1024:.1f} MiB -> {target}")
    return count, size


def install_licenses(install: Path, provider: Path, dll: Path, dll_sha256: str,
                     shader_count: int, shader_bytes: int,
                     runtime_shaders: int, runtime_bytes: int):
    gpl = PORT / "LICENSE-GPL-3.0.txt"
    if not gpl.is_file():
        fail(f"the port's GPL-3.0 text is missing: {gpl}")
    shutil.copy2(gpl, install / "LICENSE-GPL-3.0.txt")

    notices = PORT / "third_party" / "fsr4-vk" / "LICENSES"
    amd_variants = {
        "LICENSE-AMD-FidelityFX-SDK.md": notices / "AMD-FidelityFX-SDK-license.md",
        "LICENSE-AMD-FidelityFX-SDK-MIT.md": notices / "AMD-FidelityFX-SDK-MIT.md",
    }
    for label, path in amd_variants.items():
        if path.is_file():
            shutil.copy2(path, install / label)
    third_party = PORT / "third_party" / "fsr4-vk" / "THIRD_PARTY_NOTICES.md"
    if third_party.is_file():
        shutil.copy2(third_party, install / "THIRD_PARTY_NOTICES.md")

    build_date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    source_offer = f"""# FSR 4.1.2 native provider — source offer

Installed: {build_date}

- Provider: `{provider.name}` (FSR 4.1.2).
- Licence: GNU General Public License version 3 only (see `LICENSE-GPL-3.0.txt`).
- Corresponding source: the vendored GPL-3.0-only port in the MK8-online
  repository under `tools/fsr4-linux/` (including the vendored
  `third_party/fsr4-vk` base). Build it with `tools/build_fsr4_provider.py`.
  If you received the library without that tree, the source is available from
  the project repository or on request from whoever supplied this build.
- Upstream base: fsr4-vk commit `b0125ea9798633750f83ebabbc00279bcb49d8dd`,
  GPL-3.0-only. See `THIRD_PARTY_NOTICES.md` and the AMD notices beside this file.

## Inputs

- Signed AMD DLL: `{dll.name}`, {dll.stat().st_size} bytes,
  SHA-256 `{dll_sha256}`.
- FidelityFX SDK 2.4.0 source archive: {SDK_ARCHIVE_SIZE} bytes,
  SHA-256 `{SDK_ARCHIVE_SHA256}` (MIT; only its FFX API headers are used here).

The DLL is the user's own copy from AMD's SDK. It is not part of this
installation and is not redistributed by the project.

## What is installed, and what is generated

Installed runtime assets: {runtime_shaders} SPIR-V modules and the
`FP8_standard.bin` model bank, {runtime_bytes / 1024 / 1024:.1f} MiB under
`{ASSET_DIR_NAME}/`.

Build output from the extraction (kept in the work directory, not part of the
provider source): {shader_count} SPIR-V modules ({shader_bytes} bytes), four
model dispatchers / eight banks, the pass catalog and the selector tables.
These are generated from the signed DLL at build time. They are not committed
to the repository and are not part of any package.

## Rebuild

```sh
python3 tools/build_fsr4_provider.py \\
  --sdk-archive FidelityFX-SDK-v2.4.0.tar.gz \\
  --self-test
```

The provider loads `{ASSET_DIR_NAME}` next to the library by default, or the
directory named by `FSR4_ASSET_DIR`.
"""
    (install / "SOURCE-OFFER.md").write_text(source_offer, encoding="utf-8")
    print("installed licences and SOURCE-OFFER.md")


def install_library(produced: Path, install: Path) -> Path:
    install.mkdir(parents=True, exist_ok=True)
    target = install / PROVIDER_NAME
    temporary = install / (PROVIDER_NAME + ".new")
    shutil.copy2(produced, temporary)
    os.replace(temporary, target)
    print(f"installed {target} ({target.stat().st_size} bytes, sha256={sha256_of(target)})")
    return target


def compile_selftest(work: Path, sdk_root: Path) -> Path:
    binary = work / "fsr4_selftest"
    include_api = sdk_root / "Kits" / "FidelityFX" / "api" / "include"
    include_upscaler = sdk_root / "Kits" / "FidelityFX" / "upscalers" / "include"
    common = [
        "g++", "-std=c++17", "-O2",
        "-I", include_api,
        "-I", include_upscaler,
        SELFTEST, "-o", binary, "-lvulkan", "-ldl", "-include", "cstdint",
    ]
    run(common)
    return binary


def self_test(work: Path, sdk_root: Path, library: Path, assets: Path):
    binary = compile_selftest(work, sdk_root)
    env = os.environ.copy()
    env["FSR4_ASSET_DIR"] = str(assets)
    run([binary, library, "--query-only"], env=env)
    run([binary, library, "--assets", assets, "1920", "1080", "2560", "1440"], env=env)


def build(args):
    check_python()
    work = args.work_dir
    check_disk(work)

    if args.sdk_archive:
        if not args.sdk_archive.is_file():
            fail(f"SDK archive not found: {args.sdk_archive}")
        verify_pinned(args.sdk_archive,
                      args.sdk_size if args.sdk_size is not None else SDK_ARCHIVE_SIZE,
                      args.sdk_sha256 if args.sdk_sha256 is not None else SDK_ARCHIVE_SHA256,
                      "SDK archive")
    else:
        if args.sdk_size is not None or args.sdk_sha256 is not None:
            fail("--sdk-size/--sdk-sha256 apply to --sdk-archive, not --sdk-src")

    sdk_root = unpack_sdk(args.sdk_archive or args.sdk_src, work)
    dll = locate_dll(sdk_root)
    dll_sha256 = verify_pinned(dll, DLL_SIZE, DLL_SHA256, "signed upscaler DLL")

    assets = work / "assets"
    source_sha = extract_assets(dll, assets, args.reuse_assets)
    if source_sha != dll_sha256:
        fail(f"extracted manifest names another DLL: expected {dll_sha256}, got {source_sha}")
    validate_assets(assets, dll_sha256)

    build_dir = work / "build"
    env = os.environ.copy()
    env["FSR4_SDK_ROOT"] = str(sdk_root)
    run(["cmake", "-S", PORT, "-B", build_dir, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", f"-DFSR4_SDK_ROOT={sdk_root}"], env=env)
    # Only the provider is installed; the loader and the Phase 0 runner are not
    # part of the runtime path and would add compile time and an unnecessary .so.
    run(["cmake", "--build", build_dir, "--target", "amd_fidelityfx_upscaler_vk",
         "-j", str(args.jobs)], env=env)

    produced = build_dir / PROVIDER_NAME
    if not produced.is_file():
        fail("build produced no provider library")
    check_exports(produced)

    install = args.install_dir
    install.mkdir(parents=True, exist_ok=True)
    provider = install_library(produced, install)
    runtime_shaders, runtime_bytes = install_assets(assets, install)
    import json
    index = json.loads((assets / "index.json").read_text(encoding="utf-8"))
    install_licenses(install, provider, dll, dll_sha256,
                     index["blob_count"], index["total_uncompressed"],
                     runtime_shaders, runtime_bytes)

    if args.self_test:
        self_test(work, sdk_root, provider, install / ASSET_DIR_NAME)

    print("done")
    print(f"  provider: {provider}")
    print(f"  assets:   {install / ASSET_DIR_NAME}")
    print("  runtime:  MK8_FSR4=1 MK8_FSR_VERSION=4.1.2 MK8_FSR_LEGACY_ABI=1 "
          f"MK8_FSR_LIB={provider}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--sdk-archive", type=Path,
                        help="official FidelityFX SDK v2.4.0 source archive")
    source.add_argument("--sdk-src", type=Path,
                        help="already extracted SDK tree")
    parser.add_argument("--sdk-size", type=int,
                        help=f"expected archive size in bytes (default {SDK_ARCHIVE_SIZE})")
    parser.add_argument("--sdk-sha256",
                        help=f"expected archive SHA-256 (default {SDK_ARCHIVE_SHA256})")
    parser.add_argument("--work-dir", type=Path,
                        default=Path.home() / ".cache" / "mk8online" / "fsr4-build")
    parser.add_argument("--install-dir", type=Path,
                        default=Path(os.environ.get("XDG_DATA_HOME",
                                                    Path.home() / ".local" / "share"))
                        / "mk8online" / "fsr")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    parser.add_argument("--reuse-assets", action="store_true",
                        help="reuse an existing extraction from the same DLL")
    parser.add_argument("--self-test", action="store_true",
                        help="run the headless version query and 1080p->1440p dispatch")
    args = parser.parse_args()
    build(args)


if __name__ == "__main__":
    main()

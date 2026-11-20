"""Write the common portable source payload from tracked project files."""
import hashlib
import json
from pathlib import Path
import stat
import subprocess
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from package_guard import audit_archive

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "upstream/manifests/smm2recomp.json"
SOURCE_EXTENSIONS = {".c", ".cpp", ".h", ".inc", ".json", ".mk", ".ps1", ".py", ".txt", ".cs"}
PROVIDER_SOURCES = ("tools/build_fg_provider.py", "tools/gen_fg_blobs.py", "tools/build_upscaler_provider.py",
                    "tools/gen_upscaler_blobs.py", "tools/fetch_vendor_sdk.py", "tools/build_fsr4_provider.py",
                    "tools/fg-linux", "tools/fg-linux-upscaler", "tools/fsr4-linux")
HOST_FILES = ("mk8drecomp.exe", "mk8drecomp.dll.a")
FFMPEG_FILES = ("avcodec-63.dll", "avformat-63.dll", "avutil-61.dll", "swscale-10.dll")
FFMPEG_NOTICES = ("manifest.json", "LICENSE.md", "COPYING.LGPLv2.1", "configuration.txt")


def tracked(*folders):
    output = subprocess.check_output(["git", "ls-files", "-z", "--", *folders], cwd=ROOT)
    return (ROOT / name.decode("utf-8") for name in output.split(b"\0") if name)


def payload_files(windows, importer, host, host_manifest, ffmpeg):
    yield ROOT / "LICENSE", "LICENSE"
    yield MANIFEST, MANIFEST.relative_to(ROOT).as_posix()
    for path in (ROOT / "toolchain/mii_assembly.html", ROOT / "runtime/mk8/graphics/ffx/LICENSE",
                 *tracked(*PROVIDER_SOURCES)):
        yield path, path.relative_to(ROOT).as_posix()
    for path in tracked("runtime", "toolchain", "games/mk8d", "scripts", "docs/licenses"):
        if path.suffix in SOURCE_EXTENSIONS:
            yield path, path.relative_to(ROOT).as_posix()
    for name in ("mk8-import.dll", "mk8-import.runtimeconfig.json", "LibHac.dll", "build-sha256.txt",
                 "dotnet/dotnet.exe" if windows else "dotnet/dotnet"):
        yield importer / name, f"tools/mk8-import/{name}"
    runtime_extensions = {".dll", ".json", ".txt"} if windows else {".dll", ".json", ".txt", ".so"}
    for path in sorted((importer / "dotnet").rglob("*")):
        if path.is_file() and path.suffix in runtime_extensions:
            yield path, "tools/mk8-import/" + path.relative_to(importer).as_posix()
    if windows:
        for name in HOST_FILES:
            yield host / name, f"prebuilt/host/{name}"
        yield host_manifest, "prebuilt/host/manifest.json"
        for name in (*FFMPEG_FILES, *FFMPEG_NOTICES):
            yield ffmpeg / name, f"prebuilt/ffmpeg/{name}"
    for name, digest in sorted(json.loads(MANIFEST.read_text(encoding="utf-8"))["files"].items()):
        path = ROOT / "upstream/smm2recomp" / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Upstream alterado: {name}")
        yield path, path.relative_to(ROOT).as_posix()


def write_payload(payload, windows, *, importer=None, host=None, host_manifest=None, ffmpeg=None):
    importer = importer or ROOT / "tools/mk8-import"
    host = host or ROOT / "prebuilt/host"
    host_manifest = host_manifest or host / "manifest.json"
    ffmpeg = ffmpeg or ROOT / "prebuilt/ffmpeg"
    pending = payload.with_suffix(".pending.zip")
    try:
        with ZipFile(pending, "w", ZIP_DEFLATED) as archive:
            for path, name in payload_files(windows, importer, host, host_manifest, ffmpeg):
                if path.is_symlink():
                    raise ValueError(f"Link na fonte: {path}")
                entry = ZipInfo(name)
                if not windows:
                    entry.create_system = 3
                    mode = 0o755 if name == "tools/mk8-import/dotnet/dotnet" else 0o644
                    entry.external_attr = (stat.S_IFREG | mode) << 16
                archive.writestr(entry, path.read_bytes(), compress_type=ZIP_DEFLATED)
        audit_archive(pending)
        pending.replace(payload)
    finally:
        pending.unlink(missing_ok=True)
    with payload.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()

"""Build the launcher with its allowlisted preparation payload embedded."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

from cargo_target import target_dir

ROOT = Path(__file__).resolve().parents[1]
TARGET = target_dir()
OUTPUT = TARGET / "portable"
# (its own target folder: the packaged build embeds the payload, unlike target/release used in development)
BUILD = TARGET / "package"
MANIFEST = ROOT / "upstream/manifests/smm2recomp.json"
WINDOWS = os.name == "nt"
LAUNCHER = "Launcher.exe" if WINDOWS else "Launcher"
SOURCE_EXTENSIONS = {".c", ".h", ".inc", ".json", ".mk", ".ps1", ".py", ".txt", ".cs"}


def payload_files():
    yield ROOT / "LICENSE"
    yield MANIFEST
    yield ROOT / "toolchain/mii_assembly.html"
    importer = ROOT / "tools/mk8-import"
    for name in ("mk8-import.dll", "mk8-import.runtimeconfig.json", "LibHac.dll", "build-sha256.txt"):
        yield importer / name
    yield importer / "dotnet" / ("dotnet.exe" if WINDOWS else "dotnet")
    runtime_extensions = {".dll", ".json", ".txt"} if WINDOWS else {".dll", ".json", ".txt", ".so"}
    for path in sorted((importer / "dotnet").rglob("*")):
        if path.is_file() and path.suffix in runtime_extensions:
            yield path
    for folder in ("runtime", "toolchain", "games/mk8d", "scripts", "docs/licenses"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and path.suffix in SOURCE_EXTENSIONS:
                yield path
    if not WINDOWS:
        # Linux builds the host runtime from these sources after the user supplies the game. host.py compiles every
        # runtime/mk8/*.cpp (its headers ship with the runtime sources above), including the FSR bridge, frame
        # generation, DLSS/NGX and MIT FFX headers, not just fsr.cpp.
        for path in sorted((ROOT / "runtime/mk8").rglob("*.cpp")):
            yield path
        yield ROOT / "runtime/mk8/graphics/ffx/LICENSE"
        # The launcher builds the optional FSR providers locally from the user's own MIT-licensed SDK download, so
        # the build scripts and their C/C++ scaffolding must ship in the payload (they contain no AMD binaries).
        for name in ("build_fg_provider.py", "gen_fg_blobs.py", "build_upscaler_provider.py",
                     "gen_upscaler_blobs.py", "fetch_vendor_sdk.py", "build_fsr4_provider.py"):
            yield ROOT / "tools" / name
        # The vendored GPL-3.0 FSR 4.1.2 port the launcher builds one-click (sources only, no AMD assets).
        for path in sorted((ROOT / "tools" / "fsr4-linux").rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                yield path
        for folder in ("fg-linux", "fg-linux-upscaler"):
            for path in sorted((ROOT / "tools" / folder).rglob("*")):
                if path.is_file() and "__pycache__" not in path.parts:
                    yield path
    if WINDOWS:
        # The runtime executable built from these same sources (scripts/prebuild-host.py); a build checks its key.
        for name in ("mk8drecomp.exe", "mk8drecomp.dll.a", "manifest.json"):
            yield ROOT / "prebuilt/host" / name
        # The minimal FFmpeg (scripts/build-ffmpeg.py) with its LGPL notices; the build checks every hash.
        manifest = json.loads((ROOT / "prebuilt/ffmpeg/manifest.json").read_text(encoding="utf-8"))
        for name in (*manifest["files"], "manifest.json", "LICENSE.md", "COPYING.LGPLv2.1", "configuration.txt"):
            yield ROOT / "prebuilt/ffmpeg" / name
    for name, digest in sorted(json.loads(MANIFEST.read_text(encoding="utf-8"))["files"].items()):
        path = ROOT / "upstream/smm2recomp" / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Upstream alterado: {name}")
        yield path


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    # The repository publishes only the upstream manifests; the pinned sources are fetched and verified here.
    subprocess.run([sys.executable, ROOT / "scripts/fetch-upstream.py"], check=True)
    subprocess.run([sys.executable, ROOT / "scripts/setup-import-tool.py"], check=True)
    if WINDOWS:
        subprocess.run([sys.executable, ROOT / "scripts/build-ffmpeg.py"], check=True)
        subprocess.run([sys.executable, ROOT / "scripts/prebuild-host.py"], check=True)
    payload = OUTPUT / "launcher-source.zip"
    with ZipFile(payload, "w", ZIP_DEFLATED) as archive:
        for path in payload_files():
            if not path.resolve().is_relative_to(ROOT.resolve()):
                raise ValueError(f"Fonte fora do projeto: {path}")
            entry = ZipInfo(path.relative_to(ROOT).as_posix())
            if not WINDOWS:
                entry.create_system = 3
                mode = 0o755 if path == ROOT / "tools/mk8-import/dotnet/dotnet" else 0o644
                entry.external_attr = (stat.S_IFREG | mode) << 16
            archive.writestr(entry, path.read_bytes(), compress_type=ZIP_DEFLATED)
    digest = hashlib.sha256(payload.read_bytes()).hexdigest()
    env = dict(os.environ, MK8_LAUNCHER_PAYLOAD=str(payload), MK8_LAUNCHER_PAYLOAD_ID=digest)
    flags = env["CARGO_ENCODED_RUSTFLAGS"].split("\x1f") if env.get("CARGO_ENCODED_RUSTFLAGS") else env.get("RUSTFLAGS", "").split()
    # Rust panic/source locations otherwise expose the packager's Windows account in the executable.
    for original, replacement in ((Path.home(), "build-user"), (Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo")), "build-cargo"), (ROOT, "MK8-online")):
        for spelling in dict.fromkeys((str(original), original.as_posix())):
            flags.append(f"--remap-path-prefix={spelling}={replacement}")
    env["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join(flags)
    subprocess.run(
        ["cargo", "build", "--release", "--locked", "-p", "mk8-online", "--features", "tauri/custom-protocol", "--target-dir", str(BUILD)],
        cwd=ROOT, env=env,
        check=True,
    )
    launcher = OUTPUT / LAUNCHER
    shutil.copy2(BUILD / "release" / LAUNCHER, launcher)
    if WINDOWS:
        # A separate archive opts into sibling storage without changing the setup executable's behavior.
        with ZipFile(OUTPUT / "MK8-online-portable.zip", "w", ZIP_DEFLATED) as archive:
            archive.write(launcher, LAUNCHER)
            archive.writestr("portable.txt", "MK8-online portable mode\n")
            archive.write(ROOT / "LICENSE", "LICENSE")
            archive.writestr("README.txt", "Extract to a writable folder. Run Launcher.exe to prepare your own game files.\n"
                             "Run Launcher.exe --play to start an already prepared game without the launcher window.\n"
                             "Data is stored in portable/. Keep this folder when updating Launcher.exe.\n"
                             "Open this version once before moving it. Close all MK8 processes, then move the whole folder.\n"
                             "Internal paths are repaired automatically on next start; metadata backups are retained.\n"
                             "Files selected outside portable/ are external dependencies: include them inside the tree before preparing.\n"
                             "Windows x64 only; game files and keys are not included.\n")
    print(launcher)


if __name__ == "__main__":
    main()

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
SOURCE_EXTENSIONS = {".c", ".inc", ".json", ".mk", ".ps1", ".py", ".txt", ".cs"}


def payload_files():
    yield ROOT / "LICENSE"
    yield MANIFEST
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
    print(launcher)


if __name__ == "__main__":
    main()

"""Build and checksum the x86-64 Linux AppImage and tarball."""
import argparse
import gzip
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

from downloads import fetch
from release import DIST, ROOT, TARGET, TAURI, VERSION, sha256, workspace_version

TOOL_URL = "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage"
TOOL_SHA256 = "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
RUNTIME_URL = "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
RUNTIME_SHA256 = "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
RUNTIME_LICENSE_URL = "https://raw.githubusercontent.com/AppImage/type2-runtime/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/LICENSE"
RUNTIME_LICENSE_SHA256 = "aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1"

APP_RUN = """#!/bin/sh
set -eu
entry=$(readlink -f -- "$0")
MK8_PACKAGE_ROOT=$(dirname -- "$entry")
export MK8_PACKAGE_ROOT
exec "$MK8_PACKAGE_ROOT/usr/bin/Launcher" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=MK8-online
Comment=Prepare and run MK8 Deluxe locally
Exec=AppRun
Icon=io.github.mk8online.launcher
Categories=Game;
Terminal=false
"""


def write_marker(app, kind, version):
    marker = {"format": kind, "arch": "x86_64", "version": version}
    if kind == "tar.gz":
        marker["entries"] = sorted(
            path.relative_to(app).as_posix() for path in app.rglob("*")
            if path != app / ".mk8-package.json"
        )
    (app / ".mk8-package.json").write_text(json.dumps(marker) + "\n", encoding="utf-8")


def stage(app, launcher):
    binary = app / "usr/bin/Launcher"
    binary.parent.mkdir(parents=True)
    shutil.copyfile(launcher, binary)
    binary.chmod(0o755)
    (app / "AppRun").write_text(APP_RUN, encoding="utf-8")
    (app / "AppRun").chmod(0o755)
    (app / "io.github.mk8online.launcher.desktop").write_text(DESKTOP, encoding="utf-8")
    shutil.copyfile(ROOT / "apps/launcher/icons/icon.png", app / "io.github.mk8online.launcher.png")
    shutil.copyfile(ROOT / "LICENSE", app / "LICENSE")


def tarball(app, asset):
    def portable(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = 0
        return info

    with asset.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            archive.add(app, arcname="MK8-online", filter=portable)


def appimage(app, asset):
    tools = TARGET / "package-tools"
    tools.mkdir(parents=True, exist_ok=True)
    tool = fetch(TOOL_URL, tools / "appimagetool-1.9.1.AppImage", sha256=TOOL_SHA256)
    tool.chmod(0o755)
    runtime = fetch(RUNTIME_URL, tools / "appimage-runtime-20251108", sha256=RUNTIME_SHA256)
    license_file = fetch(RUNTIME_LICENSE_URL, tools / "AppImage-Type2-MIT.txt", sha256=RUNTIME_LICENSE_SHA256)
    shutil.copyfile(license_file, app / "AppImage-Type2-MIT.txt")
    subprocess.run([tool, "--appimage-extract-and-run", "--no-appstream", "--runtime-file", runtime,
                    "--mksquashfs-opt=-processors", "--mksquashfs-opt=4", app, asset],
                   check=True, env=dict(os.environ, ARCH="x86_64"))
    asset.chmod(0o755)


def build(tag=None, skip_package=False):
    if sys.platform != "linux" or platform.machine() != "x86_64":
        raise SystemExit("Build Linux x86-64 packages on Linux x86-64.")
    if not shutil.which("file"):
        raise SystemExit("Install file to build the AppImage.")
    version = workspace_version()
    if (not VERSION.fullmatch(version) or json.loads(TAURI.read_text(encoding="utf-8"))["version"] != version
            or (tag and tag != f"v{version}")):
        raise SystemExit("Cargo.toml, tauri.conf.json and the release tag must agree.")
    if not skip_package:
        subprocess.run([sys.executable, ROOT / "scripts/package-launcher.py"], cwd=ROOT, check=True)
    launcher = TARGET / "portable/Launcher"
    if not launcher.is_file():
        raise SystemExit(f"Packaged launcher is missing: {launcher}")
    DIST.mkdir(parents=True, exist_ok=True)
    basename = f"MK8-online-{version}-linux-x86_64"
    with tempfile.TemporaryDirectory(prefix="mk8-linux-", dir=DIST) as scratch:
        scratch = Path(scratch)
        app = scratch / "MK8-online.AppDir"
        stage(app, launcher)
        archive = scratch / f"{basename}.tar.gz"
        image = scratch / f"{basename}.AppImage"
        write_marker(app, "tar.gz", version)
        tarball(app, archive)
        write_marker(app, "appimage", version)
        appimage(app, image)
        for asset in (archive, image):
            checksum = asset.with_name(f"{asset.name}.sha256")
            checksum.write_text(f"{sha256(asset)}  {asset.name}\n", encoding="ascii")
            asset.replace(DIST / asset.name)
            checksum.replace(DIST / checksum.name)
            print(DIST / asset.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag")
    parser.add_argument("--skip-package", action="store_true")
    args = parser.parse_args()
    build(args.tag, args.skip_package)

"""Launch the built Linux packages with fresh private data and check the embedded importer."""
import json
import os
from pathlib import Path
import signal
import subprocess
import tarfile
import tempfile
import time
import urllib.request

from package_guard import audit_tree
from release import DIST, sha256, workspace_version

# The Linux package installs the launcher and builds the host runtime from this payload. If any of these
# disappears, the package starts but the first "Build the game" cannot produce the current runtime.
REQUIRED_RUNTIME_SOURCES = (
    "toolchain/host.py",
    "toolchain/build.py",
    "runtime/mk8/support/yaz0.c",
    "runtime/mk8/graphics/fsr_bridge.c",
    "runtime/mk8/graphics/fsr.cpp",
    "runtime/mk8/graphics/ffx/ffx_api.h",
    "runtime/mk8/graphics/ffx/ffx_upscale.h",
    "runtime/mk8/graphics/ffx/vk/ffx_api_vk.h",
)


def verify_payload(project):
    missing = [name for name in REQUIRED_RUNTIME_SOURCES if not (project / name).is_file()]
    if missing:
        raise RuntimeError(f"Runtime sources missing from the package: {', '.join(missing)}")
    audit_tree(project)


def run(entry, arguments, folder):
    local = folder / "custom game data"
    xdg = folder / "xdg"
    env = dict(os.environ, XDG_DATA_HOME=str(xdg))
    env.pop("APPIMAGE", None)
    env.pop("MK8_PACKAGE_ROOT", None)
    log = folder / "smoke.log"
    with log.open("w") as output:
        child = subprocess.Popen([entry, *arguments, "--headless", "--data-dir", local],
                                 stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                 env=env, cwd=folder, start_new_session=True)
    try:
        state = local / "launcher.json"
        deadline = time.monotonic() + 120
        while not state.is_file():
            if child.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(f"{entry.name} did not start:\n{log.read_text(errors='replace')}")
            time.sleep(0.1)
        address = json.loads(state.read_text(encoding="utf-8"))["url"]
        with urllib.request.urlopen(address, timeout=15) as page:
            if page.status != 200 or b"MK8-online" not in page.read():
                raise RuntimeError(f"{entry.name} did not serve the launcher")
        versions = xdg / "MK8-online/versions"
        importer, = versions.glob("*/tools/mk8-import/mk8-import.dll")
        status = subprocess.run([importer.parent / "dotnet/dotnet", importer],
                                cwd=folder, timeout=30, env=env).returncode
        if status != 2:
            raise RuntimeError(f"Embedded importer could not run (exit {status})")
        verify_payload(importer.parents[2])
    finally:
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        child.wait(timeout=10)


def verify_checksum(path):
    digest, name = path.with_name(path.name + ".sha256").read_text(encoding="ascii").split()
    if name != path.name or sha256(path) != digest:
        raise ValueError(f"Checksum mismatch: {path}")


def main():
    basename = f"MK8-online-{workspace_version()}-linux-x86_64"
    for suffix in (".tar.gz", ".AppImage"):
        asset = DIST / f"{basename}{suffix}"
        verify_checksum(asset)
        with tempfile.TemporaryDirectory(prefix="mk8-package-smoke-") as scratch:
            folder = Path(scratch) / "installation with spaces"
            folder.mkdir()
            if suffix == ".tar.gz":
                with tarfile.open(asset) as archive:
                    archive.extractall(folder, filter="data")
                run(folder / "MK8-online/AppRun", [], folder)
            else:
                run(asset, ["--appimage-extract-and-run"], folder)
        print(f"{asset.name}: startup and embedded importer passed")


if __name__ == "__main__":
    main()

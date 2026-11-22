"""Smoke-test Linux packages, the embedded importer and headless update recovery."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import socket
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


def wait_started(state, child, log, previous_pid=None):
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        if state.is_file():
            launch = json.loads(state.read_text(encoding="utf-8"))
            if launch["pid"] != previous_pid:
                with urllib.request.urlopen(launch["url"], timeout=15) as page:
                    if page.status == 200 and b"MK8-online" in page.read():
                        return launch
        if child and child.poll() is not None:
            break
        time.sleep(0.1)
    raise RuntimeError(f"Launcher did not start:\n{log.read_text(errors='replace')}")


def stop_group(child):
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    child.wait(timeout=10)


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
        wait_started(local / "launcher.json", child, log)
        versions = xdg / "MK8-online/versions"
        importer, = versions.glob("*/tools/mk8-import/mk8-import.dll")
        status = subprocess.run([importer.parent / "dotnet/dotnet", importer],
                                cwd=folder, timeout=30, env=env).returncode
        if status != 2:
            raise RuntimeError(f"Embedded importer could not run (exit {status})")
        verify_payload(importer.parents[2])
    finally:
        stop_group(child)


def smoke_update(asset, folder, rollback):
    # Enter the normal handoff after staging a package, with deterministic startup failure for rollback.
    with tarfile.open(asset) as archive:
        archive.extractall(folder, filter="data")
    target = folder / "MK8-online"
    stage = folder / ".MK8-online.mk8-update"
    stage.mkdir(mode=0o700)
    candidate = stage / "package"
    shutil.copytree(target, candidate)
    if rollback:
        (candidate / "AppRun").write_text("#!/bin/sh\nexit 9\n", encoding="utf-8")
    data = folder / "game data"
    data.mkdir()
    sample = data / "save"
    sample.write_bytes(b"smoke save")
    state = data / "launcher.json"
    env = dict(os.environ, XDG_DATA_HOME=str(folder / "xdg"))
    for key in ("APPIMAGE", "APPDIR", "MK8_PACKAGE_ROOT", "MK8_UPDATE_HEALTH"):
        env.pop(key, None)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]

    def identity(path):
        info = path.stat()
        return {"dev": info.st_dev, "inode": info.st_ino}

    before, after = identity(target), identity(candidate)
    journal = {"before": before, "after": after, "version": workspace_version(),
               "nonce": "smoke", "data": str(data), "format": "Tar", "headless": True, "port": port}
    (stage / "journal.json").write_text(json.dumps(journal), encoding="utf-8")
    log = folder / "updater.log"
    with log.open("w") as output, stage.with_suffix(".mk8-update.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        old = subprocess.Popen([target / "AppRun", "--headless", "--port", str(port), "--data-dir", data],
                               env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            original = wait_started(state, old, log)
            wait_fd = os.pidfd_open(old.pid)
            try:
                handoff = subprocess.Popen([target / "usr/bin/Launcher", "--linux-update-handoff", target,
                                            str(wait_fd), str(lock.fileno()), "--headless"],
                                           env=env, stdout=output, stderr=subprocess.STDOUT,
                                           pass_fds=(wait_fd, lock.fileno()), start_new_session=True)
            finally:
                os.close(wait_fd)
            try:
                old.terminate()
                old.wait(timeout=10)
                try:
                    result = handoff.wait(timeout=65)
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError(f"Updater handoff timed out:\n{log.read_text()}") from error
                if (result != 0) != rollback or stage.exists():
                    raise RuntimeError(f"Unexpected updater result {result}:\n{log.read_text()}")
                restored = wait_started(state, None, log, previous_pid=original["pid"])
                if (restored["url"] != original["url"] or identity(target) != (before if rollback else after)
                        or sample.read_bytes() != b"smoke save"):
                    raise RuntimeError("Updater did not retain the requested port, package or saved data")
            finally:
                stop_group(handoff)
        finally:
            stop_group(old)


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
        if suffix == ".tar.gz":
            for rollback in (False, True):
                with tempfile.TemporaryDirectory(prefix="mk8-update-smoke-") as scratch:
                    smoke_update(asset, Path(scratch), rollback)
            print(f"{asset.name}: headless activation and rollback passed on the requested port")


if __name__ == "__main__":
    main()

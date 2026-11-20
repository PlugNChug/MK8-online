"""Build the launcher with its allowlisted preparation payload embedded."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile

from cargo_target import target_dir
from launcher_payload import write_payload
from package_guard import audit_archive

ROOT = Path(__file__).resolve().parents[1]
TARGET = target_dir()
OUTPUT = TARGET / "portable"
# (its own target folder: the packaged build embeds the payload, unlike target/release used in development)
BUILD = TARGET / "package"
WINDOWS = os.name == "nt"
LAUNCHER = "Launcher.exe" if WINDOWS else "Launcher"



def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    # The repository publishes only the upstream manifests; the pinned sources are fetched and verified here.
    subprocess.run([sys.executable, ROOT / "scripts/fetch-upstream.py"], check=True)
    subprocess.run([sys.executable, ROOT / "scripts/setup-import-tool.py"], check=True)
    if WINDOWS:
        subprocess.run([sys.executable, ROOT / "scripts/build-ffmpeg.py"], check=True)
        subprocess.run([sys.executable, ROOT / "scripts/prebuild-host.py"], check=True)
    payload = OUTPUT / "launcher-source.zip"
    digest = write_payload(payload, WINDOWS)
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
        audit_archive(OUTPUT / "MK8-online-portable.zip")
    print(launcher)


if __name__ == "__main__":
    main()

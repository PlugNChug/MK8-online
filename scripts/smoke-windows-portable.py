"""Start the actual Windows release ZIP and verify its extracted prebuilt runtime."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.request
from zipfile import ZipFile

from cargo_target import target_dir
from release import ROOT


def verify_host(project, folder):
    profile = project / "games/mk8d/profiles/4.0.1.json"
    source = folder / "runtime"
    subprocess.run([sys.executable, project / "runtime/prepare.py", "--upstream", project / "upstream/smm2recomp",
                    "--output", source, "--profile", profile], check=True, cwd=project)
    env = dict(os.environ)
    env.setdefault("MK8_NATIVE_TOOLS", str(ROOT / "local/native-tools"))
    env.pop("MK8_HOST_FROM_SOURCE", None)
    # Use the production key and receipt check and fail at any attempt to compile.
    subprocess.run([sys.executable, "-c", """
import sys
from pathlib import Path
sys.path.insert(0, 'toolchain')
import host

use_prebuilt = host.use_prebuilt

def require_prebuilt(key, output):
    executable = use_prebuilt(key, output)
    if executable is None:
        raise RuntimeError('The extracted runtime did not match the prebuilt host key')
    return executable

host.use_prebuilt = require_prebuilt
host.build_host(Path(sys.argv[1]), Path(sys.argv[2]))
""", source, folder / "host"], cwd=project, env=env, check=True)


def main():
    if os.name != "nt":
        raise SystemExit("Run the portable smoke test on Windows.")
    archive = target_dir() / "portable/MK8-online-portable.zip"
    with tempfile.TemporaryDirectory(prefix="mk8 portable smoke ") as scratch:
        folder = Path(scratch)
        with ZipFile(archive) as bundle:
            bundle.extractall(folder)
        data = folder / "game data"
        with (folder / "launcher.log").open("w+") as log:
            child = subprocess.Popen([folder / "Launcher.exe", "--headless", "--data-dir", data],
                                     cwd=folder, stdout=log, stderr=subprocess.STDOUT)
            try:
                state = data / "launcher.json"
                deadline = time.monotonic() + 120
                while not state.is_file():
                    if child.poll() is not None or time.monotonic() >= deadline:
                        log.seek(0)
                        raise RuntimeError(f"Portable launcher failed to start:\n{log.read()}")
                    time.sleep(0.1)
                url = json.loads(state.read_text(encoding="utf-8"))["url"]
                with urllib.request.urlopen(url, timeout=15) as page:
                    if page.status != 200 or b"<title>MK8-online" not in page.read():
                        raise RuntimeError("Portable launcher did not serve its loopback page")
                manifest, = (folder / "portable").glob("*/prebuilt/host/manifest.json")
                verify_host(manifest.parents[2], folder)
            finally:
                child.terminate()
                child.wait(timeout=10)
    print("Windows portable startup and extracted host key passed")


if __name__ == "__main__":
    main()

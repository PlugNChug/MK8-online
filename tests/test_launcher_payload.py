"""Check the portable source inventory without private inputs or a launcher build."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import launcher_payload as payload
from package_guard import ForbiddenArtifact


class LauncherPayloadTests(unittest.TestCase):
    def test_windows_runtime_parity_and_dirty_provider_tree(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = root / "upstream/manifests/smm2recomp.json"

            def add(name, body=b"fixture"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(body)

            subprocess.run(["git", "init", "-q", root], check=True)
            add(str(manifest.relative_to(root)), b'{"files": {}}')
            sources = {
                "runtime/mk8/graphics/fsr.cpp", "runtime/mk8/graphics/ffx/LICENSE",
                "tools/fsr4-linux/src/provider/ffx_provider.cpp", "tools/fg-linux/CMakeLists.txt",
            }
            for name in (*sources, "LICENSE", "toolchain/mii_assembly.html",
                         "runtime/mk8/windows.manifest", "runtime/mk8/windows.rc"):
                add(name)
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            for name in ("mk8-import.dll", "mk8-import.runtimeconfig.json", "LibHac.dll", "build-sha256.txt",
                         "dotnet/dotnet", "dotnet/dotnet.exe"):
                add(f"tools/mk8-import/{name}")
            for name in (*payload.HOST_FILES, "manifest.json"):
                add(f"prebuilt/host/{name}")
            for name in (*payload.FFMPEG_FILES, *payload.FFMPEG_NOTICES):
                add(f"prebuilt/ffmpeg/{name}")
            dirty = "tools/fsr4-linux/build/models/FP8_standard.bin"
            add(dirty)
            with patch.multiple(payload, ROOT=root, MANIFEST=manifest):
                for windows in (False, True):
                    archive = root / f"{windows}.zip"
                    payload.write_payload(archive, windows)
                    with ZipFile(archive) as zipped:
                        names = set(zipped.namelist())
                        self.assertTrue(sources <= names)
                        self.assertNotIn(dirty, names)
                subprocess.run(["git", "add", dirty], cwd=root, check=True)
                with self.assertRaisesRegex(ForbiddenArtifact, "generated model"):
                    payload.write_payload(root / "rejected.zip", True)
                self.assertFalse((root / "rejected.zip").exists())

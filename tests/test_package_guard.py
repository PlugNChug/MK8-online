"""The release guard rejects AMD FidelityFX provider binaries and assets, not source files."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/package_guard.py"
spec = importlib.util.spec_from_file_location("package_guard", SCRIPT)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def shared_object(path, source):
    clang = shutil.which("clang")
    if not clang:
        raise unittest.SkipTest("clang is required to build a test shared object")
    subprocess.run([clang, "-shared", "-fPIC", "-x", "c", "-o", str(path), "-"],
                   input=source, text=True, check=True)


class PackageGuardTests(unittest.TestCase):
    def test_provider_libraries_and_private_assets_are_rejected_by_name(self):
        for name in ("libamd_fidelityfx_upscaler_vk.so", "amd_fidelityfx_loader.dll",
                     "fsr4-assets/models.bin", "fg_blobs/pass.spv", "fg_apply.spv"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"x")
                with self.assertRaises(guard.ForbiddenArtifact):
                    guard.audit_tree(root)

    def test_runtime_sources_and_mit_headers_are_allowed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ("runtime/mk8/graphics/fsr.cpp",
                         "runtime/mk8/graphics/ffx/ffx_api.h",
                         "runtime/mk8/support/yaz0.c"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("source", encoding="utf-8")
            guard.audit_tree(root)

    def test_shared_object_exporting_the_ffx_entry_point_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            shared_object(root / "provider.so", "void ffxCreateContext(void) {}\n")
            with self.assertRaises(guard.ForbiddenArtifact):
                guard.audit_tree(root)

    def test_unrelated_shared_object_is_allowed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            shared_object(root / "unrelated.so", "void mk8_package_guard_probe(void) {}\n")
            guard.audit_tree(root)

    def test_archive_names_are_checked_without_extracting(self):
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / "payload.zip"
            with zipfile.ZipFile(archive, "w") as zip_file:
                zip_file.writestr("runtime/mk8/support/yaz0.c", "source")
                zip_file.writestr("fsr4-assets/model.bin", b"x")
            with self.assertRaises(guard.ForbiddenArtifact):
                guard.audit_archive(archive)


if __name__ == "__main__":
    unittest.main()

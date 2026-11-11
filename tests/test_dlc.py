"""DLC com arquivos autorais: importação, hashes e comportamento IPC nativo."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "toolchain"))
import dlc
import import_dlc
from native import DEFAULT_COMPILER, compiler_env
from test_import_mk8 import package


def fixture_files(folder):
    folder.mkdir(parents=True)
    files = {"contents.dat": b"conteudo autoral".ljust(4096, b"!"), "version.txt": b"teste"}
    for name, payload in files.items():
        (folder / name).write_bytes(payload)
    return {name: (len(payload), hashlib.sha256(payload).hexdigest()) for name, payload in files.items()}


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#undef IN
#undef OUT
#include "dlc.c"
const char *g_romfs_dir;
void rt_log(const char *fmt, ...) { (void)fmt; }
uint32_t kernel_new_handle(KObject *object) {
    if (object->type != KOBJ_EVENT) exit(8);
    free(object);
    return 0x123;
}
static IpcResp call(uint32_t cmd, uint32_t first, uint32_t second, void *buffer, uint64_t size, unsigned bytes) {
    uint32_t input[4] = {first, second, 0, 0};
    IpcReq req = {.cmd=cmd, .data=(uint8_t *)input, .data_size=bytes, .nb=1};
    req.b[0] = (IpcBuf){(uint64_t)(uintptr_t)buffer, size};
    IpcResp resp = {0};
    mk8_add_on_content(NULL, &req, &resp);
    return resp;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    g_romfs_dir = argv[1];
    uint32_t *out = VirtualAlloc((void *)(GUEST_LO + 0x100000), 4096, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
    if (!out) return 9;
    out[0] = out[1] = out[2] = 0xa5a5a5a5;
    IpcResp count = call(2,0,0,NULL,0,8);
    IpcResp list = call(3,0,1,out+1,4,16);
    IpcResp prepare = call(7,1,0,NULL,0,16);
    IpcResp missing = call(7,2,0,NULL,0,16);
    IpcResp short_buffer = call(3,0,1,out+1,3,16);
    IpcResp overflow = call(3,0,0xffffffff,out+1,4,16);
    IpcResp bad_address = call(3,0,1,(void *)UINT64_MAX,4,16);
    IpcResp short_input = call(3,0,1,out+1,4,4);
    IpcResp later = call(3,1,1,out+1,4,16);
    IpcResp event = call(8,0,0,NULL,0,0);
    IpcResp unknown = call(999,0,0,NULL,0,16);
    char path[16384];
    int storage = mk8_dlc_storage_path(0x0100152000023001ull,path,sizeof path);
    int system = mk8_dlc_storage_path(0x0100000000000802ull,path,sizeof path);
    int other = mk8_dlc_storage_path(0x0100152000023002ull,path,sizeof path);
    printf("%u %u %u %u %u %u %u %u %u %u %u %u %u %u %d %d %d %d %u\n",
        count.result,*(uint32_t *)count.data,list.result,*(uint32_t *)list.data,out[1],
        prepare.result,missing.result,short_buffer.result,overflow.result,bad_address.result,
        short_input.result,*(uint32_t *)later.data,out[0],out[2],storage,system,other,event.ncopy,unknown.result);
    VirtualFree(out,0,MEM_RELEASE);
    return 0;
}
'''


class DlcImportTests(unittest.TestCase):
    def test_files_and_manifest_reject_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "romfs"
            files = fixture_files(folder)
            with patch.object(import_dlc, "FILES", files):
                import_dlc.validate_files(folder)
                marker = import_dlc.manifest_bytes()
                self.assertEqual(len(marker), 112)
                self.assertEqual(import_dlc.MANIFEST.unpack(marker)[:5],
                                 (b"MK8DLC1\0", import_dlc.APPLICATION, import_dlc.TITLE, 65536, 1))
                (folder / "contents.dat").write_bytes(b"x" * 4096)
                with self.assertRaises(ValueError):
                    import_dlc.validate_files(folder)
                (folder / "extra").touch()
                with self.assertRaises(ValueError):
                    import_dlc.validate_files(folder)

    def test_import_publishes_verified_files_and_preserves_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            local = root / "local"
            data = local / "game-data/test"
            data.mkdir(parents=True)
            executable = local / "runtime.exe"
            executable.write_bytes(b"executavel autoral")
            records, inputs = {}, {}
            for name in ("main", "sdk", "subsdk0", "main.npdm"):
                path = local / name
                path.write_bytes(name.encode())
                records["npdm" if name == "main.npdm" else name] = import_dlc.file_record(path)
                inputs[name] = records["npdm" if name == "main.npdm" else name]["sha256"]
            candidate = {"schema": 2, "profile": "mk8d-smm2-experimental", "gameplay_verified": False,
                         "files": records, "executable": import_dlc.file_record(executable), "data_root": str(data)}
            (local / "candidate.json").write_text(json.dumps(candidate))
            (local / "generation.json").write_text(json.dumps({"identity": "a" * 64}))
            (local / "config.json").write_text('{"fixture":true}')
            profile = root / "games/mk8d/profiles"
            profile.mkdir(parents=True)
            (profile / "4.0.1.json").write_text(json.dumps({"input_sha256": inputs}))
            authored = root / "authored"
            files = fixture_files(authored)
            contents = {"data.nca": b"NCA autoral", "meta.cnmt.nca": b"CNMT autoral"}
            hashes = {name: (len(payload), hashlib.sha256(payload).hexdigest()) for name, payload in contents.items()}
            archive = {name.encode(): payload for name, payload in contents.items()}
            archive[b"0100152000023001.fixture.tik"] = b"ticket autoral"
            package_path = root / "dlc.nsp"
            package_path.write_bytes(package(archive))
            calls = []

            def run(args, **kwargs):
                calls.append(args)
                if "--dlc" in args:
                    shutil.copytree(authored, Path(args[-1]) / "romfs")
                return subprocess.CompletedProcess(args, 0)

            with patch.multiple(import_dlc, ROOT=root, FILES=files, CONTENTS=hashes), \
                    patch.object(import_dlc, "require_space"), patch.object(import_dlc.subprocess, "run", side_effect=run):
                archive[b"data.nca"] = b"x" * len(contents["data.nca"])
                package_path.write_bytes(package(archive))
                with self.assertRaises(ValueError):
                    import_dlc.install(package_path, root / "fixture.keys", local)
                self.assertEqual(calls, [])
                self.assertEqual(json.loads((local / "candidate.json").read_text()), candidate)
                self.assertFalse((data / "extracted/dlc").exists())
                archive[b"data.nca"] = contents["data.nca"]
                package_path.write_bytes(package(archive))
                with patch.object(import_dlc.subprocess, "run", return_value=subprocess.CompletedProcess([], 20)):
                    with self.assertRaises(ValueError):
                        import_dlc.install(package_path, root / "fixture.keys", local)
                self.assertEqual(json.loads((local / "candidate.json").read_text()), candidate)
                self.assertFalse((data / "extracted/dlc").exists())
                import_dlc.install(package_path, root / "fixture.keys", local)
                installed = json.loads((local / "candidate.json").read_text())
                self.assertEqual(len(calls), 2)
                self.assertEqual((local / "config.json").read_text(), '{"fixture":true}')
                for key in ("dlc-manifest", "dlc-contents", "dlc-version"):
                    record = installed["files"][key]
                    self.assertEqual(import_dlc.sha(Path(record["path"])), record["sha256"])
                import_dlc.install(package_path, root / "fixture.keys", local)
                self.assertEqual(len(calls), 2)
                self.assertEqual(json.loads((local / "candidate.json").read_text())["identity"], installed["identity"])
                (local / "dlc.json").write_text('{"enabled":false}')
                before_reinstall = json.loads((local / "candidate.json").read_text())
                with patch.object(import_dlc.subprocess, "run", return_value=subprocess.CompletedProcess([], 20)):
                    with self.assertRaises(ValueError):
                        import_dlc.install(package_path, root / "fixture.keys", local, reinstall=True)
                self.assertEqual(json.loads((local / "candidate.json").read_text()), before_reinstall)
                self.assertEqual(list(local.glob("dlc-backup-*")), [])
                import_dlc.install(package_path, root / "fixture.keys", local, reinstall=True)
                self.assertEqual(len(calls), 4)
                self.assertEqual(json.loads((local / "dlc.json").read_text()), {"enabled": False})
                self.assertEqual(json.loads((local / "candidate.json").read_text()), before_reinstall)
                self.assertEqual(len(list(local.glob("dlc-backup-*"))), 1)
                content_path = Path(installed["files"]["dlc-contents"]["path"])
                content_path.write_bytes(b"danificado")
                with patch.object(import_dlc.subprocess, "run", return_value=subprocess.CompletedProcess([], 20)):
                    with self.assertRaises(ValueError):
                        import_dlc.install(package_path, root / "fixture.keys", local)
                self.assertEqual(content_path.read_bytes(), b"danificado")
                self.assertEqual(json.loads((local / "candidate.json").read_text()), installed)
                import_dlc.install(package_path, root / "fixture.keys", local)
                self.assertEqual(content_path.read_bytes(), (authored / "contents.dat").read_bytes())
                backups = list(local.glob("dlc-backup-*"))
                self.assertEqual(len(backups), 2)
                self.assertIn(b"danificado", [(backup / "0100152000023001/romfs/contents.dat").read_bytes() for backup in backups])
                installed["data_root"] = str(local / "../fora")
                (local / "candidate.json").write_text(json.dumps(installed))
                with self.assertRaises(ValueError):
                    import_dlc.install(package_path, root / "fixture.keys", local)
                self.assertFalse((root / "fora").exists())

    def test_adaptation_rejects_upstream_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary)
            (runtime / "services").mkdir()
            (runtime / "services/misc.c").write_text("alterado")
            with self.assertRaises(ValueError):
                dlc.prepare(runtime)

    def test_dlc_survives_new_build_and_preserves_disabled_preference(self):
        with tempfile.TemporaryDirectory() as temporary:
            local = Path(temporary)
            old = local / "game-data/old"
            folder = old / "extracted/dlc/0100152000023001/romfs"
            files = fixture_files(folder)
            (local / "dlc.json").write_text('{"enabled":false}')
            with patch.object(import_dlc, "FILES", files):
                (folder.parent.parent / "installed.bin").write_bytes(import_dlc.manifest_bytes())
                candidate = {"data_root": str(old), "files": {}}
                (local / "candidate.json").write_text(json.dumps(candidate))
                new = {"data_root": str(local / "game-data/new"), "files": {}}
                import_dlc.attach(new, local)
                stable = local / "dlc"
                self.assertTrue((stable / "installed.bin").is_file())
                import_dlc.attach(candidate, local)
                self.assertEqual(candidate["files"], new["files"])
                self.assertEqual(json.loads((local / "dlc.json").read_text()), {"enabled": False})
                (stable / "0100152000023001/romfs/contents.dat").write_bytes(b"bad")
                with self.assertRaises(ValueError):
                    import_dlc.attach(new, local)


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), "Compilador nativo necessário")
class DlcRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        harness = cls.root / "harness.c"
        harness.write_text(HARNESS)
        cls.exe = cls.root / "dlc-test.exe"
        base = ROOT / "upstream/smm2recomp/recomp"
        result = subprocess.run([DEFAULT_COMPILER, "cc", "-target", "x86_64-windows-gnu", "-O1",
                                 '-DMBEDTLS_USER_CONFIG_FILE="mbedtls_user_config.h"',
                                 "-I" + str(base / "runtime"), "-I" + str(base / "runtime/net"),
                                 "-I" + str(base / "third_party/mbedtls/include"),
                                 "-I" + str(ROOT / "runtime/mk8/services"), harness,
                                 base / "third_party/mbedtls/library/sha256.c", base / "third_party/mbedtls/library/platform_util.c",
                                 "-o", cls.exe], capture_output=True, env=compiler_env())
        if result.returncode:
            raise RuntimeError(result.stderr.decode(errors="replace"))

    def check(self, mutate=None, installed=True, disabled=False, explicit_root=False):
        with tempfile.TemporaryDirectory() as temporary:
            extracted = Path(temporary) / "extracted"
            (extracted / "romfs").mkdir(parents=True)
            folder = extracted / "dlc/0100152000023001/romfs"
            if installed:
                files = fixture_files(folder)
                with patch.object(import_dlc, "FILES", files):
                    (folder.parent.parent / "installed.bin").write_bytes(import_dlc.manifest_bytes())
                if mutate:
                    mutate(folder)
            env = dict(os.environ)
            env.pop("MK8_DLC_ROOT", None)
            if disabled:
                env["MK8_DLC_ROOT"] = ""
            elif explicit_root:
                env["MK8_DLC_ROOT"] = str(folder.parent.parent)
            result = subprocess.run([self.exe, extracted / "romfs"], capture_output=True, text=True, timeout=20, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            return list(map(int, result.stdout.split()))

    def test_no_dlc_reports_zero_and_cannot_prepare_or_mount(self):
        values = self.check(installed=False)
        self.assertEqual(values[:4], [0, 0, 0, 0])
        self.assertNotEqual(values[5], 0)
        self.assertEqual(values[14:18], [-1, 0, -1, 1])

    def test_disabled_dlc_keeps_files_but_cannot_list_prepare_or_mount(self):
        values = self.check(disabled=True)
        self.assertEqual(values[:4], [0, 0, 0, 0])
        self.assertNotEqual(values[5], 0)
        self.assertEqual(values[14:18], [-1, 0, -1, 1])

    def test_explicit_installation_root_still_requires_real_valid_files(self):
        values = self.check(explicit_root=True)
        self.assertEqual(values[:6], [0, 1, 0, 1, 1, 0])
        self.assertEqual(values[14], 1)
        values = self.check(lambda folder: (folder / "contents.dat").write_bytes(b""), explicit_root=True)
        self.assertNotEqual(values[0], 0)
        self.assertEqual(values[14], -1)

    def test_installed_dlc_lists_one_index_and_validates_buffers(self):
        values = self.check()
        self.assertEqual(values[:6], [0, 1, 0, 1, 1, 0])
        self.assertTrue(all(values[index] != 0 for index in range(6, 11)))
        self.assertEqual(values[11:18], [0, 0xa5a5a5a5, 0xa5a5a5a5, 1, 0, -1, 1])
        self.assertNotEqual(values[18], 0)

    def test_corruption_empty_files_and_truncated_manifest_are_rejected(self):
        for mutate in (lambda folder: (folder / "contents.dat").write_bytes(b""),
                       lambda folder: (folder / "contents.dat").write_bytes(b"x" * 4096),
                       lambda folder: (folder.parent.parent / "installed.bin").write_bytes(b"MK8DLC1\0")):
            with self.subTest(mutate=mutate):
                values = self.check(mutate)
                self.assertNotEqual(values[0], 0)
                self.assertEqual(values[14], -1)

    def test_manifest_identity_version_index_and_hashes_are_checked(self):
        for offset in (0, 8, 16, 24, 28, 32, 40, 48, 80):
            def mutate(folder):
                path = folder.parent.parent / "installed.bin"
                content = bytearray(path.read_bytes())
                content[offset] ^= 1
                path.write_bytes(content)
            with self.subTest(offset=offset):
                values = self.check(mutate)
                self.assertNotEqual(values[0], 0)
                self.assertEqual(values[14], -1)


if __name__ == "__main__":
    unittest.main()

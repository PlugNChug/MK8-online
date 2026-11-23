"""Authored fixtures only: no Nintendo code or assets."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


generator = module("mk8gen", ROOT / "toolchain/recompiler/generate.py")
adapter = module("mk8prepare", ROOT / "runtime/prepare.py")
pipeline = module("mk8build", ROOT / "toolchain/build.py")


class SystemDataTests(unittest.TestCase):
    def test_optional_fonts_go_to_the_active_save_directory_and_legacy_mii_is_ignored(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root/'fonts'
            source.mkdir()
            with patch.object(pipeline, 'mirror') as junction:
                pipeline.prepare_system_data({'mii': str(root/'old-mii')}, root/'local')
                junction.assert_not_called()
                pipeline.prepare_system_data({'fonts': str(source)}, root/'local')
                junction.assert_called_once_with(root/'local/user-no-mii/fonts', str(source), copy_ok=True)



class MirrorTests(unittest.TestCase):
    def test_romfs_is_real_folders_of_hard_links_and_follows_a_new_import(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            romfs = root/'imports/romfs'
            (romfs/'Course/Gu_Menu').mkdir(parents=True)
            (romfs/'Course/Gu_Menu/course.szs').write_bytes(b'v1')
            (romfs/'Old.bin').write_bytes(b'old')
            path = root/'game-data/id/extracted/romfs'
            path.parent.mkdir(parents=True)
            # Older builds left a junction, which the launcher's RedirectionTrust refuses.
            if os.name == 'nt':
                subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(path), str(romfs)], check=True, capture_output=True)
            else:
                path.symlink_to(romfs, target_is_directory=True)
            pipeline.mirror(path, romfs)
            self.assertFalse(path.is_symlink() or getattr(path, "is_junction", lambda: False)())
            self.assertTrue((romfs/'Old.bin').is_file(), 'removing the junction must keep its target')
            self.assertTrue(os.path.samefile(path/'Course/Gu_Menu/course.szs', romfs/'Course/Gu_Menu/course.szs'))
            # A new import replaces files: links follow it and stale entries go away.
            (romfs/'Course/Gu_Menu/course.szs').unlink()
            (romfs/'Course/Gu_Menu/course.szs').write_bytes(b'v2')
            (romfs/'Old.bin').unlink()
            pipeline.mirror(path, romfs)
            self.assertEqual((path/'Course/Gu_Menu/course.szs').read_bytes(), b'v2')
            self.assertFalse((path/'Old.bin').exists())

    def test_same_drive_without_hard_links_names_the_file_system(self):
        # exFAT/FAT32 answer os.link with ERROR_INVALID_FUNCTION (WinError 1), though it is the same drive.
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            romfs = root/'imports/romfs'
            (romfs/'Course').mkdir(parents=True)
            (romfs/'Course/course.szs').write_bytes(b'v1')
            refused = OSError(22, 'Incorrect function')
            refused.winerror = 1
            with patch.object(pipeline.os, 'link', side_effect=refused):
                with self.assertRaises(ValueError) as error:
                    pipeline.mirror(root/'game-data/extracted/romfs', romfs)
            self.assertIn('não aceita hard links', str(error.exception))
            self.assertIn('NTFS', str(error.exception))
            self.assertFalse((root/'game-data/extracted/romfs/Course/course.szs').exists())

def synthetic_nso(path):
    # A hand-authored function returns 42. NSO metadata describes a single export.
    text = bytearray(0x30)
    struct.pack_into("<II", text, 0, 0x14000008, 0x100)
    struct.pack_into("<II", text, 0x20, 0xd2800540, 0xd65f03c0)
    ro = bytearray(0x200)
    ro[:4] = b"MOD0"
    struct.pack_into("<6i", ro, 4, 0x20, 0x300, 0x300, 0x1f0, 0x1f0, 0)
    for offset, tag, value in [(0x20,4,0x180),(0x30,5,0x1a0),(0x40,6,0x1c0),(0x50,0,0)]:
        struct.pack_into("<QQ",ro,offset,tag,value)
    struct.pack_into("<II",ro,0x80,1,2)
    ro[0xa0:0xa9]=b"\x00fixture\x00"
    struct.pack_into("<IBBHQQ",ro,0xc0+24,1,0x12,0,1,0x20,8)
    ro[0x1f0:0x1f4] = bytes([1, 0x1b, 0x03, 0x3b])
    data = bytes(16)
    header = bytearray(256); header[:4]=b"NSO0"
    struct.pack_into("<I",header,12,0x38)
    file_off=256
    for i,(mem_off,segment) in enumerate([(0,text),(0x100,ro),(0x400,data)]):
        struct.pack_into("<III",header,0x10+i*16,file_off,mem_off,len(segment))
        struct.pack_into("<I",header,0x60+i*4,len(segment))
        header[0xa0+32*i:0xc0+32*i]=hashlib.sha256(segment).digest()
        file_off+=len(segment)
    path.write_bytes(header+text+ro+data)


def synthetic_symbol_nso(path):
    """Three authored functions: STT_FUNC, sized STT_NOTYPE, zero-sized STT_NOTYPE marker."""
    text = bytearray(0x80)
    struct.pack_into("<II", text, 0, 0x14000008, 0x100)
    for address, value in ((0x20, 42), (0x40, 7), (0x60, 9)):
        struct.pack_into("<II", text, address, 0xd2800000 | (value << 5), 0xd65f03c0)
    ro = bytearray(0x400)
    ro[:4] = b"MOD0"
    struct.pack_into("<6i", ro, 4, 0x20, 0x300, 0x300, 0x1f0, 0x1f0, 0)
    for offset, tag, value in [(0x20, 4, 0x180), (0x30, 5, 0x1a0), (0x40, 6, 0x1c0), (0x50, 0, 0)]:
        struct.pack_into("<QQ", ro, offset, tag, value)
    struct.pack_into("<II", ro, 0x80, 1, 4)  # DT_HASH: one bucket, four symbols
    names = b"\x00func_normal\x00crypto_notype_sized\x00marker_notype_zero\x00"
    ro[0xa0:0xa0+len(names)] = names
    symbols = [(0, 0, 0, 0, 0), (1, 0x12, 1, 0x20, 8), (13, 0x10, 1, 0x40, 8), (33, 0x10, 1, 0x60, 0)]
    for index, (name, info, shndx, value, size) in enumerate(symbols):
        struct.pack_into("<IBBHQQ", ro, 0xc0 + index * 24, name, info, 0, shndx, value, size)
    ro[0x1f0:0x1f4] = bytes([1, 0x1b, 0x03, 0x3b])  # empty .eh_frame_hdr search table
    data = bytes(16)
    header = bytearray(256); header[:4] = b"NSO0"
    struct.pack_into("<I", header, 12, 0x38)
    file_off = 256
    for i, (mem_off, segment) in enumerate([(0, text), (0x100, ro), (0x500, data)]):
        struct.pack_into("<III", header, 0x10 + i * 16, file_off, mem_off, len(segment))
        struct.pack_into("<I", header, 0x60 + i * 4, len(segment))
        header[0xa0+32*i:0xc0+32*i] = hashlib.sha256(segment).digest()
        file_off += len(segment)
    path.write_bytes(header + text + ro + data)


class PipelineTests(unittest.TestCase):
    def test_game_identity_rejects_other_titles_and_bad_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "main.npdm"
            raw = bytearray(0xc0)
            raw[:4] = b"META"
            raw[0x0c] = 1
            struct.pack_into("<II", raw, 0x70, 0x80, 0x40)
            raw[0x80:0x84] = b"ACI0"
            struct.pack_into("<Q", raw, 0x90, 0x0100152000022000)
            path.write_bytes(raw)
            self.assertEqual(pipeline.validate_game_identity(path), "0100152000022000")
            raw[0x0c] = 4
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "AArch32"):
                pipeline.validate_game_identity(path)
            raw[0x0c] = 1
            path.write_bytes(raw)
            (path.parent / "subsdk9").write_bytes(b"fixture")
            self.assertTrue(any("subsdk9" in e for e in pipeline.preflight({"exefs": directory})))
            struct.pack_into("<Q", raw, 0x90, 0x0100000000010000)
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "Jogo incorreto"):
                pipeline.validate_game_identity(path)
            struct.pack_into("<I", raw, 0x70, 0xffffff00)
            path.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "ACI0"):
                pipeline.validate_game_identity(path)

    def test_missing_inputs_fail_with_actionable_errors(self):
        errors=pipeline.preflight({})
        self.assertTrue(any("main.npdm" in e for e in errors))

    def test_nso_rejects_hash_corruption_and_large_segment(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/"fixture";synthetic_nso(p)
            self.assertEqual(len(generator.validate_nso(p,64*1024*1024)),64)
            raw=bytearray(p.read_bytes());raw[-1]^=1;p.write_bytes(raw)
            with self.assertRaisesRegex(ValueError,"hash"):
                generator.validate_nso(p,64*1024*1024)
            synthetic_nso(p);raw=bytearray(p.read_bytes());struct.pack_into("<I",raw,0x18,0xffffffff);p.write_bytes(raw)
            with self.assertRaisesRegex(ValueError,"bounds"):
                generator.validate_nso(p,64*1024*1024)

    def test_runtime_adapter_tracks_third_module_and_stops_unknown_hle(self):
        with tempfile.TemporaryDirectory() as directory:
            out=Path(directory)/"source"
            result=adapter.prepare(ROOT/"upstream/smm2recomp",out)
            self.assertEqual(result["abi"],3)
            self.assertFalse(result["gameplay_verified"])
            dispatch=(out/"runtime/dispatch.c").read_text()
            self.assertIn("RECOMP_MODULE_COUNT",dispatch)
            self.assertNotIn("t < 2",dispatch)
            self.assertIn("required host service is not implemented",dispatch)
            self.assertIn("RECOMP_MODULES(TABLE)",(out/"tools/gamelib_entry.c").read_text())
            self.assertIn("X(subsdk4,", (out/"runtime/module_config.h").read_text())
            with self.assertRaisesRegex(ValueError,"fresh"):
                adapter.prepare(ROOT/"upstream/smm2recomp",out)

    def test_real_generator_and_resume_with_authored_nso(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);exefs=root/"exefs";exefs.mkdir();synthetic_nso(exefs/"main")
            profile={"modules":{"main":"0x7000000000"},"module_window_bytes":64*1024*1024,"simd":"c"}
            source=ROOT/"upstream/smm2recomp/recomp";output=root/"generated"
            generator.generate(source,exefs,output,profile)
            chunk=output/"main/main_000.c"
            self.assertTrue(chunk.is_file())
            text=chunk.read_text();self.assertIn("f_7000000020",text)
            stamp=chunk.stat().st_mtime_ns
            generator.generate(source,exefs,output,profile)
            self.assertEqual(stamp,chunk.stat().st_mtime_ns)
            chunk.write_text("corrupted")
            generator.generate(source,exefs,output,profile)
            self.assertEqual(text,chunk.read_text())
            receipt = output/'main-receipt.json'
            for damaged in ('{interrupted', '[]', '{"files": []}'):
                with self.subTest(receipt=damaged):
                    receipt.write_text(damaged)
                    chunk.write_text('incomplete generated C')
                    generator.generate(source,exefs,output,profile)
                    self.assertEqual(text, chunk.read_text())
                    self.assertEqual(json.loads(receipt.read_text())['files'][chunk.name], pipeline.sha(chunk))

    def test_sized_notype_text_symbols_become_dispatch_entries(self):
        self.assertTrue(adapter.symbol_function_entry(0x12, 8))   # STT_FUNC
        self.assertTrue(adapter.symbol_function_entry(0x10, 172)) # STT_NOTYPE, sized: crypto helper
        self.assertFalse(adapter.symbol_function_entry(0x10, 0))  # zero-sized marker
        self.assertFalse(adapter.symbol_function_entry(0x11, 8))  # STT_OBJECT
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prepared = root/"runtime"
            adapter.prepare(ROOT/"upstream/smm2recomp", prepared)
            exefs = root/"exefs"; exefs.mkdir(); synthetic_symbol_nso(exefs/"sdk")
            profile = {"modules": {"sdk": "0x7004000000"}, "module_window_bytes": 64*1024*1024, "simd": "c"}
            generated = root/"generated"
            generator.generate(prepared, exefs, generated, profile)
            table = (generated/"sdk/sdk_table.c").read_text()
            self.assertIn("0x7004000020ull", table)
            self.assertIn("0x7004000040ull", table)  # the sized STT_NOTYPE entry
            self.assertNotIn("0x7004000060ull", table)  # zero-sized NOTYPE stays interior
            self.assertIn("void f_7004000040(", (generated/"sdk/sdk_000.c").read_text())

    def test_missing_runtime_receipt_never_rebaselines_changed_sources(self):
        with tempfile.TemporaryDirectory(prefix='mk8-runtime-repair-') as temporary:
            job = Path(temporary)/'build'
            job.mkdir()
            saved = Path(temporary)/'user-save.bin'
            saved.write_bytes(b'preserve')
            profile = ROOT/'games/mk8d/profiles/development.json'
            prepared = pipeline.prepare_runtime(job, ROOT/'upstream/smm2recomp', profile)
            source = prepared/'runtime/dispatch.c'
            original = source.read_bytes()
            receipt = job/'runtime-hashes.json'
            self.assertTrue(pipeline.runtime_matches(prepared, receipt))
            stamp = source.stat().st_mtime_ns
            pipeline.prepare_runtime(job, ROOT/'upstream/smm2recomp', profile)
            self.assertEqual(stamp, source.stat().st_mtime_ns)
            receipt.unlink()
            source.write_bytes(b'damaged runtime source')
            with patch('storage.shutil.disk_usage', return_value=SimpleNamespace(free=3*1024**3)):
                with self.assertRaisesRegex(ValueError, 'preservar espaço'):
                    pipeline.prepare_runtime(job, ROOT/'upstream/smm2recomp', profile)
            self.assertEqual(source.read_bytes(), b'damaged runtime source')
            pipeline.prepare_runtime(job, ROOT/'upstream/smm2recomp', profile)
            self.assertEqual(source.read_bytes(), original)
            for invalid in ('{interrupted', '[]', '{}'):
                receipt.write_text(invalid)
                self.assertFalse(pipeline.runtime_matches(prepared, receipt))
            pipeline.prepare_runtime(job, ROOT/'upstream/smm2recomp', profile)
            self.assertTrue(pipeline.runtime_matches(prepared, receipt))
            self.assertEqual(saved.read_bytes(), b'preserve')


class RuntimeDependencyManifestTests(unittest.TestCase):
    def test_linux_capabilities_read_the_undefined_symbols_of_the_game_library(self):
        # #128: Linux links every module into mk8dgame.so; the hooks are host functions it leaves undefined.
        import shutil
        from native import DEFAULT_COMPILER
        hooks = ('mk8_split_race_full_speed', 'mk8_guest_lan_hold_boot', 'mk8_guest_lan_hide_boot_logo',
                 'mk8_guest_lan_start')
        with tempfile.TemporaryDirectory() as folder:
            stage = Path(folder)
            if os.name == 'nt':
                if not DEFAULT_COMPILER.is_file():
                    self.skipTest('Requires the native compiler')
                compiler = [str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-linux-gnu']
            else:
                found = shutil.which('cc') or shutil.which('clang')
                if not found:
                    self.skipTest('Requires a C compiler')
                compiler = [found]
            def library(called):
                source, so = stage/'game.c', stage/'mk8dgame.so'
                source.write_text(''.join(f'int {name}(void);' + chr(10) for name in called)
                                  + 'int guest(void) { return 0' + ''.join(f' + {name}()' for name in called) + '; }' + chr(10))
                subprocess.run([*compiler, '-shared', '-fPIC', '-nostdlib', str(source), '-o', str(so)], check=True)
                return {'gamelib': {'path': str(so)}}
            files = library(hooks)
            self.assertTrue(set(h.encode() for h in hooks) <= pipeline.elf_undefined_symbols(files['gamelib']['path']))
            self.assertTrue(pipeline.guest_has_split_screen_hook(files))
            self.assertTrue(pipeline.guest_has_direct_lan(files))
            files = library(hooks[1:3])
            self.assertFalse(pipeline.guest_has_split_screen_hook(files))
            self.assertFalse(pipeline.guest_has_direct_lan(files))
            self.assertFalse(pipeline.guest_has_direct_lan({}))

    @unittest.skipUnless(os.name == 'nt', 'Windows PE modules (zig dlltool and an x86_64-windows-gnu DLL)')
    def test_split_cadence_capability_checks_the_linked_main_module(self):
        from native import DEFAULT_COMPILER, compiler_env
        if not DEFAULT_COMPILER.is_file():
            self.skipTest('Requires the native compiler')
        with tempfile.TemporaryDirectory() as folder:
            stage = Path(folder)
            env = compiler_env()
            definitions = stage/'host.def'
            definitions.write_text('LIBRARY mk8drecomp.exe\nEXPORTS\nmk8_split_race_full_speed\nother_symbol\n')
            lib = stage/'host.a'
            subprocess.run([DEFAULT_COMPILER, 'dlltool', '-m', 'i386:x86-64', '-d', definitions, '-l', lib],
                           env=env, check=True)
            for symbol, expected in [('other_symbol', False), ('mk8_split_race_full_speed', True)]:
                source = stage/'main.c'
                source.write_text(f'__declspec(dllimport) int {symbol}(void);\n'
                                  f'__declspec(dllexport) int guest(void) {{ return {symbol}(); }}\n')
                dll = stage/'mk8d-code-main.dll'
                subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-shared',
                                source, lib, '-o', dll], env=env, check=True)
                self.assertEqual(pipeline.guest_has_split_screen_hook({'mk8d-code-main.dll': {'path': str(dll)}}),
                                 expected)

    @unittest.skipUnless(os.name == 'nt', 'Windows PE modules (zig dlltool and an x86_64-windows-gnu DLL)')
    def test_direct_lan_requires_every_boot_hook_in_the_linked_main(self):
        from native import DEFAULT_COMPILER, compiler_env
        if not DEFAULT_COMPILER.is_file():
            self.skipTest('Requires the native compiler')
        hooks = ('mk8_guest_lan_hold_boot', 'mk8_guest_lan_hide_boot_logo', 'mk8_guest_lan_start')
        with tempfile.TemporaryDirectory() as folder:
            stage = Path(folder)
            env = compiler_env()
            defs, lib = stage/'host.def', stage/'host.a'
            defs.write_text('LIBRARY mk8drecomp.exe\nEXPORTS\n' + '\n'.join(hooks))
            subprocess.run([DEFAULT_COMPILER,'dlltool','-m','i386:x86-64','-d',defs,'-l',lib],env=env,check=True)
            dll, source = stage/'mk8d-code-main.dll', stage/'main.c'
            files = {dll.name: {'path':str(dll)}}
            for missing in (*hooks,None):
                linked = [hook for hook in hooks if hook!=missing]
                source.write_text(''.join(f'__declspec(dllimport) int {name}(void);\n' for name in linked) +
                                  '__declspec(dllexport) int guest(void) { return ' +
                                  ' + '.join(f'{name}()' for name in linked) + '; }')
                subprocess.run([DEFAULT_COMPILER,'cc','-target','x86_64-windows-gnu','-shared',source,lib,'-o',dll],
                               env=env,check=True)
                self.assertEqual(pipeline.guest_has_direct_lan(files), missing is None)
            self.assertFalse(pipeline.guest_has_direct_lan({'mk8d-code-sdk.dll':files[dll.name]}))

    def test_runtime_refresh_records_new_dll_without_changing_game_or_saves(self):
        from host import GAME_NAME, HOST_NAME, WINDOWS

        with tempfile.TemporaryDirectory(prefix='mk8-runtime-refresh-') as temporary:
            local = Path(temporary).resolve()
            source = local/'runtime'
            source.mkdir()
            (local/'runtime-hashes.json').write_text('{}')
            (local/'generation.json').write_text(json.dumps({'source': str(source), 'identity': 'fixture'}))
            output = local/'host'
            output.mkdir()
            exe, game = output/HOST_NAME, output/GAME_NAME
            exe.write_bytes(b'fixture runtime')
            game.write_bytes(b'preserved game')
            save = local/'user-save.bin'
            save.write_bytes(b'preserved save')
            native = local/'native.json'
            native.write_bytes(b'{"identity": "fixture", "object_files": []}')
            layout = output/'settings-menu/romfs/UI/cmn/menu.sarc'
            layout.parent.mkdir(parents=True)
            layout.write_bytes(b'authored settings menu')
            common_layout = layout.with_name('common.sarc')
            common_layout.write_bytes(b'authored settings common layout')
            controller_layout = output/'controller-menu/romfs/UI/cmn/common.sarc'
            controller_layout.parent.mkdir(parents=True)
            controller_layout.write_bytes(common_layout.read_bytes())
            (local/'candidate.json').write_text(json.dumps({'executable': pipeline.file_record(exe),
                'files': {'gamelib': pipeline.file_record(game)}, 'data_root': str(local/'game-data/test'),
                'gameplay_verified': False}))
            def dependencies(*args, **kwargs):
                (output/'vulkan-1.dll').write_bytes(b'verified loader fixture')
            # Archive derivation has separate authored tests; this fixture checks refresh records.
            with (patch('host.build_host', return_value=exe),
                  patch.object(pipeline.subprocess, 'run', side_effect=dependencies),
                  patch('settings_layout.prepare', return_value=layout) as prepare_layout):
                pipeline.refresh_runtime(local)
            prepare_layout.assert_called_once_with(local/'game-data/test/extracted/romfs', output)
            candidate = json.loads((local/'candidate.json').read_text())
            if WINDOWS:
                self.assertEqual(candidate['files']['vulkan-1.dll'], pipeline.file_record(output/'vulkan-1.dll'))
            self.assertEqual(candidate['files']['gamelib'], pipeline.file_record(game))
            self.assertEqual(candidate['files']['settings-layout'], pipeline.file_record(layout))
            self.assertEqual(candidate['files']['settings-common-layout'], pipeline.file_record(common_layout))
            self.assertFalse(candidate['split_screen_60fps_supported'])
            self.assertTrue(candidate['bloom_supported'])
            self.assertEqual(candidate['files']['controller-layout'], pipeline.file_record(controller_layout))
            self.assertEqual(game.read_bytes(), b'preserved game')
            self.assertEqual(save.read_bytes(), b'preserved save')
            self.assertEqual(native.read_bytes(), b'{"identity": "fixture", "object_files": []}')
            self.assertFalse(candidate['gameplay_verified'])
            self.assertFalse(candidate['direct_lan_guest_supported'])
            self.assertTrue(candidate['dlc_supported'])
            self.assertTrue(candidate['pack_supported'])  # (runtime/mk8/support/romfs_overlay.c)


if __name__=="__main__":
    unittest.main()

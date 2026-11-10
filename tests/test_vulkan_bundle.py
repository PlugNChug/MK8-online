"""Exercise the real SDL/Vulkan binaries in an isolated runtime dependency folder."""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
sys.path.insert(0, str(ROOT/'scripts'))
import host
import pefile


@unittest.skipUnless(os.name == 'nt' and (host.DEPS/'bin/vulkan-1.dll').is_file(), 'Pinned Windows dependencies required')
class VulkanBundleTests(unittest.TestCase):
    def test_launcher_probe_detects_missing_corrupt_and_missing_dependency(self):
        launcher = Path(os.environ.get('MK8_LAUNCHER_EXE', ROOT/'target/release/Launcher.exe'))
        if not launcher.is_file():
            self.skipTest('Build the launcher first')
        with tempfile.TemporaryDirectory(prefix='mk8-vulkan-probe-') as temporary:
            folder = Path(temporary)
            def probe():
                return subprocess.run([str(launcher), '--check-vulkan', str(folder)],
                    capture_output=True, timeout=25, creationflags=subprocess.CREATE_NO_WINDOW)
            missing = probe()
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn(b'vulkan-1.dll', missing.stderr)
            (folder/'vulkan-1.dll').write_bytes(b'not a DLL')
            invalid = probe()
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn(b'193', invalid.stderr)
            original = (host.DEPS/'bin/vulkan-1.dll').read_bytes()
            self.assertIn(b'CFGMGR32.dll', original)
            (folder/'vulkan-1.dll').write_bytes(original.replace(b'CFGMGR32.dll', b'MISSING0.dll'))
            dependency = probe()
            self.assertNotEqual(dependency.returncode, 0)
            self.assertIn(b'126', dependency.stderr)
            (folder/'vulkan-1.dll').write_bytes(original)
            shutil.copyfile(host.DEPS/'bin/SDL3.dll', folder/'SDL3.dll')
            resolve = runpy.run_path(str(ROOT/'scripts/runtime-dlls.py'))['resolve']
            resolve(folder, roots=[folder/'SDL3.dll', folder/'vulkan-1.dll'])
            loaded = probe()
            self.assertEqual(loaded.returncode, 0, loaded.stderr.decode(errors='replace'))

    def test_prefetch_without_additional_downloads_writes_verified_receipt(self):
        with tempfile.TemporaryDirectory(prefix='mk8-prefetch-ready-') as temporary:
            tools = Path(temporary)
            script = runpy.run_path(str(ROOT/'scripts/runtime-dlls.py'))
            prefetch = script['prefetch']
            deps = tools/'deps'
            binaries = deps/'packages/ucrt64/bin'
            binaries.mkdir(parents=True)
            for name in ('SDL3.dll', 'vulkan-1.dll', 'libshaderc_shared.dll'):
                shutil.copyfile(host.DEPS/'bin'/name, binaries/name)
            def resolved(folder, roots):
                # No secondary archives are needed, hence no DLL cache index is written.
                self.assertEqual(len(roots), 7)
                return {}
            with patch.dict(prefetch.__globals__, DEPS=deps, CACHE=deps/'dlls', resolve=resolved):
                prefetch()
            import json
            receipt = json.loads((deps/'runtime-ready.json').read_text())
            self.assertEqual(len(receipt['files']), 7)
            self.assertFalse((deps/'dlls/index.json').exists())
            for file in receipt['files']:
                base = tools if file['scope'] == 'tools' else ROOT
                self.assertEqual(host.digest(base/file['path']), file['sha256'])

    def test_sdl_dynamic_loader_is_copied_retained_and_loadable(self):
        with tempfile.TemporaryDirectory(prefix='mk8-vulkan-bundle-') as temporary:
            output = Path(temporary)
            host.copy_runtime_dlls(output)
            shutil.copyfile(ROOT/'prebuilt/host/mk8drecomp.exe', output/'runtime.exe')
            self.assertTrue((output/'vulkan-1.dll').is_file())
            with pefile.PE(str(output/'SDL3.dll')) as pe:
                self.assertNotIn(b'vulkan-1.dll', [entry.dll.lower() for entry in pe.DIRECTORY_ENTRY_IMPORT])
            resolve = runpy.run_path(str(ROOT/'scripts/runtime-dlls.py'))['resolve']
            def no_download(*args, **kwargs):
                raise AssertionError('Prepared dependencies must work offline')
            with patch.dict(resolve.__globals__, fetch=no_download, fetch_all=no_download):
                resolve(output)
                resolve(output)  # The pruning pass must not remove the dynamically loaded DLL.
            self.assertTrue((output/'vulkan-1.dll').is_file())
            self.assertEqual(host.digest(output/'vulkan-1.dll'), host.digest(host.DEPS/'bin/vulkan-1.dll'))
            script = r'''
import ctypes, os, sys
from pathlib import Path
folder = Path(sys.argv[1])
with os.add_dll_directory(str(folder)):
    sdl = ctypes.CDLL(str(folder/'SDL3.dll'))
    sdl.SDL_Init.argtypes = [ctypes.c_uint32]
    sdl.SDL_Init.restype = ctypes.c_bool
    sdl.SDL_GetError.restype = ctypes.c_char_p
    sdl.SDL_Vulkan_LoadLibrary.argtypes = [ctypes.c_char_p]
    sdl.SDL_Vulkan_LoadLibrary.restype = ctypes.c_bool
    sdl.SDL_Vulkan_GetVkGetInstanceProcAddr.restype = ctypes.c_void_p
    assert sdl.SDL_Init(0x20), sdl.SDL_GetError()
    try:
        assert sdl.SDL_Vulkan_LoadLibrary(str(folder/'vulkan-1.dll').encode()), sdl.SDL_GetError()
        assert sdl.SDL_Vulkan_GetVkGetInstanceProcAddr()
        sdl.SDL_Vulkan_UnloadLibrary()
    finally:
        sdl.SDL_Quit()
'''
            result = subprocess.run([sys.executable, '-c', script, str(output)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

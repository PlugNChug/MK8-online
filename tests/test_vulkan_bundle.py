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

"""Issue #78: the Vulkan instance and SDL's surface use the same vulkan-1.dll even when another
module (an overlay) loaded the driver's copy first."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'toolchain'))
from native import DEFAULT_COMPILER, compiler_env
import host

SYSTEM_LOADER = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/vulkan-1.dll'
UPSTREAM = ROOT / 'upstream/smm2recomp/recomp'
PROBE = r'''
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <wchar.h>
HMODULE vk_shim_module(void);
int main(void) {
    // An overlay loaded the driver's loader before the runtime started.
    if (!LoadLibraryW(L"C:\\Windows\\System32\\vulkan-1.dll")) return 77;
    const char *ext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext};
    VkInstance instance;
    if (vkCreateInstance(&ci, NULL, &instance) != VK_SUCCESS) return 78;  // (through the shim)
    // SDL_Vulkan_CreateSurface: SDL's own loader, from SDL_VULKAN_LIBRARY.
    static wchar_t path[32768];
    if (!GetEnvironmentVariableW(L"SDL_VULKAN_LIBRARY", path, 32768)) return 79;
    HMODULE sdl = LoadLibraryW(path);
    static wchar_t name[32768];
    GetModuleFileNameW(vk_shim_module(), name, 32768);
    fwprintf(stderr, L"instance loader: %ls\n", name);
    GetModuleFileNameW(sdl, name, 32768);
    fwprintf(stderr, L"surface loader: %ls\n", name);
    if (sdl != vk_shim_module()) return 1;  // (two loaders: the surface lookup is undefined, issue #78)
    PFN_vkGetInstanceProcAddr gpa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(sdl, "vkGetInstanceProcAddr");
    int found = gpa && gpa(instance, "vkCreateWin32SurfaceKHR") != NULL;
    vkDestroyInstance(instance, NULL);
    return found ? 0 : 2;
}
'''


@unittest.skipUnless(os.name == 'nt' and DEFAULT_COMPILER.is_file() and SYSTEM_LOADER.is_file()
                     and (host.DEPS / 'bin/vulkan-1.dll').is_file(),
                     'Windows, the native compiler, a Vulkan driver and the runtime dependencies are required')
class VulkanLoaderShimTest(unittest.TestCase):
    def build(self, root, patched):
        shim = root / ('patched.c' if patched else 'original.c')
        subprocess.run([sys.executable, UPSTREAM / 'tools/gen_vk_shim.py', host.DEPS / 'include',
                        UPSTREAM / 'runtime/platform/vulkan-1.exports', shim], check=True, capture_output=True)
        if patched:
            host.patch_vk_shim(shim)
        # (test access to the module the shim chose)
        shim.write_text(shim.read_text(encoding='utf-8') + '\nHMODULE vk_shim_module(void) { return vk_shim_dll; }\n',
                        encoding='utf-8')
        probe = root / 'probe.c'
        probe.write_text(PROBE, encoding='utf-8')
        exe = root / ('patched.exe' if patched else 'original.exe')
        build = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1',
                                '-I' + str(host.DEPS / 'include'), str(probe), str(shim), '-o', str(exe)],
                               env=compiler_env(), capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stderr[-3000:])
        return exe

    def test_instance_and_surface_share_the_launcher_loader(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = root / 'runtime'
            runtime.mkdir()
            bundled = runtime / 'vulkan-1.dll'  # (the runtime's own copy, as the launcher ships it)
            shutil.copyfile(host.DEPS / 'bin/vulkan-1.dll', bundled)
            env = dict(os.environ, SDL_VULKAN_LIBRARY=str(bundled))
            results = {}
            for patched in (False, True):
                result = subprocess.run([str(self.build(root, patched))], env=env, cwd=root,
                                        capture_output=True, text=True, timeout=60)
                if result.returncode in (77, 78):
                    self.skipTest('No usable Vulkan driver on this machine')
                results[patched] = result
            # The generated shim takes the overlay's loader by name, so the instance and SDL's surface lookup
            # use two loaders (undefined: the report's NULL). The patched shim uses SDL's loader.
            self.assertEqual(results[False].returncode, 1, results[False].stderr)
            self.assertIn('instance loader: ' + str(SYSTEM_LOADER.resolve()).lower(), results[False].stderr.lower())
            self.assertEqual(results[True].returncode, 0, results[True].stderr)
            self.assertIn('[vk] loader ' + str(bundled), results[True].stderr)

    def test_changed_boundary_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            shim = Path(folder) / 'shim.c'
            shim.write_text('int vk_shim_load(void) { return 0; }\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                host.patch_vk_shim(shim)


if __name__ == '__main__':
    unittest.main()

"""The guest heap import stays off shared memory on integrated GPUs (crash report: AMD APU, 0.2.0)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import shared_vram

FIXTURE = r'''
#include <stdbool.h>
#include <stdint.h>
#define HEAP_MAX 0x00D0000000ull
#define VK_MEMORY_HEAP_DEVICE_LOCAL_BIT 1u
typedef enum { VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU = 1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU = 2 } VkPhysicalDeviceType;
typedef struct { uint64_t size; uint32_t flags; } VkMemoryHeap;
typedef struct { uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16]; } VkPhysicalDeviceMemoryProperties;
''' + '%s' + r'''
#define MB(x) ((uint64_t)(x) << 20)
static VkPhysicalDeviceMemoryProperties two(uint64_t local, uint64_t system, uint32_t system_flags) {
    VkPhysicalDeviceMemoryProperties m = {2, {{local, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT}, {system, system_flags}}};
    return m;
}
int main(void) {
    VkPhysicalDeviceMemoryProperties m;
    m = two(MB(512), MB(3900), 0);   /* AMD APU, 8 GB RAM (the report's layout) */
    if (!mk8_shared_heap_too_small(&m, 1, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)) return 1;
    m = two(MB(2048), MB(15000), 0); /* AMD APU, 32 GB RAM: room for the import */
    if (mk8_shared_heap_too_small(&m, 1, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)) return 2;
    m = two(MB(8192), MB(8000), 0);  /* discrete GPU: system heap is not GPU memory */
    if (mk8_shared_heap_too_small(&m, 1, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) return 3;
    m.memoryHeapCount = 1; m.memoryHeaps[0].size = MB(4000);  /* Intel iGPU, one device-local heap (upstream rule) */
    if (!mk8_shared_heap_too_small(&m, 0, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) return 4;
    if (mk8_shared_heap_too_small(&m, -1, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)) return 5;
    if (mk8_shared_heap_too_small(&m, 3, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)) return 6;
    return 0;
}
'''


class SharedVramTests(unittest.TestCase):
    def test_boundaries_are_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            device = Path(folder)/'nvn/vulkan/device.c'
            device.parent.mkdir(parents=True)
            device.write_text('static void pick_device(void) {\n}\n', encoding='utf-8')
            with self.assertRaises(ValueError):
                shared_vram.prepare(Path(folder))

    @unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
    def test_integrated_gpus_skip_the_import_when_shared_memory_is_small(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', out/'host')
            text = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            self.assertIn('bool low_vram = mk8_shared_heap_too_small(&mem_props, host_heap, g_vk.props.deviceType);', text)
            self.assertIn('[vk] memory heap %u', text)
            start = text.index('static bool mk8_shared_heap_too_small(')
            helper = text[start:text.index('\n}\n', start) + 3]
            source = out/'shared_vram.c'
            source.write_text(FIXTURE % helper, encoding='utf-8')
            exe = out/'shared_vram.exe'
            subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O2', str(source), '-o', str(exe)],
                           env=compiler_env(), check=True, capture_output=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()

"""Run the prepared driver/layer report against synthetic Vulkan properties."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER, compiler_env
import vulkan_driver_log

HARNESS = r'''
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char out[16384];
#define LOG(...) snprintf(out + strlen(out), sizeof out - strlen(out), __VA_ARGS__)
static struct { VkPhysicalDevice phys; VkPhysicalDeviceProperties props; } g_vk;
static uint32_t available, queries;
static VkResult result_with_data = VK_SUCCESS;
static void vkGetPhysicalDeviceProperties2(VkPhysicalDevice phys, VkPhysicalDeviceProperties2 *p) {
    assert(phys == g_vk.phys && p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    VkPhysicalDeviceDriverProperties *d = p->pNext;
    assert(d && d->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    d->driverID = VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    memset(d->driverName, 'N', sizeof d->driverName);  // unterminated on purpose
    memset(d->driverInfo, 'I', sizeof d->driverInfo);
}
static VkResult vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *layers) {
    ++queries;
    if (!layers) { *count = available; return VK_SUCCESS; }
    uint32_t n = *count < available ? *count : available;
    for (uint32_t i = 0; i < n; i++) {
        memset(&layers[i], 0, sizeof layers[i]);
        if (i == 1) memset(layers[i].layerName, 'L', sizeof layers[i].layerName);
        else snprintf(layers[i].layerName, sizeof layers[i].layerName, "VK_LAYER_TEST_%u", i);
        layers[i].specVersion = VK_MAKE_API_VERSION(0, 1, 4, 357);
        layers[i].implementationVersion = i;
    }
    *count = n;
    return n < available ? VK_INCOMPLETE : result_with_data;
}
static unsigned count(const char *what) {
    unsigned n = 0;
    for (const char *p = out; (p = strstr(p, what)); p += strlen(what)) n++;
    return n;
}
static void report(void) {
BLOCK
}
int main(void) {
    g_vk.phys = (VkPhysicalDevice)(uintptr_t)0x1234;
    snprintf(g_vk.props.deviceName, sizeof g_vk.props.deviceName, "Test GPU");
    g_vk.props.driverVersion = 0x9b5c4000; g_vk.props.vendorID = 0x10de; g_vk.props.deviceID = 0x2c05;
    available = 40; report();
    assert(strstr(out, "[vk] using Test GPU\n"));
    assert(strstr(out, "(id 4, raw version 0x9b5c4000, device 10de:2c05)\n"));
    char name[VK_MAX_DRIVER_NAME_SIZE]; memset(name, 'N', sizeof name - 1); name[sizeof name - 1] = 0;
    char full[VK_MAX_DRIVER_NAME_SIZE + 1]; memset(full, 'N', VK_MAX_DRIVER_NAME_SIZE); full[VK_MAX_DRIVER_NAME_SIZE] = 0;
    assert(strstr(out, name) && !strstr(out, full));  // terminated inside the driver's fixed field
    assert(count("[vk] instance layer available: ") == 32 && queries == 2);  // a fixed buffer, VK_INCOMPLETE kept
    assert(strstr(out, "VK_LAYER_TEST_31 (spec 1.4.357, version 31)\n") && !strstr(out, "VK_LAYER_TEST_32"));
    memset(out, 0, sizeof out); queries = 0; available = 0; report();
    assert(!count("instance layer available") && queries == 1 && strstr(out, "[vk] driver "));
    memset(out, 0, sizeof out); available = 3; result_with_data = VK_ERROR_OUT_OF_HOST_MEMORY; report();
    assert(!count("instance layer available"));
    puts("driver log ok");
    return 0;
}
'''


@unittest.skipUnless(DEFAULT_COMPILER.is_file() and (DEPS/'include/vulkan/vulkan.h').is_file(), 'Native tools required')
class VulkanDriverLogTests(unittest.TestCase):
    def test_prepared_report_bounds_driver_strings_and_layer_list(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', root/'host')
            device = (runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            self.assertEqual(device.count(vulkan_driver_log.LOG_DRIVER), 1)
            # The report must run before the device exists: vkCreateDevice is where #94 and #111 stopped.
            self.assertLess(device.index(vulkan_driver_log.LOG_DRIVER), device.index('vkCreateDevice(g_vk.phys'))
            source = root/'driver_log.c'
            source.write_text(HARNESS.replace('BLOCK', vulkan_driver_log.LOG_DRIVER), encoding='utf-8')
            exe = root/'driver_log.exe'
            build = subprocess.run([DEFAULT_COMPILER, 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                    '-I'+str(DEPS/'include'), source, '-o', exe],
                                   env=compiler_env(), capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([exe], capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('driver log ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
